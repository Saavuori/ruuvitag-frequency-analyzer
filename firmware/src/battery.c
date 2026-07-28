/*
 * Coin-cell voltage. See battery.h for why it is a warning and not a gauge.
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"

LOG_MODULE_REGISTER(rfa_battery, LOG_LEVEL_INF);

static const struct adc_dt_spec vdd =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

static bool ready;

int rfa_battery_init(void)
{
	int err;

	if (!adc_is_ready_dt(&vdd)) {
		LOG_WRN("ADC not ready; battery will report 'not measured'");
		return -ENODEV;
	}
	err = adc_channel_setup_dt(&vdd);
	if (err < 0) {
		LOG_WRN("ADC channel setup failed (%d)", err);
		return err;
	}
	ready = true;
	return 0;
}

uint16_t rfa_battery_millivolts(void)
{
	if (!ready) {
		return 0;
	}

	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	if (adc_sequence_init_dt(&vdd, &seq) < 0 || adc_read_dt(&vdd, &seq) < 0) {
		return 0;
	}

	/*
	 * adc_raw_to_millivolts_dt applies the gain and reference from the
	 * devicetree, so the 1/6 gain against the 0.6 V internal reference is
	 * already accounted for here and must not be applied again.
	 */
	int32_t mv = raw;

	if (adc_raw_to_millivolts_dt(&vdd, &mv) < 0) {
		return 0;
	}
	if (mv < 0) {
		/* The SAADC is signed and reads slightly negative near zero. A
		 * negative supply is not a measurement. */
		return 0;
	}
	if (mv > UINT16_MAX) {
		mv = UINT16_MAX;
	}
	return (uint16_t)mv;
}
