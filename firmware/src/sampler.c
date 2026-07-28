/*
 * Acquisition. See sampler.h for the shape of it.
 *
 * The DC (gravity) vector is tracked with an EMA and subtracted to leave the AC
 * component. Zero-g offset drifts with temperature, which is why this is a
 * tracking estimator and not a one-time calibration (docs/01-hardware.md).
 *
 * The AC component is what both consumers see. Streaming the DC component would
 * put a ~1000 mg constant in bin 0 and, through window leakage, in bins 1 and 2
 * as well - which is 0.39 to 0.78 Hz, inside the band we are here to measure.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "adv.h"
#include "fft.h"
#include "lis2dh12.h"
#include "power.h"
#include "sampler.h"

LOG_MODULE_REGISTER(rfa_sampler, LOG_LEVEL_INF);

/* EMA shift. At 400 Hz, 1/8192 is roughly a 20 s time constant, which matches
 * what DF5's acceleration field is documented to report. */
#define DC_SHIFT 13

static struct {
	int32_t dc_q[3];         /* DC estimate, scaled by 1<<DC_SHIFT */
	bool    dc_primed;

	/* Stage 1: 400 -> 100 Hz, boxcar. Feeds the on-tag transform. */
	int32_t  s1_acc[3];
	uint8_t  s1_count;

	/* Raw ring for the GATT stream. Monotonic indices; the ring is a window
	 * onto them, not the numbering itself. */
	struct rfa_sample ring[RFA_STREAM_RING];
	uint32_t write_idx;
	uint32_t read_idx;
	bool     stream_gap;

	/* Ring for the broadcast (100 Hz) spectrum, 0-50 Hz. */
	float    wf_ring[RFA_WF_N];
	uint16_t wf_head;
	uint16_t wf_fill;
	bool     wf_overrun;     /* a FIFO overrun landed inside the current window */

	uint8_t  spectrum_db[RFA_C2_BINS];
	bool     spectrum_ready;
	bool     spectrum_invalid;
	uint32_t wf_since_transform;
} st;

static K_MUTEX_DEFINE(lock);

int rfa_sampler_init(void)
{
	int err = rfa_lis2dh12_init();

	if (err < 0) {
		return err;
	}
	memset(&st, 0, sizeof(st));
	LOG_INF("sampler ready: %u Hz raw -> %.0f Hz analysis, %u-point transform",
		rfa_lis2dh12_nominal_hz(), (double)RFA_SPECTRUM_HZ, RFA_WF_N);
	return 0;
}

/* One raw sample through the DC tracker, the stream ring and the decimator.
 * Called only from rfa_sampler_poll(), with the lock held. */
static void ingest(const int16_t mg[3])
{
	if (!st.dc_primed) {
		for (int i = 0; i < 3; i++) {
			st.dc_q[i] = (int32_t)mg[i] << DC_SHIFT;
		}
		st.dc_primed = true;
	}

	int16_t ac_mg[3];

	for (int i = 0; i < 3; i++) {
		int32_t dc = st.dc_q[i] >> DC_SHIFT;
		int32_t ac = (int32_t)mg[i] - dc;

		st.dc_q[i] += ac;   /* EMA: dc += (x - dc) >> shift */
		ac_mg[i] = (int16_t)(ac > 32767 ? 32767 : (ac < -32768 ? -32768 : ac));
		st.s1_acc[i] += ac * 1000;   /* to microg for the transform */
	}

	/* --- raw ring, 400 Hz, all three axes --- */
	struct rfa_sample *slot = &st.ring[st.write_idx % RFA_STREAM_RING];

	memcpy(slot->mg, ac_mg, sizeof(ac_mg));
	st.write_idx++;
	if (st.write_idx - st.read_idx > RFA_STREAM_RING) {
		/* The consumer fell behind and we have just overwritten samples it
		 * never saw. Move its cursor to the oldest sample still present and
		 * flag it; the index arithmetic on the host then shows the gap at
		 * its true width. */
		st.read_idx = st.write_idx - RFA_STREAM_RING;
		st.stream_gap = true;
	}

	if (++st.s1_count < RFA_DEC_SPECTRUM) {
		return;
	}
	st.s1_count = 0;

	/* --- 100 Hz stage: the broadcast spectrum, 0-50 Hz ---
	 *
	 * Z only. The broadcast channel has one byte per bin and no room for
	 * three axes; Z is the gravity-normal axis on a flat-mounted tag, which
	 * is where out-of-plane vibration shows up. The GATT stream carries all
	 * three and is what you use if the axis matters.
	 */
	int32_t s1[3];

	for (int i = 0; i < 3; i++) {
		s1[i] = st.s1_acc[i] / (int32_t)RFA_DEC_SPECTRUM;
		st.s1_acc[i] = 0;
	}

	st.wf_ring[st.wf_head] = (float)s1[2];
	st.wf_head = (st.wf_head + 1) % RFA_WF_N;
	if (st.wf_fill < RFA_WF_N) {
		st.wf_fill++;
	}
	st.wf_since_transform++;
}

void rfa_sampler_poll(void)
{
	static struct rfa_fifo_batch batch;
	int n = rfa_lis2dh12_read(&batch);

	if (n <= 0) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (batch.overrun) {
		/* S-5: a gap of unknown length sits somewhere in this window, so a
		 * transform over it is wrong. Flag rather than analyse. The raw
		 * stream carries its own gap signalling through the indices. */
		st.wf_overrun = true;
		st.stream_gap = true;
	}
	for (uint8_t i = 0; i < batch.count; i++) {
		ingest(batch.samples[i].mg);
	}

	/*
	 * Recompute the broadcast spectrum once a full window has accumulated
	 * since the last one. RFA_WF_HOP of 128 against a 256-point window is
	 * 50% overlap while streaming continuously.
	 *
	 * In a duty-cycled burst the ring was cleared on entry, so the first
	 * transform only becomes possible once RFA_WF_N fresh samples have
	 * arrived - 2.56 s at the 100 Hz stage. That is what ends the burst.
	 */
	bool produced = false;

	if (st.wf_fill >= RFA_WF_N && st.wf_since_transform >= RFA_WF_HOP) {
		static float wf[RFA_WF_N];

		st.wf_since_transform = 0;
		for (int i = 0; i < RFA_WF_N; i++) {
			wf[i] = st.wf_ring[(st.wf_head + i) % RFA_WF_N];
		}
		rfa_spectrum_bins(wf, RFA_WF_N, st.spectrum_db, RFA_C2_BINS);
		st.spectrum_ready = true;
		st.spectrum_invalid = st.wf_overrun;
		st.wf_overrun = false;
		produced = true;
	}
	k_mutex_unlock(&lock);

	/* Outside the lock: rfa_power_burst_complete() takes its own, and
	 * holding both in one order here and the other order elsewhere is how
	 * deadlocks are built. */
	if (produced) {
		rfa_power_burst_complete();
	}
}

void rfa_sampler_discard(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	/* A burst starts from silence, not from whatever the low-power stage
	 * dribbled in at 10 Hz. Those samples are 8-bit and 40x too slow; left
	 * in the ring they would be transformed as if they were 100 Hz data. */
	st.wf_fill = 0;
	st.wf_head = 0;
	st.wf_since_transform = 0;
	st.wf_overrun = false;
	st.s1_count = 0;
	memset(st.s1_acc, 0, sizeof(st.s1_acc));
	k_mutex_unlock(&lock);
}

void rfa_sampler_dc(int16_t out_mg[3])
{
	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < 3; i++) {
		int32_t v = st.dc_primed ? (st.dc_q[i] >> DC_SHIFT) : 0;

		out_mg[i] = (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
	}
	k_mutex_unlock(&lock);
}

size_t rfa_sampler_stream_read(struct rfa_sample *out, size_t max,
			       uint32_t *first_index, bool *gap)
{
	size_t n = 0;

	k_mutex_lock(&lock, K_FOREVER);

	*first_index = st.read_idx;
	*gap = st.stream_gap;
	st.stream_gap = false;

	while (n < max && st.read_idx != st.write_idx) {
		out[n++] = st.ring[st.read_idx % RFA_STREAM_RING];
		st.read_idx++;
	}

	k_mutex_unlock(&lock);
	return n;
}

void rfa_sampler_stream_reset(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	st.read_idx = st.write_idx;
	st.stream_gap = false;
	k_mutex_unlock(&lock);
}

bool rfa_sampler_spectrum(uint8_t out_db[RFA_C2_BINS], bool *invalid)
{
	bool ready;

	k_mutex_lock(&lock, K_FOREVER);
	ready = st.spectrum_ready;
	if (ready) {
		memcpy(out_db, st.spectrum_db, RFA_C2_BINS);
		*invalid = st.spectrum_invalid;
	}
	k_mutex_unlock(&lock);
	return ready;
}

uint32_t rfa_sampler_measured_mhz(void)
{
	return rfa_lis2dh12_measured_mhz();
}
