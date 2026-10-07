#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * AI 工具可访问的设备能力边界：聊天模块只能经由这些接口读取状态、
 * 扫描网络，不能直接修改主控模块的私有全局变量。
 *
 * The slice of the dashboard that the chat tools are allowed to touch.
 *
 * main.c keeps its Wi-Fi scan buffers and the status serializer private; the
 * chat module goes through these three wrappers instead of reaching into them,
 * so there is exactly one place where "what an AI may do to this board" is
 * defined.
 */

/** 执行阻塞式 Wi-Fi 扫描，并把结果发布给仪表盘。 */
esp_err_t device_wifi_scan(void);

/** 返回信号最强的前 `limit` 个网络数组，按信号强度降序排列。 */
cJSON *device_network_summary(size_t limit);

/** 返回完整的 /api/status 负载（设备、AP、STA、LED 与网络列表）。 */
cJSON *device_status_snapshot(void);
