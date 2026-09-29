#include "led_control.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "led_control";

#define LED_NVS_NAMESPACE "ledcfg"
#define LED_NVS_VERSION_KEY "version"
#define LED_NVS_VERSION_CURRENT 2
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_DUTY_BITS LEDC_TIMER_10_BIT
#define LEDC_DUTY_MAX ((1u << 10) - 1)
#define LEDC_FREQ_HZ 5000
#define LED_TASK_PERIOD_MS 10

static led_state_t s_state = {
    .gpio = LED_DEFAULT_GPIO,
    .mode = LED_MODE_OFF,
    .brightness = 100,
    .period_ms = 1000,
    .active_high = true,
};
static SemaphoreHandle_t s_lock;
static bool s_channel_ready;
static int s_configured_gpio = -1;

static bool led_gpio_is_output_capable(int gpio)
{
    if (gpio < 0 || gpio > LED_MAX_GPIO) {
        return false;
    }
    /* GPIO 6..11 are wired to the SPI flash and must never be driven.
     * GPIO 34..39 are input-only on the classic ESP32. */
    if (gpio >= 6 && gpio <= 11) {
        return false;
    }
    if (gpio >= 34) {
        return false;
    }
    return true;
}

static void led_apply_level_locked(uint32_t percent)
{
    if (!s_channel_ready) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }

    uint32_t on_duty = (LEDC_DUTY_MAX * percent) / 100u;
    uint32_t duty = s_state.active_high ? on_duty : (LEDC_DUTY_MAX - on_duty);

    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static esp_err_t led_attach_channel_locked(int gpio)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_BITS,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t channel_config = {
        .gpio_num = gpio,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = s_state.active_high ? 0 : LEDC_DUTY_MAX,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_channel_ready = true;
    s_configured_gpio = gpio;
    return ESP_OK;
}

static void led_detach_channel_locked(void)
{
    if (!s_channel_ready) {
        return;
    }
    ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
    s_channel_ready = false;
    if (s_configured_gpio >= 0) {
        gpio_reset_pin(s_configured_gpio);
    }
    s_configured_gpio = -1;
}

static void led_task(void *argument)
{
    (void)argument;

    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t level = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        led_mode_t mode = s_state.mode;
        uint32_t brightness = s_state.brightness;
        uint32_t period = s_state.period_ms ? s_state.period_ms : 1000;
        xSemaphoreGive(s_lock);

        switch (mode) {
        case LED_MODE_OFF:
            level = 0;
            break;
        case LED_MODE_ON:
            level = brightness;
            break;
        case LED_MODE_BLINK:
            level = (now_ms % period) < (period / 2) ? brightness : 0;
            break;
        case LED_MODE_BREATHE: {
            uint32_t phase = now_ms % period;
            uint32_t half = period / 2 ? period / 2 : 1;
            uint32_t triangle = phase < half ? phase : (period - phase);
            level = brightness * triangle / half;
            break;
        }
        default:
            level = 0;
            break;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        led_apply_level_locked(level);
        xSemaphoreGive(s_lock);

        vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS));
    }
}

esp_err_t led_control_configure(int gpio, bool active_high)
{
    if (!led_gpio_is_output_capable(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (gpio != s_configured_gpio || active_high != s_state.active_high) {
        led_detach_channel_locked();
        s_state.active_high = active_high;
        esp_err_t err = led_attach_channel_locked(gpio);
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            return err;
        }
        s_state.gpio = gpio;
        ESP_LOGI(TAG, "LED moved to GPIO%d (%s)", gpio,
                 active_high ? "active-high" : "active-low");
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t led_control_set_mode(led_mode_t mode, uint8_t brightness,
                               uint16_t period_ms)
{
    if (mode > LED_MODE_BREATHE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (brightness > 100) {
        brightness = 100;
    }
    if (period_ms < LED_MIN_PERIOD_MS) {
        period_ms = LED_MIN_PERIOD_MS;
    }
    if (period_ms > LED_MAX_PERIOD_MS) {
        period_ms = LED_MAX_PERIOD_MS;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.mode = mode;
    s_state.brightness = brightness;
    s_state.period_ms = period_ms;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void led_control_get_state(led_state_t *out)
{
    if (out == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

esp_err_t led_control_save(void)
{
    led_state_t snapshot;
    led_control_get_state(&snapshot);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t gpio = (uint8_t)snapshot.gpio;
    uint8_t mode = (uint8_t)snapshot.mode;
    uint8_t brightness = snapshot.brightness;
    uint16_t period = snapshot.period_ms;
    uint8_t active_high = snapshot.active_high ? 1 : 0;

    err = nvs_set_u8(handle, "gpio", gpio);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "mode", mode);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "bright", brightness);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "period", period);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "ahigh", active_high);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, LED_NVS_VERSION_KEY,
                         LED_NVS_VERSION_CURRENT);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t led_control_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t gpio = LED_DEFAULT_GPIO;
    uint8_t mode = LED_MODE_OFF;
    uint8_t brightness = 100;
    uint16_t period = 1000;
    uint8_t active_high = 1;
    uint8_t version = 0;

    if (nvs_get_u8(handle, "gpio", &gpio) != ESP_OK) {
        gpio = LED_DEFAULT_GPIO;
    }
    if (nvs_get_u8(handle, "mode", &mode) != ESP_OK) {
        mode = LED_MODE_OFF;
    }
    if (nvs_get_u8(handle, "bright", &brightness) != ESP_OK) {
        brightness = 100;
    }
    if (nvs_get_u16(handle, "period", &period) != ESP_OK) {
        period = 1000;
    }
    if (nvs_get_u8(handle, "ahigh", &active_high) != ESP_OK) {
        active_high = 1;
    }
    (void)nvs_get_u8(handle, LED_NVS_VERSION_KEY, &version);
    nvs_close(handle);

    /* Firmware before version 2 assumed GPIO2 for every ESP32 board.  Move
     * that legacy default to the Goouuu ESP-32F's actual user LED.  A setting
     * saved by this firmware carries version 2 and is therefore never changed
     * if the user deliberately selects GPIO2 for an external LED. */
    if (version < LED_NVS_VERSION_CURRENT && gpio == 2 && active_high != 0) {
        gpio = LED_DEFAULT_GPIO;
        active_high = 1;
        ESP_LOGI(TAG, "Migrated legacy LED default to GPIO%d", gpio);
    }

    if (!led_gpio_is_output_capable(gpio)) {
        gpio = LED_DEFAULT_GPIO;
    }
    if (mode > LED_MODE_BREATHE) {
        mode = LED_MODE_OFF;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.gpio = gpio;
    s_state.mode = (led_mode_t)mode;
    s_state.brightness = brightness > 100 ? 100 : brightness;
    s_state.period_ms = period;
    s_state.active_high = active_high != 0;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Restored LED config: GPIO%d mode=%u brightness=%u%%",
             gpio, mode, s_state.brightness);
    return ESP_OK;
}

esp_err_t led_control_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    led_control_load();

    led_state_t snapshot;
    led_control_get_state(&snapshot);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = led_attach_channel_locked(snapshot.gpio);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(led_task, "led_ctrl", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED driver ready on GPIO%d", snapshot.gpio);
    return ESP_OK;
}
