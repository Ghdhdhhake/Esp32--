#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Small pin toolbox exposed to the web UI.
 *
 * Everything here works on a single board with nothing else attached, which is
 * exactly the "one development board only" case: drive a pin and read it back,
 * or measure a voltage on an ADC1 pin.
 */

#define GPIO_PANEL_MAX_PIN 39
#define GPIO_PANEL_ADC_CHANNELS 8

esp_err_t gpio_panel_init(void);

/** True when the pin exists on this chip and can be driven as an output. */
bool gpio_panel_is_output_capable(int gpio);

/** Configure `gpio` as a push-pull output and drive it to `level`. */
esp_err_t gpio_panel_set_output(int gpio, int level);

/** Return the pin to a high-impedance input. */
esp_err_t gpio_panel_release(int gpio);

/** Sample a pin as an input, optionally with the internal pull-up enabled. */
esp_err_t gpio_panel_read_input(int gpio, bool pullup, int *level);

/** GPIO backing an ADC1 channel, or -1 when the channel does not exist. */
int gpio_panel_adc_gpio(int channel);

/** Read an ADC1 channel; returns both the raw code and the calibrated mV. */
esp_err_t gpio_panel_read_adc(int channel, int *raw, int *millivolts);
