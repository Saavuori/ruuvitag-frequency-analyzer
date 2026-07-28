/*
 * Mode state machine. See power.h for the shape of it.
 *
 * Nothing here touches the sensor from interrupt context. The heartbeat timer
 * and the INT1 pin both run in ISRs, and switching the accelerometer's mode is
 * a series of SPI writes - so both only submit work, and every register access
 * happens on the system workqueue. Zephyr runs k_timer expiry functions in the
 * clock ISR, which makes this an easy mistake to make and a hard one to find.
 */

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lis2dh12.h"
#include "power.h"
#include "sampler.h"

LOG_MODULE_REGISTER(rfa_power, LOG_LEVEL_INF);

#define ACCEL_NODE DT_NODELABEL(lis2dh12)

/*
 * A burst must outlast one transform window plus the sensor's turn-on
 * transient. The window is 2.56 s of decimated samples; the rest covers ODR
 * settling and FIFO poll granularity. rfa_power_burst_complete() normally ends
 * a burst well inside this - the timeout is a backstop against a sensor that
 * stopped delivering, so a stuck burst cannot sit at 400 Hz until the cell is
 * flat.
 */
#define BURST_TIMEOUT_MS 4500

static struct {
	enum rfa_power_mode mode;
	uint32_t mode_since_ms;
	uint32_t boot_ms;

	uint32_t idle_ms;
	uint32_t burst_ms;
	uint32_t active_ms;
	uint16_t bursts;
	uint16_t motion_events;

	bool streaming;
	struct gpio_callback int1_cb;
} pw;

static K_MUTEX_DEFINE(lock);

static void begin_burst_fn(struct k_work *w);
static void burst_timeout_fn(struct k_work *w);
static void streaming_fn(struct k_work *w);
static K_WORK_DEFINE(begin_burst, begin_burst_fn);
static K_WORK_DEFINE(streaming_change, streaming_fn);
static K_WORK_DELAYABLE_DEFINE(burst_timeout, burst_timeout_fn);

/* Set by the GATT layer, applied on the workqueue. */
static atomic_t want_streaming = ATOMIC_INIT(0);

/* Heartbeat: guarantees a spectrum even from something that never trips the
 * motion threshold, so silence means "nothing there" rather than "threshold set
 * too high". */
static void heartbeat_fn(struct k_timer *t);
static K_TIMER_DEFINE(heartbeat, heartbeat_fn, NULL);

#if IS_ENABLED(CONFIG_RFA_WAKE_ON_MOTION)
static const struct gpio_dt_spec int1 =
	GPIO_DT_SPEC_GET_BY_IDX(ACCEL_NODE, irq_gpios, 0);
#endif

static void account(uint32_t now)
{
	uint32_t dt = now - pw.mode_since_ms;

	switch (pw.mode) {
	case RFA_MODE_IDLE:   pw.idle_ms += dt; break;
	case RFA_MODE_BURST:  pw.burst_ms += dt; break;
	case RFA_MODE_ACTIVE: pw.active_ms += dt; break;
	}
	pw.mode_since_ms = now;
}

/* Caller holds the lock, and is on a thread - this does SPI. */
static void enter(enum rfa_power_mode mode)
{
	if (mode == pw.mode) {
		return;
	}
	account(k_uptime_get_32());

	switch (mode) {
	case RFA_MODE_IDLE:
		rfa_lis2dh12_set_mode(RFA_ACCEL_LOWPOWER);
#if IS_ENABLED(CONFIG_RFA_WAKE_ON_MOTION)
		/* Clear anything latched while we were busy, or the pin stays
		 * asserted and the next real event never produces an edge. */
		rfa_lis2dh12_clear_motion();
#endif
		break;

	case RFA_MODE_BURST:
	case RFA_MODE_ACTIVE:
		if (mode == RFA_MODE_BURST) {
			pw.bursts++;
		}
		/* Drop what the 10 Hz low-power stage left in the decimator before
		 * sampling resumes. Those samples are 8-bit and forty times too
		 * slow; transformed as if they were 100 Hz data they would put a
		 * fabricated low-frequency component into the spectrum. */
		rfa_sampler_discard();
		rfa_lis2dh12_set_mode(RFA_ACCEL_ACTIVE);
		break;
	}

	pw.mode = mode;
}

static void begin_burst_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	k_mutex_lock(&lock, K_FOREVER);
	if (pw.mode == RFA_MODE_IDLE) {
		enter(RFA_MODE_BURST);
		k_work_reschedule(&burst_timeout, K_MSEC(BURST_TIMEOUT_MS));
	}
	k_mutex_unlock(&lock);
}

static void burst_timeout_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	k_mutex_lock(&lock, K_FOREVER);
	if (pw.mode == RFA_MODE_BURST && !pw.streaming) {
		LOG_WRN("burst did not complete in %d ms; forcing idle", BURST_TIMEOUT_MS);
		enter(RFA_MODE_IDLE);
	}
	k_mutex_unlock(&lock);
}

/* ISR context. Submit and leave. */
static void heartbeat_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&begin_burst);
}

#if IS_ENABLED(CONFIG_RFA_WAKE_ON_MOTION)
static void int1_handler(const struct device *dev, struct gpio_callback *cb,
			 uint32_t pins)
{
	ARG_UNUSED(dev); ARG_UNUSED(cb); ARG_UNUSED(pins);

	/* ISR context: count it and submit. The SPI that clears the sensor's
	 * latch happens on the transition into BURST, from the workqueue. */
	pw.motion_events++;
	k_work_submit(&begin_burst);
}
#endif

int rfa_power_init(void)
{
	memset(&pw, 0, sizeof(pw));
	pw.boot_ms = k_uptime_get_32();
	pw.mode_since_ms = pw.boot_ms;
	/* Boot straight into a burst so the first spectrum does not wait out a
	 * whole idle period. rfa_lis2dh12_init() already left the sensor active. */
	pw.mode = RFA_MODE_BURST;
	pw.bursts = 1;
	k_work_reschedule(&burst_timeout, K_MSEC(BURST_TIMEOUT_MS));

#if IS_ENABLED(CONFIG_RFA_WAKE_ON_MOTION)
	if (gpio_is_ready_dt(&int1)) {
		int err = gpio_pin_configure_dt(&int1, GPIO_INPUT);

		if (err == 0) {
			err = gpio_pin_interrupt_configure_dt(&int1,
							      GPIO_INT_EDGE_TO_ACTIVE);
		}
		if (err == 0) {
			gpio_init_callback(&pw.int1_cb, int1_handler, BIT(int1.pin));
			gpio_add_callback(int1.port, &pw.int1_cb);
			rfa_lis2dh12_arm_motion(CONFIG_RFA_MOTION_THRESHOLD_MG,
						CONFIG_RFA_MOTION_DURATION);
			LOG_INF("wake-on-motion armed on INT1");
		} else {
			LOG_WRN("INT1 setup failed (%d); heartbeat only", err);
		}
	} else {
		LOG_WRN("INT1 GPIO not ready; heartbeat only");
	}
#endif

	k_timer_start(&heartbeat, K_SECONDS(CONFIG_RFA_IDLE_PERIOD_S),
		      K_SECONDS(CONFIG_RFA_IDLE_PERIOD_S));

	LOG_INF("power: idle period %d s, wake-on-motion %s",
		CONFIG_RFA_IDLE_PERIOD_S,
		IS_ENABLED(CONFIG_RFA_WAKE_ON_MOTION) ? "on" : "off");
	return 0;
}

enum rfa_power_mode rfa_power_mode(void)
{
	return pw.mode;
}

bool rfa_power_sampling(void)
{
	return pw.mode != RFA_MODE_IDLE;
}

/*
 * Applied here rather than in the caller because every caller is the Bluetooth
 * RX thread - a CCC write, a control write, a disconnect - and this path does
 * SPI. Blocking that thread on a sensor transaction is the same class of
 * mistake as the advertising restarts in ADR-0004, and the stream's first
 * samples depend on the ordering below.
 */
static void streaming_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	bool streaming = atomic_get(&want_streaming) != 0;

	k_mutex_lock(&lock, K_FOREVER);
	pw.streaming = streaming;
	if (streaming) {
		k_work_cancel_delayable(&burst_timeout);
		enter(RFA_MODE_ACTIVE);
		/*
		 * Only now is the sensor at 400 Hz. Resetting the stream cursor
		 * here rather than when the host wrote START is what stops the
		 * first notifications carrying 10 Hz low-power samples labelled
		 * as 400 Hz data - a forty-fold time-base error, and exactly the
		 * kind that looks like a real measurement.
		 */
		rfa_sampler_stream_reset();
	} else {
		enter(RFA_MODE_IDLE);
	}
	k_mutex_unlock(&lock);
}

void rfa_power_set_streaming(bool streaming)
{
	atomic_set(&want_streaming, streaming ? 1 : 0);
	k_work_submit(&streaming_change);
}

void rfa_power_burst_complete(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	/* A host may have started streaming mid-burst; do not drop it to idle. */
	if (pw.mode == RFA_MODE_BURST && !pw.streaming) {
		k_work_cancel_delayable(&burst_timeout);
		enter(RFA_MODE_IDLE);
	}
	k_mutex_unlock(&lock);
}

void rfa_power_stats(struct rfa_power_stats *out)
{
	k_mutex_lock(&lock, K_FOREVER);

	uint32_t now = k_uptime_get_32();

	account(now);   /* fold in time spent in the current mode so far */

	out->uptime_s = (now - pw.boot_ms) / 1000U;
	out->idle_ms = pw.idle_ms;
	out->burst_ms = pw.burst_ms;
	out->active_ms = pw.active_ms;
	out->bursts = pw.bursts;
	out->motion_events = pw.motion_events;

	k_mutex_unlock(&lock);
}

int rfa_power_set_motion_threshold(uint16_t mg)
{
#if IS_ENABLED(CONFIG_RFA_WAKE_ON_MOTION)
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = rfa_lis2dh12_arm_motion(mg, CONFIG_RFA_MOTION_DURATION);
	k_mutex_unlock(&lock);
	return err;
#else
	ARG_UNUSED(mg);
	return -ENOTSUP;
#endif
}

uint8_t rfa_power_movement_counter(void)
{
	uint16_t n = pw.motion_events;

	/* 255 is DF5's "invalid" code, so saturate one below it - a busy tag
	 * must not start claiming it has no movement counter. */
	return n > 254 ? 254 : (uint8_t)n;
}
