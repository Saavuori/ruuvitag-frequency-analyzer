/*
 * Raw sample streaming over GATT. See gatt_stream.h for the wire format.
 */

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "gatt_stream.h"
#include "lis2dh12.h"
#include "sampler.h"

LOG_MODULE_REGISTER(rfa_gatt, LOG_LEVEL_INF);

#define UUID_BASE(w) BT_UUID_128_ENCODE(0xf1a70000 | (w), 0x9c3f, 0x4f5a, 0x8b21, 0x2d6a3c9e7d10)

static struct bt_uuid_128 uuid_service = BT_UUID_INIT_128(UUID_BASE(1));
static struct bt_uuid_128 uuid_samples = BT_UUID_INIT_128(UUID_BASE(2));
static struct bt_uuid_128 uuid_control = BT_UUID_INIT_128(UUID_BASE(3));
static struct bt_uuid_128 uuid_info    = BT_UUID_INIT_128(UUID_BASE(4));

static struct {
	struct bt_conn *conn;
	bool subscribed;      /* CCC enabled */
	bool started;         /* host wrote START */
	uint32_t sent;
	uint32_t gaps;
} gs;

static K_SEM_DEFINE(wake, 0, 1);

bool rfa_gatt_stream_active(void)
{
	return gs.conn && gs.subscribed && gs.started;
}

/*
 * Info characteristic. The host must never have to assume a sample rate: the
 * LIS2DH12's ODR comes from an RC oscillator that is a few percent off nominal
 * and drifts with temperature, and a 3% rate error is a 3% error on every
 * frequency the analyser draws (S-6). Reading this is how the host learns the
 * measured rate rather than the label.
 */
static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	uint8_t info[12];

	info[0] = RFA_STREAM_PROTO_VERSION;
	info[1] = 3;                                    /* axes */
	sys_put_le32(rfa_lis2dh12_nominal_hz() * 1000u, &info[2]);
	sys_put_le32(rfa_sampler_measured_mhz(), &info[6]);
	info[10] = 1;                                   /* 1 mg per int16 LSB */
	info[11] = 2;                                   /* +/- 2 g full scale */

	return bt_gatt_attr_read(conn, attr, buf, len, offset, info, sizeof(info));
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr); ARG_UNUSED(conn); ARG_UNUSED(flags);

	if (offset != 0 || len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (((const uint8_t *)buf)[0]) {
	case RFA_STREAM_CMD_START:
		/* Drop whatever accumulated while nobody was listening. A host
		 * that connects at t=0 should get samples from t=0, not a
		 * second of history it will plot as if it were live. */
		rfa_sampler_stream_reset();
		gs.started = true;
		gs.sent = 0;
		gs.gaps = 0;
		k_sem_give(&wake);
		LOG_INF("stream started");
		break;
	case RFA_STREAM_CMD_STOP:
		gs.started = false;
		LOG_INF("stream stopped after %u packets, %u gaps", gs.sent, gs.gaps);
		break;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	gs.subscribed = (value == BT_GATT_CCC_NOTIFY);
	if (!gs.subscribed) {
		gs.started = false;
	}
	LOG_INF("notifications %s", gs.subscribed ? "on" : "off");
}

BT_GATT_SERVICE_DEFINE(rfa_stream_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_service),

	BT_GATT_CHARACTERISTIC(&uuid_samples.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&uuid_control.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_control, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_info.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_info, NULL, NULL),
);

/* Attribute 1 is the Samples value; notifying on the characteristic
 * declaration is the documented Zephyr idiom for "this characteristic". */
#define SAMPLES_ATTR (&rfa_stream_svc.attrs[1])

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (0x%02x)", err);
		return;
	}
	gs.conn = bt_conn_ref(conn);
	LOG_INF("host connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	LOG_INF("host disconnected (0x%02x)", reason);
	if (gs.conn) {
		bt_conn_unref(gs.conn);
		gs.conn = NULL;
	}
	gs.subscribed = false;
	gs.started = false;
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/*
 * How many samples fit in one notification, given the negotiated MTU.
 *
 * Computed per packet rather than latched at connect: MTU exchange is initiated
 * by the central and may land after the first notification goes out. Assuming
 * the large MTU before it is agreed produces a silent truncation, which on this
 * wire format would corrupt sample alignment for the rest of the connection.
 */
static uint16_t samples_per_packet(struct bt_conn *conn)
{
	uint16_t mtu = bt_gatt_get_mtu(conn);
	int payload = (int)mtu - 3 - RFA_STREAM_HEADER_LEN;

	if (payload < RFA_STREAM_BYTES_PER_SAMPLE) {
		return 0;
	}
	return (uint16_t)(payload / RFA_STREAM_BYTES_PER_SAMPLE);
}

static void stream_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	/* Sized from the configured MTU, so raising CONFIG_BT_L2CAP_TX_MTU
	 * cannot silently outgrow the buffer that carries its packets. */
	static uint8_t pkt[CONFIG_BT_L2CAP_TX_MTU];
	static struct rfa_sample samples[(CONFIG_BT_L2CAP_TX_MTU - RFA_STREAM_HEADER_LEN)
					 / RFA_STREAM_BYTES_PER_SAMPLE];

	while (1) {
		if (!rfa_gatt_stream_active()) {
			k_sem_take(&wake, K_MSEC(200));
			continue;
		}

		struct bt_conn *conn = gs.conn;
		uint16_t cap = samples_per_packet(conn);

		if (cap == 0) {
			k_msleep(20);
			continue;
		}
		if (cap > ARRAY_SIZE(samples)) {
			cap = ARRAY_SIZE(samples);
		}

		uint32_t first;
		bool gap;
		size_t n = rfa_sampler_stream_read(samples, cap, &first, &gap);

		if (n == 0) {
			/* Ring empty. At 400 Hz a full packet accumulates in
			 * ~100 ms; sleeping a fraction of that keeps latency low
			 * without spinning. */
			k_msleep(10);
			continue;
		}

		pkt[0] = (RFA_STREAM_PROTO_VERSION << 4) | (gap ? RFA_STREAM_FLAG_GAP : 0);
		pkt[1] = (uint8_t)n;
		sys_put_le32(first, &pkt[2]);
		for (size_t i = 0; i < n; i++) {
			uint8_t *p = &pkt[RFA_STREAM_HEADER_LEN + i * RFA_STREAM_BYTES_PER_SAMPLE];

			sys_put_le16((uint16_t)samples[i].mg[0], p);
			sys_put_le16((uint16_t)samples[i].mg[1], p + 2);
			sys_put_le16((uint16_t)samples[i].mg[2], p + 4);
		}

		int err = bt_gatt_notify(conn, SAMPLES_ATTR, pkt,
					 RFA_STREAM_HEADER_LEN + n * RFA_STREAM_BYTES_PER_SAMPLE);

		if (err == -ENOMEM || err == -EAGAIN) {
			/*
			 * Out of ACL buffers: the radio has not drained what we
			 * already queued. Back off and let it. The samples stay
			 * in the ring, and if we stall long enough to overflow
			 * it the gap flag reports that honestly rather than the
			 * host seeing a seamless stream that quietly lost time.
			 */
			k_msleep(5);
			continue;
		}
		if (err) {
			LOG_WRN("notify failed (%d)", err);
			k_msleep(50);
			continue;
		}
		gs.sent++;
		if (gap) {
			gs.gaps++;
		}
	}
}

K_THREAD_DEFINE(rfa_streamer, 2048, stream_thread, NULL, NULL, NULL, 6, 0, 0);

void rfa_gatt_stream_init(void)
{
	memset(&gs, 0, sizeof(gs));
	LOG_INF("stream service registered");
}
