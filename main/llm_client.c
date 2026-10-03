#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "llm_client.h"

static const char *TAG = "llm";

#define NVS_NAMESPACE "llm_cfg"
#define NVS_KEY_URL "base_url"
#define NVS_KEY_TOKEN "api_key"
#define NVS_KEY_MODEL "model"
#define NVS_KEY_SYSTEM "system"
#define NVS_KEY_TOKENS "max_tokens"

#define DEFAULT_BASE_URL "https://api.deepseek.com/chat/completions"
/* "deepseek-flash" is the current V4.1 Flash route.  The older aliases
 * (deepseek-v4-flash, deepseek-chat) still resolve, but new setups should name
 * the model they actually want. */
#define DEFAULT_MODEL "deepseek-flash"
#define DEFAULT_SYSTEM \
    "你是接在 ESP32 开发板上的助手。回答简短口语化，尽量控制在 80 字以内。" \
    "涉及新闻、时事、最新动态这类训练数据之后才发生的事实时，" \
    "必须先调用 read_news 查一下再回答，不要凭记忆编。没有查到就说不知道。" \
    "需要操作硬件时调用提供的工具，不要凭空猜测引脚状态。"
#define DEFAULT_MAX_TOKENS 512
#define MIN_MAX_TOKENS 64
#define MAX_MAX_TOKENS 2048

/* Defaults shipped before read_news existed.  Recognising them lets the
 * upgrade happen automatically instead of asking the user to retype the
 * prompt.  A prompt the user edited themselves matches neither and is left
 * alone. */
#define LEGACY_SYSTEM_V1 \
    "你是接在 ESP32 开发板上的助手。回答简短口语化，尽量控制在 80 字以内。" \
    "需要操作硬件时调用提供的工具，不要凭空猜测引脚状态。"
#define LEGACY_SYSTEM_V2 \
    "你是接在 ESP32 开发板上的助手。回答简短口语化，尽量控制在 80 字以内。" \
    "涉及新闻、时事、天气、股价、比赛结果这类训练数据之后才发生的事实时，" \
    "必须先调用 web_search 查一下再回答，不要凭记忆编。没有搜索结果就说不知道。" \
    "需要操作硬件时调用提供的工具，不要凭空猜测引脚状态。"

/* One SSE line rarely exceeds a few hundred bytes; 1024 keeps the parser
 * buffer small enough to live on the stack of the chat task. */
#define SSE_LINE_MAX 1024
#define HTTP_TIMEOUT_MS 30000
#define HTTP_BUF_SIZE 1024

/* ------------------------------------------------------------------------- */
/* Configuration (NVS)                                                       */
/* ------------------------------------------------------------------------- */

void llm_config_defaults(llm_config_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->base_url, DEFAULT_BASE_URL, sizeof(out->base_url));
    strlcpy(out->model, DEFAULT_MODEL, sizeof(out->model));
    strlcpy(out->system, DEFAULT_SYSTEM, sizeof(out->system));
    out->max_tokens = DEFAULT_MAX_TOKENS;
}

static void nvs_read_str(nvs_handle_t handle, const char *key, char *out,
                         size_t size)
{
    size_t length = size;
    /* On any failure `out` keeps whatever the caller already put there, which
     * is how the defaults survive a partially written namespace. */
    (void)nvs_get_str(handle, key, out, &length);
}

esp_err_t llm_config_load(llm_config_t *out)
{
    llm_config_defaults(out);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err; /* nothing stored yet - defaults are already in place */
    }

    nvs_read_str(handle, NVS_KEY_URL, out->base_url, sizeof(out->base_url));
    nvs_read_str(handle, NVS_KEY_TOKEN, out->api_key, sizeof(out->api_key));
    nvs_read_str(handle, NVS_KEY_MODEL, out->model, sizeof(out->model));
    nvs_read_str(handle, NVS_KEY_SYSTEM, out->system, sizeof(out->system));

    /* The default prompt gained the read_news instruction.  Anyone still on an
     * earlier default would never see it, because the stored value wins
     * forever.  Migrate those exact strings only - a prompt the user wrote
     * themselves is left untouched. */
    if (strcmp(out->system, LEGACY_SYSTEM_V1) == 0 ||
        strcmp(out->system, LEGACY_SYSTEM_V2) == 0) {
        strlcpy(out->system, DEFAULT_SYSTEM, sizeof(out->system));
        ESP_LOGI(TAG, "system prompt upgraded to the read_news-aware default");
    }

    int32_t tokens = 0;
    if (nvs_get_i32(handle, NVS_KEY_TOKENS, &tokens) == ESP_OK &&
        tokens >= MIN_MAX_TOKENS && tokens <= MAX_MAX_TOKENS) {
        out->max_tokens = (int)tokens;
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t llm_config_save(const llm_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_URL, config->base_url);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_TOKEN, config->api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_MODEL, config->model);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_SYSTEM, config->system);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, NVS_KEY_TOKENS, config->max_tokens);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

bool llm_config_ready(void)
{
    llm_config_t config;
    llm_config_load(&config);
    return config.api_key[0] != '\0';
}

/* ------------------------------------------------------------------------- */
/* SSE stream parsing                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    const llm_config_t *config;
    llm_delta_cb_t on_delta;
    void *user;
    llm_tool_call_t *tools;
    volatile bool *abort;
    char line[SSE_LINE_MAX];
    size_t line_length;
    bool overflow;
    bool saw_done;
    bool aborted;
} sse_context_t;

static void bounded_append(char *destination, size_t capacity, size_t *length,
                           const char *source)
{
    size_t available = capacity - 1 - *length;
    size_t incoming = strlen(source);

    if (incoming > available) {
        incoming = available;
    }
    if (incoming == 0) {
        return;
    }

    memcpy(destination + *length, source, incoming);
    *length += incoming;
    destination[*length] = '\0';
}

/* Tool call deltas are keyed by `index` and arrive split across many frames:
 * the id and name come first, then the JSON arguments a few characters at a
 * time.  Slots are claimed on first sight of an index. */
static llm_tool_call_t *tool_slot(sse_context_t *context, int index)
{
    for (size_t i = 0; i < LLM_MAX_TOOL_CALLS; i++) {
        if (context->tools[i].used && context->tools[i].index == index) {
            return &context->tools[i];
        }
    }
    for (size_t i = 0; i < LLM_MAX_TOOL_CALLS; i++) {
        if (!context->tools[i].used) {
            context->tools[i].used = true;
            context->tools[i].index = index;
            context->tools[i].arguments[0] = '\0';
            context->tools[i].arguments_len = 0;
            return &context->tools[i];
        }
    }
    return NULL;
}

static void handle_delta_object(sse_context_t *context, const cJSON *delta)
{
    const cJSON *content = cJSON_GetObjectItem(delta, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL &&
        context->on_delta != NULL) {
        if (!context->on_delta(content->valuestring,
                               strlen(content->valuestring), context->user)) {
            context->aborted = true;
        }
    }

    const cJSON *calls = cJSON_GetObjectItem(delta, "tool_calls");
    if (!cJSON_IsArray(calls)) {
        return;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, calls)
    {
        const cJSON *index_item = cJSON_GetObjectItem(item, "index");
        int index = cJSON_IsNumber(index_item) ? index_item->valueint : 0;

        llm_tool_call_t *slot = tool_slot(context, index);
        if (slot == NULL) {
            ESP_LOGW(TAG, "more than %d tool calls in one answer; ignoring",
                     LLM_MAX_TOOL_CALLS);
            continue;
        }

        const cJSON *id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(id) && id->valuestring != NULL) {
            strlcpy(slot->id, id->valuestring, sizeof(slot->id));
        }

        const cJSON *function = cJSON_GetObjectItem(item, "function");
        if (!cJSON_IsObject(function)) {
            continue;
        }

        const cJSON *name = cJSON_GetObjectItem(function, "name");
        if (cJSON_IsString(name) && name->valuestring != NULL) {
            strlcpy(slot->name, name->valuestring, sizeof(slot->name));
        }

        const cJSON *arguments = cJSON_GetObjectItem(function, "arguments");
        if (cJSON_IsString(arguments) && arguments->valuestring != NULL) {
            bounded_append(slot->arguments, sizeof(slot->arguments),
                           &slot->arguments_len, arguments->valuestring);
        }
    }
}

static void handle_sse_line(sse_context_t *context, const char *line,
                            size_t length)
{
    static const char prefix[] = "data:";
    if (length < sizeof(prefix) - 1 ||
        strncmp(line, prefix, sizeof(prefix) - 1) != 0) {
        return; /* comments, blank separators, event: headers */
    }

    const char *payload = line + sizeof(prefix) - 1;
    while (*payload == ' ') {
        payload++;
    }

    if (strcmp(payload, "[DONE]") == 0) {
        context->saw_done = true;
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return;
    }

    const cJSON *choices = cJSON_GetObjectItem(root, "choices");
    const cJSON *choice =
        cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *delta =
        cJSON_IsObject(choice) ? cJSON_GetObjectItem(choice, "delta") : NULL;
    if (cJSON_IsObject(delta)) {
        handle_delta_object(context, delta);
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------------- */
/* Request                                                                   */
/* ------------------------------------------------------------------------- */

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

/* A bare status code tells the user nothing actionable, and "HTTP 401" alone
 * sends people hunting for the wrong problem. */
static const char *http_status_hint(int status)
{
    switch (status) {
    case 400:
        return "请求格式被拒绝";
    case 401:
        return "API Key 无效";
    case 402:
        return "账户余额不足";
    case 403:
        return "无权访问该模型";
    case 404:
        return "Base URL 路径不对，应指向 /chat/completions";
    case 422:
        return "参数不合法";
    case 429:
        return "请求过于频繁，稍后再试";
    case 500:
    case 502:
    case 503:
    case 504:
        return "服务端临时故障";
    default:
        return NULL;
    }
}

/* Providers wrap the useful sentence in {"error":{"message":"..."}}; showing
 * the raw JSON instead costs the user a round of guessing. */
static void describe_error_body(const char *body, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        strlcpy(out, body, out_size);
        return;
    }

    const cJSON *error = cJSON_GetObjectItem(root, "error");
    const cJSON *message = NULL;
    if (cJSON_IsObject(error)) {
        message = cJSON_GetObjectItem(error, "message");
    } else if (cJSON_IsString(error)) {
        message = error;
    }
    if (message == NULL) {
        message = cJSON_GetObjectItem(root, "message");
    }

    if (cJSON_IsString(message) && message->valuestring != NULL) {
        strlcpy(out, message->valuestring, out_size);
    } else {
        strlcpy(out, body, out_size);
    }
    cJSON_Delete(root);
}

esp_err_t llm_stream_request(const llm_config_t *config, const char *body,
                             llm_delta_cb_t on_delta, void *user,
                             llm_tool_call_t *tool_calls, size_t *tool_count,
                             volatile bool *abort, char *error,
                             size_t error_size)
{
    memset(tool_calls, 0, sizeof(llm_tool_call_t) * LLM_MAX_TOOL_CALLS);
    *tool_count = 0;

    if (config->base_url[0] == '\0' || config->api_key[0] == '\0') {
        set_error(error, error_size, "尚未配置 API Key");
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t http_config = {
        .url = config->base_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF_SIZE,
        .buffer_size_tx = HTTP_BUF_SIZE,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        set_error(error, error_size, "无法创建 HTTP 客户端");
        return ESP_FAIL;
    }

    char authorization[LLM_API_KEY_MAX + 16];
    snprintf(authorization, sizeof(authorization), "Bearer %s",
             config->api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Accept", "text/event-stream");

    esp_err_t result = ESP_FAIL;
    size_t body_length = strlen(body);

    if (esp_http_client_open(client, body_length) != ESP_OK) {
        set_error(error, error_size, "连接失败（检查 Base URL 与网络）");
        goto cleanup;
    }

    if (esp_http_client_write(client, body, body_length) !=
        (int)body_length) {
        set_error(error, error_size, "请求发送失败");
        goto cleanup;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        set_error(error, error_size, "响应头读取失败");
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        char raw[256];
        int got = esp_http_client_read(client, raw, sizeof(raw) - 1);
        raw[got > 0 ? got : 0] = '\0';

        char detail[160];
        describe_error_body(raw, detail, sizeof(detail));

        const char *hint = http_status_hint(status);
        if (hint != NULL) {
            snprintf(error, error_size, "HTTP %d（%s）%.90s", status, hint,
                     detail);
        } else {
            snprintf(error, error_size, "HTTP %d %.110s", status, detail);
        }
        goto cleanup;
    }

    ESP_LOGI(TAG, "streaming %u bytes to %s (free heap %u)", (unsigned)body_length,
             config->model, (unsigned)esp_get_free_heap_size());

    sse_context_t context = {
        .config = config,
        .on_delta = on_delta,
        .user = user,
        .tools = tool_calls,
        .abort = abort,
    };

    char chunk[HTTP_BUF_SIZE];
    while (true) {
        if (abort != NULL && *abort) {
            context.aborted = true;
            break;
        }

        int received = esp_http_client_read(client, chunk, sizeof(chunk));
        if (received < 0) {
            set_error(error, error_size, "连接中断");
            goto cleanup;
        }
        if (received == 0) {
            break; /* terminating chunk reached or peer closed */
        }

        for (int i = 0; i < received; i++) {
            char character = chunk[i];
            if (character != '\n') {
                if (context.line_length < sizeof(context.line) - 1) {
                    context.line[context.line_length++] = character;
                } else {
                    context.overflow = true;
                }
                continue;
            }

            if (!context.overflow) {
                context.line[context.line_length] = '\0';
                if (context.line_length > 0 &&
                    context.line[context.line_length - 1] == '\r') {
                    context.line[--context.line_length] = '\0';
                }
                handle_sse_line(&context, context.line,
                                context.line_length);
            }
            context.line_length = 0;
            context.overflow = false;
        }

        if (context.aborted || context.saw_done) {
            break;
        }
    }

    size_t used = 0;
    for (size_t i = 0; i < LLM_MAX_TOOL_CALLS; i++) {
        if (tool_calls[i].used) {
            used++;
        }
    }
    *tool_count = used;

    if (context.aborted) {
        result = ESP_ERR_INVALID_STATE; /* caller treats this as "cancelled" */
    } else {
        result = ESP_OK;
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}
