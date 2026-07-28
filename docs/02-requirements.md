# 02 — Requirements

Numbered so commits and tests can cite them. **M** = met and measured on
hardware, **P** = partly met, **O** = open.

## Functional

| # | Requirement | State |
|---|---|---|
| F-1 | Resolve components across **1–150 Hz** | **M** — 1.1 to 149.7 Hz, 408 bins, measured |
| F-2 | Emit **unmodified DF5**, byte-compatible with the Ruuvi ecosystem | **M** — checked against Ruuvi's published vectors |
| F-3 | Stream raw samples to a host fast enough for a continuous record | **M** — 377.9 Hz effective, 2.36 kB/s |
| F-4 | Transform length, overlap, window and axis selectable without reflashing | **M** |
| F-5 | Report a spectrum without a connection | **P** — 0xC2 works, but ~1 frame/10 s and lossy |
| F-6 | Show a live spectrum and a scrolling waterfall on one shared frequency axis | **M** |

## Signal

| # | Requirement | State |
|---|---|---|
| S-1 | Bin amplitude accurate to ±2% for a tone on a bin centre | **M** — +0.0% measured against planted tones |
| S-2 | Scalloping bounded at the window's theoretical limit, and recoverable | **M** — ≤1.42 dB Hann; interpolator returns 51250 µg for 50000 planted |
| S-3 | No uncontrolled aliasing into the band | **P** — 400 Hz gives 190 Hz Nyquist, but the ÷4 stage feeding 0xC2 is a boxcar, not a real FIR |
| S-4 | Band levels correct for multi-component signals | **M** — ENBW-corrected, within 5% of theory |
| S-5 | Lost samples detectable and never interpolated | **M** — indices reopen gaps at true width; 0 losses over 30 s on the bench |
| S-6 | Every reported frequency scaled by the **measured** ODR, not the nominal | **M** — measured 379.7 Hz against 400 nominal, a 5.1% error |
| S-7 | Decline to report a tone when the spectrum is noise | **M** — ≤3 false tones in 30 pure-noise spectra; a still tag reports none |

## Platform

| # | Requirement | State |
|---|---|---|
| P-1 | Fits the nRF52832: 512 kB flash, 64 kB RAM | **M** — 30.5% flash, 82.6% RAM |
| P-2 | Recoverable over SWD after any firmware fault | **M** |
| P-3 | Never stop advertising permanently | **M** — see [ADR-0004](adr/0004-advertising-is-reasserted-every-slot.md); two designs failed this before the current one |
| P-4 | Host side runs offline: no CDN, no fonts, no cloud, no account | **M** |
| P-5 | Idle draw low enough for a CR2477 to last a year | **P** — 30.5 µA modelled from a measured 92.3% idle duty, ~3 years. The duty cycle is measured; the currents are datasheet ([08 Power](08-power.md)) |
| P-6 | Report enough about itself to be diagnosed without being fetched | **M** — battery voltage in DF5, duty cycle and motion count over GATT |

## Constraints

| # | Constraint |
|---|---|
| C-1 | DF5 output is byte-compatible or it is broken. `webapp/protocol.py` is checked against Ruuvi's published vectors. |
| C-2 | This is not a medical or safety device and must not be presented as one. |
| C-3 | Numbers in the docs are measured or marked as unmeasured. No plausible-looking placeholders. |

## Open

- **O-1** No current measurement. The power model's per-mode currents are
  datasheet values; only the duty cycle is measured. A Power Profiler Kit II
  would close this in an afternoon.
- **O-2** The ÷4 decimation feeding 0xC2 is a boxcar average — a genuine
  low-pass with a poor stopband. A 20-tap FIR is the eventual answer (S-3).
- **O-3** The 150–188 Hz region is measurably noisier (581 µg/bin against
  ~270 below it) and is not covered by the default band. It remains reachable
  with `?fhi=188`.
- **O-5** `RFA_MOTION_THRESHOLD_MG` is site-specific. 256 mg works on this
  bench; nothing says it works on a pump. Tune with `tools/tune_motion.py`.
- **O-4** No absolute amplitude calibration against a reference shaker. All
  amplitude claims are self-consistent and verified against synthetic tones,
  which is not the same as traceable.
