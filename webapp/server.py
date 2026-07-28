#!/usr/bin/env python3
"""HTTP API and static file server.

One process holds the radio, the database and the UI, so there is one command
to remember and no way for the three to disagree about what was captured:

    python -m webapp.server

Then open http://127.0.0.1:8765.

    python -m webapp.server --no-collect        serve stored data, leave the radio alone
    python -m webapp.server --stream AA:BB:...  start capturing immediately
    python -m webapp.server --port 9000

The transform runs here rather than in the browser. numpy is faster and, more
to the point, `webapp/dsp.py` is the same code the tests exercise - a second
implementation in JavaScript would be a second thing to be wrong.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import numpy as np

from . import dsp, protocol, store

STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

# Cap on columns returned in one response. A 15-minute window at a 0.16 s hop
# is 5600 columns; past roughly a thousand they are narrower than a screen
# pixel, so the rest is bandwidth spent on something nobody can see.
MAX_COLUMNS = 1200

_collector = None
_conn = None


def _json_safe(x):
    """NaN and Infinity are not JSON. Nulls are, and null is what a lost
    window means - the UI draws it as a gap rather than as silence."""
    if isinstance(x, float) and not math.isfinite(x):
        return None
    return x


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass          # the default logs every static asset to stderr

    # -- plumbing ----------------------------------------------------------

    def _send(self, body: bytes, ctype: str, code: int = 200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, payload, code: int = 200):
        body = json.dumps(payload, default=_json_safe, allow_nan=False).encode()
        self._send(body, "application/json", code)

    def _static(self, path: str):
        name = "index.html" if path in ("/", "") else path.lstrip("/")
        full = os.path.normpath(os.path.join(STATIC_DIR, name))
        if not full.startswith(STATIC_DIR) or not os.path.isfile(full):
            self._send(b"not found", "text/plain", 404)
            return
        ctype = {".html": "text/html; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}.get(os.path.splitext(full)[1],
                                                        "application/octet-stream")
        with open(full, "rb") as f:
            self._send(f.read(), ctype)

    # -- routes ------------------------------------------------------------

    def do_GET(self):
        u = urlparse(self.path)
        if not u.path.startswith("/api/"):
            self._static(u.path)
            return

        q = parse_qs(u.query)
        mac = (q.get("mac", [None])[0] or "").upper() or None
        since = float(q.get("since", ["300"])[0])

        try:
            if u.path == "/api/tags":
                self._json(self._tags())
            elif u.path == "/api/status":
                self._json(self._status())
            elif u.path == "/api/spectrogram" and mac:
                self._json(self._spectrogram(mac, since, q))
            elif u.path == "/api/broadcast" and mac:
                self._json(self._broadcast(mac, since))
            elif u.path == "/api/series" and mac:
                self._json(store.series(_conn, mac, since))
            else:
                self._json({"error": "not found"}, 404)
        except Exception as exc:
            self._json({"error": str(exc)}, 500)

    def do_POST(self):
        u = urlparse(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            self._json({"error": "bad json"}, 400)
            return

        if u.path != "/api/stream":
            self._json({"error": "not found"}, 404)
            return
        if _collector is None:
            self._json({"error": "server started with --no-collect"}, 409)
            return
        try:
            mac = body.get("mac")
            _collector.request_stream(mac.upper() if mac else None)
            self._json(_collector.stream_status())
        except Exception as exc:
            self._json({"error": str(exc)}, 500)

    # -- handlers ----------------------------------------------------------

    def _tags(self):
        rows = store.tags(_conn)
        for r in rows:
            if r.get("info_json"):
                try:
                    r["info"] = json.loads(r["info_json"])
                except ValueError:
                    r["info"] = None
            del r["info_json"]
        return {"tags": rows, "stream": self._stream_status()}

    def _stream_status(self):
        return _collector.stream_status() if _collector else {"mac": None, "connected": False}

    def _status(self):
        s = {"collecting": _collector is not None, "stream": self._stream_status(),
             "now": time.time()}
        if _collector:
            s["stats"] = _collector.stats
            s["last_packet_at"] = _collector.last_packet_at
        return s

    def _spectrogram(self, mac: str, since: float, q: dict):
        """The waterfall, computed from raw samples."""
        cfg = dsp.StftConfig(
            nfft=int(q.get("nfft", ["1024"])[0]),
            overlap=float(q.get("overlap", ["0.75"])[0]),
            window=q.get("window", ["hann"])[0],
            axis=q.get("axis", ["z"])[0],
            f_lo=float(q.get("flo", [str(dsp.BAND_LO_HZ)])[0]),
            f_hi=float(q.get("fhi", [str(dsp.BAND_HI_HZ)])[0]),
        )
        if cfg.nfft not in (256, 512, 1024, 2048, 4096):
            raise ValueError("nfft must be one of 256, 512, 1024, 2048, 4096")
        if not 0.0 <= cfg.overlap < 1.0:
            raise ValueError("overlap must be in [0, 1)")

        raw = store.blocks(_conn, mac, since)
        if not raw:
            return {"empty": True,
                    "reason": "no raw samples in this window - is the tag streaming?"}

        # Blocks from before the tag last rebooted belong to a different time
        # base; keeping them would render the index reset as one enormous gap.
        run = dsp.latest_run(raw)
        if len(run) < len(raw):
            raw = raw[len(raw) - len(run):]
        samples, first_index, missing = dsp.assemble(run)

        fs_mhz = store.block_rate_mhz(_conn, mac)
        fs = (fs_mhz / 1000.0) if fs_mhz else protocol.NOMINAL_RATE_HZ

        signal = dsp.select_axis(samples, cfg.axis)
        # select_axis on a magnitude turns NaN rows into NaN, and on a single
        # axis it carries them through untouched. Either way the holes survive.
        res = dsp.stft(signal, fs, cfg)
        if res.times_s.size == 0:
            return {"empty": True, "reason": res.notes[0] if res.notes else "not enough data"}

        # Anchor the sample index to wall time using the first block. The tag
        # has no clock; this is the only link, and it is only as good as the
        # host's own timestamping - good to a notification interval, which is
        # far finer than a window length.
        idx0, arr0, ts0 = raw[0]
        anchor = ts0 - (len(arr0) / fs)          # ts is written when the block closes
        t0 = anchor + (first_index - idx0) / fs

        bands = {
            "1-10": dsp.band_amplitude(res.freqs_hz, res.amp_ug, 1, 10, res.enbw_bins),
            "10-25": dsp.band_amplitude(res.freqs_hz, res.amp_ug, 10, 25, res.enbw_bins),
            "25-50": dsp.band_amplitude(res.freqs_hz, res.amp_ug, 25, 50, res.enbw_bins),
        }

        # Decimate columns and band traces together, so a point in one lines up
        # with the column above it. Plain subsampling, not averaging: averaging
        # dB across dropped columns would smear a transient into its neighbours
        # and make it look longer than it was.
        cols = res.amp_ug
        times = res.times_s
        step = 1
        if len(times) > MAX_COLUMNS:
            step = math.ceil(len(times) / MAX_COLUMNS)
            cols = cols[::step]
            times = times[::step]

        def to_db(a):
            return np.where(np.isnan(a), np.nan, 20.0 * np.log10(np.maximum(a, 1e-6)))

        db = to_db(cols)
        latest = res.amp_ug[-1]
        latest_db = to_db(latest)
        peak = dsp.dominant_peak(res.freqs_hz, latest)

        return {
            "empty": False,
            "fs_hz": fs,
            "fs_measured": bool(fs_mhz),
            "bin_hz": res.bin_hz,
            "window_seconds": res.window_seconds,
            "hop_seconds": res.hop_seconds,
            "nfft": cfg.nfft,
            "window": cfg.window,
            "axis": cfg.axis,
            "t0": t0,
            "freqs": [round(float(f), 4) for f in res.freqs_hz],
            "times": [round(float(t), 3) for t in times],
            "columns": [[None if not math.isfinite(v) else round(float(v), 1) for v in row]
                        for row in db],
            "latest": [None if not math.isfinite(v) else round(float(v), 1)
                       for v in latest_db],
            "peak": peak,
            "flatness": _json_safe(dsp.spectral_flatness(latest)),
            "bands": {k: [_json_safe(round(float(x), 1)) for x in v[::step]]
                      for k, v in bands.items()},
            "missing_samples": missing,
            "lost_windows": res.n_lost,
            "notes": res.notes,
        }

    def _broadcast(self, mac: str, since: float):
        """The 0xC2 waterfall: what the tag computed for itself.

        Kept alongside the raw path so a tag nobody is connected to still has a
        view, and so the two can be compared - they use the same normalisation
        and should agree on a steady tone.
        """
        frames = store.spectra(_conn, mac, since)
        return {
            "bin_hz": protocol.C2_BIN_HZ,
            "bins": protocol.C2_BINS,
            "frames": [{"ts": f["ts"], "invalid": f["invalid"],
                        # transmitted byte is 0.5 dB/LSB, so halve for real dB
                        "db": [b / 2.0 for b in f["bins_db"]]} for f in frames],
        }


def main() -> int:
    global _collector, _conn

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--no-collect", action="store_true",
                   help="serve stored data without touching the radio")
    p.add_argument("--stream", metavar="MAC", help="begin capturing from this tag at startup")
    args = p.parse_args()

    _conn = store.init()

    if not args.no_collect:
        from .collector import Collector
        _collector = Collector()
        _collector.run_in_thread(stream_mac=args.stream)
        print("scanning for tags")
    else:
        print("serving stored data only")

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"http://{args.host}:{args.port}  (db: {store.DB_PATH})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
