# 03 — Architecture

## Firmware threads

| Thread | Stack | Priority | Job |
|--------|-------|----------|-----|
| `main` | 2 kB | — | Advertising: re-assert every 1.28 s slot, rotate DF5 and 0xC2 chunks |
| `rfa_sampler_thread` | 4 kB | 5 | Drain the FIFO every 40 ms, run the DC tracker and decimator, recompute the broadcast spectrum every 1.28 s |
| `rfa_streamer` | 2 kB | 6 | Pull from the raw ring and notify over GATT |
| Zephyr BT RX/TX | — | — | The stack's own |

The sampler gets 4 kB because the transform runs on it, and
`rfa_spectrum_bins` keeps its two 256-float working buffers `static` for the
same reason — 2 kB of automatics there overflows the thread. That failure is
caught as a named stack overflow only because `CONFIG_HW_STACK_PROTECTION` is
on. It stays on.

**Connection callbacks do no work.** They set an atomic and return. Zephyr runs
them on the Bluetooth RX thread, where calling `bt_le_adv_start()` does not
work; discovering that cost two firmware revisions
([ADR-0004](adr/0004-advertising-is-reasserted-every-slot.md)).

## Buffers

| Buffer | Size | Purpose |
|--------|------|---------|
| Hardware FIFO | 32 samples (80 ms) | Sensor-clocked; removes read-timing jitter |
| Raw ring | 512 samples (1.28 s) | Between sampler and streamer |
| Waterfall ring | 256 samples @ 100 Hz (2.56 s) | The on-tag transform window |

The raw ring is single-producer, single-consumer, and is a window onto
**monotonic indices** — the ring is not the numbering. If the consumer falls
behind, the read cursor jumps to the oldest surviving sample and a gap flag is
set; the indices then tell the host exactly how many samples went missing, so
the hole can be reopened at its true width rather than spliced shut.

## Host

```
protocol.py   pure stdlib, no numpy. Wire formats only.
dsp.py        numpy. STFT, bands, peaks. No I/O.
store.py      SQLite. Raw samples as ~1 s int16 BLOBs, not rows.
collector.py  asyncio + bleak. One scanner, one optional GATT session.
server.py     stdlib http.server, threaded. Computes the STFT per request.
static/       vanilla JS, hand-rolled canvas. No dependencies.
```

Layering is strictly downward: `dsp` does not import `store`, `store` does not
import `collector`. `protocol` imports nothing of ours, which is what lets a
minimal consumer decode packets without numpy.

Samples are stored as ~1-second BLOBs rather than rows because 400 Hz × 3 axes
is 1200 values a second, and a row per value would spend more time in SQLite's
row overhead than on the data.

### Threading

The HTTP server runs on the main thread; the collector owns its own thread with
its own asyncio loop. `Collector.request_stream()` is the only crossing point
and uses `run_coroutine_threadsafe`. SQLite is opened in WAL mode so a page load
cannot block a capture in progress.

### One subtlety worth knowing

The collector hands `BleakClient` the `BLEDevice` object its own scanner
discovered, rather than an address string. Connecting by address makes bleak run
its own discovery, and WinRT will not run two scans at once — our scanner
starves it and the connect fails with "device not found".
