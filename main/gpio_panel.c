#include "gpio_panel.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "gpio_panel";

/* ESP32: ADC1 channels 0..7 map to these GPIOs. ADC2 is unusable while Wi-Fi
 * is running, so it is deliberately not exposed. */
static const int s_adc1_gpio[GPIO_PANEL_ADC_CHANNELS] = {
    36, 37, 38, 39, 32, 33, 34, 35,
};

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_adc_ready;
static bool s_cali_ready;
static SemaphoreHandle_t s_lock;

static bool gpio_panel_is_reserved(int gpio)
{
    /* GPIO 6..11 drive the SPI flash; GPIO 20/24 do not exist on the classic
     * ESP32; GPIO 34..39 are input only. */
    if (gpio < 0 || gpio > GPIO_PANEL_MAX_PIN) {
        return true;
    }
    if (gpio >= 6 && gpio <= 11) {
        return true;
    }
    if (gpio == 20 || gpio == 24) {
        return true;
    }
    return false;
}

bool gpio_panel_is_output_capable(int gpio)
{
    if (gpio_panel_is_reserved(gpio)) {
        return false;
    }
    /* GPIO 12/15 are strapping pins (MTDI/MTDO).  The toolbox leaves a pin
     * driven until it is released again, so driving one of them can break the
     * next boot; reading them as inputs stays allowed. */
    if (gpio == 12 || gpio == 15) {
        return false;
    }
    if (gpio >= 34) {
        return false;
    }
    return true;
}

esp_err_t gpio_panel_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit unavailable: %s", esp_err_to_name(err));
        return ESP_OK; /* The pin toolbox still works without the ADC. */
    }
    s_adc_ready = true;

    /* The classic ESP32 characterizes its ADC with a line-fitting scheme; the
     * newer parts use curve fitting. Pick whichever this target supports. */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
#if CONFIG_IDF_TARGET_ESP32
        .default_vref = 1100,
#endif
    };
    err = adc_cali_create_scheme_line_fitting(&cali_config, &s_cali_handle);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err == ESP_OK) {
        s_cali_ready = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Pin toolbox ready (ADC1 calibrated=%d)", s_cali_ready);
    return ESP_OK;
}

esp_err_t gpio_panel_set_output(int gpio, int level)
{
    if (!gpio_panel_is_output_capable(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        /* gpio_panel_init() never obtained a mutex: the toolbox is unusable. */
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
        err = gpio_set_level(gpio, level ? 1 : 0);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t gpio_panel_release(int gpio)
{
    /* Reserved pins must never be reconfigured: turning an SPI-flash pin
     * (6..11) into a floating input hangs the chip at the next cache miss, and
     * GPIO 20/24 do not exist on the classic ESP32. */
    if (gpio_panel_is_reserved(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = gpio_config(&config);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t gpio_panel_read_input(int gpio, bool pullup, int *level)
{
    if (level == NULL || gpio_panel_is_reserved(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
        *level = gpio_get_level(gpio);
    }
    xSemaphoreGive(s_lock);
    return err;
}

int gpio_panel_adc_gpio(int channel)
{
    if (channel < 0 || channel >= GPIO_PANEL_ADC_CHANNELS) {
        return -1;
    }
    return s_adc1_gpio[channel];
}

esp_err_t gpio_panel_read_adc(int channel, int *raw, int *millivolts)
{
    if (!s_adc_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel < 0 || channel >= GPIO_PANEL_ADC_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    esp_err_t err = adc_oneshot_config_channel(
        s_adc_handle, (adc_channel_t)channel, &channel_config);

    int sample = 0;
    if (err == ESP_OK) {
        err = adc_oneshot_read(s_adc_handle, (adc_channel_t)channel, &sample);
    }
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        return err;
    }

    if (raw != NULL) {
        *raw = sample;
    }

    if (millivolts != NULL) {
        int voltage = 0;
        if (s_cali_ready) {
            if (adc_cali_raw_to_voltage(s_cali_handle, sample, &voltage) !=
                ESP_OK) {
                voltage = 0;
            }
        } else {
            /* 12 dB attenuation, 12-bit resolution: ~3.3 V full scale. */
            voltage = (sample * 3300) / 4095;
        }
        *millivolts = voltage;
    }

    return ESP_OK;
}
