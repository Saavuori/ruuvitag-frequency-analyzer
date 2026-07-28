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

### Measured noise floor

With the tag at rest on a desk, band levels read **1.1–1.4 mg RMS** across
1–10, 10–25 and 25–50 Hz. The spectrum is flat to within about 2 dB and the
spectral flatness reads 0.60 — broadband noise with no tone, which is exactly
what `dominant_peak` reports for it.

That is the floor. Vibration below it is not recoverable by any amount of
processing, and no amount of firmware cleverness changes the sensor.

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
