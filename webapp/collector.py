#!/usr/bin/env python3
"""BLE collection: passive discovery, and connected capture.

Two jobs on one event loop.

**Scanning** runs continuously. It finds tags, stores DF5 for context, and
reassembles 0xC2 broadcast spectra. This is what works with no connection and
any number of listeners, and it is the only thing that works when the tag is
somewhere you cannot reach.

**Streaming** connects to one tag and subscribes to raw samples. This is the
capture path: 400 Hz x 3 axes, indexed so losses are measurable. Only one tag
at a time - the firmware allows one connection, and two simultaneous captures
would compete for the same radio here as well.

Run standalone:
    python -m webapp.collector                     # scan only, until Ctrl-C
    python -m webapp.collector --stream AA:BB:...  # scan and capture
    python -m webapp.collector -d 60               # stop after 60 s
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import threading
import time

import numpy as np
from bleak import BleakClient, BleakScanner

from . import protocol, store

# Flush a block roughly once a second. Smaller means more SQLite round trips
# for no benefit; larger means a crash loses more, and the live view lags by
# however long the block is.
BLOCK_SAMPLES = 400

RECONNECT_DELAY_S = 3.0


class StreamSession:
    """One connected capture. Owns its buffer and its reconnect loop.

    `resolve` hands back the BLEDevice our own scanner most recently saw for
    this address. Connecting by address string instead fails on Windows with
    "device not found": BleakClient runs its own discovery, and WinRT will not
    run two scans at once, so ours starves it. Passing the object we already
    have skips that second discovery entirely.
    """

    def __init__(self, mac: str, conn, stats: dict, resolve):
        self.mac = mac.upper()
        self._conn = conn
        self._stats = stats
        self._resolve = resolve
        self._buf: list[np.ndarray] = []
        self._buf_index: int | None = None
        self._buf_n = 0
        self._next_index: int | None = None
        self._pending_gap = False
        self._fs_mhz = 0
        self.info: dict | None = None
        self.connected = False
        self.error: str | None = None
        self._stop = asyncio.Event()

    # -- buffering ---------------------------------------------------------

    def _flush(self) -> None:
        if not self._buf or self._buf_index is None:
            return
        block = np.concatenate(self._buf, axis=0)
        try:
            store.insert_block(self._conn, self.mac, self._buf_index, block,
                               fs_mhz=self._fs_mhz, gap=self._pending_gap)
            self._stats["samples"] += int(block.shape[0])
            self._stats["blocks"] += 1
        except Exception as exc:                       # keep capturing
            self._stats["errors"] += 1
            print(f"block store error: {exc}", file=sys.stderr)
        self._buf = []
        self._buf_index = None
        self._buf_n = 0
        self._pending_gap = False

    def _on_notify(self, _sender, data: bytearray) -> None:
        try:
            pkt = protocol.decode_stream_packet(bytes(data))
        except ValueError as exc:
            self._stats["errors"] += 1
            print(f"stream decode error: {exc}", file=sys.stderr)
            return

        self._stats["packets"] += 1
        idx = pkt["first_index"]
        n = pkt["count"]
        if n == 0:
            return

        samples = np.asarray(pkt["samples_mg"], dtype=np.int16).reshape(n, 3)

        # A discontinuity ends the current block. Blocks carry one starting
        # index each, so appending across a gap would claim samples are
        # contiguous when they are not - and the reassembler downstream would
        # believe it, shifting everything after the gap earlier in time.
        broken = (self._next_index is not None and idx != self._next_index) or pkt["gap"]
        if broken:
            missing = (idx - self._next_index) if self._next_index is not None else 0
            self._stats["gaps"] += 1
            self._stats["lost_samples"] += max(0, int(missing))
            self._flush()
            self._pending_gap = True

        if self._buf_index is None:
            self._buf_index = idx
        self._buf.append(samples)
        self._buf_n += n
        self._next_index = idx + n

        if self._buf_n >= BLOCK_SAMPLES:
            self._flush()

    # -- connection --------------------------------------------------------

    async def run(self) -> None:
        while not self._stop.is_set():
            try:
                await self._session()
            except Exception as exc:
                self.error = str(exc)
                self._stats["errors"] += 1
                print(f"stream session error: {exc}", file=sys.stderr)
            finally:
                self.connected = False
                self._flush()
            if self._stop.is_set():
                break
            # A tag that went out of range comes back; reconnecting beats
            # making the operator notice and click something.
            await asyncio.sleep(RECONNECT_DELAY_S)

    async def _session(self) -> None:
        target = self._resolve(self.mac)
        if target is None:
            # Not heard yet. The tag advertises every 1.28 s, so this resolves
            # in a couple of seconds or the tag is not there at all.
            self.error = f"waiting to hear {self.mac} advertise"
            await asyncio.sleep(2.0)
            return

        async with BleakClient(target, timeout=25.0) as client:
            self.connected = True
            self.error = None

            raw = await client.read_gatt_char(protocol.CHAR_INFO)
            self.info = protocol.decode_info(bytes(raw))
            self._fs_mhz = int(round((self.info.get("measured_hz")
                                      or self.info.get("nominal_hz") or 0) * 1000))
            store.set_info(self._conn, self.mac, json.dumps(self.info))

            await client.start_notify(protocol.CHAR_SAMPLES, self._on_notify)
            await client.write_gatt_char(protocol.CHAR_CONTROL, protocol.CMD_START,
                                         response=True)
            print(f"streaming from {self.mac} at "
                  f"{protocol.effective_rate_hz(self.info):.1f} Hz")

            try:
                while not self._stop.is_set() and client.is_connected:
                    await asyncio.sleep(0.5)
                    # Flush a partial block if the tag went quiet, so the live
                    # view does not stall waiting for a block that will not fill.
                    if self._buf_n and self._buf_n < BLOCK_SAMPLES:
                        self._flush()
            finally:
                if client.is_connected:
                    try:
                        await client.write_gatt_char(protocol.CHAR_CONTROL,
                                                     protocol.CMD_STOP, response=False)
                        await client.stop_notify(protocol.CHAR_SAMPLES)
                    except Exception:
                        # Already gone. Nothing to say about it that the
                        # disconnect has not said.
                        pass

    def stop(self) -> None:
        self._stop.set()


class Collector:
    """Scans continuously; streams from one tag on request."""

    def __init__(self, db_path: str | None = None):
        self.stats = {"received": 0, "stored": 0, "duplicates": 0, "unknown": 0,
                      "errors": 0, "spectra": 0, "chunks": 0,
                      "packets": 0, "samples": 0, "blocks": 0,
                      "gaps": 0, "lost_samples": 0}
        self._spectrum = protocol.SpectrumAssembler()
        self._last: dict = {}      # mac -> last stored (fmt, counter)
        self._devices: dict = {}   # mac -> BLEDevice, for connecting without a second scan
        self._seen_service: set[str] = set()
        self._conn = None
        self._db_path = db_path
        self._lock = threading.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._session: StreamSession | None = None
        self._session_task: asyncio.Task | None = None
        self.started_at = time.time()
        self.last_packet_at: float | None = None

    # -- scanning ----------------------------------------------------------

    def handle(self, device, adv) -> None:
        mac = device.address.upper()
        self._devices[mac] = device

        # The service UUID rides in the scan response, so an active scan tells
        # us which tags can be captured from before anyone tries to connect.
        uuids = {u.lower() for u in (adv.service_uuids or ())}
        if protocol.SERVICE_UUID in uuids and mac not in self._seen_service:
            self._seen_service.add(mac)
            with self._lock:
                self._conn.execute(
                    "INSERT INTO tags (mac, streamable, first_seen, last_seen) "
                    "VALUES (?, 1, ?, ?) ON CONFLICT(mac) DO UPDATE SET streamable = 1",
                    (mac, time.time(), time.time()))
                self._conn.commit()

        payload = adv.manufacturer_data.get(protocol.RUUVI_COMPANY_ID)
        if payload is None:
            return
        self.stats["received"] += 1

        try:
            d = protocol.decode(bytes(payload))
        except ValueError:
            # Other Ruuvi products (0x06, 0xE1) share the company ID. Not an error.
            self.stats["unknown"] += 1
            return

        if d["format"] == "0xC2":
            # Chunks reassemble into frames; a repeated chunk is useful rather
            # than a duplicate to drop.
            self.stats["chunks"] += 1
            frame = self._spectrum.add(mac, d, time.time())
            if frame:
                try:
                    with self._lock:
                        store.insert_spectrum(self._conn, frame)
                    self.stats["spectra"] += 1
                    self.last_packet_at = time.time()
                except Exception as exc:
                    self.stats["errors"] += 1
                    print(f"spectrum store error: {exc}", file=sys.stderr)
            return

        # Windows delivers each advertisement several times. Drop repeats of the
        # sequence counter we just stored, so rates reflect transmissions rather
        # than reception luck.
        key = (d["format"], d.get("sequence"))
        if self._last.get(mac) == key:
            self.stats["duplicates"] += 1
            return
        self._last[mac] = key

        try:
            with self._lock:
                store.insert_reading(self._conn, mac, adv.rssi, d)
            self.stats["stored"] += 1
            self.last_packet_at = time.time()
        except Exception as exc:       # keep scanning even if one write fails
            self.stats["errors"] += 1
            print(f"store error: {exc}", file=sys.stderr)

    # -- stream control (called from the HTTP thread) ----------------------

    def stream_status(self) -> dict:
        s = self._session
        return {
            "mac": s.mac if s else None,
            "connected": bool(s and s.connected),
            "error": s.error if s else None,
            "info": s.info if s else None,
            "packets": self.stats["packets"],
            "samples": self.stats["samples"],
            "gaps": self.stats["gaps"],
            "lost_samples": self.stats["lost_samples"],
        }

    def request_stream(self, mac: str | None) -> None:
        """Start capturing from `mac`, or stop if None. Thread-safe."""
        if self._loop is None:
            raise RuntimeError("collector is not running")
        asyncio.run_coroutine_threadsafe(self._switch_stream(mac), self._loop).result(10)

    async def _switch_stream(self, mac: str | None) -> None:
        if self._session is not None:
            self._session.stop()
            if self._session_task:
                try:
                    await asyncio.wait_for(self._session_task, timeout=5)
                except (asyncio.TimeoutError, asyncio.CancelledError):
                    self._session_task.cancel()
            self._session = None
            self._session_task = None
        if mac:
            for k in ("packets", "samples", "blocks", "gaps", "lost_samples"):
                self.stats[k] = 0
            self._session = StreamSession(mac, self._conn, self.stats,
                                          self._devices.get)
            self._session_task = asyncio.create_task(self._session.run())

    # -- lifecycle ---------------------------------------------------------

    async def run(self, duration: float | None = None, stream_mac: str | None = None):
        self._conn = store.init(self._db_path)
        self._loop = asyncio.get_running_loop()

        scanner = BleakScanner(detection_callback=self.handle, scanning_mode="active")
        await scanner.start()
        if stream_mac:
            await self._switch_stream(stream_mac)
        try:
            if duration:
                await asyncio.sleep(duration)
            else:
                while True:
                    await asyncio.sleep(3600)
        finally:
            await self._switch_stream(None)
            await scanner.stop()

    def run_in_thread(self, stream_mac: str | None = None) -> threading.Thread:
        """Start the asyncio loop on its own thread. The HTTP server stays on
        the main thread, so a slow page render cannot stall the radio."""
        ready = threading.Event()

        def target():
            async def boot():
                ready.set()
                await self.run(stream_mac=stream_mac)
            asyncio.run(boot())

        t = threading.Thread(target=target, daemon=True, name="ble-collector")
        t.start()
        ready.wait(5)
        # The loop attribute is set inside run(); give it a moment to appear so
        # an immediate request_stream() from the UI does not race startup.
        for _ in range(50):
            if self._loop is not None:
                break
            time.sleep(0.05)
        return t


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("-d", "--duration", type=float, help="seconds to collect")
    p.add_argument("--stream", metavar="MAC", help="also capture raw samples from this tag")
    args = p.parse_args()

    c = Collector()
    print(f"collecting into {store.DB_PATH}")
    try:
        asyncio.run(c.run(args.duration, args.stream))
    except KeyboardInterrupt:
        pass
    print(f"\n{c.stats}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
