/*
 * Status LEDs - bench visibility only.
 *
 * The power budget says LEDs cost real battery and should be off in a tag left
 * running. They are here because during bring-up there is otherwise no way to
 * tell a powered tag from a dead one, and on a tag whose radio does not work
 * they are the *only* sign of life.
 *
 * Gated behind CONFIG_RFA_STATUS_LEDS.
 */

#ifndef RFA_LEDS_H
#define RFA_LEDS_H

#include <stdbool.h>

/* Safe to call even if the LEDs are absent or disabled; all become no-ops. */
void rfa_leds_init(void);

/* Both LEDs on briefly, so "it booted" is visible from across the room. */
void rfa_leds_boot(void);

/* Green flash: one advertisement went out. At 1.28 s spacing this reads as a
 * steady heartbeat, and its absence means the main loop has stopped. */
void rfa_leds_advert(void);

/* Red solid while a host is streaming over GATT. */
void rfa_leds_stream(bool on);

#endif /* RFA_LEDS_H */
