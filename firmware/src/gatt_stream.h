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
 *     Stats    f1a70005-...  read     duty cycle and battery, for the power model
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

/*
 * Stats characteristic, 22 bytes little-endian:
 *
 *   0-3    uint32  uptime, seconds
 *   4-7    uint32  ms spent idle
 *   8-11   uint32  ms spent bursting
 *   12-15  uint32  ms spent streaming
 *   16-17  uint16  bursts
 *   18-19  uint16  motion events
 *   20-21  uint16  battery millivolts, 0 = not measured
 *
 * This exists because battery life cannot be measured here. A CR2477 barely
 * moves in voltage for months, so a trend cannot confirm a multi-year figure.
 * Reporting the duty cycle instead turns the power number into a model whose
 * inputs are measured on the tag, even though the per-mode currents come from
 * the datasheet. See docs/08-power.md.
 */
#define RFA_STATS_LEN 22
#define RFA_STREAM_HEADER_LEN    6
#define RFA_STREAM_BYTES_PER_SAMPLE 6      /* int16 x 3 axes */

#define RFA_STREAM_FLAG_GAP 0x01

/* Control characteristic commands. */
#define RFA_STREAM_CMD_STOP  0x00
#define RFA_STREAM_CMD_START 0x01
/*
 * 0x02 <uint16 LE mg>: set the wake-on-motion threshold.
 *
 * The right value depends entirely on what the tag is bolted to, and finding it
 * by reflashing is miserable. This is not persisted - a reboot returns to
 * CONFIG_RFA_MOTION_THRESHOLD_MG - because it is a tuning aid: find the value
 * here, then put it in the build.
 */
#define RFA_STREAM_CMD_THRESHOLD 0x02

/* Register the service and start the notification thread. Call after
 * bt_enable(). */
void rfa_gatt_stream_init(void);

/* True while a host is subscribed and has asked for data. main() uses this to
 * decide whether the broadcast spectrum is worth the advertising slots. */
bool rfa_gatt_stream_active(void);

#endif /* RFA_GATT_STREAM_H */
