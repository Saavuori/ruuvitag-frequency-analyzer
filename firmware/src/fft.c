/*
 * Radix-2 FFT and the transmitted magnitude encoding.
 */

#include <math.h>
#include <string.h>

#include "fft.h"

#ifndef M_PIf
#define M_PIf 3.14159265358979323846f
#endif

void rfa_fft(float *re, float *im, int n)
{
	/* Bit-reversal permutation. */
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}

	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * M_PIf / (float)len;
		float wr = cosf(ang), wi = sinf(ang);

		for (int i = 0; i < n; i += len) {
			float cr = 1.0f, ci = 0.0f;

			for (int k = 0; k < len / 2; k++) {
				float ur = re[i + k], ui = im[i + k];
				float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
				float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;

				re[i + k] = ur + vr;
				im[i + k] = ui + vi;
				re[i + k + len / 2] = ur - vr;
				im[i + k + len / 2] = ui - vi;

				/* Twiddle recurrence. Cheap, and the drift over
				 * 128 steps is far below our resolution. */
				float nr = cr * wr - ci * wi;

				ci = cr * wi + ci * wr;
				cr = nr;
			}
		}
	}
}

void rfa_spectrum_bins(const float *samples_ug, int n, uint8_t *out_db, int n_bins)
{
	/* Static, not automatic: 256 floats each is 2 kB of stack between them,
	 * which overflows the sampler thread. One caller, one thread. */
	static float re[RFA_WF_N];
	static float im[RFA_WF_N];

	for (int i = 0; i < n_bins; i++) {
		out_db[i] = 0;
	}
	if (n <= 0 || n > RFA_WF_N || n_bins <= 0) {
		return;
	}

	/* Mean removal. Residual DC dominates bin 0 and leaks into its
	 * neighbours - 0.39 and 0.78 Hz - which is inside the band of interest. */
	float mean = 0.0f;

	for (int i = 0; i < n; i++) {
		mean += samples_ug[i];
	}
	mean /= (float)n;

	/* Hann window: a rectangular window's sidelobes would smear a strong
	 * low-frequency component across the whole band. */
	float sum_w = 0.0f;

	for (int i = 0; i < RFA_WF_N; i++) {
		if (i < n) {
			float w = 0.5f * (1.0f - cosf(2.0f * M_PIf * (float)i / (float)(n - 1)));

			re[i] = (samples_ug[i] - mean) * w;
			sum_w += w;
		} else {
			re[i] = 0.0f;   /* zero-pad a short window */
		}
		im[i] = 0.0f;
	}
	rfa_fft(re, im, RFA_WF_N);

	if (sum_w <= 0.0f) {
		return;
	}

	const int half = RFA_WF_N / 2;
	const int lim = n_bins < half ? n_bins : half;

	/*
	 * Coherent-gain normalisation: A = 2 |X_k| / sum(w), so a bin reads the
	 * amplitude in microg of a tone sitting on it. Same convention as the
	 * host's webapp/dsp.py, which is what lets a broadcast spectrum be laid
	 * over a host-computed one and compared.
	 *
	 * Not the Parseval form sqrt(4|X|^2 / (N * sum_w2)): that one is correct
	 * only when summed across the whole main lobe, and used per bin it reads
	 * an on-centre tone 18% low, because a Hann window puts only half the
	 * amplitude in the centre bin. tests/test_dsp.py pins this down.
	 */
	for (int i = 0; i < lim; i++) {
		float p = re[i] * re[i] + im[i] * im[i];
		float amp = 2.0f * sqrtf(p) / sum_w;

		out_db[i] = amp < 1.0f ? 0 : (uint8_t)fminf(255.0f, 40.0f * log10f(amp) + 0.5f);
	}
}
