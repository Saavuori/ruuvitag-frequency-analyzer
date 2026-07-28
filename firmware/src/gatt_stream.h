/*
 * Raw sample streaming over a GATT connection.
 *
 * This is the primary transport. The broadcast 0xC2 spectrum takes eight
 * advertisements to move one 2.56 s snapshot, which is one frame per ten
 * seconds before packet loss - fine for "is anything happening in that room",
 * useless for looking at a machine. This channel instead ships the samples
 * themselves and lets the host transform them (ADR-0002, ADR-0003).
 *
 * 400 Hz x 3 axes x 2 bytes is 2.4 kB/s. That is unremarkable for BLE and is
 * the whole reason this is the better trade: the tag stops deciding what the
 * analysis is.
 *
 *   Service  f1a70001-9c3f-4f5a-8b21-2d6a3c9e7d10
 *     Samples  f1a70002-...  notify   raw AC samples, gravity removed
 *     Control  f1a70003-...  write    start/stop
 *     Info     f1a70004-...  read     rates and units, so the host never guesses
 *
 * Wire format for one Samples notification:
 *
 *   | 0     | uint8     | bits 7-4 version (1), bit 0 = gap before this packet |
 *   | 1     | uint8     | sample count n                                      |
 *   | 2-5   | uint32 LE | index of the first sample, monotonic                |
 *   | 6...  | int16 LE  | x,y,z per sample, mg, little-endian                 |
 *
 * The index is what makes packet loss recoverable. A host that receives
 * index 4000 after index 3000 with 39 samples knows exactly how many samples
 * are missing and can leave a hole of the right width; splicing the gap shut
 * instead would shift every subsequent sample in time, and a time shift in a
 * spectrum analyser is a frequency error.
 */

#ifndef RFA_GATT_STREAM_H
#define RFA_GATT_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#define RFA_STREAM_PROTO_VERSION 1
#define RFA_STREAM_HEADER_LEN    6
#define RFA_STREAM_BYTES_PER_SAMPLE 6      /* int16 x 3 axes */

#define RFA_STREAM_FLAG_GAP 0x01

/* Control characteristic commands. */
#define RFA_STREAM_CMD_STOP  0x00
#define RFA_STREAM_CMD_START 0x01

/* Register the service and start the notification thread. Call after
 * bt_enable(). */
void rfa_gatt_stream_init(void);

/* True while a host is subscribed and has asked for data. main() uses this to
 * decide whether the broadcast spectrum is worth the advertising slots. */
bool rfa_gatt_stream_active(void);

#endif /* RFA_GATT_STREAM_H */
