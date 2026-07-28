/*
 * Advertisement encoders.
 *
 * Deliberately free of Zephyr, Nordic and libc dependencies: pure bit packing
 * over plain integers. That is what lets these run in a host unit test against
 * Ruuvi's published vectors (firmware/test/test_adv.c) instead of only on
 * hardware, and it is the entire cost of the DF5 compatibility requirement.
 *
 * Field layout: docs/05-ble-protocol.md
 */

#ifndef RFA_ADV_H
#define RFA_ADV_H

#include <stdbool.h>
#include <stdint.h>

#define RFA_RUUVI_COMPANY_ID 0x0499
#define RFA_DF5_FORMAT       0x05
#define RFA_C2_FORMAT        0xC2
#define RFA_ADV_PAYLOAD_LEN  24

/*
 * Format 0xC2 - spectrum frame, the connectionless transport.
 *
 * A full 0-50 Hz spectrum does not fit in one advertisement, so a frame is
 * split across chunks that carry a frame id and a starting bin index. A
 * receiver reassembles by frame id and drops frames it did not hear in full;
 * at the packet loss measured in a house, some frames will be partial and a gap
 * in the waterfall is the honest rendering of that.
 *
 * This is the *broadcast* path: one spectrum roughly every 10 s, no connection,
 * any number of listeners. The GATT stream (gatt_stream.h) is the path for
 * actual analysis work and carries raw samples instead. ADR-0002 explains why
 * both exist.
 *
 * The bin scale is fixed by spec version rather than sent in every chunk -
 * six header bytes already cost a third of a chunk. Version 1 is 100 Hz
 * effective sample rate with a 256-point transform: 0.390625 Hz per bin,
 * 128 bins, 0 to 49.6 Hz.
 *
 * No MAC in the payload, unlike DF5. The BLE advertisement header already
 * carries the address, which is what a scanner joins on anyway, and those six
 * bytes buy a third of another chunk.
 */
#define RFA_C2_SPEC_VERSION  1
#define RFA_C2_BINS          128
#define RFA_C2_BIN_HZ        0.390625f
#define RFA_C2_HEADER_LEN    6
#define RFA_C2_BINS_PER_CHUNK (RFA_ADV_PAYLOAD_LEN - RFA_C2_HEADER_LEN)   /* 18 */
#define RFA_C2_CHUNKS        ((RFA_C2_BINS + RFA_C2_BINS_PER_CHUNK - 1) / RFA_C2_BINS_PER_CHUNK)

#define RFA_C2_FLAG_INVALID  0x01

/* Invalid sentinels, per the Ruuvi spec. A field that could not be measured
 * must carry these rather than a plausible-looking zero. */
#define RFA_TEMP_INVALID     INT16_MIN
#define RFA_HUMIDITY_INVALID UINT16_MAX
#define RFA_PRESSURE_INVALID UINT16_MAX
#define RFA_ACCEL_INVALID    INT16_MIN
#define RFA_SEQ_INVALID      UINT16_MAX
#define RFA_MOVES_INVALID    UINT8_MAX

/* Put this in rfa_df5_fields.battery_mv when no ADC reading is available; the
 * encoder then emits DF5's invalid battery code (11 bits all set). */
#define RFA_BATTERY_NOT_MEASURED 0

/* Raw sensor state, already in DF5's units so the encoder stays dumb. */
struct rfa_df5_fields {
	int16_t  temperature;      /* 0.005 degC/LSB */
	uint16_t humidity;         /* 0.0025 %/LSB   */
	uint16_t pressure;         /* 1 Pa/LSB, already offset by -50000 */
	int16_t  accel_mg[3];      /* mg, the 20 s EMA DC vector */
	uint16_t battery_mv;       /* millivolts, absolute */
	int8_t   tx_power_dbm;
	uint8_t  movement_counter;
	uint16_t sequence;
	uint8_t  mac[6];
};

/* Writes exactly RFA_ADV_PAYLOAD_LEN bytes and cannot fail. */
void rfa_encode_df5(const struct rfa_df5_fields *f, uint8_t out[RFA_ADV_PAYLOAD_LEN]);

/*
 * Encode one chunk of a spectrum frame. `bins_db` holds RFA_C2_BINS values
 * already in the transmitted dB encoding. Bins past the end of the array are
 * padded with zero, which decodes as the floor rather than as missing data.
 */
void rfa_encode_c2(const uint8_t *bins_db, uint8_t frame_id, uint8_t chunk_idx,
		   bool data_invalid, uint8_t out[RFA_ADV_PAYLOAD_LEN]);

/*
 * Amplitude (microg) to the 0xC2 dB encoding: 0.5 dB per LSB referenced to
 * 1 ug, so 0 is 1 ug and 255 is 127.5 dB = 2.37 g. A spectrum spans the noise
 * floor to a hard knock - five orders of magnitude - and a linear byte would
 * quantise the quiet end into two or three distinct values.
 */
uint8_t rfa_spectrum_db(uint32_t magnitude_ug);

/* Helpers that convert real-world units into DF5 encodings, clamping rather
 * than wrapping. Wrapping here would produce a plausible wrong number, which is
 * far worse than a saturated one. */
int16_t  rfa_temp_from_millicelsius(int32_t millicelsius);
uint16_t rfa_humidity_from_millipercent(int32_t millipercent);
uint16_t rfa_pressure_from_pascal(int32_t pascal);
uint16_t rfa_power_field(uint16_t battery_mv, int8_t tx_power_dbm);
uint16_t rfa_amplitude_from_microg(uint32_t microg);

#endif /* RFA_ADV_H */
