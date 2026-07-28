# 04 — Signal processing

The chain, end to end:

```
LIS2DH12  400 Hz, ±2 g, 12-bit high-res, hardware FIFO
   │
   ├─ DC tracker (EMA, ~20 s)  ──────────────►  DF5 acceleration field
   │        │
   │        └─ subtract ──► AC samples, mg
   │                          │
   │                          ├─ raw ring ──► GATT ──► host STFT   ◄── the instrument
   │                          │
   │                          └─ ÷4 boxcar ──► 100 Hz ──► on-tag 256-pt FFT ──► 0xC2
```

Everything the analyser displays comes from the top branch. The bottom branch
exists so a tag nobody is connected to still says something.

## Acquisition

**400 Hz through the FIFO, not polled.** Polling a free-running ODR from a
thread the BLE stack preempts puts the sampling jitter straight into the
spectrum, as a peak at the advertising cadence and its harmonics. With the FIFO
the sensor timestamps samples on its own clock, so host read timing costs buffer
depth rather than sample spacing. The FIFO is 32 slots — 80 ms at 400 Hz — and
is polled every 40 ms, a 2× margin.

**±2 g, not ±4 g.** 1 mg/LSB matters more than headroom. High-resolution
(12-bit) mode rather than normal mode for the same reason: 4 mg/LSB would put
quantisation above the entire amplitude range of small vibration.

**The DC vector is tracked and subtracted, not calibrated once.** Zero-g offset
drifts with temperature. Streaming the DC component would put ~1000 mg in bin 0
and, through window leakage, into bins 1 and 2 — 0.39 to 0.78 Hz, inside the
band of interest.

## The measured sample rate is not 400 Hz

The LIS2DH12's ODR comes from an internal RC oscillator. On the bench tag:

```
nominal   400.000 Hz
measured  378.9 – 379.7 Hz     (−5.1%, drifting slightly with temperature)
```

The tag counts its own delivered samples against the RTC and reports the result
in the Info characteristic; the host scales every frequency by that. Taking the
nominal rate instead would put 5.4% on every frequency the analyser draws — a
50 Hz component would read 52.7 Hz.

This is why the UI labels the rate as *measured* or *assumed*, and why the
displayed resolution is 0.370 Hz rather than the 0.391 Hz the nominal rate would
suggest.

## The transform

Host side, in `webapp/dsp.py`, with everything configurable per request:

| Setting | Default | Range |
|---------|---------|-------|
| Length | 1024 | 256 … 4096 |
| Overlap | 75% | 0 … 87.5% |
| Window | Hann | Hann, Hamming, Blackman, flat top, none |
| Axis | Z | X, Y, Z, \|xyz\| |
| Band | 1–50 Hz | any |

**Resolution and window length are the same fact.** ΔF ≈ fs/N and the window
spans N/fs seconds, so 0.39 Hz bins require a 2.56 s window. Always. Overlap
slides that window more often — it does not shorten it. A waterfall updating
1.5×/s where each column integrates 2.7 s shows smooth motion, not fast motion,
and the UI reports both numbers side by side so the trade is visible.

### Amplitude calibration

Bins are **coherent-gain** normalised:

```
A_k = 2 |X_k| / Σ w_i
```

so a bin reads the amplitude, in µg, of a tone sitting on it. Verified against
planted tones: on a bin centre, **+0.0%**.

The tempting alternative — `sqrt(4|X|²/(N·Σw²))`, from Parseval — is correct
only when *summed across the whole main lobe*. Used per bin it reads an
on-centre tone **18% low**, because a Hann window puts only half the amplitude
in the centre bin. That was a real bug in this code, caught by a test planting a
known tone on a known bin, and the firmware carried the same error.

The cost of coherent gain is **scalloping**: a tone halfway between bin centres
reads up to 1.42 dB (15%) low with Hann. Two things address it:

- `dominant_peak` fits a parabola to the peak bin and its neighbours *in dB*.
  A windowed main lobe is nearly parabolic on a log scale, so the same fit that
  locates the sub-bin frequency also recovers the lost amplitude. Measured on a
  worst-case tone: 51250 µg against 50000 planted, at 9.995 Hz against 10.000.
- The **flat-top** window trades resolution (3.8 bins) for amplitude accuracy of
  ~0.01 dB regardless of offset. Use it when you care what something measures
  rather than where it is.

### Band levels

Bins are amplitudes, so a band's RMS is

```
RMS = sqrt( Σ A_k² / 2 / ENBW )
```

where ENBW = N·Σw²/(Σw)² is the window's equivalent noise bandwidth — 1.0
rectangular, 1.5 Hann, 3.77 flat top. Without that division a single tone reads
22% high with Hann, purely because its own skirts were counted as signal.

### Naming a tone

Every spectrum has a tallest bin; only some have a tone. A peak must exceed the
band's **median** bin power by 13.5× before it is reported. The median is a
robust noise-floor estimate — one strong tone barely moves it, where a mean
would be dragged up by the very peak under test.

The threshold is derived, not guessed. Power bins of white noise are
exponentially distributed, so over *m* bins the largest sits around ln(m) times
the mean by chance, and the median of an exponential is ln(2) times its mean.
For m ≈ 120 that puts the expected noise peak near 7× the median. Targeting a
1% false-tone rate:

```
peak/median ≈ ln(m / 0.01) / ln(2) = ln(12000) / 0.693 ≈ 13.5
```

Below that the answer is **"no tonal peak"**, which is a real answer. On the
bench, a tag sitting still on a desk reports exactly that, with its top five
bins within 2 dB of each other and a flatness of 0.60.

## Gaps

Every sample carries a monotonic index. Where packets were lost the host reopens
the hole at its true width and any transform window touching it produces NaN,
which the API serialises as null and the UI draws as a gap.

Two things are never done:

**Splicing.** Concatenating what arrived would compress time across every
dropout, and compressed time is a frequency error that looks exactly like a
measurement.

**Zero-filling.** A zero-filled hole is a step discontinuity — a broadband click
with energy at every frequency, which is the artefact most easily mistaken for a
real event.

## What the sensor cannot do

The LIS2DH12's noise floor is ~220 µg/√Hz with 1 mg/LSB quantisation. Measured
on the bench with the tag at rest, band levels sit at 1.1–1.4 mg RMS. Anything
below that is not there to be recovered, by this or any other processing.

The `|xyz|` axis rectifies, so a tone swinging symmetrically about zero appears
at **twice** its frequency. Verified: a 6 Hz tone reads 12 Hz. It is offered
because it answers "how much is moving" regardless of orientation, and labelled
in the UI because it is a trap.
