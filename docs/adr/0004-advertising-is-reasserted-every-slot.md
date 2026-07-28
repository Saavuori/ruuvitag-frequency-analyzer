# ADR-0004 — The main loop re-asserts advertising every slot

**Status:** Accepted after two failed designs on hardware.

## Context

The tag must advertise DF5 to stay compatible with the Ruuvi ecosystem, and must
advertise connectably so a host can attach and stream. Those two facts interact
with connection lifecycle, and getting the interaction wrong produced the worst
failure this project has: **a tag that stops advertising and never comes back.**

A silent tag looks exactly like a tag that is out of range, or has a flat
battery, or was never flashed. It is the one failure that cannot be diagnosed
from the receiving end.

Two designs were tried and both failed on real hardware.

### Attempt 1 — restart advertising in the connection callbacks

`bt_le_adv_start()` called directly from the `disconnected` callback. Zephyr runs
those callbacks on the Bluetooth RX thread and the call does not work from
there. The tag went silent the moment a host disconnected, every time, and had
to be reset over SWD.

### Attempt 2 — swap to non-connectable advertising for the duration

Nicer in principle: DF5 keeps flowing during a capture, and a second host cannot
arrive mid-capture. The callbacks only set a flag; the main loop did the
`bt_le_adv_stop()` / `bt_le_adv_start()` reconciliation.

RTT from the tag showed the switch working:

```
[00:00:21.582] <inf> rfa_gatt: host connected
[00:00:22.240] <inf> rfa: advertising non-connectable
```

and then nothing. The tag stayed silent in non-connectable mode — a mode nothing
was scanning for — and never returned to connectable.

## Decision

**One advertising mode, connectable, re-asserted by the main loop every 1.28 s
slot whenever no host is connected.**

```c
if (atomic_get(&host_connected)) {
        continue;              /* the stack owns the radio */
}
adv_ensure();                  /* -EALREADY is the normal answer */
```

The connection callbacks do nothing but set an atomic flag. There is no mode
switching, no state machine, and no path by which a single failed call leaves
the tag permanently quiet — a failed `bt_le_adv_start()` is simply retried on the
next slot, forever.

## Rationale

The two failures share a shape: **advertising state was changed once, at an
event, and never re-checked.** Any single failure was therefore permanent.
Re-asserting from a loop makes the correct state continuously enforced rather
than transitionally applied, so recovery needs no error handling at all.

`-EALREADY` from `bt_le_adv_start()` costs nothing and is the expected answer on
almost every slot.

## Consequences

- **DF5 pauses while a host is connected.** Ruuvi Station will show a gap for
  the length of a capture. This is a real regression against the original goal
  and is accepted deliberately: a tag that reliably comes back is worth more
  than one that broadcasts through a capture and then dies.
- The `0xC2` broadcast is suspended during a capture too, which costs nothing —
  the host has the same data at 400 Hz.
- Verified on hardware: four consecutive connect / stream / disconnect /
  reconnect cycles, all successful.

## A note on measuring this

Scanning for the tag after a disconnect is **not** a reliable test.
Windows/WinRT suppresses a recently-disconnected peer from scan results for
some seconds, which reads as "the tag went silent" when it did not. During
development this produced two false failures in three trials.

Probe by **reconnecting** instead. A successful reconnect proves connectable
advertising is running; a scan proves only that Windows felt like reporting it.
