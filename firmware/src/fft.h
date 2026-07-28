/*
 * The on-tag transform, for the connectionless broadcast path only.
 *
 * This exists so a tag nobody is connected to still reports something. The
 * analysis anyone actually reads happens on the host from the GATT sample
 * stream, where the window, the overlap and the transform length are settings
 * rather than compile-time constants (ADR-0003). Do not grow this file: every
 * feature added here is a feature that needs a reflash to change.
 *
 * Dependency-free apart from libm, so it runs under host tests against
 * synthetic signals with known answers. Single-precision throughout: the
 * nRF52832 is a Cortex-M4F, so float is both simpler and cheaper here than
 * fixed point.
 */

#ifndef RFA_FFT_H
#define RFA_FFT_H

#include <stdint.h>

/*
 * 256 points at the 100 Hz analysis stage: 0.390625 Hz bins over 0-50 Hz, from
 * a 2.56 s window. That resolution and that window length are the same fact -
 * dF x dT ~ 1 is not a design choice, and no amount of overlap buys resolution
 * the window length does not already contain.
 */
#define RFA_WF_N 256
#define RFA_WF_BINS 128

/*
 * Hop between transforms, in samples of the 100 Hz stage. 128 is 50% overlap:
 * a new spectrum every 1.28 s.
 *
 * There is no point going faster on this path. A frame takes 8 advertisements
 * to transmit, which at the 1.28 s advertising slot is 10.24 s - so the
 * transform is already running 8x faster than the radio can drain it, and the
 * broadcast spectrum is a periodic snapshot rather than a continuous record.
 * Continuity is what the GATT stream is for.
 */
#define RFA_WF_HOP 128

/* In-place radix-2 complex FFT. n must be a power of two. */
void rfa_fft(float *re, float *im, int n);

/*
 * Full magnitude spectrum, encoded for transmission: one byte per bin at
 * 0.5 dB/LSB referenced to 1 ug (see rfa_spectrum_db in adv.h).
 *
 * n must be RFA_WF_N. Writes n_bins values covering DC to n/2. Hann window and
 * mean removal, normalised so a tone reads its true amplitude regardless of
 * where it falls between bin centres.
 */
void rfa_spectrum_bins(const float *samples_ug, int n, uint8_t *out_db, int n_bins);

#endif /* RFA_FFT_H */
