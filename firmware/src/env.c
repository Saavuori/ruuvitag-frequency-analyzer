/*
 * BME280 temperature / humidity / pressure.
 *
 * The upstream RuuviTag board devicetree instantiates a BME280. Newer tag
 * revisions ship SHTC3 + DPS310 instead; that is TODO(hw-2) in
 * docs/01-hardware.md and is a devicetree-overlay change, not a code change,
 * because everything below goes through the Zephyr sensor API.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "env.h"

LOG_MODULE_REGISTER(rfa_env, LOG_LEVEL_INF);

#define ENV_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bme280)

#if DT_NODE_HAS_STATUS(ENV_NODE, okay)
static const struct device *const env_dev = DEVICE_DT_GET(ENV_NODE);
#else
static const struct device *const env_dev;
#endif

int rfa_env_init(void)
{
	if (env_dev == NULL || !device_is_ready(env_dev)) {
		LOG_WRN("environmental sensor unavailable; DF5 will carry invalid codes");
		return -ENODEV;
	}
	LOG_INF("environmental sensor ready");
	return 0;
}

void rfa_env_read(struct rfa_env *out)
{
	struct sensor_value t, h, p;

	memset(out, 0, sizeof(*out));

	if (env_dev == NULL || !device_is_ready(env_dev)) {
		return;
	}
	if (sensor_sample_fetch(env_dev) < 0) {
		return;
	}
	if (sensor_channel_get(env_dev, SENSOR_CHAN_AMBIENT_TEMP, &t) < 0 ||
	    sensor_channel_get(env_dev, SENSOR_CHAN_HUMIDITY, &h) < 0 ||
	    sensor_channel_get(env_dev, SENSOR_CHAN_PRESS, &p) < 0) {
		return;
	}

	out->temperature_mc = t.val1 * 1000 + t.val2 / 1000;
	out->humidity_mpct = h.val1 * 1000 + h.val2 / 1000;
	/* Zephyr reports pressure in kilopascal. */
	out->pressure_pa = p.val1 * 1000 + p.val2 / 1000;
	out->valid = true;
}
