/*
 * AI 对话模块：接收 WebSocket 消息，在独立任务中请求兼容 OpenAI 的接口，
 * 将流式回复回传网页，并受控地执行模型申请的设备工具调用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "chat_ws.h"
#include "device_api.h"
#include "gpio_panel.h"
#include "llm_client.h"
#include "rss_news.h"

static const char *TAG = "chat";

#define CHAT_QUEUE_DEPTH 4
#define CHAT_TASK_STACK 10240
#define CHAT_TASK_PRIORITY 4
#define CHAT_INPUT_MAX 1200
#define CHAT_REPLY_MAX 2048
#define CHAT_HISTORY_MAX 12 /* messages kept, system prompt excluded */
#define CHAT_TOOL_ROUNDS 3
#define CHAT_MAX_JSON_BODY 2048
#define WIFI_SUMMARY_LIMIT 8

extern const unsigned char chat_html_start[] asm("_binary_chat_html_start");
extern const unsigned char chat_html_end[] asm("_binary_chat_html_end");

/* Only one conversation is served at a time.  esp_http_server runs every
 * request on a single task, so a second concurrent stream would not be faster
 * anyway - it would just interleave two answers on the same socket. */
static httpd_handle_t s_server;
static volatile int s_ws_fd = -1;
static QueueHandle_t s_requests;
static volatile bool s_busy;
static volatile bool s_abort;
static volatile bool s_reset_history;

/* Owned by the chat task.  s_reply and s_calls are file scope rather than
 * stack locals because they are several kilobytes each and the callback that
 * fills them runs deep inside the HTTP client. */
static cJSON *s_messages;
static char s_reply[CHAT_REPLY_MAX];
static size_t s_reply_length;
static llm_tool_call_t s_calls[LLM_MAX_TOOL_CALLS];

/* ------------------------------------------------------------------------- */
/* WebSocket 基础设施：连接存活判断与异步 JSON 发送                         */
/* ------------------------------------------------------------------------- */

static bool ws_client_alive(int fd)
{
    return fd >= 0 && s_server != NULL;
}

static void ws_send_json(int fd, cJSON *root)
{
    if (root == NULL) {
        return;
    }
    if (!ws_client_alive(fd)) {
        cJSON_Delete(root);
        return;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };
    esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
    if (err != ESP_OK) {
        /* A browser that navigates away closes the socket without a CLOSE
         * frame ever reaching this handler, so the first failed send is the
         * only disconnect signal available.  Drop the session instead of
         * logging once per token for the rest of the answer. */
        ESP_LOGW(TAG, "ws send failed on fd %d: %s", fd, esp_err_to_name(err));
        if (s_ws_fd == fd) {
            s_ws_fd = -1;
        }
    }
    free(payload);
}

static void chat_emit(int fd, const char *type, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "type", type);
    if (text != NULL) {
        cJSON_AddStringToObject(root, "text", text);
    }
    ws_send_json(fd, root);
}

/* ------------------------------------------------------------------------- */
/* 模型可调用的受控工具                                                      */
/* ------------------------------------------------------------------------- */

static int json_int(const cJSON *object, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static cJSON *tool_read_news(const cJSON *arguments)
{
    int count = clamp_int(json_int(arguments, "count", 6), 1, RSS_MAX_ITEMS);

    const cJSON *keyword_item = cJSON_GetObjectItem(arguments, "keyword");
    const char *keyword = cJSON_IsString(keyword_item) &&
                                  keyword_item->valuestring != NULL
                              ? keyword_item->valuestring
                              : "";

    rss_config_t news;
    rss_config_load(&news);

    cJSON *items = NULL;
    char error[160] = {0};

    esp_err_t err = rss_news_run(news.feeds, count, keyword, &items, error,
                                 sizeof(error));

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(items);
        return NULL;
    }

    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    if (keyword[0] != '\0') {
        cJSON_AddStringToObject(result, "keyword", keyword);
    }

    if (err == ESP_OK) {
        cJSON_AddItemToObject(result, "news", items);
        /* Without this the model tends to answer from memory and treat the
         * payload as decoration. */
        cJSON_AddStringToObject(
            result, "note",
            "以下是刚从新闻源抓到的最新条目，请只依据它们回答，并在末尾注明"
            "来源站点；条目里没有的就直说没查到，不要凭记忆补充。");
    } else {
        cJSON_Delete(items);
        cJSON_AddStringToObject(result, "error", error);
        cJSON_AddStringToObject(result, "note",
                                "抓取失败，请如实告诉用户你查不到实时新闻，"
                                "不要凭记忆编造。");
    }
    return result;
}

static cJSON *tool_read_gpio(const cJSON *arguments)
{
    int gpio = json_int(arguments, "gpio", -1);
    int level = 0;
    esp_err_t err = gpio_panel_read_input(gpio, false, &level);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(result, "gpio", gpio);
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(result, "level", level);
    } else {
        cJSON_AddStringToObject(
            result, "error",
            "该引脚不能作为输入（6-11 是 SPI Flash，34-39 只能输入）");
    }
    return result;
}

static cJSON *tool_write_gpio(const cJSON *arguments)
{
    int gpio = json_int(arguments, "gpio", -1);
    int level = json_int(arguments, "level", 0) ? 1 : 0;

    cJSON *result = cJSON_CreateObject();
    esp_err_t err = gpio_panel_set_output(gpio, level);
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(result, "gpio", gpio);
    cJSON_AddNumberToObject(result, "level", level);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(result, "error", "引脚不合法或不可作为输出");
    }
    return result;
}

static cJSON *tool_read_adc(const cJSON *arguments)
{
    int channel = clamp_int(json_int(arguments, "channel", 0), 0,
                            GPIO_PANEL_ADC_CHANNELS - 1);
    int raw = 0;
    int millivolts = 0;
    esp_err_t err = gpio_panel_read_adc(channel, &raw, &millivolts);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(result, "channel", channel);
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(result, "gpio", gpio_panel_adc_gpio(channel));
        cJSON_AddNumberToObject(result, "millivolts", millivolts);
        cJSON_AddNumberToObject(result, "volts", millivolts / 1000.0);
    } else {
        cJSON_AddStringToObject(result, "error", "ADC 通道不可用（只有 ADC1 的 0-7）");
    }
    return result;
}

static cJSON *tool_scan_wifi(void)
{
    cJSON *result = cJSON_CreateObject();
    esp_err_t err = device_wifi_scan();
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
        return result;
    }
    cJSON_AddItemToObject(result, "networks",
                          device_network_summary(WIFI_SUMMARY_LIMIT));
    return result;
}

static cJSON *tool_device_status(void)
{
    cJSON *status = device_status_snapshot();
    if (status == NULL) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "状态读取失败");
        return result;
    }

    /* Only the three blocks the model can actually talk about; the network
     * list and LED block would just burn tokens. */
    cJSON *device = cJSON_DetachItemFromObject(status, "device");
    cJSON *station = cJSON_DetachItemFromObject(status, "sta");
    cJSON *access_point = cJSON_DetachItemFromObject(status, "ap");
    cJSON_Delete(status);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    if (device != NULL) {
        cJSON_AddItemToObject(result, "device", device);
    }
    if (station != NULL) {
        cJSON_AddItemToObject(result, "sta", station);
    }
    if (access_point != NULL) {
        cJSON_AddItemToObject(result, "ap", access_point);
    }
    return result;
}

static cJSON *execute_tool(const char *name, const cJSON *arguments)
{
    if (strcmp(name, "read_news") == 0) {
        return tool_read_news(arguments);
    }
    if (strcmp(name, "read_gpio") == 0) {
        return tool_read_gpio(arguments);
    }
    if (strcmp(name, "write_gpio") == 0) {
        return tool_write_gpio(arguments);
    }
    if (strcmp(name, "read_adc") == 0) {
        return tool_read_adc(arguments);
    }
    if (strcmp(name, "scan_wifi") == 0) {
        return tool_scan_wifi();
    }
    if (strcmp(name, "device_status") == 0) {
        return tool_device_status();
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON_AddStringToObject(result, "error", "未知工具");
    return result;
}

/* ------------------------------------------------------------------------- */
/* 工具 Schema：告知模型每个工具的名称、用途和参数                           */
/* ------------------------------------------------------------------------- */

static cJSON *add_property(cJSON *properties, const char *name, const char *type,
                           const char *description)
{
    cJSON *item = cJSON_AddObjectToObject(properties, name);
    if (item == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(item, "type", type);
    if (description != NULL) {
        cJSON_AddStringToObject(item, "description", description);
    }
    return item;
}

static cJSON *begin_tool(const char *name, const char *description,
                         cJSON **properties_out)
{
    cJSON *tool = cJSON_CreateObject();
    if (tool == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(tool, "type", "function");

    cJSON *function = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(function, "name", name);
    cJSON_AddStringToObject(function, "description", description);

    cJSON *parameters = cJSON_AddObjectToObject(function, "parameters");
    cJSON_AddStringToObject(parameters, "type", "object");
    *properties_out = cJSON_AddObjectToObject(parameters, "properties");
    return tool;
}

static void require_property(cJSON *tool, const char *name)
{
    cJSON *function = cJSON_GetObjectItem(tool, "function");
    cJSON *parameters = cJSON_GetObjectItem(function, "parameters");
    cJSON *required = cJSON_AddArrayToObject(parameters, "required");
    if (required != NULL) {
        cJSON_AddItemToArray(required, cJSON_CreateString(name));
    }
}

static cJSON *build_tools(void)
{
    cJSON *tools = cJSON_CreateArray();
    if (tools == NULL) {
        return NULL;
    }

    cJSON *properties = NULL;

    /* Listed first on purpose: this is the tool the model most often forgets
     * it has, and the one that turns "我不知道，我没联网" into a real answer. */
    cJSON *tool = begin_tool(
        "read_news",
        "读取已配置新闻源(RSS)的最新条目。涉及新闻、时事、今天发生了什么、"
        "最新动态这类训练数据之后才发生的事实时，必须先调用它再回答。",
        &properties);
    add_property(properties, "count", "integer",
                 "最多返回几条，默认 6，最多 12");
    add_property(properties, "keyword", "string",
                 "可选。只保留标题或摘要里含该词的条目，"
                 "比如问「新闻联播」就填「新闻联播」");
    cJSON_AddItemToArray(tools, tool);

    tool = begin_tool("read_gpio", "读取某个 GPIO 的当前电平", &properties);
    add_property(properties, "gpio", "integer", "引脚编号");
    require_property(tool, "gpio");
    cJSON_AddItemToArray(tools, tool);

    tool = begin_tool("write_gpio", "把某个 GPIO 设为输出并输出高或低电平",
                      &properties);
    add_property(properties, "gpio", "integer", "引脚编号");
    add_property(properties, "level", "integer", "0 输出低电平，1 输出高电平");
    require_property(tool, "gpio");
    require_property(tool, "level");
    cJSON_AddItemToArray(tools, tool);

    tool = begin_tool("read_adc", "读取 ADC1 某个通道的电压", &properties);
    add_property(properties, "channel", "integer", "ADC1 通道号 0-7");
    require_property(tool, "channel");
    cJSON_AddItemToArray(tools, tool);

    tool = begin_tool("scan_wifi", "扫描附近的 Wi-Fi 热点", &properties);
    cJSON_AddItemToArray(tools, tool);

    tool = begin_tool("device_status",
                      "读取开发板状态：可用内存、运行时间、IP、芯片型号",
                      &properties);
    cJSON_AddItemToArray(tools, tool);

    return tools;
}

/* ------------------------------------------------------------------------- */
/* 会话历史：保留系统提示与有限轮次，避免请求体无限增长                      */
/* ------------------------------------------------------------------------- */

static void history_reset(void)
{
    if (s_messages != NULL) {
        cJSON_Delete(s_messages);
        s_messages = NULL;
    }
}

static cJSON *history_get(void)
{
    if (s_messages != NULL) {
        return s_messages;
    }

    llm_config_t config;
    llm_config_load(&config);

    s_messages = cJSON_CreateArray();
    if (s_messages == NULL) {
        return NULL;
    }

    cJSON *system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", config.system);
    cJSON_AddItemToArray(s_messages, system);
    return s_messages;
}

static void history_trim(void)
{
    if (s_messages == NULL) {
        return;
    }

    while (cJSON_GetArraySize(s_messages) > 1 + CHAT_HISTORY_MAX) {
        cJSON *oldest = cJSON_DetachItemFromArray(s_messages, 1);
        if (oldest == NULL) {
            break;
        }
        cJSON_Delete(oldest);
    }

    /* A `tool` message whose assistant parent just got evicted would be
     * rejected by the provider, so drop any that ended up leading the array. */
    while (cJSON_GetArraySize(s_messages) > 1) {
        cJSON *first = cJSON_GetArrayItem(s_messages, 1);
        const cJSON *role = cJSON_GetObjectItem(first, "role");
        if (!cJSON_IsString(role) || strcmp(role->valuestring, "tool") != 0) {
            break;
        }
        cJSON_Delete(cJSON_DetachItemFromArray(s_messages, 1));
    }
}

/* V4.1 Flash is a reasoning model: with thinking left on, a two-word answer
 * can take several seconds and burn tokens this panel never displays.  Only
 * DeepSeek understands the field, so gate it on the host name. */
static void add_provider_options(cJSON *root, const char *base_url)
{
    if (base_url == NULL || strstr(base_url, "deepseek") == NULL) {
        return;
    }
    cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
    if (thinking != NULL) {
        cJSON_AddStringToObject(thinking, "type", "disabled");
    }
}

static char *build_body(const llm_config_t *config, bool with_tools)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", config->model);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "max_tokens", config->max_tokens);
    add_provider_options(root, config->base_url);
    /* Reference rather than duplicate: the history can run to several
     * kilobytes and the heap is the scarcest thing on this board. */
    cJSON_AddItemReferenceToObject(root, "messages", s_messages);

    if (with_tools) {
        cJSON *tools = build_tools();
        if (tools != NULL) {
            cJSON_AddItemToObject(root, "tools", tools);
        }
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* ------------------------------------------------------------------------- */
/* 单轮对话：请求模型、执行工具并把最终结果回传浏览器                        */
/* ------------------------------------------------------------------------- */

static bool delta_callback(const char *text, size_t length, void *user)
{
    (void)user;
    if (s_abort) {
        return false;
    }

    /* Keep a bounded copy for the history; the browser still gets the whole
     * fragment even when the history copy is already full. */
    size_t room = sizeof(s_reply) - 1 - s_reply_length;
    if (room > 0) {
        size_t take = length < room ? length : room;
        memcpy(s_reply + s_reply_length, text, take);
        s_reply_length += take;
        s_reply[s_reply_length] = '\0';
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return true;
    }
    cJSON_AddStringToObject(root, "type", "delta");
    cJSON_AddStringToObject(root, "text", text);
    ws_send_json(s_ws_fd, root);
    return true;
}

static void history_append(const char *role, const char *content)
{
    if (s_messages == NULL) {
        return;
    }
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) {
        return;
    }
    cJSON_AddStringToObject(message, "role", role);
    cJSON_AddStringToObject(message, "content", content);
    cJSON_AddItemToArray(s_messages, message);
}

static void history_append_tool_calls(const llm_tool_call_t *calls,
                                      size_t count)
{
    if (s_messages == NULL) {
        return;
    }
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) {
        return;
    }
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddNullToObject(message, "content");

    cJSON *array = cJSON_AddArrayToObject(message, "tool_calls");
    for (size_t i = 0; i < count && array != NULL; i++) {
        if (!calls[i].used) {
            continue;
        }
        cJSON *call = cJSON_CreateObject();
        cJSON_AddStringToObject(call, "id", calls[i].id);
        cJSON_AddStringToObject(call, "type", "function");
        cJSON *function = cJSON_AddObjectToObject(call, "function");
        cJSON_AddStringToObject(function, "name", calls[i].name);
        cJSON_AddStringToObject(function, "arguments", calls[i].arguments);
        cJSON_AddItemToArray(array, call);
    }
    cJSON_AddItemToArray(s_messages, message);
}

static void run_tool_call(const llm_tool_call_t *call)
{
    int64_t started = esp_timer_get_time();

    cJSON *arguments = cJSON_Parse(call->arguments[0] != '\0'
                                       ? call->arguments
                                       : "{}");
    if (arguments == NULL) {
        arguments = cJSON_CreateObject();
    }

    cJSON *result = execute_tool(call->name, arguments);
    cJSON_Delete(arguments);

    int elapsed_ms = (int)((esp_timer_get_time() - started) / 1000);

    /* The chip under the answer is what makes the demo land: it shows the
     * model actually reached into the board. */
    cJSON *event = cJSON_CreateObject();
    if (event != NULL) {
        cJSON_AddStringToObject(event, "type", "tool");
        cJSON_AddStringToObject(event, "name", call->name);
        cJSON_AddNumberToObject(event, "ms", elapsed_ms);
        cJSON_AddItemToObject(event, "result", cJSON_Duplicate(result, true));
        ws_send_json(s_ws_fd, event);
    }

    char *text = result != NULL ? cJSON_PrintUnformatted(result) : NULL;
    cJSON_Delete(result);

    if (s_messages != NULL) {
        cJSON *message = cJSON_CreateObject();
        if (message != NULL) {
            cJSON_AddStringToObject(message, "role", "tool");
            cJSON_AddStringToObject(message, "tool_call_id", call->id);
            cJSON_AddStringToObject(message, "content",
                                    text != NULL ? text : "{}");
            cJSON_AddItemToArray(s_messages, message);
        }
    }
    free(text);
}

static void run_turn(const char *user_text)
{
    if (s_reset_history) {
        history_reset();
        s_reset_history = false;
    }

    llm_config_t config;
    llm_config_load(&config);

    if (config.api_key[0] == '\0') {
        chat_emit(s_ws_fd, "error", "尚未配置 API Key，请先点右上角设置");
        return;
    }

    if (history_get() == NULL) {
        chat_emit(s_ws_fd, "error", "内存不足，无法建立会话");
        return;
    }

    history_append("user", user_text);
    history_trim();
    chat_emit(s_ws_fd, "start", NULL);

    for (int round = 0; round < CHAT_TOOL_ROUNDS; round++) {
        char *body = build_body(&config, true);
        if (body == NULL) {
            chat_emit(s_ws_fd, "error", "请求体构造失败（内存不足）");
            return;
        }

        s_reply_length = 0;
        s_reply[0] = '\0';

        size_t call_count = 0;
        char error[192] = {0};

        esp_err_t err = llm_stream_request(&config, body, delta_callback, NULL,
                                           s_calls, &call_count, &s_abort,
                                           error, sizeof(error));
        free(body);

        if (err == ESP_ERR_INVALID_STATE) {
            /* Cancelled by the user, or a configuration problem reported
             * through `error`. */
            if (s_abort) {
                if (s_reply_length > 0) {
                    history_append("assistant", s_reply);
                }
                chat_emit(s_ws_fd, "aborted", NULL);
                return;
            }
            chat_emit(s_ws_fd, "error",
                      error[0] != '\0' ? error : "请求失败");
            return;
        }

        if (err != ESP_OK) {
            chat_emit(s_ws_fd, "error",
                      error[0] != '\0' ? error : "请求失败");
            return;
        }

        if (call_count == 0) {
            history_append("assistant", s_reply);
            history_trim();
            chat_emit(s_ws_fd, "done", NULL);
            return;
        }

        history_append_tool_calls(s_calls, LLM_MAX_TOOL_CALLS);
        for (size_t i = 0; i < LLM_MAX_TOOL_CALLS && !s_abort; i++) {
            if (s_calls[i].used) {
                run_tool_call(&s_calls[i]);
            }
        }
        if (s_abort) {
            chat_emit(s_ws_fd, "aborted", NULL);
            return;
        }
        /* Loop again so the model can narrate what it just did, or chain a
         * second action ("先开灯，再把亮度调到 30"). */
    }

    /* 工具轮数耗尽时，历史最后一条是 tool 结果。再发一次禁用工具的请求，
     * 让模型把结果真正回答给用户，避免界面只收到 done 却没有正文。 */
    char *body = build_body(&config, false);
    if (body == NULL) {
        chat_emit(s_ws_fd, "error", "最终回答构造失败（内存不足）");
        return;
    }

    s_reply_length = 0;
    s_reply[0] = '\0';
    size_t ignored_call_count = 0;
    char error[192] = {0};
    esp_err_t err = llm_stream_request(&config, body, delta_callback, NULL,
                                       s_calls, &ignored_call_count, &s_abort,
                                       error, sizeof(error));
    free(body);

    if (err != ESP_OK) {
        if (s_abort) {
            if (s_reply_length > 0) {
                history_append("assistant", s_reply);
            }
            chat_emit(s_ws_fd, "aborted", NULL);
        } else {
            chat_emit(s_ws_fd, "error",
                      error[0] != '\0' ? error : "最终回答生成失败");
        }
        return;
    }

    history_append("assistant", s_reply);
    history_trim();
    chat_emit(s_ws_fd, "done", NULL);
}

static void chat_task(void *argument)
{
    (void)argument;
    char *message = NULL;

    while (xQueueReceive(s_requests, &message, portMAX_DELAY) == pdTRUE) {
        s_busy = true;
        s_abort = false;
        run_turn(message);
        free(message);
        message = NULL;
        s_busy = false;
        chat_emit(s_ws_fd, "idle", NULL);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* HTTP 配置页与连通性测试路由                                               */
/* ------------------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *request, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(request, payload);
    free(payload);
    return result;
}

static esp_err_t send_error(httpd_req_t *request, const char *status,
                            const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_custom_err(request, status, message);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    httpd_resp_set_status(request, status);
    return send_json(request, root);
}

static esp_err_t chat_page_handler(httpd_req_t *request)
{
    size_t page_size = (size_t)(chat_html_end - chat_html_start);
    if (page_size > 0 && chat_html_start[page_size - 1] == '\0') {
        page_size--;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)chat_html_start, page_size);
}

static esp_err_t config_get_handler(httpd_req_t *request)
{
    llm_config_t config;
    llm_config_load(&config);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error(request, "500 Internal Server Error", "OOM");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "base_url", config.base_url);
    cJSON_AddStringToObject(root, "model", config.model);
    cJSON_AddStringToObject(root, "system", config.system);
    cJSON_AddNumberToObject(root, "max_tokens", config.max_tokens);

    /* The key itself never leaves the board - only a tail hint so the user can
     * tell which key is stored. */
    cJSON_AddBoolToObject(root, "has_key", config.api_key[0] != '\0');
    char hint[24] = {0};
    size_t key_length = strlen(config.api_key);
    if (key_length > 4) {
        snprintf(hint, sizeof(hint), "****%s", config.api_key + key_length - 4);
    }
    cJSON_AddStringToObject(root, "key_hint", hint);

    /* News feeds live in their own namespace; the page edits them here so the
     * user has one settings panel instead of two. */
    rss_config_t news;
    rss_config_load(&news);
    cJSON_AddStringToObject(root, "news_feeds", news.feeds);

    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());

    return send_json(request, root);
}

static void copy_if_present(const cJSON *root, const char *key, char *out,
                            size_t size)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(out, item->valuestring, size);
    }
}

static esp_err_t config_post_handler(httpd_req_t *request)
{
    size_t length = request->content_len;
    if (length == 0 || length > CHAT_MAX_JSON_BODY) {
        return send_error(request, "400 Bad Request", "请求体长度不合法");
    }

    char *body = malloc(length + 1);
    if (body == NULL) {
        return send_error(request, "500 Internal Server Error", "OOM");
    }

    size_t received = 0;
    int timeouts = 0;
    while (received < length) {
        int result =
            httpd_req_recv(request, body + received, length - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                free(body);
                return send_error(request, "408 Request Timeout", "读取超时");
            }
            continue;
        }
        if (result <= 0) {
            free(body);
            return send_error(request, "400 Bad Request", "读取失败");
        }
        timeouts = 0;
        received += (size_t)result;
    }
    body[length] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_error(request, "400 Bad Request", "JSON 解析失败");
    }

    llm_config_t config;
    llm_config_load(&config);

    copy_if_present(root, "base_url", config.base_url, sizeof(config.base_url));
    copy_if_present(root, "model", config.model, sizeof(config.model));
    copy_if_present(root, "system", config.system, sizeof(config.system));

    /* An empty api_key means "keep the stored one", so the page can be saved
     * without ever sending the secret back to the browser. */
    const cJSON *key_item = cJSON_GetObjectItem(root, "api_key");
    if (cJSON_IsString(key_item) && key_item->valuestring != NULL &&
        key_item->valuestring[0] != '\0') {
        strlcpy(config.api_key, key_item->valuestring, sizeof(config.api_key));
    }

    /* Feed list is stored on its own; an empty value means "keep the default
     * two channels" rather than "no news at all". */
    const cJSON *feeds_item = cJSON_GetObjectItem(root, "news_feeds");
    bool feeds_changed = cJSON_IsString(feeds_item) &&
                         feeds_item->valuestring != NULL;
    rss_config_t news;
    if (feeds_changed) {
        rss_config_load(&news);
        strlcpy(news.feeds, feeds_item->valuestring, sizeof(news.feeds));
    }

    const cJSON *tokens_item = cJSON_GetObjectItem(root, "max_tokens");
    if (cJSON_IsNumber(tokens_item)) {
        config.max_tokens = clamp_int(tokens_item->valueint, 64, 2048);
    }
    cJSON_Delete(root);

    if (config.base_url[0] == '\0' || config.model[0] == '\0') {
        return send_error(request, "400 Bad Request", "Base URL 和模型名不能为空");
    }

    esp_err_t err = llm_config_save(&config);
    if (err != ESP_OK) {
        return send_error(request, "500 Internal Server Error",
                          esp_err_to_name(err));
    }

    if (feeds_changed) {
        err = rss_config_save(&news);
        if (err != ESP_OK) {
            return send_error(request, "500 Internal Server Error",
                              esp_err_to_name(err));
        }
    }

    /* The system prompt may have changed, so the cached history is stale.  The
     * chat task owns it and will rebuild on its next turn. */
    s_reset_history = true;
    ESP_LOGI(TAG, "LLM config saved (model=%s, base=%s)", config.model,
             config.base_url);

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        return send_error(request, "500 Internal Server Error", "OOM");
    }
    cJSON_AddBoolToObject(response, "ok", true);
    return send_json(request, response);
}

/* ------------------------------------------------------------------------- */
/* 连通性探测：用当前（含未保存）配置发起一条最小模型请求                    */
/* ------------------------------------------------------------------------- */

#define PROBE_REPLY_MAX 160
#define PROBE_PROMPT "只回复两个字：接通"

static char s_probe_reply[PROBE_REPLY_MAX];
static size_t s_probe_length;
/* File scope rather than a stack local: the array is ~2.5 KB and the httpd
 * task stack has to survive the SSE parser underneath it. */
static llm_tool_call_t s_probe_calls[LLM_MAX_TOOL_CALLS];

static bool probe_callback(const char *text, size_t length, void *user)
{
    (void)user;
    size_t room = sizeof(s_probe_reply) - 1 - s_probe_length;
    if (room > 0) {
        size_t take = length < room ? length : room;
        memcpy(s_probe_reply + s_probe_length, text, take);
        s_probe_length += take;
        s_probe_reply[s_probe_length] = '\0';
    }
    return true;
}

/* Reads the request body into `config`, so the button can test unsaved edits
 * instead of forcing a save first. */
static void overlay_config_from_body(httpd_req_t *request, llm_config_t *config)
{
    size_t length = request->content_len;
    if (length == 0 || length > CHAT_MAX_JSON_BODY) {
        return;
    }

    char *body = malloc(length + 1);
    if (body == NULL) {
        return;
    }

    size_t received = 0;
    int timeouts = 0;
    while (received < length) {
        int result =
            httpd_req_recv(request, body + received, length - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                free(body);
                return;
            }
            continue;
        }
        if (result <= 0) {
            free(body);
            return;
        }
        timeouts = 0;
        received += (size_t)result;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return;
    }

    copy_if_present(root, "base_url", config->base_url,
                    sizeof(config->base_url));
    copy_if_present(root, "model", config->model, sizeof(config->model));

    const cJSON *key_item = cJSON_GetObjectItem(root, "api_key");
    if (cJSON_IsString(key_item) && key_item->valuestring != NULL &&
        key_item->valuestring[0] != '\0') {
        strlcpy(config->api_key, key_item->valuestring,
                sizeof(config->api_key));
    }
    cJSON_Delete(root);
}

static esp_err_t llm_test_handler(httpd_req_t *request)
{
    llm_config_t config;
    llm_config_load(&config);
    overlay_config_from_body(request, &config);

    if (config.api_key[0] == '\0') {
        return send_error(request, "400 Bad Request",
                          "还没填 API Key，先在上面的输入框里粘贴一个");
    }
    if (config.base_url[0] == '\0' || config.model[0] == '\0') {
        return send_error(request, "400 Bad Request", "Base URL 和模型名不能为空");
    }

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return send_error(request, "500 Internal Server Error", "OOM");
    }
    cJSON_AddStringToObject(payload, "model", config.model);
    cJSON_AddBoolToObject(payload, "stream", true);
    cJSON_AddNumberToObject(payload, "max_tokens", 24);
    add_provider_options(payload, config.base_url);

    cJSON *messages = cJSON_AddArrayToObject(payload, "messages");
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", PROBE_PROMPT);
    cJSON_AddItemToArray(messages, message);

    char *request_body = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (request_body == NULL) {
        return send_error(request, "500 Internal Server Error", "OOM");
    }

    s_probe_length = 0;
    s_probe_reply[0] = '\0';

    volatile bool abort_flag = false;
    size_t call_count = 0;
    char error[192] = {0};

    int64_t started = esp_timer_get_time();
    esp_err_t err = llm_stream_request(&config, request_body, probe_callback,
                                       NULL, s_probe_calls, &call_count,
                                       &abort_flag, error, sizeof(error));
    int latency_ms = (int)((esp_timer_get_time() - started) / 1000);
    free(request_body);

    ESP_LOGI(TAG, "probe %s in %d ms (%s)", err == ESP_OK ? "ok" : "failed",
             latency_ms, error);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return send_error(request, "500 Internal Server Error", "OOM");
    }
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(result, "latency_ms", latency_ms);
    cJSON_AddStringToObject(result, "model", config.model);
    cJSON_AddStringToObject(result, "reply", s_probe_reply);
    cJSON_AddNumberToObject(result, "free_heap", esp_get_free_heap_size());
    if (err != ESP_OK) {
        cJSON_AddStringToObject(result, "error",
                                error[0] != '\0' ? error : "请求失败");
    }
    return send_json(request, result);
}

static void send_hello(int fd)
{
    cJSON *hello = cJSON_CreateObject();
    if (hello == NULL) {
        return;
    }
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddBoolToObject(hello, "configured", llm_config_ready());
    cJSON_AddBoolToObject(hello, "busy", s_busy);
    cJSON_AddNumberToObject(hello, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(hello, "min_free_heap",
                            esp_get_minimum_free_heap_size());
    ws_send_json(fd, hello);
}

static void handle_client_message(const char *text, int fd)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        /* The browser pings on open; this is how the page learns the heap and
         * whether an API key is configured, since the handshake itself never
         * reaches this module. */
        send_hello(fd);
    } else if (strcmp(type->valuestring, "user") == 0) {
        const cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text_item) && text_item->valuestring != NULL &&
            text_item->valuestring[0] != '\0') {
            char *copy = strdup(text_item->valuestring);
            if (copy == NULL) {
                chat_emit(fd, "error", "内存不足");
            } else if (xQueueSend(s_requests, &copy, 0) != pdTRUE) {
                free(copy);
                chat_emit(fd, "error", "上一条还在回答中，请稍候");
            }
        }
    } else if (strcmp(type->valuestring, "abort") == 0) {
        if (s_busy) {
            s_abort = true;
        }
    } else if (strcmp(type->valuestring, "clear") == 0) {
        if (!s_busy) {
            s_reset_history = true;
        }
        chat_emit(fd, "cleared", NULL);
    }

    cJSON_Delete(root);
}

static esp_err_t ws_handler(httpd_req_t *request)
{
    /* esp_http_server answers the upgrade request itself and returns before
     * calling this handler - there is no handshake callback to learn the
     * socket from.  The fd therefore comes from the first frame, and every
     * later frame refreshes it, which also makes a reconnecting browser
     * self-healing instead of needing a reboot. */
    int fd = httpd_req_to_sockfd(request);
    if (fd >= 0 && fd != s_ws_fd) {
        s_ws_fd = fd;
        ESP_LOGI(TAG, "chat session on fd %d (free heap %u)", fd,
                 (unsigned)esp_get_free_heap_size());
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t err = httpd_ws_recv_frame(request, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.len == 0 || frame.len > CHAT_INPUT_MAX) {
        return ESP_OK;
    }

    uint8_t *payload = malloc(frame.len + 1);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = payload;

    err = httpd_ws_recv_frame(request, &frame, frame.len);
    if (err == ESP_OK) {
        payload[frame.len] = '\0';
        handle_client_message((const char *)payload, fd);
    }
    free(payload);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* 路由与后台对话任务注册                                                    */
/* ------------------------------------------------------------------------- */

esp_err_t chat_register(httpd_handle_t server)
{
    s_server = server;

    s_requests = xQueueCreate(CHAT_QUEUE_DEPTH, sizeof(char *));
    if (s_requests == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(chat_task, "chat", CHAT_TASK_STACK, NULL,
                    CHAT_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/chat", .method = HTTP_GET, .handler = chat_page_handler},
        {.uri = "/api/llm/config",
         .method = HTTP_GET,
         .handler = config_get_handler},
        {.uri = "/api/llm/config",
         .method = HTTP_POST,
         .handler = config_post_handler},
        {.uri = "/api/llm/test",
         .method = HTTP_POST,
         .handler = llm_test_handler},
        {.uri = "/ws/chat",
         .method = HTTP_GET,
         .handler = ws_handler,
         .is_websocket = true},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "chat panel ready at /chat");
    return ESP_OK;
}
