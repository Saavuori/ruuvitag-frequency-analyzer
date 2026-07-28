#!/usr/bin/env python3
"""Short-time Fourier analysis of the raw sample stream.

This module is the reason the firmware got simpler. Transform length, overlap,
window function, axis and reference level are arguments here, not constants
compiled into a tag you have to unscrew from a wall to change (ADR-0003).

Two rules hold throughout:

**Resolution and window length are one fact.** dF ~ fs/N, and the window spans
N/fs seconds. 0.39 Hz bins therefore require a 2.56 s window at 400 Hz, always.
Overlap slides that window more often; it does not shorten it. A waterfall
updating ten times a second where each column integrates 2.56 s is showing you
smooth motion, not fast motion, and `SpectrogramResult.window_seconds` is
reported so the UI can say so.

**Gaps are not zeros.** A window containing lost samples produces NaN, which the
API serialises as null and the UI draws as a gap. Zero-filling a dropout puts a
broadband click into the spectrum - a transient with energy at every frequency,
which is precisely the artefact most likely to be mistaken for a real event.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

# Analysis band.
#
# 1 Hz at the bottom because the DC tracker's residual drift and the window's
# own leakage make anything below it untrustworthy.
#
# 150 Hz at the top because that is where the sensor stops being trustworthy,
# measured rather than assumed. A 30 s capture at 0.18 Hz resolution gave a
# median per-bin noise floor of:
#
#     1-10 Hz    374 ug        50-100 Hz   242 ug
#     10-50 Hz   361 ug       100-150 Hz   268 ug
#                             150-188 Hz   581 ug
#
# Flat to 150 Hz - slightly better above 50 than below it, since the low end
# carries the DC tracker's residue - then doubling as it approaches the 188 Hz
# Nyquist, where there is no anti-alias filter to protect it.
#
# Widening the span does not raise the per-bin floor: bin noise follows bin
# width, not total bandwidth. It only raises band-RMS figures, which integrate
# over more of it.
#
# f_hi is not clamped, so `?fhi=188` still works if you want to look at the
# degraded top end. The broadcast 0xC2 channel stays 0-50 Hz regardless - it is
# defined at the 100 Hz decimated stage and cannot widen without a new spec
# version.
BAND_LO_HZ = 1.0
BAND_HI_HZ = 150.0

WINDOWS = ("hann", "hamming", "blackman", "flattop", "rect")

# Prominence a peak must have over the band's median bin before it counts as a
# tone rather than the tallest piece of noise.
#
# Not arbitrary. Power bins of white noise are exponentially distributed, so
# over m bins the largest sits around ln(m) times the mean by chance alone, and
# the median of an exponential is ln(2) times its mean. For m ~ 120 the expected
# noise peak is near 7x the median, which is why a threshold of 8 rejects almost
# nothing. Targeting a ~1% false-tone rate:
#
#     peak/median ~ ln(m / 0.01) / ln(2) = ln(12000) / 0.693 ~ 13.5
DEFAULT_PROMINENCE = 13.5


@dataclass
class StftConfig:
    nfft: int = 1024
    overlap: float = 0.75
    window: str = "hann"
    axis: str = "z"            # x | y | z | mag
    f_lo: float = BAND_LO_HZ
    f_hi: float = BAND_HI_HZ

    def hop(self) -> int:
        h = int(round(self.nfft * (1.0 - self.overlap)))
        return max(1, min(h, self.nfft))


@dataclass
class SpectrogramResult:
    freqs_hz: np.ndarray             # (n_bins,)
    times_s: np.ndarray              # (n_cols,) centre of each window
    amp_ug: np.ndarray               # (n_cols, n_bins), NaN where data was lost
    fs_hz: float
    window_seconds: float
    bin_hz: float
    hop_seconds: float
    enbw_bins: float = 1.0           # equivalent noise bandwidth of the window
    n_lost: int = 0
    notes: list[str] = field(default_factory=list)


def enbw(w: np.ndarray) -> float:
    """Equivalent noise bandwidth of a window, in bins.

    A window spreads a tone across its main lobe, so summing power over bins
    counts more bandwidth than one bin's worth. ENBW = N·Σw² / (Σw)² is how much
    more: 1.0 for a rectangular window, 1.5 for Hann, 3.77 for flat top.

    Needed because the two things this module reports want opposite
    normalisations. A displayed bin should read the amplitude of a tone sitting
    on it (coherent gain). A band level should read the total energy in the band,
    which means dividing the summed power back down by ENBW - otherwise a single
    tone reads 22% high with Hann simply because its skirts were counted too.
    """
    n = len(w)
    s = w.sum()
    return float(n * (w ** 2).sum() / (s * s)) if s else 1.0


def _window(name: str, n: int) -> np.ndarray:
    """Window functions, with the trade each one makes.

    hann      the default. -31 dB sidelobes, 1.5 bin main lobe. Good enough at
              everything, best at nothing.
    hamming   lower first sidelobe than Hann but a slower far-field rolloff.
    blackman  -58 dB sidelobes for a 2 bin main lobe: use when a strong tone is
              burying a weak one nearby.
    flattop   terrible resolution (3.8 bins), amplitude accurate to ~0.01 dB
              regardless of where the tone falls between bins. Use when you care
              what something measures, not where it is.
    rect      no window. Only honest choice for a transient that starts and ends
              inside the frame; otherwise it smears everything.
    """
    if name == "hann":
        return np.hanning(n)
    if name == "hamming":
        return np.hamming(n)
    if name == "blackman":
        return np.blackman(n)
    if name == "flattop":
        # Standard 5-term flat top (SRS / Matlab coefficients).
        a = [0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368]
        k = np.arange(n)
        w = np.zeros(n)
        for i, c in enumerate(a):
            w += ((-1) ** i) * c * np.cos(2 * np.pi * i * k / (n - 1))
        return w
    if name == "rect":
        return np.ones(n)
    raise ValueError(f"unknown window {name!r}; choose from {WINDOWS}")


def select_axis(samples_mg: np.ndarray, axis: str) -> np.ndarray:
    """(n, 3) in mg to (n,) in microg.

    'mag' is the vector magnitude. It is convenient and it is a trap for
    frequency work: |v| rectifies, so a tone that swings symmetrically about
    zero appears at twice its true frequency. It is offered because it answers
    "how much is moving" without caring about orientation, and labelled in the
    UI for the same reason.
    """
    if axis == "mag":
        return np.sqrt((samples_mg.astype(np.float64) ** 2).sum(axis=1)) * 1000.0
    idx = {"x": 0, "y": 1, "z": 2}.get(axis)
    if idx is None:
        raise ValueError(f"unknown axis {axis!r}")
    return samples_mg[:, idx].astype(np.float64) * 1000.0


def stft(samples_ug: np.ndarray, fs_hz: float, cfg: StftConfig) -> SpectrogramResult:
    """Amplitude spectrogram in microg.

    `samples_ug` may contain NaN for samples known to be missing; any window
    touching one produces a NaN column.

    **Coherent-gain normalisation**: `A = 2|X_k| / sum(w)`, so a bin reads the
    amplitude of a tone sitting on it. This is what a spectrum display must do,
    and it matches the firmware's `rfa_spectrum_bins` so a tag's broadcast
    spectrum can be laid over a host-computed one.

    The tempting alternative - `sqrt(4|X|^2 / (N * sum_w2))`, from Parseval - is
    correct only when *summed across the main lobe*. Used per bin it reads a
    perfectly on-centre tone 18% low, because a Hann window puts only half the
    amplitude in the centre bin. That was a real bug here, caught by
    tests/test_dsp.py planting a tone of known amplitude on a known bin.

    The cost of coherent gain is scalloping: a tone between bin centres reads
    low by up to 1.42 dB with Hann. `dominant_peak` interpolates that back out,
    and the flat-top window exists for when you want it gone entirely.
    """
    n = cfg.nfft
    hop = cfg.hop()
    total = len(samples_ug)
    notes: list[str] = []

    if total < n:
        return SpectrogramResult(
            freqs_hz=np.zeros(0), times_s=np.zeros(0),
            amp_ug=np.zeros((0, 0)), fs_hz=fs_hz,
            window_seconds=n / fs_hz, bin_hz=fs_hz / n,
            hop_seconds=hop / fs_hz,
            notes=[f"need {n} samples for a {n}-point transform, have {total}"],
        )

    w = _window(cfg.window, n)
    sum_w = float(w.sum())
    window_enbw = enbw(w)

    starts = np.arange(0, total - n + 1, hop)
    frames = np.lib.stride_tricks.sliding_window_view(samples_ug, n)[starts]

    # A window is unusable if any sample in it is missing. Detect before the
    # transform: NaN propagates through the FFT to every bin anyway, but doing
    # it explicitly means the count is reportable.
    bad = np.isnan(frames).any(axis=1)
    n_lost = int(bad.sum())
    if n_lost:
        notes.append(f"{n_lost} of {len(starts)} windows contained lost samples")

    clean = np.nan_to_num(frames, nan=0.0)
    # Per-frame mean removal. Residual DC dominates bin 0 and leaks into its
    # neighbours, which at 0.39 Hz spacing is exactly where the band starts.
    clean = clean - clean.mean(axis=1, keepdims=True)

    spec = np.fft.rfft(clean * w, axis=1)
    amp = 2.0 * np.abs(spec) / sum_w if sum_w else np.zeros_like(np.abs(spec))
    amp[bad, :] = np.nan

    freqs = np.fft.rfftfreq(n, d=1.0 / fs_hz)

    lo = max(cfg.f_lo, 0.0)
    hi = cfg.f_hi if cfg.f_hi > lo else fs_hz / 2.0
    keep = (freqs >= lo) & (freqs <= hi)
    if not keep.any():
        keep = np.ones_like(freqs, dtype=bool)
        notes.append(f"band {lo}-{hi} Hz is empty at {fs_hz:.1f} Hz; showing all bins")

    times = (starts + n / 2.0) / fs_hz

    return SpectrogramResult(
        freqs_hz=freqs[keep],
        times_s=times,
        amp_ug=amp[:, keep],
        fs_hz=fs_hz,
        window_seconds=n / fs_hz,
        bin_hz=fs_hz / n,
        hop_seconds=hop / fs_hz,
        enbw_bins=window_enbw,
        n_lost=n_lost,
        notes=notes,
    )


def band_amplitude(freqs_hz: np.ndarray, amp_ug: np.ndarray,
                   lo: float, hi: float, enbw_bins: float = 1.0) -> np.ndarray:
    """RMS amplitude across a frequency band, per column, in microg.

    Root-sum-square of the bin amplitudes, not their mean: uncorrelated
    components add in power. A mean would report a single strong tone as weaker
    than it is by the width of the band it sits in.

    `enbw_bins` divides the summed power back down by the window's equivalent
    noise bandwidth. The bins are coherent-gain amplitudes, so a single tone
    also appears in its neighbours; without this correction one tone reads 22%
    high with Hann purely because its own skirts were counted as signal.
    """
    sel = (freqs_hz >= lo) & (freqs_hz <= hi)
    if not sel.any():
        return np.full(amp_ug.shape[0], np.nan)

    band = amp_ug[:, sel]
    # A_k is the peak amplitude of a tone on bin k, whose RMS is A_k/sqrt(2).
    rms = np.sqrt(np.nansum(band ** 2, axis=1) / 2.0 / max(enbw_bins, 1e-9))
    # nansum makes an all-lost column read as 0, which is a measurement of
    # silence rather than an absence of measurement. Put the hole back.
    rms[np.isnan(band).all(axis=1)] = np.nan
    return rms


def dominant_peak(freqs_hz: np.ndarray, amp_ug: np.ndarray,
                  prominence: float = DEFAULT_PROMINENCE) -> dict | None:
    """The strongest tone in one spectrum, or None if it is all noise.

    Returning None is a real answer, not a failure. Every spectrum has a tallest
    bin; only some have a tone. Reporting the argmax unconditionally means a
    motionless tag on a quiet desk names a confident frequency drawn entirely
    from its own noise floor, and somebody eventually builds a theory on it.

    Frequency and amplitude both come from a parabola fitted to the peak bin and
    its two neighbours **in dB**. A windowed tone's main lobe is close to
    parabolic on a log scale, so this recovers both the sub-bin offset and the
    scalloping loss that offset caused - a tone halfway between Hann bins reads
    1.42 dB low, and the same fit that says where it is says how much was lost.
    """
    if amp_ug.ndim != 1 or amp_ug.size < 3 or np.isnan(amp_ug).all():
        return None

    amp = np.nan_to_num(amp_ug, nan=0.0)
    power = amp ** 2
    best = int(np.argmax(power))
    if power[best] <= 0.0:
        return None

    median = float(np.median(power[power > 0])) if (power > 0).any() else 0.0
    if median > 0.0 and power[best] < median * prominence:
        return None

    freq = float(freqs_hz[best])
    peak_amp = float(amp[best])

    if 0 < best < len(amp) - 1 and amp[best - 1] > 0 and amp[best + 1] > 0:
        a = 20.0 * math.log10(amp[best - 1])
        b = 20.0 * math.log10(amp[best])
        c = 20.0 * math.log10(amp[best + 1])
        denom = a - 2.0 * b + c
        if denom != 0.0:
            delta = 0.5 * (a - c) / denom
            if -1.0 < delta < 1.0:
                step = float(freqs_hz[1] - freqs_hz[0]) if len(freqs_hz) > 1 else 0.0
                freq += delta * step
                peak_db = b - 0.25 * (a - c) * delta
                peak_amp = 10.0 ** (peak_db / 20.0)

    return {
        "hz": freq,
        "amplitude_ug": peak_amp,
        "bin": best,
        "prominence": float(power[best] / median) if median > 0 else float("inf"),
    }


def spectral_flatness(amp_ug: np.ndarray) -> float:
    """Geometric mean over arithmetic mean of the power spectrum.

    0 is a pure tone, 1 is white noise. Useful for telling a machine from a
    room: a running motor drops flatness sharply while its amplitude rises,
    where somebody walking past raises both.
    """
    p = np.nan_to_num(amp_ug, nan=0.0) ** 2
    p = p[p > 0]
    if p.size == 0:
        return float("nan")
    return float(np.exp(np.log(p).mean()) / p.mean())


def latest_run(blocks: list[tuple[int, np.ndarray, float]]
               ) -> list[tuple[int, np.ndarray]]:
    """Drop everything before the tag's most recent reboot.

    The sample index is monotonic *within one boot* and restarts at zero after a
    reset. Handed the lot, `assemble` reads that restart as a gap hundreds of
    thousands of samples wide and marks every window in the span as lost - which
    is exactly what a live capture across a reflash did.

    A reboot is not missing data, it is a new time base. Blocks arrive in host
    time order, so a first_index that goes *backwards* is the reset; keep only
    what follows the last one.
    """
    if not blocks:
        return []

    start = 0
    for i in range(1, len(blocks)):
        if blocks[i][0] < blocks[i - 1][0]:
            start = i
    return [(i, a) for i, a, _ in blocks[start:]]


def assemble(blocks: list[tuple[int, np.ndarray]], expect_gaps: bool = True
             ) -> tuple[np.ndarray, int, int]:
    """Stitch indexed sample blocks into one array, NaN across what was lost.

    `blocks` is [(first_index, (n,3) int16 array)], in any order. Returns
    (samples, first_index, n_missing).

    This is where the wire protocol's monotonic sample index earns its four
    bytes. Without it, concatenating what arrived would silently compress time
    across every dropout - and time compression in a spectrum analyser reads as
    a frequency shift, an error that looks exactly like a real measurement.
    """
    if not blocks:
        return np.zeros((0, 3), dtype=np.float64), 0, 0

    blocks = sorted(blocks, key=lambda b: b[0])
    first = blocks[0][0]
    last = blocks[-1][0] + len(blocks[-1][1])
    span = last - first

    out = np.full((span, 3), np.nan, dtype=np.float64)
    for idx, data in blocks:
        start = idx - first
        end = start + len(data)
        if start < 0 or end > span:
            continue
        out[start:end] = data

    missing = int(np.isnan(out[:, 0]).sum())
    if not expect_gaps and missing:
        out = np.nan_to_num(out, nan=0.0)
    return out, first, missing
