#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * 网页与 AI 共用的 GPIO/ADC 抽象层。所有公开函数都会先校验引脚与运行状态，
 * 从而阻止触碰 SPI Flash 引脚、无效 GPIO 或 Wi-Fi 下不可用的 ADC2。
 *
 * Small pin toolbox exposed to the web UI.
 *
 * Everything here works on a single board with nothing else attached, which is
 * exactly the "one development board only" case: drive a pin and read it back,
 * or measure a voltage on an ADC1 pin.
 */

#define GPIO_PANEL_MAX_PIN 39
#define GPIO_PANEL_ADC_CHANNELS 8

esp_err_t gpio_panel_init(void);

/** 当引脚存在且可安全配置为输出时返回 true。 */
bool gpio_panel_is_output_capable(int gpio);

/** 把 `gpio` 配置为推挽输出，并驱动到 `level` 电平。 */
esp_err_t gpio_panel_set_output(int gpio, int level);

/** 将引脚释放为高阻输入。 */
esp_err_t gpio_panel_release(int gpio);

/** 以输入模式采样引脚，可选择是否启用内部上拉。 */
esp_err_t gpio_panel_read_input(int gpio, bool pullup, int *level);

/** 返回 ADC1 通道对应的 GPIO；通道不存在时返回 -1。 */
int gpio_panel_adc_gpio(int channel);

/** 读取 ADC1 通道，同时返回原始码值与校准后的毫伏值。 */
esp_err_t gpio_panel_read_adc(int channel, int *raw, int *millivolts);
