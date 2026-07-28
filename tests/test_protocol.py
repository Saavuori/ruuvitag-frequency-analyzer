#!/usr/bin/env python3
"""Wire format tests.

    python -m pytest tests/ -q          (or: python tests/test_protocol.py)

DF5 cases are Ruuvi's own published test vectors, so a pass means we agree with
the reference implementation rather than merely with ourselves. The 0xC2 and
GATT stream cases are built from the field tables in docs/05-ble-protocol.md,
which makes them the spec's only executable check - the format is written down
twice, once in C and once here, and these are what keep the two honest.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from webapp import protocol as p                                   # noqa: E402


# --- DF5, against Ruuvi's published vectors ---------------------------------

def test_df5_valid():
    d = p.decode(bytes.fromhex("0512FC5394C37C0004FFFC040CAC364200CDCBB8334C884F"))
    assert d["temperature_c"] == 24.3
    assert d["humidity_pct"] == 53.49
    assert d["pressure_pa"] == 100044
    assert d["accel_mg"] == (4, -4, 1036)
    assert d["battery_mv"] == 2977
    assert d["tx_power_dbm"] == 4
    assert d["sequence"] == 205
    assert d["mac"] == "CB:B8:33:4C:88:4F"


def test_df5_max():
    d = p.decode(bytes.fromhex("057FFFFFFEFFFE7FFF7FFF7FFFFFDEFEFFFECBB8334C884F"))
    assert d["temperature_c"] == 163.835
    assert d["pressure_pa"] == 115534
    assert d["accel_mg"] == (32767, 32767, 32767)
    assert d["battery_mv"] == 3646
    assert d["tx_power_dbm"] == 20


def test_df5_min():
    d = p.decode(bytes.fromhex("058001000000008001800180010000000000CBB8334C884F"))
    assert d["temperature_c"] == -163.835
    assert d["pressure_pa"] == 50000
    assert d["battery_mv"] == 1600
    assert d["tx_power_dbm"] == -40


def test_df5_invalid_is_none_not_a_number():
    """A field that could not be measured must decode to None.

    This is the whole point of the sentinels: a fabricated 0 gets plotted and
    believed, where a None leaves a visible hole."""
    d = p.decode(bytes.fromhex("058000FFFFFFFF800080008000FFFFFFFFFFFFFFFFFFFFFF"))
    for k in ("temperature_c", "humidity_pct", "pressure_pa", "battery_mv",
              "tx_power_dbm", "movement_counter", "sequence", "mac"):
        assert d[k] is None, k
    assert d["accel_mg"] == (None, None, None)


def test_unknown_format_raises():
    """Ruuvi's company ID carries several products. Guessing is how a 0xC2
    chunk becomes a plausible-looking temperature."""
    try:
        p.decode(bytes([0x06] + [0] * 23))
    except ValueError:
        return
    raise AssertionError("decode accepted an unknown format byte")


# --- 0xC2 broadcast spectrum ------------------------------------------------

def _c2_chunk(frame_id, idx, bins, invalid=False):
    head = bytes([0xC2, (1 << 4) | (1 if invalid else 0), frame_id, idx,
                  p.C2_CHUNKS, idx * p.C2_BINS_PER_CHUNK])
    return head + bytes(bins)


def test_c2_chunk_decode():
    body = list(range(p.C2_BINS_PER_CHUNK))
    d = p.decode(_c2_chunk(7, 2, body))
    assert d["format"] == "0xC2"
    assert d["spec_version"] == 1
    assert d["frame_id"] == 7
    assert d["chunk_idx"] == 2
    assert d["first_bin"] == 2 * p.C2_BINS_PER_CHUNK
    assert d["bins_db"] == body


def test_c2_bin_scale_covers_the_stated_band():
    """128 bins at 0.390625 Hz is 0 to 49.6 Hz. If either constant moves, the
    frequency axis silently lies."""
    assert p.C2_BINS == 128
    assert abs(p.C2_BIN_HZ - 0.390625) < 1e-9
    assert 49.0 < p.bin_hz(p.C2_BINS - 1) < 50.0


def test_assembler_releases_only_whole_frames():
    a = p.SpectrumAssembler()
    for i in range(p.C2_CHUNKS - 1):
        got = a.add("AA", p.decode(_c2_chunk(1, i, [i] * p.C2_BINS_PER_CHUNK)), 0.0)
        assert got is None, "released a frame before every chunk arrived"
    frame = a.add("AA", p.decode(
        _c2_chunk(1, p.C2_CHUNKS - 1, [9] * p.C2_BINS_PER_CHUNK)), 0.0)
    assert frame is not None
    assert len(frame["bins_db"]) == p.C2_BINS


def test_assembler_drops_a_partial_frame():
    """A half-heard spectrum must never be padded into a waterfall column: the
    padding renders as a band of silence that never happened."""
    a = p.SpectrumAssembler(max_pending=2)
    a.add("AA", p.decode(_c2_chunk(1, 0, [1] * p.C2_BINS_PER_CHUNK)), 1.0)
    for f in range(2, 6):
        a.add("AA", p.decode(_c2_chunk(f, 0, [1] * p.C2_BINS_PER_CHUNK)), float(f))
    assert a.dropped > 0


def test_db_encoding_round_trips():
    """0.5 dB per LSB from 1 ug. 255 must still be under the +/-2 g full scale,
    or the top of the range encodes something the sensor cannot measure."""
    assert abs(p.db_to_microg(0) - 1.0) < 1e-9
    assert 2.3e6 < p.db_to_microg(255) < 2.5e6
    for ug in (10, 1000, 100000):
        db = 40.0 * __import__("math").log10(ug)
        assert abs(p.db_to_microg(db) - ug) / ug < 1e-6


# --- GATT sample stream -----------------------------------------------------

def _stream_packet(first_index, samples, gap=False):
    head = bytes([(1 << 4) | (1 if gap else 0), len(samples)]) + struct.pack("<I", first_index)
    body = b"".join(struct.pack("<hhh", *s) for s in samples)
    return head + body


def test_stream_packet_decode():
    samples = [(1, -2, 1003), (-32768, 32767, 0)]
    d = p.decode_stream_packet(_stream_packet(123456, samples))
    assert d["version"] == 1
    assert d["gap"] is False
    assert d["count"] == 2
    assert d["first_index"] == 123456
    assert list(d["samples_mg"]) == [1, -2, 1003, -32768, 32767, 0]


def test_stream_packet_gap_flag():
    d = p.decode_stream_packet(_stream_packet(9, [(0, 0, 0)], gap=True))
    assert d["gap"] is True


def test_stream_packet_rejects_a_short_body():
    """Truncation must be an error, not a silently shorter frame. A shorter
    frame would still decode, and every sample after it would be misaligned."""
    good = _stream_packet(0, [(1, 2, 3), (4, 5, 6)])
    try:
        p.decode_stream_packet(good[:-4])
    except ValueError:
        return
    raise AssertionError("accepted a truncated stream packet")


def test_info_decode():
    raw = bytes([1, 3]) + struct.pack("<II", 400000, 398_720) + bytes([1, 2])
    d = p.decode_info(raw)
    assert d["axes"] == 3
    assert d["nominal_hz"] == 400.0
    assert abs(d["measured_hz"] - 398.72) < 1e-9
    assert d["full_scale_g"] == 2


def test_stats_decode():
    raw = (struct.pack("<IIII", 3600, 3_300_000, 300_000, 0)
           + struct.pack("<HHH", 55, 12, 2890))
    d = p.decode_stats(raw)
    assert d["uptime_s"] == 3600
    assert d["bursts"] == 55
    assert d["motion_events"] == 12
    assert d["battery_mv"] == 2890


def test_stats_battery_zero_means_not_measured():
    """0 is the firmware's "ADC unavailable" code and must not decode as a flat
    cell - a fabricated 0 mV gets plotted and believed."""
    raw = struct.pack("<IIII", 10, 10_000, 0, 0) + struct.pack("<HHH", 1, 0, 0)
    assert p.decode_stats(raw)["battery_mv"] is None


def test_power_model_matches_duty_cycle():
    """A tag idle 90% of the time must model far below one that is awake.

    The numbers here are the two the bench actually produced, before and after
    the motion threshold was raised."""
    busy = {"uptime_s": 208, "idle_ms": 16_037, "burst_ms": 192_274,
            "active_ms": 0, "bursts": 71, "motion_events": 71, "battery_mv": 2890}
    quiet = {"uptime_s": 141, "idle_ms": 130_000, "burst_ms": 10_800,
             "active_ms": 0, "bursts": 4, "motion_events": 1, "battery_mv": 2876}

    mb, mq = p.power_model(busy), p.power_model(quiet)
    assert mb["duty_idle"] < 0.15 and mq["duty_idle"] > 0.85
    assert mb["average_ua"] > 5 * mq["average_ua"]
    assert mq["cell_days"] > 365


def test_power_model_waits_for_enough_uptime():
    """Thirty seconds of occupancy says nothing. Returning None beats returning
    a confident number derived from one burst."""
    assert p.power_model({"idle_ms": 500, "burst_ms": 2700, "active_ms": 0}) is None


def test_effective_rate_prefers_measured():
    """The oscillator is an RC part a few percent off nominal. Taking the label
    puts that error on every frequency the analyser draws."""
    assert p.effective_rate_hz({"nominal_hz": 400.0, "measured_hz": 389.0}) == 389.0
    assert p.effective_rate_hz({"nominal_hz": 400.0, "measured_hz": None}) == 400.0
    assert p.effective_rate_hz(None) == p.NOMINAL_RATE_HZ


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
