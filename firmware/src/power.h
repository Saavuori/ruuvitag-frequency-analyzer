/*
 * Duty cycling: what the tag does when nobody is watching.
 *
 * Running the accelerometer at 400 Hz and waking the CPU 25 times a second is
 * correct while a host is streaming and pure waste the rest of the time. The
 * band needs 400 Hz (ADR-0001), so the answer is not a slower sensor - it is
 * the same sensor, run less often.
 *
 *   IDLE    low-power 10 Hz, activity interrupt armed, CPU asleep
 *   BURST   400 Hz for one transform window, then back to IDLE
 *   ACTIVE  400 Hz continuous, for the GATT stream
 *
 * IDLE -> BURST on the heartbeat timer or on motion. BURST -> IDLE once the
 * spectrum is queued. ACTIVE is entered when a host starts streaming and left
 * when it disconnects.
 *
 * Advertising is deliberately *not* part of this. One connectable mode runs
 * throughout, re-asserted every slot by main.c; ADR-0004 records two designs
 * that switched advertising state on events and left the tag permanently
 * silent. Only the interval is tuned, and only at startup.
 */

#ifndef RFA_POWER_H
#define RFA_POWER_H

#include <stdbool.h>
#include <stdint.h>

enum rfa_power_mode {
	RFA_MODE_IDLE = 0,
	RFA_MODE_BURST,
	RFA_MODE_ACTIVE,
};

/* Accumulated occupancy, so the power figure is a model over measured duty
 * cycles rather than an estimate. Reported over GATT; see docs/08-power.md. */
struct rfa_power_stats {
	uint32_t uptime_s;
	uint32_t idle_ms;
	uint32_t burst_ms;
	uint32_t active_ms;
	uint16_t bursts;
	uint16_t motion_events;
};

int rfa_power_init(void);

enum rfa_power_mode rfa_power_mode(void);

/* Called by the GATT layer when a host starts or stops streaming. */
void rfa_power_set_streaming(bool streaming);

/* True while the sensor should be sampled. The sampler thread polls the FIFO
 * at 40 ms when this holds and sleeps long when it does not. */
bool rfa_power_sampling(void);

/* The sampler calls this once a burst has collected a full transform window,
 * which is what ends the burst. */
void rfa_power_burst_complete(void);

void rfa_power_stats(struct rfa_power_stats *out);

/*
 * Re-arm the activity interrupt at a new threshold, in mg. Not persisted; a
 * reboot returns to CONFIG_RFA_MOTION_THRESHOLD_MG. Returns -ENOTSUP when
 * wake-on-motion is compiled out.
 */
int rfa_power_set_motion_threshold(uint16_t mg);

/* Motion events since boot, saturating at 255. DF5's movement counter carries
 * this - which is what that field means in Ruuvi's ecosystem. */
uint8_t rfa_power_movement_counter(void);

#endif /* RFA_POWER_H */
