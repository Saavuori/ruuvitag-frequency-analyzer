#!/usr/bin/env python3
"""HTTP API tests, against a real server on a throwaway database.

    python -m pytest tests/ -q          (or: python tests/test_server.py)

No radio: the server runs as with --no-collect, and blocks are written the
way the collector writes them.
"""

import json
import os
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from webapp import server, store                                   # noqa: E402

MAC = "AA:BB:CC:DD:EE:FF"


def _db_with_blocks(fs_mhz):
    conn = store.init(os.path.join(tempfile.mkdtemp(), "t.db"))
    rng = np.random.default_rng(5)
    for i in range(10):
        store.insert_block(conn, MAC, i * 400, rng.integers(-5, 5, (400, 3)),
                           fs_mhz=fs_mhz)
    return conn


def _get(port, path):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def _serve():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def test_an_unmeasured_rate_is_not_reported_as_measured():
    """Blocks written before the tag has measured its rate carry 0. The UI
    must then say "assumed", because the nominal 400 Hz is 5% off (S-6)."""
    server._conn = _db_with_blocks(fs_mhz=0)
    d = server.Handler._spectrogram(None, MAC, 300, {})
    assert d["fs_measured"] is False
    assert d["fs_hz"] == 400.0

    server._conn = _db_with_blocks(fs_mhz=379_658)
    d = server.Handler._spectrogram(None, MAC, 300, {})
    assert d["fs_measured"] is True
    assert abs(d["fs_hz"] - 379.658) < 1e-9


def test_static_files_stay_inside_the_static_directory():
    assert server._static_file("/") == os.path.join(server.STATIC_DIR, "index.html")
    assert server._static_file("/app.js") is not None
    # A sibling whose name starts with "static" passed the old prefix check.
    sibling = server.STATIC_DIR + "_probe"
    os.makedirs(sibling, exist_ok=True)
    try:
        with open(os.path.join(sibling, "x.js"), "w") as f:
            f.write("//")
        assert server._static_file("/../static_probe/x.js") is None
    finally:
        os.remove(os.path.join(sibling, "x.js"))
        os.rmdir(sibling)
    assert server._static_file("/../server.py") is None


def test_a_bad_query_parameter_is_a_400_not_a_dropped_connection():
    server._conn = _db_with_blocks(fs_mhz=0)
    srv = _serve()
    port = srv.server_address[1]
    try:
        code, body = _get(port, f"/api/spectrogram?mac={MAC}&since=abc")
        assert code == 400 and "error" in json.loads(body)
        code, _ = _get(port, f"/api/spectrogram?mac={MAC}&nfft=100")
        assert code == 400
        code, body = _get(port, f"/api/spectrogram?mac={MAC}")
        assert code == 200 and json.loads(body)["empty"] is False
    finally:
        srv.shutdown()
        srv.server_close()


if __name__ == "__main__":
    fails = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  ok    {name}")
            except Exception as exc:
                fails += 1
                print(f"  FAIL  {name}: {exc}")
    print(f"\n{'FAILED' if fails else 'passed'} ({fails} failures)")
    sys.exit(1 if fails else 0)
