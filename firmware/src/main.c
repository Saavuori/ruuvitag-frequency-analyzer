/*
 * RuuviTag frequency analyzer - firmware entry point.
 *
 * Two transports, deliberately different in kind (docs/05-ble-protocol.md):
 *
 *   broadcast  DF5 every other slot, 0xC2 spectrum chunks in between. No
 *              connection, any number of listeners. The sensor is duty-cycled
 *              between bursts, so a spectrum arrives once per idle period or
 *              when something moves (power.h).
 *
 *   connected  GATT notifications carrying raw 400 Hz samples. This is what the
 *              analyser uses; the host does the transform.
 *
 * DF5 is emitted unmodified so Ruuvi Station, Ruuvi Gateway and the Home
 * Assistant integration keep working on a tag running this firmware. It pauses
 * while a host is connected, because the stack owns the radio then; see the
 * note above adv_ensure() for why that is the accepted trade.
 */

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "adv.h"
#include "battery.h"
#include "env.h"
#include "gatt_stream.h"
#include "leds.h"
#include "power.h"
#include "sampler.h"

LOG_MODULE_REGISTER(rfa, LOG_LEVEL_INF);

/* One payload per advertising interval. Matching the two means the radio never
 * sends the same bytes twice and the CPU never wakes to prepare bytes that are
 * not sent - both of which are pure waste on a battery. */
#define ADV_SLOT_MS CONFIG_RFA_ADV_INTERVAL_MS

/* BLE advertising intervals are in 0.625 ms units. */
#define ADV_UNITS(ms) ((ms) * 8 / 5)

/*
 * FIFO poll period. The hardware FIFO is 32 slots deep; at 400 Hz that is 80 ms
 * of headroom, so polling at 40 ms leaves a 2x margin against a late thread.
 * Sample *timing* does not depend on this - the sensor timestamps samples at its
 * own ODR - so a late poll costs buffer depth, never sample spacing.
 */
#define FIFO_POLL_MS 40

static uint8_t mfg_data[2 + RFA_ADV_PAYLOAD_LEN];

static const struct bt_data adv_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

/* The 128-bit service UUID does not fit alongside 24 bytes of manufacturer data
 * in one 31-byte advertisement, so it goes in the scan response. A host
 * filtering for analyser-capable tags does an active scan and sees it there. */
static const struct bt_data adv_sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_128_ENCODE(0xf1a70001, 0x9c3f, 0x4f5a, 0x8b21, 0x2d6a3c9e7d10)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/*
 * BT_LE_ADV_OPT_USE_IDENTITY, not a resolvable private address: DF5 carries the
 * MAC in its payload, and a rotating advertised address would stop a scanner
 * joining the two. Identity is also what makes a tag findable across restarts.
 */
/* No BT_LE_ADV_OPT_SCANNABLE: it is mutually exclusive with _CONN, because
 * legacy connectable advertising (ADV_IND) is already scannable. The scan
 * response still goes out - that is where the service UUID lives. */
static const struct bt_le_adv_param adv_param_conn = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
	ADV_UNITS(CONFIG_RFA_ADV_INTERVAL_MS),
	ADV_UNITS(CONFIG_RFA_ADV_INTERVAL_MS) + 16, NULL);

static struct {
	uint8_t  mac[6];
	uint16_t sequence;

	/* Latest broadcast spectrum, and how far through transmitting it we are. */
	uint8_t  spectrum_db[RFA_C2_BINS];
	uint8_t  spectrum_frame;
	uint8_t  spectrum_chunk;
	bool     spectrum_have;
	bool     spectrum_invalid;
} app;

static K_MUTEX_DEFINE(state_lock);

/* DF5 carries the MAC big-endian; Zephyr hands it to us little-endian. */
static void load_identity(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	bt_id_get(addrs, &count);
	if (count == 0) {
		memset(app.mac, 0xFF, sizeof(app.mac));
		return;
	}
	for (int i = 0; i < 6; i++) {
		app.mac[i] = addrs[0].a.val[5 - i];
	}
}

static void build_df5(uint8_t out[RFA_ADV_PAYLOAD_LEN])
{
	struct rfa_df5_fields f;
	struct rfa_env env;
	int16_t dc[3];

	memset(&f, 0, sizeof(f));
	rfa_env_read(&env);
	rfa_sampler_dc(dc);

	if (env.valid) {
		f.temperature = rfa_temp_from_millicelsius(env.temperature_mc);
		f.humidity = rfa_humidity_from_millipercent(env.humidity_mpct);
		f.pressure = rfa_pressure_from_pascal(env.pressure_pa);
	} else {
		f.temperature = RFA_TEMP_INVALID;
		f.humidity = RFA_HUMIDITY_INVALID;
		f.pressure = RFA_PRESSURE_INVALID;
	}

	for (int i = 0; i < 3; i++) {
		f.accel_mg[i] = dc[i];
	}

	/* 0 when the ADC could not be read, which the encoder maps to DF5's
	 * invalid battery code - "not measured" rather than a flat cell. */
	f.battery_mv = rfa_battery_millivolts();

	/* Report what the radio is actually configured for. Claiming +4 dBm
	 * while the controller transmits at 0 puts a 4 dB error into anyone's
	 * path-loss estimate, and the field exists precisely so that estimate is
	 * possible. */
	f.tx_power_dbm = CONFIG_BT_CTLR_TX_PWR_DBM;

	k_mutex_lock(&state_lock, K_FOREVER);
	f.sequence = app.sequence++;
	k_mutex_unlock(&state_lock);

	/* Activity-interrupt events. That is exactly what DF5's movement counter
	 * means in Ruuvi's ecosystem, and watching it is how the motion threshold
	 * gets checked against a real machine. */
	f.movement_counter = rfa_power_movement_counter();

	memcpy(f.mac, app.mac, sizeof(f.mac));
	rfa_encode_df5(&f, out);
}

/*
 * Advertising is re-asserted by the main loop, every slot, forever.
 *
 * Two earlier designs failed on hardware and both are worth recording, because
 * the failure mode is the same and it is the worst one this project has:
 *
 *   1. Restarting advertising inside the connection callbacks. Those run on the
 *      Bluetooth RX thread, and bt_le_adv_start() from there does not work.
 *      The tag went silent the moment a host disconnected.
 *
 *   2. Switching to non-connectable advertising for the duration of a
 *      connection, so DF5 kept flowing during a capture, and switching back on
 *      disconnect. The switch to non-connectable succeeded - RTT confirmed it -
 *      and the tag then never came back. It stayed silent in a mode nothing
 *      was scanning for.
 *
 * So: one mode, connectable, and no cleverness. While a host is connected the
 * stack owns the radio and advertising stops; DF5 pauses for the duration of
 * the capture, which is the price of never going permanently quiet. The moment
 * the connection drops, the next slot starts advertising again, and a start
 * that fails is simply retried on the next slot.
 *
 * A tag that reliably comes back is worth more than one that broadcasts through
 * a capture and then dies.
 */
static atomic_t host_connected = ATOMIC_INIT(0);

static void advertise(const uint8_t payload[RFA_ADV_PAYLOAD_LEN])
{
	int err;

	mfg_data[0] = RFA_RUUVI_COMPANY_ID & 0xFF;   /* company ID is little-endian */
	mfg_data[1] = RFA_RUUVI_COMPANY_ID >> 8;
	memcpy(&mfg_data[2], payload, RFA_ADV_PAYLOAD_LEN);

	err = bt_le_adv_update_data(adv_ad, ARRAY_SIZE(adv_ad), adv_sd, ARRAY_SIZE(adv_sd));
	if (err) {
		LOG_WRN("advertisement update failed (%d)", err);
	}
}

/* Start advertising if it is not already running. -EALREADY is the normal
 * answer and is not worth a log line. */
static void adv_ensure(void)
{
	static bool running;
	int err = bt_le_adv_start(&adv_param_conn, adv_ad, ARRAY_SIZE(adv_ad),
				  adv_sd, ARRAY_SIZE(adv_sd));

	if (err == -EALREADY) {
		running = true;
		return;
	}
	if (err == 0) {
		if (!running) {
			LOG_INF("advertising");
		}
		running = true;
		return;
	}
	running = false;
	LOG_WRN("advertising start failed (%d), retrying next slot", err);
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);
	if (err) {
		return;
	}
	atomic_set(&host_connected, 1);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason);
	atomic_set(&host_connected, 0);
}

BT_CONN_CB_DEFINE(adv_conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* Sampling runs on its own thread so a slow sensor read cannot stretch the
 * advertising cadence. */
static void sample_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		if (!rfa_power_sampling()) {
			/* Sensor is in low-power mode and nothing is arriving.
			 * Sleeping here rather than polling at 40 ms is most of
			 * what duty cycling buys: 25 CPU wakes a second become
			 * one, and the FIFO holds nothing worth reading anyway. */
			k_msleep(200);
			continue;
		}

		rfa_sampler_poll();

		/* Pick up a completed transform if one is waiting and the
		 * previous frame has finished transmitting. Overwriting a frame
		 * mid-transmission would splice two spectra into one waterfall
		 * column. */
		k_mutex_lock(&state_lock, K_FOREVER);
		bool idle = !app.spectrum_have;
		k_mutex_unlock(&state_lock);

		if (idle) {
			uint8_t db[RFA_C2_BINS];
			bool invalid = false;

			if (rfa_sampler_spectrum(db, &invalid)) {
				k_mutex_lock(&state_lock, K_FOREVER);
				memcpy(app.spectrum_db, db, sizeof(app.spectrum_db));
				app.spectrum_invalid = invalid;
				app.spectrum_frame++;
				app.spectrum_chunk = 0;
				app.spectrum_have = true;
				k_mutex_unlock(&state_lock);
			}
		}

		k_msleep(FIFO_POLL_MS);
	}
}

/*
 * 4 kB. The transform runs on this thread and rfa_spectrum_bins keeps its
 * working buffers static for exactly that reason; the margin is for the SPI
 * batch and the 128-byte spectrum copy. CONFIG_HW_STACK_PROTECTION turns an
 * overflow here into a named error rather than a random fault.
 */
K_THREAD_DEFINE(rfa_sampler_thread, 4096, sample_thread, NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
	uint8_t payload[RFA_ADV_PAYLOAD_LEN];
	int err;
	int slot = 0;

	LOG_INF("ruuvitag-frequency-analyzer starting");

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bluetooth init failed (%d)", err);
		return err;
	}
	load_identity();

	rfa_leds_init();
	rfa_leds_boot();

	if (rfa_sampler_init() < 0) {
		LOG_ERR("accelerometer init failed; no spectra will be produced");
	}
	rfa_env_init();
	rfa_battery_init();
	rfa_gatt_stream_init();
	rfa_power_init();

	build_df5(payload);
	mfg_data[0] = RFA_RUUVI_COMPANY_ID & 0xFF;
	mfg_data[1] = RFA_RUUVI_COMPANY_ID >> 8;
	memcpy(&mfg_data[2], payload, RFA_ADV_PAYLOAD_LEN);

	adv_ensure();
	LOG_INF("advertising as %02X:%02X:%02X:%02X:%02X:%02X",
		app.mac[0], app.mac[1], app.mac[2], app.mac[3], app.mac[4], app.mac[5]);

	while (1) {
		k_msleep(ADV_SLOT_MS);

		if (atomic_get(&host_connected)) {
			/* The stack owns the radio for the duration. Sampling and
			 * the GATT stream carry on; there is nothing to advertise
			 * into. */
			rfa_leds_stream(rfa_gatt_stream_active());
			continue;
		}

		/* Re-assert advertising every slot. Cheap when it is already
		 * running, and the only thing standing between a failed start
		 * and a permanently silent tag. */
		adv_ensure();

		/*
		 * Rotation: DF5, then spectrum chunks until the frame is out.
		 * DF5 keeps its cadence whatever else is queued - Ruuvi
		 * compatibility outranks the broadcast spectrum, which is the
		 * lesser of the two transports anyway.
		 *
		 * While a host is streaming over GATT the broadcast spectrum is
		 * skipped entirely: it would spend radio time duplicating, badly,
		 * data the host already has at full rate.
		 */
		if (slot == 0 || rfa_gatt_stream_active()) {
			slot = 0;
			build_df5(payload);
		} else {
			bool sent = false;

			k_mutex_lock(&state_lock, K_FOREVER);
			if (app.spectrum_have && app.spectrum_chunk < RFA_C2_CHUNKS) {
				rfa_encode_c2(app.spectrum_db, app.spectrum_frame,
					      app.spectrum_chunk, app.spectrum_invalid,
					      payload);
				if (++app.spectrum_chunk >= RFA_C2_CHUNKS) {
					app.spectrum_have = false;
				}
				sent = true;
			}
			k_mutex_unlock(&state_lock);

			if (!sent) {
				/* Nothing queued: fall straight back to DF5 rather
				 * than idling through the spectrum slots. */
				slot = 0;
				build_df5(payload);
			}
		}
		slot = (slot + 1) % (1 + RFA_C2_CHUNKS);

		advertise(payload);
		rfa_leds_advert();
		rfa_leds_stream(rfa_gatt_stream_active());
	}

	return 0;
}
