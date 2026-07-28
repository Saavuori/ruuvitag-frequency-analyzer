/*
 * Status LEDs. Both RuuviTag LEDs are active-low; the devicetree already says
 * so via GPIO_ACTIVE_LOW, and GPIO_OUTPUT_INACTIVE plus gpio_pin_set(1) means
 * "on" without this file having to know the polarity.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "leds.h"

LOG_MODULE_REGISTER(rfa_leds, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_RFA_STATUS_LEDS) && DT_NODE_EXISTS(DT_ALIAS(led0))

static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static bool ready;

/* The advertising flash is fired from a work item so the caller never sleeps
 * just to light an LED. */
static void green_off(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(green_work, green_off);

static void green_off(struct k_work *w)
{
	ARG_UNUSED(w);
	gpio_pin_set_dt(&led_green, 0);
}

void rfa_leds_init(void)
{
	if (!gpio_is_ready_dt(&led_red) || !gpio_is_ready_dt(&led_green)) {
		LOG_WRN("LEDs unavailable");
		return;
	}
	gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	ready = true;
	LOG_INF("status LEDs enabled (disable CONFIG_RFA_STATUS_LEDS for battery work)");
}

void rfa_leds_boot(void)
{
	if (!ready) {
		return;
	}
	gpio_pin_set_dt(&led_red, 1);
	gpio_pin_set_dt(&led_green, 1);
	k_msleep(CONFIG_RFA_LED_BOOT_MS);
	gpio_pin_set_dt(&led_red, 0);
	gpio_pin_set_dt(&led_green, 0);
}

void rfa_leds_advert(void)
{
	if (!ready) {
		return;
	}
	gpio_pin_set_dt(&led_green, 1);
	k_work_reschedule(&green_work, K_MSEC(CONFIG_RFA_LED_FLASH_MS));
}

/* Solid, not a flash: "a host is capturing right now" is a state, and reading a
 * state off a blinking light means waiting to see whether it blinks again. */
void rfa_leds_stream(bool on)
{
	if (!ready) {
		return;
	}
	gpio_pin_set_dt(&led_red, on ? 1 : 0);
}

#else /* LEDs disabled or absent */

void rfa_leds_init(void) { }
void rfa_leds_boot(void) { }
void rfa_leds_advert(void) { }
void rfa_leds_stream(bool on) { ARG_UNUSED(on); }

#endif
