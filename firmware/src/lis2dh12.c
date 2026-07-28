/*
 * LIS2DH12 over SPI, in FIFO stream mode. See lis2dh12.h for why this exists
 * rather than using Zephyr's driver.
 */

#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lis2dh12.h"

LOG_MODULE_REGISTER(rfa_lis2dh12, LOG_LEVEL_INF);

#define ACCEL_NODE DT_NODELABEL(lis2dh12)

/* Registers */
#define REG_WHO_AM_I   0x0F
#define REG_CTRL1      0x20
#define REG_CTRL2      0x21
#define REG_CTRL3      0x22
#define REG_CTRL4      0x23
#define REG_CTRL5      0x24
#define REG_REFERENCE  0x26
#define REG_OUT_X_L    0x28
#define REG_FIFO_CTRL  0x2E
#define REG_FIFO_SRC   0x2F
#define REG_INT1_CFG   0x30
#define REG_INT1_SRC   0x31
#define REG_INT1_THS   0x32
#define REG_INT1_DUR   0x33

#define WHO_AM_I_VALUE 0x33

/* SPI framing: bit7 read, bit6 auto-increment. */
#define SPI_READ       0x80
#define SPI_AUTOINC    0x40

/* CTRL_REG1: ODR in the top nibble, all three axes enabled, LPen clear. */
#define CTRL1_ODR_POWERDOWN 0x00
#define CTRL1_ODR_10HZ  0x20
#define CTRL1_ODR_400HZ 0x70
#define CTRL1_LPEN      0x08
#define CTRL1_AXES_EN   0x07

/* CTRL_REG2: route the high-pass filter to the interrupt generator only.
 * HPCF normal mode, cutoff from the ODR. Without this the interrupt threshold
 * competes with 1000 mg of gravity and never triggers on anything sane. */
#define CTRL2_HPIS1     0x01
#define CTRL2_HPM_NORMAL 0x00

/* CTRL_REG3: route the INT1 activity interrupt to the INT1 pin. */
#define CTRL3_I1_IA1    0x40

/* INT1_CFG: OR of the high events on all three axes - motion on any axis. */
#define INT1_CFG_OR_HIGH 0x2A

/* CTRL_REG4: BDU set (a reading cannot be torn across two samples), +/-2 g,
 * high resolution. Both of these were wrong by default in Zephyr's driver and
 * cost us a factor of two in noise floor. */
#define CTRL4_BDU       0x80
#define CTRL4_FS_2G     0x00
#define CTRL4_HR        0x08

#define CTRL5_FIFO_EN   0x40
/* Latch INT1 until INT1_SRC is read. Without this the pin follows the
 * threshold condition and chatters across it, so a single passing lorry
 * produces a burst of edges instead of one event. */
#define CTRL5_LIR_INT1  0x08

/* FIFO_CTRL_REG: stream mode keeps the newest samples and overwrites the
 * oldest, which is the right failure mode - a late read loses history, never
 * the present. */
#define FIFO_MODE_BYPASS 0x00
#define FIFO_MODE_STREAM 0x80

#define FIFO_SRC_OVRN   0x40
#define FIFO_SRC_FSS     0x1F

/* The trailing 0 is the deprecated CS delay argument; Zephyr takes the real
 * value from the devicetree, and the macro still requires the parameter. */
static const struct spi_dt_spec bus = SPI_DT_SPEC_GET(
	ACCEL_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER, 0);

/*
 * Publish a measured rate only after this much *active* sampling. Ten seconds
 * is four bursts at the default idle cadence, which is enough for the count to
 * be dominated by real samples rather than by where the window happened to open
 * and close relative to a burst boundary.
 */
#define RATE_WINDOW_MS 10000U

static struct {
	uint32_t nominal_hz;
	enum rfa_accel_mode mode;

	/* Rate measurement. active_ms accumulates only while ACTIVE, so an idle
	 * tag does not dilute the window - see the header. */
	uint64_t counted;
	uint32_t active_ms;
	uint32_t last_read_ms;
	uint32_t measured_mhz;
} st;

static int reg_write(uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { reg, val };
	struct spi_buf b = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set set = { .buffers = &b, .count = 1 };

	return spi_write_dt(&bus, &set);
}

static int reg_read(uint8_t reg, uint8_t *val, size_t len)
{
	uint8_t cmd = reg | SPI_READ | (len > 1 ? SPI_AUTOINC : 0);
	struct spi_buf tx_b = { .buf = &cmd, .len = 1 };
	struct spi_buf_set tx = { .buffers = &tx_b, .count = 1 };
	struct spi_buf rx_b[2] = {
		{ .buf = NULL, .len = 1 },   /* discard the byte clocked out with cmd */
		{ .buf = val, .len = len },
	};
	struct spi_buf_set rx = { .buffers = rx_b, .count = 2 };

	return spi_transceive_dt(&bus, &tx, &rx);
}

uint32_t rfa_lis2dh12_nominal_hz(void)
{
	return st.nominal_hz;
}

uint32_t rfa_lis2dh12_measured_mhz(void)
{
	return st.measured_mhz;
}

enum rfa_accel_mode rfa_lis2dh12_mode(void)
{
	return st.mode;
}

int rfa_lis2dh12_set_mode(enum rfa_accel_mode mode)
{
	int err;

	if (mode == st.mode) {
		return 0;
	}

	switch (mode) {
	case RFA_ACCEL_OFF:
		err = reg_write(REG_CTRL1, CTRL1_ODR_POWERDOWN);
		break;

	case RFA_ACCEL_LOWPOWER:
		/* 8-bit at 10 Hz. Nothing reads these samples; the mode exists so
		 * the activity interrupt stays armed for microamps. HR must be
		 * cleared before LPen is set - the two are mutually exclusive and
		 * setting both leaves the part in an undefined resolution. */
		err = reg_write(REG_CTRL4, CTRL4_BDU | CTRL4_FS_2G);
		if (err < 0) {
			break;
		}
		err = reg_write(REG_CTRL1, CTRL1_ODR_10HZ | CTRL1_LPEN | CTRL1_AXES_EN);
		break;

	case RFA_ACCEL_ACTIVE:
		err = reg_write(REG_CTRL4, CTRL4_BDU | CTRL4_FS_2G | CTRL4_HR);
		if (err < 0) {
			break;
		}
		err = reg_write(REG_CTRL1, CTRL1_ODR_400HZ | CTRL1_AXES_EN);
		if (err < 0) {
			break;
		}
		/* Re-arm the FIFO: bypass then stream resets the write pointer, so
		 * a burst starts on a clean buffer rather than on whatever the
		 * low-power mode dribbled in. */
		err = reg_write(REG_FIFO_CTRL, FIFO_MODE_BYPASS);
		if (err < 0) {
			break;
		}
		err = reg_write(REG_FIFO_CTRL, FIFO_MODE_STREAM);
		break;

	default:
		return -EINVAL;
	}

	if (err < 0) {
		LOG_WRN("mode change to %d failed (%d)", mode, err);
		return err;
	}

	st.mode = mode;
	if (mode == RFA_ACCEL_ACTIVE) {
		/* Open the rate window at the moment sampling starts. The counts
		 * carry over from previous bursts - it is elapsed *active* time
		 * that must not include the idle gap between them. */
		st.last_read_ms = k_uptime_get_32();
	}
	return 0;
}

int rfa_lis2dh12_arm_motion(uint16_t threshold_mg, uint8_t duration_samples)
{
	uint8_t dummy;
	int err;

	/* High-pass filter onto the interrupt path only. Without it the
	 * threshold competes with ~1000 mg of gravity and either never fires or
	 * fires permanently, depending on which way up the tag is. */
	err = reg_write(REG_CTRL2, CTRL2_HPM_NORMAL | CTRL2_HPIS1);
	if (err < 0) {
		return err;
	}
	/* Reading REFERENCE resets the filter, otherwise it carries whatever DC
	 * was present when it was enabled. */
	(void)reg_read(REG_REFERENCE, &dummy, 1);

	/* 16 mg per LSB at +/-2 g; clamp rather than wrap, since a wrapped
	 * threshold is a plausible wrong number. */
	uint32_t ths = threshold_mg / 16U;

	if (ths > 0x7F) {
		ths = 0x7F;
	}
	if (ths == 0) {
		ths = 1;
	}

	err = reg_write(REG_INT1_THS, (uint8_t)ths);
	if (err < 0) {
		return err;
	}
	err = reg_write(REG_INT1_DUR, duration_samples);
	if (err < 0) {
		return err;
	}
	err = reg_write(REG_CTRL3, CTRL3_I1_IA1);
	if (err < 0) {
		return err;
	}
	/* CFG last: writing it enables the generator, so everything it depends
	 * on is already in place. */
	err = reg_write(REG_INT1_CFG, INT1_CFG_OR_HIGH);
	if (err < 0) {
		return err;
	}

	LOG_INF("motion interrupt armed at %u mg (%u LSB), duration %u",
		threshold_mg, (unsigned)ths, duration_samples);
	return 0;
}

void rfa_lis2dh12_clear_motion(void)
{
	uint8_t src;

	/* Reading INT1_SRC is what releases a latched interrupt. */
	(void)reg_read(REG_INT1_SRC, &src, 1);
}

int rfa_lis2dh12_init(void)
{
	uint8_t who = 0;
	int err;

	if (!spi_is_ready_dt(&bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	err = reg_read(REG_WHO_AM_I, &who, 1);
	if (err < 0) {
		LOG_ERR("WHO_AM_I read failed (%d)", err);
		return err;
	}
	if (who != WHO_AM_I_VALUE) {
		LOG_ERR("WHO_AM_I is 0x%02X, expected 0x%02X", who, WHO_AM_I_VALUE);
		return -ENODEV;
	}

	/* Clear the FIFO before configuring: bypass then stream is the sequence
	 * the datasheet requires to reset the write pointer. */
	err = reg_write(REG_FIFO_CTRL, FIFO_MODE_BYPASS);
	if (err < 0) {
		return err;
	}
	err = reg_write(REG_CTRL5, CTRL5_FIFO_EN | CTRL5_LIR_INT1);
	if (err < 0) {
		return err;
	}
	err = reg_write(REG_FIFO_CTRL, FIFO_MODE_STREAM);
	if (err < 0) {
		return err;
	}

	st.nominal_hz = 400;
	st.counted = 0;
	st.active_ms = 0;
	st.measured_mhz = 0;
	st.mode = RFA_ACCEL_OFF;

	err = rfa_lis2dh12_set_mode(RFA_ACCEL_ACTIVE);
	if (err < 0) {
		return err;
	}

	LOG_INF("LIS2DH12 ready: 400 Hz, high-res, BDU, FIFO stream mode");
	return 0;
}

int rfa_lis2dh12_read(struct rfa_fifo_batch *out)
{
	uint8_t src = 0;
	int err;

	out->count = 0;
	out->overrun = false;

	err = reg_read(REG_FIFO_SRC, &src, 1);
	if (err < 0) {
		return err;
	}

	out->overrun = (src & FIFO_SRC_OVRN) != 0;
	uint8_t n = src & FIFO_SRC_FSS;

	if (out->overrun) {
		/* S-5: samples were lost. The epoch built from this batch must be
		 * flagged rather than analysed - a gap of unknown length in the
		 * middle of a window makes both RMS and any spectrum wrong. */
		n = RFA_FIFO_DEPTH;
	}
	if (n == 0) {
		return 0;
	}
	if (n > RFA_FIFO_DEPTH) {
		n = RFA_FIFO_DEPTH;
	}

	/* One burst: auto-increment wraps within the OUT registers and pops a
	 * FIFO slot every six bytes. */
	static uint8_t raw[RFA_FIFO_DEPTH * 6];

	err = reg_read(REG_OUT_X_L, raw, (size_t)n * 6);
	if (err < 0) {
		return err;
	}

	for (uint8_t i = 0; i < n; i++) {
		const uint8_t *p = &raw[i * 6];

		for (int a = 0; a < 3; a++) {
			int16_t v = (int16_t)((uint16_t)p[a * 2] | ((uint16_t)p[a * 2 + 1] << 8));

			/* 12-bit left-justified in high-resolution mode: shift by 4,
			 * and at +/-2 g one LSB is 1 mg. Getting this shift wrong is
			 * the classic LIS2DH12 bug - it yields a plausible signal
			 * that is silently 16x too large (docs/01-hardware.md). */
			out->samples[i].mg[a] = (int16_t)(v >> 4);
		}
	}
	out->count = n;

	/*
	 * S-6: measure the real rate. The ODR is derived from an internal RC
	 * oscillator, so the nominal 400 Hz is only approximate and drifts with
	 * temperature. Counting delivered samples against the RTC gives the
	 * number every frequency we report must be scaled by.
	 *
	 * Elapsed time accumulates only across intervals when the sensor was
	 * ACTIVE. Using wall-clock instead would divide a burst's worth of
	 * samples by a whole idle period and report ~17 Hz for a 400 Hz sensor -
	 * a twentyfold error on every frequency, arrived at silently.
	 */
	if (st.mode == RFA_ACCEL_ACTIVE) {
		uint32_t now = k_uptime_get_32();

		st.counted += n;
		st.active_ms += now - st.last_read_ms;
		st.last_read_ms = now;

		if (st.active_ms >= RATE_WINDOW_MS) {
			st.measured_mhz =
				(uint32_t)((st.counted * 1000000ULL) / st.active_ms);
			st.counted = 0;
			st.active_ms = 0;
		}
	}

	if (out->overrun) {
		/* Re-arm: bypass then stream resets the write pointer, otherwise a
		 * full FIFO in stream mode keeps reporting overrun. */
		reg_write(REG_FIFO_CTRL, FIFO_MODE_BYPASS);
		reg_write(REG_FIFO_CTRL, FIFO_MODE_STREAM);
	}

	return n;
}
