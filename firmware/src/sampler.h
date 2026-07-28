/*
 * Acquisition: FIFO ingest, DC tracking, and the two consumers that hang off it.
 *
 * One acquisition path feeds two very different transports:
 *
 *   raw ring, 400 Hz    -> GATT stream -> host does the STFT   (ADR-0003)
 *   100 Hz decimated    -> on-tag FFT  -> 0xC2 broadcast       (ADR-0002)
 *
 * The raw ring is the product. The on-tag spectrum exists so a tag nobody is
 * connected to still says something useful, not because computing an FFT on a
 * Cortex-M4 is a good idea when a laptop is three metres away.
 *
 * There is no scoring, no thresholds and no classification here. This module
 * reports what the sensor measured; deciding what it means is the analyst's
 * job and happens on the host where it can be changed without a reflash.
 */

#ifndef RFA_SAMPLER_H
#define RFA_SAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lis2dh12.h"

/*
 * Raw ring depth in samples. At 400 Hz this is 1.28 s of slack between the
 * sampler thread and the GATT thread. The connection interval is at most
 * 30 ms and a notification carries ~39 samples, so this is roughly 13x the
 * buffer a healthy link needs - the margin is there for the stalls that happen
 * when the host is busy, not for steady-state operation.
 */
#define RFA_STREAM_RING 512

/* Analysis-band decimation: 400 Hz raw / 4 = 100 Hz, Nyquist 50 Hz. */
#define RFA_DEC_SPECTRUM 4
#define RFA_SPECTRUM_HZ  (400.0f / (float)RFA_DEC_SPECTRUM)

int rfa_sampler_init(void);

/* Drain the hardware FIFO into both rings. Called from the sampler thread. */
void rfa_sampler_poll(void);

/* Latest DC (gravity) vector in mg. Safe from any thread; DF5 needs it on
 * every advertisement. */
void rfa_sampler_dc(int16_t out_mg[3]);

/*
 * Copy up to `max` raw samples out of the stream ring.
 *
 * Returns the number written. `*first_index` receives the monotonic index of
 * the first sample copied, so a host that loses a notification can tell how
 * many samples went missing rather than silently splicing the gap shut - a
 * spliced gap is a frequency error, and a frequency error is the one thing this
 * instrument must not invent.
 *
 * `*gap` is set when the consumer fell behind far enough that the ring
 * overwrote unread samples. The indices still tell the host exactly how many.
 */
size_t rfa_sampler_stream_read(struct rfa_sample *out, size_t max,
			       uint32_t *first_index, bool *gap);

/* Discard anything buffered and restart indexing. Called when a host subscribes
 * so it does not receive a second of history it did not ask for. */
void rfa_sampler_stream_reset(void);

/*
 * Latest broadcast spectrum, already in the 0xC2 dB encoding.
 *
 * Returns false when no complete transform window has accumulated yet, which is
 * the honest answer for the first 2.56 s after boot. `*invalid` marks a window
 * that contained a FIFO overrun: the samples either side of the gap are fine
 * individually but the transform over them is not.
 */
bool rfa_sampler_spectrum(uint8_t out_db[128], bool *invalid);

/* Measured ODR in milli-Hz, 0 before enough samples have been counted. Every
 * frequency the host reports is scaled by this rather than by the nominal 400
 * (S-6): the LIS2DH12's oscillator is an RC, several percent off and drifting. */
uint32_t rfa_sampler_measured_mhz(void);

#endif /* RFA_SAMPLER_H */
