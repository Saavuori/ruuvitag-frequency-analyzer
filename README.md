# RuuviTag frequency analyzer

Custom firmware and a web app that turn a RuuviTag into a **1–50 Hz vibration
spectrum analyzer**. The tag streams raw accelerometer samples over BLE; the
browser shows a live spectrum and a scrolling waterfall.

> Not affiliated with or endorsed by Ruuvi Innovations Ltd. "RuuviTag" is their
> product name; this project merely runs on their hardware.

![band](https://img.shields.io/badge/band-1--50%20Hz-orange) ![rate](https://img.shields.io/badge/sample%20rate-400%20Hz-blue) ![resolution](https://img.shields.io/badge/resolution-0.39%20Hz-blue)

---

## What it does

A RuuviTag has an LIS2DH12 accelerometer. Stock firmware reports one heavily
averaged gravity vector per advertisement — about 0.4 Hz of DC, which cannot
represent anything in the band this project cares about. This firmware runs the
sensor at **400 Hz through its hardware FIFO** and ships the samples to a host,
which does the transform.

| | |
|---|---|
| Band | 1–50 Hz (Nyquist is 190 Hz; nothing above 50 has been checked against a known source) |
| Sample rate | 400 Hz, 3 axes, ±2 g, 12-bit high-resolution mode |
| Resolution | 0.39 Hz at the default 1024-point transform — and therefore a 2.56 s window |
| Transport | GATT notifications, ~2.4 kB/s, plus a connectionless 0xC2 broadcast |
| Compatibility | Emits unmodified Ruuvi Data Format 5, so Ruuvi Station and Home Assistant keep working |

## Verified on hardware

Everything below was measured on a real tag (nRF52832, `D6:3A:83:30:44:86`),
not asserted:

```
INFO      nominal 400.0 Hz   measured 379.658 Hz   3 axes   ±2 g
stream    742 packets in 30.2 s → 11416 samples = 377.9 Hz effective
loss      0 gap flags, 0 samples lost by index
bandwidth 2.36 kB/s
```

**The measured rate is 5.1% below nominal.** That is not a defect, it is what an
RC oscillator does, and it is why the tag reports its own measured ODR and the
host scales every frequency by it. Take the label at face value and a 50 Hz
component reads as 52.7 Hz.

## Quick start

```bash
python run.py --stream AA:BB:CC:DD:EE:FF
```

Then open <http://127.0.0.1:8765>. Without `--stream` it scans and you pick the
tag in the UI.

```bash
python run.py --no-collect          # serve stored data, leave the radio alone
python tools/fake_source.py -h      # synthetic scene, no hardware needed
python -m pytest tests/ -q          # host tests
```

Building and flashing the firmware is in
[`docs/07-build-and-flash.md`](docs/07-build-and-flash.md).

## How it is put together

```
firmware/     Zephyr application for the RuuviTag (nRF52832)
webapp/       protocol decode, DSP, storage, BLE collection, HTTP API, UI
tools/        synthetic source and CLI utilities
tests/        host tests - no hardware required
docs/         the specification, and the reasoning behind it
```

Two transports, deliberately different in kind:

**GATT stream** — connect, and the tag notifies raw 400 Hz samples carrying a
monotonic index. This is the instrument. The host holds the samples and can
re-transform them at any window length, overlap or window function without
touching the tag.

**0xC2 broadcast** — no connection, any number of listeners, one 128-bin
spectrum split across 8 advertisements. That is roughly one 2.56 s snapshot per
10 s before packet loss, and at typical indoor loss most frames arrive
incomplete. Useful for "is anything happening in that room"; not an instrument.
Measured on the bench: 6 of 8 chunks heard over 30 s, 0 complete frames.

## Three things this project refuses to do

**Splice a gap shut.** Every sample carries an index. When packets are lost the
host reopens the hole at its true width and the affected transform windows come
back as null, drawn as gaps. Concatenating what arrived would compress time, and
compressed time is a frequency error that looks exactly like a measurement.

**Zero-fill a dropout.** A zero-filled hole is a broadband click — energy at
every frequency — which is the artefact most easily mistaken for a real event.

**Name a frequency in noise.** Every spectrum has a tallest bin; only some have
a tone. A peak must clear the band median by ~13.5× before it is reported, which
targets a 1% false-tone rate. Otherwise the answer is "no tonal peak", and that
is a real answer.

## Limits worth knowing

- **The noise floor is the sensor's, not the analyser's.** The LIS2DH12 is
  ~220 µg/√Hz with 1 mg/LSB quantisation. Small, high-frequency vibration
  disappears into it, and no amount of processing recovers it.
- **`|xyz|` doubles frequencies.** Vector magnitude rectifies, so a tone
  swinging symmetrically about zero appears at 2f. It is offered because it
  answers "how much is moving" regardless of orientation, and labelled because
  it is a trap.
- **Resolution and window length are the same fact.** 0.39 Hz bins require a
  2.56 s window. Overlap slides that window more often; it does not shorten it.
- **This is not a medical or safety device.** It measures vibration.

## Documentation

| Doc | Contents |
|-----|----------|
| [00 Overview](docs/00-overview.md) | What the system is, end to end |
| [01 Hardware](docs/01-hardware.md) | RuuviTag revisions, LIS2DH12 errata, the noise floor |
| [02 Requirements](docs/02-requirements.md) | Numbered, testable |
| [03 Architecture](docs/03-architecture.md) | Threads, buffers, timing |
| [04 Signal processing](docs/04-signal-processing.md) | Sampling, decimation, the transform, calibration |
| [05 BLE protocol](docs/05-ble-protocol.md) | DF5, 0xC2, and the GATT stream, byte by byte |
| [06 Web app](docs/06-webapp.md) | The UI, and why it is laid out that way |
| [07 Build and flash](docs/07-build-and-flash.md) | Zephyr workspace, SWD, recovery |
| [ADRs](docs/adr/) | Decisions and their reasoning |

## Licence

MIT. See [LICENSE](LICENSE).
