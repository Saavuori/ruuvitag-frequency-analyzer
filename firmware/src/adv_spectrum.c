/*
 * Format 0xC2 - chunked spectrum frames, the broadcast transport.
 *
 * See adv.h for the frame layout and why the bin scale is fixed by spec
 * version rather than transmitted.
 */

#include <math.h>

#include "adv.h"

uint8_t rfa_spectrum_db(uint32_t magnitude_ug)
{
	if (magnitude_ug < 1u) {
		return 0;
	}

	/* 0.5 dB per LSB, referenced to 1 ug. */
	float db = 40.0f * log10f((float)magnitude_ug);

	if (db <= 0.0f) {
		return 0;
	}
	if (db >= 255.0f) {
		return 255;
	}
	return (uint8_t)(db + 0.5f);
}

void rfa_encode_c2(const uint8_t *bins_db, uint8_t frame_id, uint8_t chunk_idx,
		  bool data_invalid, uint8_t out[RFA_ADV_PAYLOAD_LEN])
{
	uint16_t first = (uint16_t)chunk_idx * RFA_C2_BINS_PER_CHUNK;

	out[0] = RFA_C2_FORMAT;
	out[1] = (uint8_t)((RFA_C2_SPEC_VERSION & 0x0F) << 4);
	if (data_invalid) {
		out[1] |= RFA_C2_FLAG_INVALID;
	}
	out[2] = frame_id;
	out[3] = chunk_idx;
	out[4] = RFA_C2_CHUNKS;
	out[5] = (uint8_t)first;

	for (int i = 0; i < RFA_C2_BINS_PER_CHUNK; i++) {
		uint16_t bin = first + (uint16_t)i;

		/* The final chunk runs past the end of the spectrum. Pad with the
		 * floor value rather than leaving the buffer uninitialised - a
		 * receiver cannot tell padding from data, so it must at least be
		 * a value that reads as silence. */
		out[RFA_C2_HEADER_LEN + i] = (bin < RFA_C2_BINS) ? bins_db[bin] : 0;
	}
}
