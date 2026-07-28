/*
 * Direct LIS2DH12 driver with FIFO ingest.
 *
 * Zephyr's lis2dh driver has no FIFO support - it exposes one sample at a time
 * through the sensor API. That forced us to poll, and polling a free-running
 * ODR from a thread the BLE stack preempts caused four measured problems:
 *
 *   S-1  noise floor inflated by sampling jitter
 *   S-3  no control over what aliases into band
 *   S-5  FIFO overruns undetectable, because no FIFO was used
 *   S-6  effective sample rate assumed rather than measured
 *
 * plus a self-inflicted spectral artifact at the advertising cadence and its
 * harmonics, sitting inside the 0.5-2 Hz band the FFT exists to measure.
 *
 * With the FIFO, samples are timestamped by the sensor's own clock. Our read
 * timing stops mattering: late reads cost buffer depth, not sample spacing.
 * That is what removes the artifact rather than merely attenuating it.
 *
 * The Zephyr driver is disabled (CONFIG_LIS2DH=n) and we take the same
 * devicetree node for its bus, chip select and interrupt pins.
 */

#ifndef RFA_LIS2DH12_H
#define RFA_LIS2DH12_H

#include <stdbool.h>
#include <stdint.h>

/* 32 slots x 3 axes, the whole hardware FIFO. */
#define RFA_FIFO_DEPTH 32

struct rfa_sample {
	int16_t mg[3];
};

struct rfa_fifo_batch {
	struct rfa_sample samples[RFA_FIFO_DEPTH];
	uint8_t count;
	bool overrun;        /* S-5: samples were lost before we read them */
};

int rfa_lis2dh12_init(void);

/* Drain whatever the FIFO holds. Returns samples read, or negative on error. */
int rfa_lis2dh12_read(struct rfa_fifo_batch *out);

/*
 * Measured output data rate in milli-Hz, or 0 before enough samples have been
 * counted. The ODR comes from an internal RC oscillator that is off by a few
 * percent and drifts with temperature, so every frequency we report has to be
 * scaled by this rather than by the nominal rate (S-6).
 */
uint32_t rfa_lis2dh12_measured_mhz(void);

/* Nominal configured rate, for comparison and for startup before the measured
 * rate is available. */
uint32_t rfa_lis2dh12_nominal_hz(void);

#endif /* RFA_LIS2DH12_H */
