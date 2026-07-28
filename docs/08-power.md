# 08 — Power

## What is measured and what is not

There is no current meter on this bench, so the microamp figures here are a
**model**. Its inputs are half measured and half taken on trust:

| Input | Source |
|---|---|
| Time spent in each mode | **Measured on the tag**, reported over GATT |
| Current drawn in each mode | Datasheet, not verified |
| Cell capacity | CR2477 rating, derated 20% for pulsed BLE loads |

A voltage trend cannot settle it either: a CR2477 holds ~3.0 V across most of
its life and then falls off a cliff, so a trend can *contradict* a multi-year
estimate but never confirm one. That is why the tag counts its own occupancy
instead — it converts an unverifiable claim into arithmetic over numbers that
were actually observed.

Treat every figure below as an estimate with a measured denominator.

## Modes

| Mode | Accelerometer | CPU | Radio | Modelled |
|---|---|---|---|---|
| `IDLE` | low-power 10 Hz, 8-bit, INT1 armed | asleep, ~5 wakes/s | advertising, 2 s | ~18 µA |
| `BURST` | 400 Hz, 12-bit high-res | FIFO poll 40 ms | unchanged | ~180 µA |
| `ACTIVE` | 400 Hz, 12-bit high-res | FIFO poll 40 ms | GATT, ~2.4 kB/s | ~3500 µA |

A burst is one transform window plus settling — about 2.7 s in practice. At the
default 60 s idle period that is a ~5% duty cycle, and the model puts a quiet
tag near **25 µA**, which is years rather than months.

`ACTIVE` is not a battery mode and is not budgeted as one. The tag is a bench
instrument while a host is streaming.

## The levers, in the order they matter

1. **`RFA_IDLE_PERIOD_S`** (default 60). The main one. A burst costs a fixed
   ~2.7 s, so 10 s would be ~30% duty and several times the draw.
2. **`RFA_MOTION_THRESHOLD_MG`** (default 256). Set this too low and the tag
   never reaches idle — see below. This is the one that actually bit us.
3. **`RFA_STATUS_LEDS`** (default **off**). The green flash at advertising
   cadence costs roughly what the radio does.
4. **`RFA_ADV_INTERVAL_MS`** (default 2000). Longer is cheaper. The floor is the
   broadcast spectrum, not discovery: a 0xC2 frame is 8 advertisements, so
   2000 ms means ~16 s per spectrum, which still fits inside a 60 s period.

Already handled by the board and not worth revisiting: the DC/DC regulator is
enabled in the devicetree, and the 32 kHz crystal is in use rather than the RC.

## The threshold was wrong, and the instrumentation is how we knew

First deployment of the duty cycling, with a 64 mg threshold, reported this
after 208 seconds:

```
idle 16 s   burst 192 s   bursts 71   motion events 71
→ 92% of its life awake, ~168 µA modelled, ~200 days
```

Every burst was motion-triggered, and the tag re-triggered within ~230 ms of
finishing each one. The cause was in the data we had already captured: a steady
**72 mg component at 44.5 Hz** where the tag was sitting. A 64 mg threshold sat
underneath it, so the tag never stopped waking.

Two things changed:

- The threshold default went to **256 mg**, clear of that source. It is quantised
  to 16 mg steps and applied in 8-bit low-power mode, where one LSB *is* 16 mg —
  so this is a coarse threshold on a coarse signal, not the 1 mg resolution the
  analyser sees during a burst.
- The interrupt is now **latched** (`LIR_INT1`). Without it the pin follows the
  threshold condition and chatters across it, so one event on the bench could
  produce a stream of edges.

The right value is site-specific and 256 mg is still a starting point, not an
answer. Tune it against the real machine:

```bash
python tools/tune_motion.py --mac AA:BB:CC:DD:EE:FF --threshold 400
```

That writes command `0x02` to the Control characteristic and re-arms the sensor
live. It is **not persisted** — a reboot returns to the Kconfig value — because
it is a tuning aid. Find the number, then put it in the build.

Watch DF5's movement counter for a day afterwards. It carries the motion-event
count, which is exactly what that field means in Ruuvi's ecosystem, and it is
visible without connecting.

## Reading it back

The Stats characteristic (`f1a70005-…`, 22 bytes, see
[05 BLE protocol](05-ble-protocol.md)) reports uptime, milliseconds in each
mode, burst and motion counts, and the battery voltage. The web app shows it in
the Power panel, and marks a tag that is spending most of its life awake — that
is what a mis-set threshold looks like, and it is the failure this panel exists
to make visible.

## What is still open

- **No current measurement.** O-1. A Power Profiler Kit II in series with the
  cell would replace the datasheet column with measurements in an afternoon and
  is the single most valuable thing anyone could add here.
- **No long-run battery trend yet.** DF5 now carries a real voltage, so this is
  now merely a matter of waiting.
- **`ACTIVE` current is a guess** with a wide error bar. It depends on
  connection interval and PHY, both negotiated by the host.
