#!/usr/bin/env python3
"""SQLite storage.

Three streams with three different shapes, so three tables:

  readings  DF5 environment and the DC vector. One row per advertisement.
  blocks    raw GATT samples, as int16 BLOBs of about a second each.
  spectra   0xC2 broadcast frames, one row per fully-received frame.

Raw samples are stored as blocks rather than rows because 400 Hz x 3 axes is
1200 values a second, and a row per value would spend more time in SQLite's
row overhead than in the data. A block is ~2.4 kB of int16 and carries the
tag's own sample index, which is what lets a gap be reconstructed later at its
true width instead of being closed up.

No index on (mac, first_index) is UNIQUE. The tag's sample index wraps at 2^32
- 124 days at 400 Hz - and a tag that reboots restarts it at zero. A uniqueness
constraint would start rejecting good data at that point, which is worse than
the duplicate it would prevent.
"""

from __future__ import annotations

import os
import sqlite3
import time

import numpy as np

DB_PATH = os.environ.get(
    "RFA_DB",
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "analyzer.db"),
)

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
    id            INTEGER PRIMARY KEY,
    mac           TEXT NOT NULL,
    ts            REAL NOT NULL,
    rssi          INTEGER,
    temperature_c REAL,
    humidity_pct  REAL,
    pressure_pa   REAL,
    accel_x       INTEGER,
    accel_y       INTEGER,
    accel_z       INTEGER,
    battery_mv    INTEGER,
    sequence      INTEGER
);
CREATE INDEX IF NOT EXISTS idx_readings_mac_ts ON readings(mac, ts);

CREATE TABLE IF NOT EXISTS blocks (
    id          INTEGER PRIMARY KEY,
    mac         TEXT NOT NULL,
    ts          REAL NOT NULL,   -- host clock when the block was closed
    first_index INTEGER NOT NULL,-- tag's own monotonic sample index
    n           INTEGER NOT NULL,
    fs_mhz      INTEGER,         -- measured rate, milli-Hz, 0 if unknown
    gap         INTEGER NOT NULL DEFAULT 0,
    data        BLOB NOT NULL    -- int16 little-endian, n*3, interleaved xyz
);
CREATE INDEX IF NOT EXISTS idx_blocks_mac_ts ON blocks(mac, ts);

CREATE TABLE IF NOT EXISTS spectra (
    id       INTEGER PRIMARY KEY,
    mac      TEXT NOT NULL,
    ts       REAL NOT NULL,
    frame_id INTEGER,
    invalid  INTEGER NOT NULL DEFAULT 0,
    bins     BLOB NOT NULL       -- 128 bytes, the transmitted dB encoding
);
CREATE INDEX IF NOT EXISTS idx_spectra_mac_ts ON spectra(mac, ts);

CREATE TABLE IF NOT EXISTS tags (
    mac            TEXT PRIMARY KEY,
    first_seen     REAL,
    last_seen      REAL,
    last_rssi      INTEGER,
    packets        INTEGER NOT NULL DEFAULT 0,
    stream_samples INTEGER NOT NULL DEFAULT 0,
    streamable     INTEGER NOT NULL DEFAULT 0,  -- advertised the service UUID
    info_json      TEXT,
    stats_json     TEXT
);
"""


def connect(path: str | None = None) -> sqlite3.Connection:
    path = path or DB_PATH
    os.makedirs(os.path.dirname(path), exist_ok=True)
    conn = sqlite3.connect(path, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    # WAL so the HTTP thread can read while the collector writes. Without it a
    # capture in progress blocks every page load.
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    return conn


def init(path: str | None = None) -> sqlite3.Connection:
    conn = connect(path)
    conn.executescript(SCHEMA)
    # Databases created before the power work lack this column, and CREATE TABLE
    # IF NOT EXISTS will not add it. Cheaper than a migration framework for one
    # column, and it keeps an existing capture readable.
    cols = {r[1] for r in conn.execute("PRAGMA table_info(tags)")}
    if "stats_json" not in cols:
        conn.execute("ALTER TABLE tags ADD COLUMN stats_json TEXT")
    conn.commit()
    return conn


def set_stats(conn: sqlite3.Connection, mac: str, stats_json: str) -> None:
    conn.execute(
        "INSERT INTO tags (mac, stats_json) VALUES (?, ?) "
        "ON CONFLICT(mac) DO UPDATE SET stats_json = excluded.stats_json",
        (mac, stats_json))
    conn.commit()


def _touch_tag(conn: sqlite3.Connection, mac: str, ts: float,
               rssi: int | None = None, samples: int = 0,
               streamable: bool | None = None) -> None:
    conn.execute(
        """INSERT INTO tags (mac, first_seen, last_seen, last_rssi, packets, stream_samples)
           VALUES (?, ?, ?, ?, 1, ?)
           ON CONFLICT(mac) DO UPDATE SET
             last_seen = excluded.last_seen,
             last_rssi = COALESCE(excluded.last_rssi, tags.last_rssi),
             packets = tags.packets + 1,
             stream_samples = tags.stream_samples + excluded.stream_samples""",
        (mac, ts, ts, rssi, samples),
    )
    if streamable is not None:
        conn.execute("UPDATE tags SET streamable = ? WHERE mac = ?", (int(streamable), mac))


def insert_reading(conn: sqlite3.Connection, mac: str, rssi: int | None,
                   d: dict, ts: float | None = None) -> None:
    ts = ts if ts is not None else time.time()
    ax, ay, az = d.get("accel_mg", (None, None, None))
    conn.execute(
        """INSERT INTO readings
           (mac, ts, rssi, temperature_c, humidity_pct, pressure_pa,
            accel_x, accel_y, accel_z, battery_mv, sequence)
           VALUES (?,?,?,?,?,?,?,?,?,?,?)""",
        (mac, ts, rssi, d.get("temperature_c"), d.get("humidity_pct"),
         d.get("pressure_pa"), ax, ay, az, d.get("battery_mv"), d.get("sequence")),
    )
    _touch_tag(conn, mac, ts, rssi)
    conn.commit()


def insert_block(conn: sqlite3.Connection, mac: str, first_index: int,
                 samples: np.ndarray, fs_mhz: int = 0, gap: bool = False,
                 ts: float | None = None) -> None:
    """`samples` is (n, 3) of int16-compatible values in mg."""
    ts = ts if ts is not None else time.time()
    arr = np.asarray(samples, dtype="<i2")
    conn.execute(
        "INSERT INTO blocks (mac, ts, first_index, n, fs_mhz, gap, data) VALUES (?,?,?,?,?,?,?)",
        (mac, ts, int(first_index), int(arr.shape[0]), int(fs_mhz), int(gap), arr.tobytes()),
    )
    _touch_tag(conn, mac, ts, None, samples=int(arr.shape[0]))
    conn.commit()


def insert_spectrum(conn: sqlite3.Connection, frame: dict) -> None:
    conn.execute(
        "INSERT INTO spectra (mac, ts, frame_id, invalid, bins) VALUES (?,?,?,?,?)",
        (frame["mac"], frame["ts"], frame.get("frame_id"),
         int(frame.get("invalid", False)), bytes(frame["bins_db"])),
    )
    conn.commit()


def set_info(conn: sqlite3.Connection, mac: str, info_json: str) -> None:
    conn.execute(
        "INSERT INTO tags (mac, info_json, streamable) VALUES (?, ?, 1) "
        "ON CONFLICT(mac) DO UPDATE SET info_json = excluded.info_json, streamable = 1",
        (mac, info_json),
    )
    conn.commit()


def blocks(conn: sqlite3.Connection, mac: str, since_s: float = 300
           ) -> list[tuple[int, np.ndarray, float]]:
    """Sample blocks for the last `since_s` seconds.

    Returns (first_index, (n,3) samples, ts). The timestamp is host clock when
    the block was written, which is the only thing tying the tag's sample index
    to wall time - the tag has no clock and never claims to.
    """
    cutoff = time.time() - since_s
    rows = conn.execute(
        "SELECT first_index, n, data, ts FROM blocks WHERE mac = ? AND ts >= ? ORDER BY ts",
        (mac, cutoff),
    ).fetchall()
    out = []
    for r in rows:
        arr = np.frombuffer(r["data"], dtype="<i2")
        if arr.size != r["n"] * 3:
            # A short write means the row is not what its header claims. Drop
            # it rather than reshaping into a plausible-looking wrong answer.
            continue
        out.append((int(r["first_index"]), arr.reshape(-1, 3), float(r["ts"])))
    return out


def block_rate_mhz(conn: sqlite3.Connection, mac: str) -> int:
    row = conn.execute(
        "SELECT fs_mhz FROM blocks WHERE mac = ? AND fs_mhz > 0 ORDER BY ts DESC LIMIT 1",
        (mac,),
    ).fetchone()
    return int(row["fs_mhz"]) if row else 0


def spectra(conn: sqlite3.Connection, mac: str, since_s: float = 900,
            limit: int = 2000) -> list[dict]:
    cutoff = time.time() - since_s
    rows = conn.execute(
        "SELECT ts, frame_id, invalid, bins FROM spectra "
        "WHERE mac = ? AND ts >= ? ORDER BY ts LIMIT ?",
        (mac, cutoff, limit),
    ).fetchall()
    return [{"ts": r["ts"], "frame_id": r["frame_id"],
             "invalid": bool(r["invalid"]), "bins_db": list(r["bins"])} for r in rows]


def series(conn: sqlite3.Connection, mac: str, since_s: float = 3600) -> list[dict]:
    cutoff = time.time() - since_s
    rows = conn.execute(
        "SELECT ts, rssi, temperature_c, humidity_pct, pressure_pa, "
        "accel_x, accel_y, accel_z, battery_mv FROM readings "
        "WHERE mac = ? AND ts >= ? ORDER BY ts",
        (mac, cutoff),
    ).fetchall()
    return [dict(r) for r in rows]


def tags(conn: sqlite3.Connection) -> list[dict]:
    rows = conn.execute(
        "SELECT mac, first_seen, last_seen, last_rssi, packets, stream_samples, "
        "streamable, info_json, stats_json FROM tags ORDER BY last_seen DESC"
    ).fetchall()
    return [dict(r) for r in rows]


def prune(conn: sqlite3.Connection, keep_s: float) -> int:
    """Drop data older than `keep_s`. Raw blocks are ~2.4 kB/s per streaming
    tag, which is 200 MB a day - large enough that an unattended capture needs
    a retention policy rather than a disk-full surprise."""
    cutoff = time.time() - keep_s
    n = 0
    for table in ("blocks", "spectra", "readings"):
        cur = conn.execute(f"DELETE FROM {table} WHERE ts < ?", (cutoff,))
        n += cur.rowcount or 0
    conn.commit()
    return n
