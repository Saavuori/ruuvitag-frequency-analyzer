#!/usr/bin/env python3
"""BLE wire formats, in one place.

Three of them, and they are not variations on a theme - they are three
different answers to "how do you get vibration data off a coin-cell tag":

  DF5   (0x05)   Ruuvi Data Format 5, emitted unmodified. One DC accelerometer
                 sample per advertisement, so ~0.4 Hz of a gravity vector. It
                 is here for compatibility with the Ruuvi ecosystem and is
                 useless for spectrum work - see the note in `decode_df5`.

  0xC2           Chunked spectrum frames, broadcast. The tag transforms and
                 sends 128 bins across 8 advertisements. One spectrum per
                 ~10 s, connectionless, any number of listeners.

  GATT stream    Raw 400 Hz samples over a connection. This is the one that
                 makes the thing an analyser; the host transforms.

Decoding dispatches on the format byte and raises on anything unknown, which is
what a correct consumer must do - a parser that assumes every 0x0499 payload is
DF5 decodes a 0xC2 chunk as a nonsense temperature and a plausible pressure.

Verified against Ruuvi's published DF5 valid/max/min/invalid test vectors; run
`python -m pytest tests/test_protocol.py`.
"""

from __future__ import annotations

import math
import struct

# Stand-in for log10(0) when a bin is exactly zero. Well below the LIS2DH12's
# noise floor (~220 ug/sqrt(Hz)), so it can never be mistaken for a measurement.
DB_FLOOR = -180.0

RUUVI_COMPANY_ID = 0x0499
DF5 = 0x05
FMT_C2 = 0xC2

# --- 0xC2 broadcast spectrum -------------------------------------------------
#
# The bin scale is fixed by spec version rather than carried in every chunk;
# version 1 is a 256-point transform at a 100 Hz effective rate.
C2_BINS = 128
C2_BIN_HZ = 100.0 / 256.0          # 0.390625 Hz
C2_HEADER_LEN = 6
C2_BINS_PER_CHUNK = 24 - C2_HEADER_LEN
C2_CHUNKS = -(-C2_BINS // C2_BINS_PER_CHUNK)

# --- GATT sample stream ------------------------------------------------------

SERVICE_UUID = "f1a70001-9c3f-4f5a-8b21-2d6a3c9e7d10"
CHAR_SAMPLES = "f1a70002-9c3f-4f5a-8b21-2d6a3c9e7d10"
CHAR_CONTROL = "f1a70003-9c3f-4f5a-8b21-2d6a3c9e7d10"
CHAR_INFO    = "f1a70004-9c3f-4f5a-8b21-2d6a3c9e7d10"

STREAM_HEADER_LEN = 6
STREAM_BYTES_PER_SAMPLE = 6
STREAM_FLAG_GAP = 0x01

CMD_STOP = bytes([0x00])
CMD_START = bytes([0x01])

NOMINAL_RATE_HZ = 400.0


def _mac(payload: bytes) -> str | None:
    """DF5 carries the MAC in the last 6 bytes."""
    raw = payload[18:24]
    if raw == b"\xff" * 6:
        return None
    return ":".join(f"{b:02X}" for b in raw)


def decode_df5(payload: bytes) -> dict:
    """Ruuvi Data Format 5 (RAWv2). `payload` excludes the company ID.

    The acceleration triplet here is a ~20 s exponential average of the gravity
    vector, sampled once per advertisement. Do not plot it next to a spectrum
    and expect them to agree about anything: at ~0.4 Hz of a heavily smoothed
    DC value it cannot represent motion above ~0.2 Hz, which is below the entire
    band this project exists to measure.
    """
    if len(payload) < 24:
        raise ValueError(f"DF5 needs 24 bytes, got {len(payload)}")

    temp_raw, hum_raw, pres_raw, ax, ay, az, power, moves, seq = struct.unpack(
        ">hHHhhhHBH", payload[1:18]
    )

    battery = power >> 5
    tx_power = power & 0x1F

    return {
        "format": "DF5",
        "temperature_c": None if temp_raw == -0x8000 else temp_raw * 0.005,
        "humidity_pct": None if hum_raw == 0xFFFF else hum_raw * 0.0025,
        "pressure_pa": None if pres_raw == 0xFFFF else pres_raw + 50000,
        "accel_mg": tuple(None if v == -0x8000 else v for v in (ax, ay, az)),
        "battery_mv": None if battery == 0x7FF else battery + 1600,
        "tx_power_dbm": None if tx_power == 0x1F else -40 + 2 * tx_power,
        "movement_counter": None if moves == 0xFF else moves,
        "sequence": None if seq == 0xFFFF else seq,
        "mac": _mac(payload),
    }


def decode_c2(payload: bytes) -> dict:
    """One chunk of a broadcast spectrum frame.

    Returns the chunk, not a spectrum. Reassembly is the caller's job because
    chunks arrive over several advertising intervals and some go missing; see
    SpectrumAssembler.
    """
    if len(payload) < 24:
        raise ValueError(f"0xC2 needs 24 bytes, got {len(payload)}")

    flags = payload[1]
    return {
        "format": "0xC2",
        "spec_version": flags >> 4,
        "invalid": bool(flags & 0x01),
        "frame_id": payload[2],
        "chunk_idx": payload[3],
        "n_chunks": payload[4],
        "first_bin": payload[5],
        "bins_db": list(payload[C2_HEADER_LEN:24]),
    }


def db_to_microg(db_value: int | float) -> float:
    """Inverse of the transmitted encoding: 0.5 dB per LSB referenced to 1 ug."""
    return 10.0 ** (db_value / 40.0)


def microg_to_db(microg: float) -> float:
    """Amplitude in ug to dB referenced to 1 ug.

    Note this is *real* dB, not the transmitted byte. The 0xC2 encoding packs
    0.5 dB per LSB, so its byte value is twice the dB figure; a chart axis
    should be labelled in these units, not in those.
    """
    if microg <= 0.0:
        return DB_FLOOR
    return 20.0 * math.log10(microg)


def bin_hz(index: int) -> float:
    return index * C2_BIN_HZ


def decode(payload: bytes) -> dict:
    """Dispatch on the format byte, exactly as a correct consumer must."""
    if not payload:
        raise ValueError("empty payload")
    fmt = payload[0]
    if fmt == DF5:
        return decode_df5(payload)
    if fmt == FMT_C2:
        return decode_c2(payload)
    raise ValueError(f"unknown Ruuvi format 0x{fmt:02X}")


class SpectrumAssembler:
    """Reassemble 0xC2 chunks into whole spectra, per tag.

    Frames are only released once every chunk has arrived. A partially heard
    frame is discarded rather than padded: a waterfall column built from half a
    spectrum shows a band of silence that never happened, which is worse than a
    gap the viewer can see.
    """

    def __init__(self, max_pending: int = 8):
        self._pending: dict = {}          # (mac, frame_id) -> {chunk_idx: bins}
        self._max_pending = max_pending
        self.dropped = 0

    def add(self, mac: str, chunk: dict, ts: float) -> dict | None:
        key = (mac, chunk["frame_id"])
        entry = self._pending.setdefault(key, {"chunks": {}, "ts": ts,
                                               "invalid": chunk["invalid"]})
        entry["chunks"][chunk["chunk_idx"]] = chunk["bins_db"]
        entry["invalid"] = entry["invalid"] or chunk["invalid"]

        n = chunk["n_chunks"]
        if len(entry["chunks"]) < n:
            # Bound memory, and count what we lose rather than losing it quietly.
            while len(self._pending) > self._max_pending:
                oldest = min(self._pending, key=lambda k: self._pending[k]["ts"])
                if oldest == key:
                    break
                del self._pending[oldest]
                self.dropped += 1
            return None

        bins = []
        for i in range(n):
            bins.extend(entry["chunks"][i])
        del self._pending[key]
        return {"mac": mac, "ts": entry["ts"], "frame_id": chunk["frame_id"],
                "invalid": entry["invalid"], "bins_db": bins[:C2_BINS]}


def decode_stream_packet(data: bytes) -> dict:
    """One GATT Samples notification.

    Layout, little-endian throughout:

        | 0     | uint8     | bits 7-4 version, bit 0 = gap before this packet |
        | 1     | uint8     | sample count n                                   |
        | 2-5   | uint32    | index of the first sample, monotonic             |
        | 6...  | int16 x3n | x,y,z per sample, mg, gravity already removed    |

    `first_index` is what makes loss recoverable. A receiver that sees index
    4000 after index 3000 knows 1000 samples are missing and can leave a hole of
    exactly that width. Splicing the gap shut would shift every later sample in
    time, and a time shift is a frequency error - the one error a spectrum
    analyser must never introduce silently.
    """
    if len(data) < STREAM_HEADER_LEN:
        raise ValueError(f"stream packet needs {STREAM_HEADER_LEN} bytes, got {len(data)}")

    version = data[0] >> 4
    gap = bool(data[0] & STREAM_FLAG_GAP)
    count = data[1]
    first_index = struct.unpack_from("<I", data, 2)[0]

    want = STREAM_HEADER_LEN + count * STREAM_BYTES_PER_SAMPLE
    if len(data) < want:
        raise ValueError(f"stream packet claims {count} samples ({want} bytes), got {len(data)}")

    body = data[STREAM_HEADER_LEN:want]
    flat = struct.unpack(f"<{count * 3}h", body)

    return {
        "version": version,
        "gap": gap,
        "count": count,
        "first_index": first_index,
        # (n, 3) as a flat list of x,y,z triples in mg.
        "samples_mg": flat,
    }


def decode_info(data: bytes) -> dict:
    """The Info characteristic: what the host must not have to assume.

    The measured rate matters more than it looks. The LIS2DH12's ODR comes from
    an RC oscillator a few percent off nominal and drifting with temperature, so
    taking the label at face value puts a few percent of error on every
    frequency the analyser draws. 50 Hz mains would land at 48.5 and look like
    something else.
    """
    if len(data) < 12:
        raise ValueError(f"info needs 12 bytes, got {len(data)}")

    version = data[0]
    axes = data[1]
    nominal_mhz, measured_mhz = struct.unpack_from("<II", data, 2)
    mg_per_lsb = data[10]
    full_scale_g = data[11]

    return {
        "proto_version": version,
        "axes": axes,
        "nominal_hz": nominal_mhz / 1000.0,
        "measured_hz": (measured_mhz / 1000.0) if measured_mhz else None,
        "mg_per_lsb": mg_per_lsb,
        "full_scale_g": full_scale_g,
    }


def effective_rate_hz(info: dict | None) -> float:
    """The rate to hand the transform. Measured where available, nominal
    otherwise, and never a guess of our own."""
    if info:
        return info.get("measured_hz") or info.get("nominal_hz") or NOMINAL_RATE_HZ
    return NOMINAL_RATE_HZ
