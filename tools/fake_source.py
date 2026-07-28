#!/usr/bin/env python3
"""Synthetic capture, for working on the analyser without hardware.

Writes sample blocks that look exactly like a streaming tag's, including the
monotonic sample index and optional dropouts, so the whole path downstream -
assembly, STFT, the waterfall - runs on data whose right answer is known.

    python tools/fake_source.py --duration 120
    python tools/fake_source.py --live            # keep writing in real time
    python tools/fake_source.py --loss 0.05       # drop 5% of blocks

The default scene is three tones plus noise:

    3.1 Hz  @ 40 mg   something slow and large, well inside the band
   12.5 Hz  @ 8 mg    mid-band, and deliberately not on a bin centre at
                      nfft=1024 (bin spacing 0.39 Hz) so scalloping and the
                      parabolic interpolator both get exercised
   33.0 Hz  @ 3 mg    small and high, near where the tag's own noise floor
                      starts to matter

plus 2.5 mg RMS of white noise, which is roughly the LIS2DH12's measured floor.
A tag on a real desk will not look like this. That is the point - a synthetic
scene you can check beats a real one you can only admire.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from webapp import store                                     # noqa: E402

FAKE_MAC = "FA:KE:00:00:00:01"
RATE_HZ = 400.0
BLOCK = 400          # one second per block, matching the collector

TONES = [(3.1, 40.0), (12.5, 8.0), (33.0, 3.0)]     # (Hz, mg amplitude)
NOISE_MG = 2.5


def generate(n: int, start_sample: int, rng: np.random.Generator) -> np.ndarray:
    """n samples of the scene, phase-continuous from `start_sample`."""
    t = (start_sample + np.arange(n)) / RATE_HZ
    z = np.zeros(n)
    for hz, mg in TONES:
        z += mg * np.sin(2 * math.pi * hz * t)
    z += rng.normal(0.0, NOISE_MG, n)

    # X and Y get one tone each at different amplitudes, so switching axis in
    # the UI visibly changes the picture rather than looking like a no-op.
    x = 15.0 * np.sin(2 * math.pi * 7.3 * t) + rng.normal(0.0, NOISE_MG, n)
    y = 5.0 * np.sin(2 * math.pi * 21.0 * t) + rng.normal(0.0, NOISE_MG, n)

    return np.stack([x, y, z], axis=1).round().astype(np.int16)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--duration", type=float, default=60.0,
                   help="seconds of signal to write (default 60)")
    p.add_argument("--live", action="store_true",
                   help="write in real time and keep going until Ctrl-C")
    p.add_argument("--loss", type=float, default=0.0,
                   help="fraction of blocks to drop, to exercise gap handling")
    p.add_argument("--mac", default=FAKE_MAC)
    p.add_argument("--seed", type=int, default=1)
    args = p.parse_args()

    rng = np.random.default_rng(args.seed)
    conn = store.init()

    n_blocks = int(args.duration * RATE_HZ / BLOCK)
    written = 0
    dropped = 0
    now = time.time()

    # Resume the sample index where this MAC left off, so a backdated batch and
    # a --live top-up join without a fake discontinuity. Restarting at 0 would
    # look to the reassembler exactly like a tag that rebooted.
    row = conn.execute(
        "SELECT MAX(first_index + n) FROM blocks WHERE mac = ?", (args.mac.upper(),)
    ).fetchone()
    index = int(row[0] or 0)

    # Backdate a batch run so the data lands inside the UI's default window
    # instead of appearing to arrive all at once, right now.
    ts = now - (args.duration if not args.live else 0.0)

    try:
        i = 0
        while args.live or i < n_blocks:
            data = generate(BLOCK, index, rng)

            if args.loss and rng.random() < args.loss:
                # Advance the index without writing: exactly what a lost
                # notification looks like from the host's side.
                dropped += 1
                index += BLOCK
                ts += BLOCK / RATE_HZ
                i += 1
                if args.live:
                    time.sleep(BLOCK / RATE_HZ)
                continue

            store.insert_block(conn, args.mac.upper(), index, data,
                               fs_mhz=int(RATE_HZ * 1000), gap=False, ts=ts)
            written += 1
            index += BLOCK
            ts += BLOCK / RATE_HZ
            i += 1
            if args.live:
                time.sleep(BLOCK / RATE_HZ)
    except KeyboardInterrupt:
        pass

    print(f"wrote {written} blocks ({written * BLOCK} samples, "
          f"{written * BLOCK / RATE_HZ:.1f} s) as {args.mac.upper()}")
    if dropped:
        print(f"dropped {dropped} blocks on purpose")
    print(f"database: {store.DB_PATH}")
    print("tones:", ", ".join(f"{hz} Hz @ {mg} mg" for hz, mg in TONES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
