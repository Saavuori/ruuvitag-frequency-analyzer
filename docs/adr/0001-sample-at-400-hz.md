# ADR-0001 — Sample at 400 Hz through the FIFO

**Status:** Accepted.

## Context

The goal band is 1–50 Hz. Nyquist alone demands more than 100 Hz, and the
LIS2DH12 has no meaningful anti-alias filter, so whatever sits above half the
sample rate folds into the band and cannot be removed afterwards. That is the
one thing that has to be right at acquisition time.

The LIS2DH12's options are 1, 10, 25, 50, 100, 200, 400 Hz (and 1344 Hz in
low-power mode only, which costs the 12-bit resolution this needs).

## Options

| ODR | Nyquist | Margin over 50 Hz | Verdict |
|-----|---------|-------------------|---------|
| 100 Hz | 50 Hz | none — the band edge *is* Nyquist | Rejected |
| 200 Hz | 100 Hz | 2× | Workable |
| 400 Hz | 190 Hz | ~4× | **Chosen** |

## Decision

**400 Hz, ±2 g, 12-bit high-resolution, driven through the hardware FIFO.**

The Zephyr `lis2dh` driver has no FIFO support — it exposes one sample at a time
— so `firmware/src/lis2dh12.c` drives the chip directly over SPI using the same
devicetree node, with `CONFIG_LIS2DH=n` to keep two owners off one chip.

## Rationale

**The FIFO matters more than the rate.** Polling a free-running ODR from a
thread the BLE stack preempts puts sampling jitter into the spectrum as a peak
at the advertising cadence and its harmonics — inside the band of interest. With
the FIFO the sensor timestamps samples on its own clock, so a late read costs
buffer depth, not sample spacing. 32 slots is 80 ms at 400 Hz; polling at 40 ms
leaves 2× margin.

**400 over 200 because it costs almost nothing here.** The extra current is tens
of µA on a tag that is already streaming over BLE, and the headroom means the
band edge at 50 Hz is nowhere near the point where an absent anti-alias filter
starts to matter.

**±2 g and high-resolution, not ±4 g or normal mode.** 1 mg/LSB matters more
than headroom; 4 mg/LSB would put quantisation above the entire amplitude range
of small vibration.

## Consequences

- 400 Hz × 3 axes × 2 bytes = 2.4 kB/s to move. Measured: 2.36 kB/s, comfortable.
- The rate is **not** 400 Hz. The oscillator is an RC part; the bench tag
  measures 379.7 Hz, 5.1% low. The tag reports its measured rate and the host
  scales by it (S-6).
- Nyquist is 190 Hz but nothing above 50 Hz has been checked against a known
  source (O-3). Raising `f_hi` in the UI is a one-parameter change if anyone
  wants to look.
- The ÷4 boxcar feeding the on-tag 0xC2 spectrum is a poor anti-alias filter for
  *that* path. It does not affect the GATT stream, which is undecimated (O-2).
