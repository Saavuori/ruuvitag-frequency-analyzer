/*
 * Coin-cell voltage, through the SAADC's internal VDD input.
 *
 * This exists because a tag that cannot report its own supply cannot be left on
 * a battery and understood. Without it DF5 carries the "not measured" code, and
 * the only way to know how a deployment is going is to go and fetch the tag.
 *
 * Reading VDD is not a fuel gauge. A CR2477 holds ~3.0 V across most of its
 * life and then falls off a cliff, so the voltage says "fine" for a year and
 * "nearly done" for a week. It is a warning, not a percentage, and nothing here
 * converts it into one.
 */

#ifndef RFA_BATTERY_H
#define RFA_BATTERY_H

#include <stdint.h>

int rfa_battery_init(void);

/*
 * Supply voltage in millivolts, or 0 if it could not be read.
 *
 * 0 is deliberate: build_df5() maps it to DF5's invalid battery code, so a
 * failed ADC reads as "not measured" rather than as a flat cell. A fabricated
 * voltage gets plotted and believed.
 */
uint16_t rfa_battery_millivolts(void);

#endif /* RFA_BATTERY_H */
