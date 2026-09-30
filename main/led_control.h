#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * On-board LED driver.
 *
 * The LED is driven through LEDC so brightness, blinking and breathing all work
 * on a plain (non-addressable) on-board LED. Settings survive a reboot because
 * they are mirrored into NVS.
 */

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON = 1,
    LED_MODE_BLINK = 2,
    LED_MODE_BREATHE = 3,
} led_mode_t;

/* The Goouuu ESP-32F board's user LED is wired to GPIO5 and is active-HIGH.
 * Both the pin and the polarity remain adjustable from the web UI for an
 * external LED or a different board. */
#define LED_DEFAULT_GPIO 5
#define LED_MIN_PERIOD_MS 1000
#define LED_MAX_PERIOD_MS 5000
#define LED_MAX_GPIO 39

typedef struct {
    int gpio;
    led_mode_t mode;
    uint8_t brightness; /* 0..100 */
    uint16_t period_ms; /* blink / breathe period */
    bool active_high;
} led_state_t;

esp_err_t led_control_init(void);

/** Move the LED to another pin and/or flip the driving polarity. */
esp_err_t led_control_configure(int gpio, bool active_high);

/** Apply a new mode / brightness / period. */
esp_err_t led_control_set_mode(led_mode_t mode, uint8_t brightness,
                               uint16_t period_ms);

void led_control_get_state(led_state_t *out);

/** Persist the current state into NVS. */
esp_err_t led_control_save(void);

/** Restore the persisted state (called by led_control_init). */
esp_err_t led_control_load(void);
