# 00 — Overview

## The problem

A RuuviTag has a perfectly good accelerometer and a firmware that throws away
almost everything it measures. Data Format 5 carries one heavily averaged
gravity vector per advertisement — roughly 0.4 Hz of DC. Anything vibrating is
invisible.

This project runs the same hardware at 400 Hz and gets the samples out.

## The shape of it

```
   RuuviTag                          host (laptop)
 ┌────────────────────┐            ┌──────────────────────────────┐
 │ LIS2DH12 @ 400 Hz  │            │ collector   BLE scan + GATT  │
 │   FIFO             │            │ store       SQLite, blocks   │
 │   DC tracker       │  ── GATT ──►│ dsp         STFT, bands     │
 │   raw ring         │  raw       │ server      HTTP + UI        │
 │                    │  samples   │                              │
 │   ÷4 → 100 Hz      │            │ browser     spectrum         │
 │   256-pt FFT       │  ── adv ──►│             waterfall        │
 │   0xC2 chunks      │  spectra   │             band levels      │
 └────────────────────┘            └──────────────────────────────┘
```

The tag acquires and transports. The host analyses. That split is deliberate and
is the subject of [ADR-0003](adr/0003-host-side-analysis.md): every analysis
decision then lives somewhere it can be changed without a reflash.

## Glossary

| Term | Meaning here |
|------|--------------|
| **Bin** | One frequency slot of the transform. 0.37 Hz wide at the default settings. |
| **Window** | Both the weighting function (Hann, flat top…) and the span of time one transform covers. They are different things with one name; the docs say which. |
| **Overlap** | How much consecutive transform windows share. Raises the column rate, never the resolution. |
| **Column** | One vertical line of the waterfall: one transform, one moment. |
| **Scalloping** | Amplitude lost when a tone falls between bin centres. ≤1.42 dB with Hann. |
| **ENBW** | Equivalent noise bandwidth of a window, in bins. Needed to turn bin amplitudes into a band level. |
| **Coherent gain** | The normalisation where a bin reads the amplitude of a tone sitting on it. |
| **Gap** | Samples known to be missing. Drawn as a hole, never interpolated or zero-filled. |
| **DF5** | Ruuvi Data Format 5, emitted unmodified for ecosystem compatibility. |
| **0xC2** | Our broadcast spectrum format: 128 bins across 8 advertisements. |

## Where to read next

- [02 Requirements](02-requirements.md) — what it must do, numbered
- [04 Signal processing](04-signal-processing.md) — the maths, and the calibration bug that was in it
- [05 BLE protocol](05-ble-protocol.md) — the wire formats, byte by byte
- [ADRs](adr/) — why it is like this
