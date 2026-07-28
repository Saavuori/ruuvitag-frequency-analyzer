#!/usr/bin/env python3
"""Signal-processing tests, against signals whose right answer is known.

    python -m pytest tests/ -q          (or: python tests/test_dsp.py)

Every case here plants a tone of a stated frequency and amplitude and checks
what comes back out. That is the only way to know the analyser is calibrated
rather than merely self-consistent: a spectrum that is internally coherent and
uniformly 15% low looks entirely convincing on screen.
"""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from webapp import dsp                                             # noqa: E402

FS = 400.0


def tone(hz, amp_ug, n, fs=FS, phase=0.0):
    t = np.arange(n) / fs
    return amp_ug * np.sin(2 * math.pi * hz * t + phase)


def test_amplitude_is_calibrated_on_bin_centre():
    """A tone landing exactly on a bin must read its true amplitude."""
    nfft = 1024
    on_centre = 8 * FS / nfft          # 3.125 Hz, exactly bin 8
    x = tone(on_centre, 50_000, 4096)
    res = dsp.stft(x, FS, dsp.StftConfig(nfft=nfft, overlap=0.5))
    col = res.amp_ug[0]
    k = int(np.argmin(np.abs(res.freqs_hz - on_centre)))
    assert abs(col[k] - 50_000) / 50_000 < 0.02


def test_amplitude_survives_a_tone_between_bins():
    """Scalloping. A tone halfway between bins loses up to ~15% from the peak
    bin alone; summing the main lobe is what makes the reading offset-
    independent, and dominant_peak is what does that summing."""
    nfft = 1024
    bin_hz = FS / nfft
    off_centre = (8 + 0.5) * bin_hz
    x = tone(off_centre, 50_000, 4096)
    res = dsp.stft(x, FS, dsp.StftConfig(nfft=nfft, overlap=0.5))
    pk = dsp.dominant_peak(res.freqs_hz, res.amp_ug[0])
    assert pk is not None
    assert abs(pk["amplitude_ug"] - 50_000) / 50_000 < 0.05
    assert abs(pk["hz"] - off_centre) < bin_hz / 4


def test_resolution_and_window_length_are_one_fact():
    """dF x dT ~ 1. If this ever stops holding, one of the two numbers the UI
    reports is a fiction."""
    for nfft in (256, 1024, 4096):
        res = dsp.stft(tone(10, 1000, 8192), FS, dsp.StftConfig(nfft=nfft))
        assert abs(res.bin_hz * res.window_seconds - 1.0) < 1e-9


def test_overlap_changes_column_rate_not_resolution():
    a = dsp.stft(tone(10, 1000, 20000), FS, dsp.StftConfig(nfft=1024, overlap=0.0))
    b = dsp.stft(tone(10, 1000, 20000), FS, dsp.StftConfig(nfft=1024, overlap=0.75))
    assert a.bin_hz == b.bin_hz
    assert a.window_seconds == b.window_seconds
    assert len(b.times_s) > 3 * len(a.times_s)


def test_two_tones_resolve():
    """Separated by four bins, both must appear at their own amplitude.

    Both are placed on bin centres, so this measures resolution and leakage
    only. Put them between centres and each reads ~10% low from scalloping,
    which is a different property with its own test below.
    """
    nfft = 1024
    bin_hz = FS / nfft
    f1 = 26 * bin_hz                       # exactly bin 26
    f2 = 30 * bin_hz                       # exactly bin 30, four bins away
    x = tone(f1, 40_000, 8192) + tone(f2, 20_000, 8192)
    res = dsp.stft(x, FS, dsp.StftConfig(nfft=nfft))
    col = res.amp_ug[0]
    k1 = int(np.argmin(np.abs(res.freqs_hz - f1)))
    k2 = int(np.argmin(np.abs(res.freqs_hz - f2)))
    assert abs(col[k1] - 40_000) / 40_000 < 0.02
    assert abs(col[k2] - 20_000) / 20_000 < 0.02


def test_scalloping_is_bounded_and_recoverable():
    """A tone between bin centres reads low - at most 1.42 dB (~15%) with Hann.

    Pinned because it is the price of coherent-gain normalisation, and because
    a regression here would show up as amplitudes that are quietly wrong rather
    than as anything failing loudly.
    """
    nfft = 1024
    bin_hz = FS / nfft
    worst = (26 + 0.5) * bin_hz            # halfway between bins: worst case
    res = dsp.stft(tone(worst, 50_000, 8192), FS, dsp.StftConfig(nfft=nfft))
    col = res.amp_ug[0]
    k = int(np.argmax(col))
    assert 0.84 < col[k] / 50_000 < 1.0, "scalloping outside the Hann bound"

    pk = dsp.dominant_peak(res.freqs_hz, col)
    assert pk is not None
    assert abs(pk["amplitude_ug"] - 50_000) / 50_000 < 0.05
    assert abs(pk["hz"] - worst) < bin_hz / 4


def test_default_band_covers_one_to_onefifty():
    """The default span. 150 Hz is where the measured noise floor degrades, not
    where Nyquist is - see the note in dsp.py."""
    res = dsp.stft(tone(10, 1000, 8192), FS, dsp.StftConfig(nfft=1024))
    assert res.freqs_hz[0] >= 1.0
    assert res.freqs_hz[-1] <= 150.0
    assert res.freqs_hz[-1] > 149.0


def test_band_can_be_widened_to_nyquist():
    """f_hi is a request parameter, not a wall. Someone who wants the degraded
    150-188 Hz region should be able to look at it."""
    res = dsp.stft(tone(10, 1000, 8192), FS, dsp.StftConfig(nfft=1024, f_hi=FS / 2))
    assert res.freqs_hz[-1] > 180.0


def test_lost_samples_produce_a_gap_not_a_zero():
    """Zero-filling a dropout adds a broadband click - a transient with energy
    at every frequency, which is exactly the artefact most easily mistaken for
    a real event. NaN in, NaN out, and the UI draws a hole."""
    on_centre = 26 * FS / 1024             # on a bin, so scalloping is not in play
    x = tone(on_centre, 50_000, 8192)
    x[3000:3100] = np.nan
    res = dsp.stft(x, FS, dsp.StftConfig(nfft=1024, overlap=0.75))
    assert res.n_lost > 0
    assert np.isnan(res.amp_ug).any(axis=1).sum() == res.n_lost
    # The windows that missed the hole are untouched.
    good = res.amp_ug[~np.isnan(res.amp_ug).any(axis=1)]
    assert good.shape[0] > 0
    k = int(np.argmin(np.abs(res.freqs_hz - on_centre)))
    assert abs(good[0][k] - 50_000) / 50_000 < 0.02


def test_dominant_peak_declines_to_name_a_frequency_in_noise():
    """Every spectrum has a tallest bin; only some have a tone. Reporting the
    argmax regardless is how a motionless tag names a confident frequency drawn
    entirely from its own noise floor."""
    rng = np.random.default_rng(7)
    found = 0
    for i in range(30):
        noise = rng.normal(0, 2500, 4096)
        res = dsp.stft(noise, FS, dsp.StftConfig(nfft=1024))
        if dsp.dominant_peak(res.freqs_hz, res.amp_ug[0]) is not None:
            found += 1
    # The threshold targets ~1% false tones; allow slack, but not 30%.
    assert found <= 3, f"named a tone in {found}/30 pure-noise spectra"


def test_dominant_peak_finds_a_real_tone_in_noise():
    rng = np.random.default_rng(3)
    x = tone(17.0, 30_000, 4096) + rng.normal(0, 2500, 4096)
    res = dsp.stft(x, FS, dsp.StftConfig(nfft=1024))
    pk = dsp.dominant_peak(res.freqs_hz, res.amp_ug[0])
    assert pk is not None
    assert abs(pk["hz"] - 17.0) < 0.5


def test_band_amplitude_matches_rms():
    """A sinusoid of amplitude A has RMS A/sqrt(2). Components add in power, so
    two tones in one band read as the root of the sum of squares."""
    x = tone(5.0, 40_000, 8192) + tone(8.0, 30_000, 8192)
    res = dsp.stft(x, FS, dsp.StftConfig(nfft=1024))
    got = dsp.band_amplitude(res.freqs_hz, res.amp_ug, 1, 10, res.enbw_bins)[0]
    want = math.sqrt((40_000 ** 2 + 30_000 ** 2) / 2)
    assert abs(got - want) / want < 0.05


def test_band_amplitude_keeps_a_hole_as_a_hole():
    x = tone(5.0, 40_000, 8192)
    x[:] = np.nan
    res = dsp.stft(x, FS, dsp.StftConfig(nfft=1024))
    got = dsp.band_amplitude(res.freqs_hz, res.amp_ug, 1, 10, res.enbw_bins)
    assert np.isnan(got).all(), "an all-lost column read as a measurement of silence"


def test_flatness_separates_a_tone_from_noise():
    rng = np.random.default_rng(11)
    res_t = dsp.stft(tone(12, 40_000, 4096), FS, dsp.StftConfig(nfft=1024))
    res_n = dsp.stft(rng.normal(0, 5000, 4096), FS, dsp.StftConfig(nfft=1024))
    assert dsp.spectral_flatness(res_t.amp_ug[0]) < 0.05
    assert dsp.spectral_flatness(res_n.amp_ug[0]) > 0.3


def test_assemble_reopens_a_gap_at_its_true_width():
    """The wire protocol's sample index earns its four bytes here. Concatenating
    what arrived would compress time across the dropout, and time compression
    reads as a frequency shift - an error that looks like a measurement."""
    a = np.ones((100, 3), dtype=np.int16)
    b = np.full((100, 3), 2, dtype=np.int16)
    out, first, missing = dsp.assemble([(0, a), (500, b)])
    assert first == 0
    assert out.shape[0] == 600
    assert missing == 400
    assert np.isnan(out[100:500]).all()
    assert (out[:100] == 1).all() and (out[500:] == 2).all()


def test_assemble_accepts_blocks_out_of_order():
    a = np.ones((10, 3), dtype=np.int16)
    b = np.full((10, 3), 2, dtype=np.int16)
    out, first, missing = dsp.assemble([(10, b), (0, a)])
    assert first == 0 and missing == 0
    assert (out[:10] == 1).all() and (out[10:] == 2).all()


def test_magnitude_axis_doubles_a_symmetric_tone():
    """Documented trap, pinned here so nobody quietly 'fixes' it. |v| rectifies,
    so a tone swinging about zero appears at twice its frequency. The UI labels
    the axis for this reason."""
    n = 8192
    x = np.zeros((n, 3))
    x[:, 2] = tone(6.0, 40_000, n) / 1000.0        # select_axis expects mg
    sig = dsp.select_axis(x, "mag")
    res = dsp.stft(sig, FS, dsp.StftConfig(nfft=1024))
    pk = dsp.dominant_peak(res.freqs_hz, res.amp_ug[0])
    assert pk is not None
    assert abs(pk["hz"] - 12.0) < 0.5


def test_windows_all_run_and_flattop_is_the_amplitude_accurate_one():
    """Flat top trades resolution for amplitude accuracy. Off bin centre it
    should beat Hann on the peak-bin reading, which is the only reason to pay
    its 3.8-bin main lobe."""
    nfft = 1024
    off = (20 + 0.5) * FS / nfft
    x = tone(off, 50_000, 4096)
    err = {}
    for wname in dsp.WINDOWS:
        res = dsp.stft(x, FS, dsp.StftConfig(nfft=nfft, window=wname))
        col = res.amp_ug[0]
        err[wname] = abs(col.max() - 50_000) / 50_000
    assert err["flattop"] < err["hann"]
    assert err["flattop"] < 0.02


def test_short_capture_says_so_instead_of_guessing():
    res = dsp.stft(tone(10, 1000, 100), FS, dsp.StftConfig(nfft=1024))
    assert res.times_s.size == 0
    assert res.notes and "1024" in res.notes[0]


def test_latest_run_drops_everything_before_a_reboot():
    """The sample index restarts at zero when the tag resets. Treating that as a
    gap marks every window in the capture as lost - which is what a live capture
    across a reflash actually did before this existed."""
    a = np.ones((100, 3), dtype=np.int16)
    b = np.full((100, 3), 2, dtype=np.int16)
    c = np.full((100, 3), 3, dtype=np.int16)
    # Two boots: indices 900000 and 900100, then a reset back to 0.
    blocks = [(900_000, a, 1.0), (900_100, b, 2.0), (0, c, 3.0)]
    run = dsp.latest_run(blocks)
    assert len(run) == 1 and run[0][0] == 0

    out, first, missing = dsp.assemble(run)
    assert missing == 0
    assert out.shape[0] == 100


def test_latest_run_keeps_an_uninterrupted_capture_whole():
    a = np.ones((100, 3), dtype=np.int16)
    blocks = [(0, a, 1.0), (100, a, 2.0), (200, a, 3.0)]
    assert len(dsp.latest_run(blocks)) == 3


if __name__ == "__main__":
    fails = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  ok    {name}")
            except Exception as exc:
                fails += 1
                print(f"  FAIL  {name}: {exc}")
    print(f"\n{'FAILED' if fails else 'passed'} ({fails} failures)")
    sys.exit(1 if fails else 0)
