# ADR-0002 — Stream raw samples over GATT; keep 0xC2 broadcast as the fallback

**Status:** Accepted. Both paths measured on hardware.

## Context

The obvious way to get a spectrum off a Ruuvi tag is to put it in
advertisements: 128 bins, one byte each, split across 8 packets under Ruuvi's
company ID. It is connectionless and any number of listeners can hear it, which
is genuinely valuable.

It is also, as an instrument, unusable. Measured on the bench:

- 8 advertisements per frame at the 1.28 s slot is **one spectrum per 10.24 s**
  before any loss.
- Over 30 s of listening at −84 dBm we heard chunks `[0, 2, 3, 4, 5, 6]` and
  reassembled **zero complete frames**. With ~20% chunk loss the probability of
  hearing all 8 is `0.8^8 ≈ 17%`.
- Every frame is a 2.56 s snapshot with 1.28 s of dead time between them, so
  even a perfect link would not be a continuous record.

It also puts the transform on a Cortex-M4 with a fixed window, fixed overlap
and a fixed window function, all compiled in. Changing any of them means a
reflash.

## Decision

Add a **GATT service that streams raw accelerometer samples** and make it the
primary transport. Keep `0xC2` broadcast, unchanged, for the connectionless case.

The two are different in kind and both are kept because neither covers the
other's case:

| | GATT stream | 0xC2 broadcast |
|---|---|---|
| Needs a connection | yes | no |
| Listeners | one | any number |
| Rate | continuous, 400 Hz | one 2.56 s snapshot per ~10 s |
| Analysis settings | changed on the host, live | compiled into the tag |
| Battery | high | moderate |

## Rationale

**The bandwidth objection does not survive arithmetic.** 400 Hz × 3 axes ×
2 bytes is 2.4 kB/s. That is unremarkable for BLE. Measured: **2.36 kB/s, 742
notifications in 30.2 s, 11416 samples, zero gaps, zero samples lost by index.**

**Sending samples instead of spectra moves every analysis decision to where it
can be changed.** Window length, overlap, window function, axis, band limits and
reference level are request parameters. The tag has no opinions about the
analysis, and the firmware is smaller for it: one transform, for the broadcast
path, and no feature extraction at all.

**Raw samples are auditable.** A spectrum computed on the tag can only be
checked against itself. Samples can be re-transformed, re-windowed, compared
against a second implementation, and stored for later re-analysis when you
realise you were looking at the wrong band.

## Consequences

- The tag must be a BLE **peripheral**, not a broadcaster only. That costs RAM
  (the build sits at 82.6% of the nRF52832's 64 kB) and adds an attack surface
  that a broadcast-only tag does not have.
- One host at a time. `CONFIG_BT_MAX_CONN=1`; a second capture would halve the
  radio time available to the first.
- **Advertising stops while a host is connected**, so DF5 pauses for the
  duration of a capture. This was not the original intent — see
  ADR-0004 — and it is the price of an advertising path that reliably comes back.
- Battery life during a capture is not budgeted and is not intended to be. This
  is a bench instrument while connected and a broadcaster when not.
- The 0xC2 path is now clearly labelled as what it is: a low-rate,
  lossy, connectionless view for "is anything happening in that room". It is not
  presented as an analysis channel.

## Revisit if

- A use case appears for several simultaneous listeners at full rate. L2CAP
  connection-oriented channels or periodic advertising with BLE 5 extended
  advertising would both be worth measuring, and the nRF52832 supports the
  latter.
- Battery-powered continuous capture becomes a requirement. It is not one now,
  and streaming at 400 Hz will not meet it.
