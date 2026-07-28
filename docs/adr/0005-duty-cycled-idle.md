# ADR-0005 — Duty-cycle the sensor when nobody is connected

**Status:** Accepted. Measured on hardware, including the first version being wrong.

## Context

The firmware ran the LIS2DH12 at 400 Hz continuously and woke the CPU 25 times a
second, whether or not anything was listening. Correct while a host is
streaming; waste the rest of the time. Estimated idle draw was 150–250 µA —
months on a CR2477, not years.

There was also no way to check any of this: DF5 carried the invalid battery code,
so the tag could not report its own supply.

## Options

**Lower the ODR when idle.** Rejected. 400 Hz is precisely what buys the
1–150 Hz band ([ADR-0001](0001-sample-at-400-hz.md)); a slower sensor is a
narrower instrument. The problem is not that the sensor is too fast, it is that
it is running when nothing needs it.

**Sleep and wake on a timer only.** Workable, and what a continuously-vibrating
mount wants. But on intermittent machinery it either wastes bursts on nothing or
misses the event entirely.

**Sleep and wake on motion, with a timer as a floor.** Chosen.

## Decision

Three modes, with the sensor at 400 Hz only in two of them:

```
IDLE    low-power 10 Hz, 8-bit, activity interrupt armed, CPU asleep
BURST   400 Hz for one transform window (~2.7 s), then back to IDLE
ACTIVE  400 Hz continuous, for the GATT stream
```

`IDLE → BURST` on the heartbeat (`RFA_IDLE_PERIOD_S`, default 60) **or** on the
LIS2DH12's activity interrupt, whose INT1 line was already wired in the board
devicetree and previously unused.

The heartbeat is not redundant with the interrupt. Without it, a threshold set
too high produces a tag that says nothing and looks identical to a tag watching
something silent.

## Two things this got wrong first

**Advertising is not part of the state machine.** One connectable mode runs
throughout, only its interval changed. [ADR-0004](0004-advertising-is-reasserted-every-slot.md)
records two designs that switched advertising on events and left the tag
permanently silent; that lesson was not re-learned here.

**Nothing touches the sensor from an ISR.** Zephyr runs `k_timer` expiry
functions in the clock ISR, and the connection callbacks run on the Bluetooth RX
thread. Both only submit work; every SPI register write happens on the system
workqueue. The first draft did mode changes directly in the timer handler.

## The threshold was wrong, and that is the interesting part

64 mg looked generous against a measured 1.2 mg noise floor. On hardware:

```
uptime 208 s   idle 16 s   burst 192 s   bursts 71   motion events 71
→ 92% awake, ~168 µA modelled, ~200 days
```

The tag re-triggered within ~230 ms of finishing every burst. The cause was
already in a capture taken hours earlier: a steady **72 mg component at 44.5 Hz**
on that bench. The threshold sat underneath it.

Raising the default to 256 mg and latching the interrupt (`LIR_INT1`, so one
event means one trigger rather than a stream of edges across the threshold):

```
uptime 141 s   bursts 4   motion events 1
→ 92.3% idle, 30.5 µA modelled, ~3 years
```

The same 92.3% figure, inverted.

Two consequences worth stating plainly. The threshold is **site-specific** and
256 mg is a starting point, not an answer — `tools/tune_motion.py` sets it live
over GATT so it can be found against a real machine. And on something that
vibrates continuously, wake-on-motion fires constantly and saves nothing;
`RFA_WAKE_ON_MOTION=n` and rely on the heartbeat there.

## Consequences

- Idle draw modelled at ~30 µA against ~200 previously. Modelled, not measured:
  the duty cycle comes from the tag, the per-mode currents from the datasheet
  ([08 Power](../08-power.md)).
- **A spectrum arrives once a minute instead of continuously** when unconnected.
  That is the trade, and it is why the heartbeat period is configurable.
- The measured-ODR window had to change with it. It accumulates only across
  *active* intervals; a wall-clock window would have divided one burst's samples
  by a whole idle period and reported ~17 Hz for a 400 Hz sensor — a twentyfold
  error on every frequency, arrived at silently. Verified still correct after
  the change: 374.0 Hz.
- Samples taken in low-power mode are discarded on entering a burst. They are
  8-bit and forty times too slow; left in the decimator they would be
  transformed as if they were 100 Hz data.
- Status LEDs now default to off.

## Revisit if

- A current meter appears. The datasheet column in the power model is the
  weakest part of it.
- Someone wants continuous coverage on a battery. This design cannot give it;
  that is a different instrument with a different sensor.
