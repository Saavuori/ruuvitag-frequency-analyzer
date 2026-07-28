# 01 — Hardware

## The tag

RuuviTag: nRF52832 (Cortex-M4F, 512 kB flash, 64 kB RAM), LIS2DH12
accelerometer and BME280 environment sensor on SPI, two LEDs, CR2477 coin cell.

Verified on a tag reporting FICR `INFO.PART = 0x00052832`, variant `AAB0`,
QFN48 package, 64 kB RAM.

## LIS2DH12

| | |
|---|---|
| Range | ±2 g (configured) |
| Resolution | 12-bit high-resolution mode, 1 mg/LSB |
| ODR | 400 Hz configured |
| FIFO | 32 slots × 3 axes, stream mode |
| Noise density | ~220 µg/√Hz (datasheet) |

### The ODR is not what the register says

The output data rate comes from an internal RC oscillator, not a crystal.
Measured on the bench tag across several runs:

```
nominal   400.000 Hz
measured  378.896, 379.158, 379.658 Hz
```

That is −5.1%, and it moves with temperature. Every frequency computed from an
assumed 400 Hz would be 5.4% high — a 50 Hz component would read 52.7 Hz. The
firmware counts delivered samples against the RTC and publishes the result in
the Info characteristic; the host scales by it. See requirement S-6.

### Measured noise floor, and where the usable band ends

With the tag at rest, band levels read **1.1–1.4 mg RMS**. The spectrum is flat
to within about 2 dB with a spectral flatness of 0.60 — broadband noise and no
tone, which is exactly what `dominant_peak` reports for it.

Per-bin noise across the whole band to Nyquist, from a 30 s capture at 0.18 Hz
resolution:

| Band | Median noise per bin |
|---|---|
| 1–10 Hz | 374 µg |
| 10–50 Hz | 361 µg |
| 50–100 Hz | 242 µg |
| 100–150 Hz | 268 µg |
| **150–188 Hz** | **581 µg** |

Flat to 150 Hz — slightly *better* above 50 Hz than below it, since the low end
carries the DC tracker's residue — then roughly doubling as it approaches the
188 Hz Nyquist, where there is no anti-alias filter to protect it. That is why
the default band stops at 150 rather than at Nyquist.

That is the floor. Vibration below it is not recoverable by any amount of
processing, and no amount of firmware cleverness changes the sensor.

### Going faster than 400 Hz

The ODR ladder tops out for our purposes at 400 Hz, because the higher rates
cost resolution:

| ODR | Nyquist | Resolution | Note |
|---|---|---|---|
| **400 Hz** | 188 Hz | 12-bit, 1 mg/LSB | current |
| 1344 Hz | ~630 Hz | 10-bit, 4 mg/LSB | normal mode only |
| 1620 / 5376 Hz | 760 / 2500 Hz | 8-bit, 16 mg/LSB | low-power mode only |

Two measured constraints on top of that: the BLE stream has ~11% headroom at
400 Hz (17 samples per notification at 24.6 notifications/s against 376
samples/s needed), and the 32-slot FIFO is 80 ms at 400 Hz against a 40 ms poll
— at 1344 Hz it would be 24 ms and overrun. Both would need work before a
higher ODR is usable.

### Low-power mode

While idle the sensor runs at **10 Hz in 8-bit low-power mode**, where one LSB
is 16 mg rather than 1 mg. Nothing reads those samples — the mode exists so the
activity interrupt stays armed for microamps — but it is why the motion
threshold is coarse and quantised to 16 mg steps ([08 Power](08-power.md)).

## Debug access

SWD only — there is no exposed UART, so logging goes over SEGGER RTT
([07 Build and flash](07-build-and-flash.md)). A Nordic DK's on-board J-Link
works as the probe.

Flashing with `--chiperase` removes Ruuvi's DFU bootloader, after which SWD is
the only way in or out.

## Open hardware questions

- `TODO(hw-1)` The RuuviTag revision on the bench is not recorded. Revisions
  differ in LED pin assignment; the devicetree handles that, but a power
  measurement would need to state which board it was taken on.
- `TODO(hw-2)` No power measurement has been taken while streaming (O-1).
- `TODO(hw-3)` No absolute amplitude calibration against a reference shaker.
  Amplitudes are verified against synthetic tones, which is self-consistency,
  not traceability (O-4).
