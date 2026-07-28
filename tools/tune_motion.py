#!/usr/bin/env python3
"""Tune the wake-on-motion threshold against a real machine.

The right threshold depends entirely on what the tag is bolted to, and finding
it by reflashing is miserable. This writes it live.

    python tools/tune_motion.py --mac AA:BB:CC:DD:EE:FF                 # read stats
    python tools/tune_motion.py --mac AA:BB:CC:DD:EE:FF --threshold 400 # set, then watch
    python tools/tune_motion.py --mac AA:BB:CC:DD:EE:FF --watch 600     # watch for 10 min

**The value is not persisted.** A reboot returns to
CONFIG_RFA_MOTION_THRESHOLD_MG. That is deliberate: this is for finding the
number, not for configuring a deployment. Once you know it, put it in the build.

How to read the result: leave it running and watch the idle duty. A tag that is
doing its job sits above ~90% idle. Anything much below that means the threshold
is under whatever is vibrating nearby, and the tag is awake almost continuously
- which was exactly the case at the 64 mg default, where a steady 72 mg
component at 44.5 Hz kept it up permanently.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from webapp import protocol as p                                   # noqa: E402


async def find(mac: str, seconds: float):
    found = {}

    def cb(dev, adv):
        if dev.address.upper() == mac.upper():
            found["dev"] = dev

    scanner = BleakScanner(detection_callback=cb, scanning_mode="active")
    await scanner.start()
    for _ in range(int(seconds)):
        await asyncio.sleep(1)
        if "dev" in found:
            break
    await scanner.stop()
    return found.get("dev")


def report(stats: dict) -> None:
    model = p.power_model(stats)
    print(f"  uptime {stats['uptime_s']:6d} s   "
          f"bursts {stats['bursts']:4d}   motion {stats['motion_events']:4d}   "
          f"battery {stats['battery_mv'] or 0} mV")
    if model:
        idle = 100 * model["duty_idle"]
        flag = "  <-- awake far too much" if idle < 80 else ""
        print(f"  idle {idle:5.1f} %   modelled {model['average_ua']:6.1f} uA   "
              f"{model['cell_days'] / 365:.2f} years{flag}")
    else:
        print("  not enough uptime yet for a duty cycle")


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mac", required=True)
    ap.add_argument("--threshold", type=int, help="new threshold in mg (16-2000)")
    ap.add_argument("--watch", type=float, default=0,
                    help="seconds to keep reporting the duty cycle")
    ap.add_argument("--discover", type=float, default=45,
                    help="seconds to wait for the tag to advertise")
    args = ap.parse_args()

    dev = await find(args.mac, args.discover)
    if dev is None:
        print(f"{args.mac} did not advertise within {args.discover:.0f} s", file=sys.stderr)
        return 1

    async with BleakClient(dev, timeout=30.0) as client:
        if args.threshold is not None:
            if not 16 <= args.threshold <= 2000:
                print("threshold must be 16-2000 mg", file=sys.stderr)
                return 2
            await client.write_gatt_char(
                p.CHAR_CONTROL,
                bytes([p.CMD_SET_THRESHOLD]) + struct.pack("<H", args.threshold),
                response=True)
            # Quantised to 16 mg steps by the sensor, so say what it will
            # actually use rather than what was asked for.
            print(f"threshold set to {args.threshold // 16 * 16} mg "
                  f"(asked {args.threshold}, quantised to 16 mg steps; not persisted)")

        stats = p.decode_stats(bytes(await client.read_gatt_char(p.CHAR_STATS)))
        print(f"\n{time.strftime('%H:%M:%S')}")
        report(stats)

        deadline = time.time() + args.watch
        while time.time() < deadline:
            await asyncio.sleep(min(30.0, max(1.0, deadline - time.time())))
            stats = p.decode_stats(bytes(await client.read_gatt_char(p.CHAR_STATS)))
            print(f"\n{time.strftime('%H:%M:%S')}")
            report(stats)

    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
