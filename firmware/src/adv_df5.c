/*
 * Ruuvi Data Format 5 (RAWv2) encoder.
 *
 * Emitted unmodified so Ruuvi Station, Ruuvi Gateway and the Home Assistant
 * integration keep working (goal G1). Big-endian throughout - DF5 is
 * network-order, unlike almost everything else on this chip.
 */

#include "adv.h"

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xFF);
}

static void put_i16(uint8_t *p, int16_t v)
{
	put_u16(p, (uint16_t)v);
}

void rfa_encode_df5(const struct rfa_df5_fields *f, uint8_t out[RFA_ADV_PAYLOAD_LEN])
{
	out[0] = RFA_DF5_FORMAT;
	put_i16(&out[1], f->temperature);
	put_u16(&out[3], f->humidity);
	put_u16(&out[5], f->pressure);
	put_i16(&out[7], f->accel_mg[0]);
	put_i16(&out[9], f->accel_mg[1]);
	put_i16(&out[11], f->accel_mg[2]);
	put_u16(&out[13], rfa_power_field(f->battery_mv, f->tx_power_dbm));
	out[15] = f->movement_counter;
	put_u16(&out[16], f->sequence);
	for (int i = 0; i < 6; i++) {
		out[18 + i] = f->mac[i];
	}
}

int16_t rfa_temp_from_millicelsius(int32_t millicelsius)
{
	/* 0.005 degC/LSB: value = mC / 5. Range +-163.835 degC, far outside
	 * anything a nursery produces, so clamping is a safety net only. */
	int32_t v = millicelsius / 5;

	if (v > 32767) {
		return 32767;
	}
	if (v < -32767) {
		return -32767;
	}
	return (int16_t)v;
}

uint16_t rfa_humidity_from_millipercent(int32_t millipercent)
{
	/* 0.0025 %/LSB: value = m% / 2.5 = m% * 2 / 5. */
	int32_t v = (millipercent * 2) / 5;

	if (v < 0) {
		return 0;
	}
	if (v > 40000) {   /* 100.00 % */
		return 40000;
	}
	return (uint16_t)v;
}

uint16_t rfa_pressure_from_pascal(int32_t pascal)
{
	/* 1 Pa/LSB with a -50000 Pa offset. Below 50 kPa the format simply
	 * cannot represent the value; report the floor, not a wrapped number. */
	int32_t v = pascal - 50000;

	if (v < 0) {
		return 0;
	}
	if (v > 65534) {
		return 65534;
	}
	return (uint16_t)v;
}

uint16_t rfa_power_field(uint16_t battery_mv, int8_t tx_power_dbm)
{
	/* 11 bits of millivolts above 1600 mV, then 5 bits of TX power as
	 * (dBm + 40) / 2. Both saturate to their invalid code on overflow. */
	uint32_t volt;
	uint32_t tx;

	if (battery_mv == RFA_BATTERY_NOT_MEASURED) {
		/* Emit DF5's invalid code rather than a plausible number. A
		 * fabricated battery reading gets plotted and believed. */
		volt = 2047;
	} else if (battery_mv < 1600) {
		volt = 0;
	} else {
		volt = (uint32_t)(battery_mv - 1600);
		if (volt > 2046) {
			volt = 2046;
		}
	}

	if (tx_power_dbm < -40) {
		tx = 0;
	} else {
		tx = (uint32_t)((tx_power_dbm + 40) / 2);
		if (tx > 30) {
			tx = 30;
		}
	}

	return (uint16_t)((volt << 5) | tx);
}
