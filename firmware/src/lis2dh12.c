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
#define REG_CTRL3      0x22
#define REG_CTRL4      0x23
#define REG_CTRL5      0x24
#define REG_OUT_X_L    0x28
#define REG_FIFO_CTRL  0x2E
#define REG_FIFO_SRC   0x2F

#define WHO_AM_I_VALUE 0x33

/* SPI framing: bit7 read, bit6 auto-increment. */
#define SPI_READ       0x80
#define SPI_AUTOINC    0x40

/* CTRL_REG1: ODR in the top nibble, all three axes enabled, LPen clear. */
#define CTRL1_ODR_400HZ 0x70
#define CTRL1_AXES_EN   0x07

/* CTRL_REG4: BDU set (a reading cannot be torn across two samples), +/-2 g,
 * high resolution. Both of these were wrong by default in Zephyr's driver and
 * cost us a factor of two in noise floor. */
#define CTRL4_BDU       0x80
#define CTRL4_FS_2G     0x00
#define CTRL4_HR        0x08

#define CTRL5_FIFO_EN   0x40

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

static struct {
	uint32_t nominal_hz;
	uint64_t counted;         /* samples seen since the rate window opened */
	uint32_t window_start_ms;
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

	err = reg_write(REG_CTRL4, CTRL4_BDU | CTRL4_FS_2G | CTRL4_HR);
	if (err < 0) {
		return err;
	}
	err = reg_write(REG_CTRL1, CTRL1_ODR_400HZ | CTRL1_AXES_EN);
	if (err < 0) {
		return err;
	}
	err = reg_write(REG_CTRL5, CTRL5_FIFO_EN);
	if (err < 0) {
		return err;
	}
	err = reg_write(REG_FIFO_CTRL, FIFO_MODE_STREAM);
	if (err < 0) {
		return err;
	}

	st.nominal_hz = 400;
	st.counted = 0;
	st.window_start_ms = k_uptime_get_32();
	st.measured_mhz = 0;

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
	 */
	st.counted += n;
	uint32_t elapsed = k_uptime_get_32() - st.window_start_ms;

	if (elapsed >= 10000U) {
		st.measured_mhz = (uint32_t)((st.counted * 1000000ULL) / elapsed);
		st.counted = 0;
		st.window_start_ms = k_uptime_get_32();
	}

	if (out->overrun) {
		/* Re-arm: bypass then stream resets the write pointer, otherwise a
		 * full FIFO in stream mode keeps reporting overrun. */
		reg_write(REG_FIFO_CTRL, FIFO_MODE_BYPASS);
		reg_write(REG_FIFO_CTRL, FIFO_MODE_STREAM);
	}

	return n;
}
