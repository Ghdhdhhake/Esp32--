#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * 面向 OpenAI 兼容接口的最小流式客户端声明。配置与网络传输集中在此模块，
 * 调用者只提供请求 JSON、取消标志以及接收文本片段的回调。
 *
 * Minimal OpenAI-compatible chat client.
 *
 * The board does not run a model - it forwards the conversation to a hosted
 * API over TLS and streams the answer back to the browser one token at a time.
 * That split is deliberate: a 4-bit 0.5B model needs a few hundred megabytes of
 * RAM, three orders of magnitude more than the ~185 KB this board has.
 *
 * Everything the TLS layer needs is already in the image (mbedTLS, esp-tls and
 * the 200-entry public CA bundle), so this module only has to speak HTTP.
 */

#define LLM_BASE_URL_MAX 192
#define LLM_API_KEY_MAX 192
#define LLM_MODEL_MAX 64
#define LLM_SYSTEM_MAX 256

/* 单次请求最多包含的工具数；参数文本也设置上限，防止异常模型流耗尽堆内存。 */
#define LLM_MAX_TOOL_CALLS 4
#define LLM_TOOL_ARGS_MAX 512
#define LLM_TOOL_NAME_MAX 40
#define LLM_TOOL_ID_MAX 48

typedef struct {
    char base_url[LLM_BASE_URL_MAX];
    char api_key[LLM_API_KEY_MAX];
    char model[LLM_MODEL_MAX];
    char system[LLM_SYSTEM_MAX];
    int max_tokens;
} llm_config_t;

typedef struct {
    int index;
    bool used;
    bool truncated;
    char id[LLM_TOOL_ID_MAX];
    char name[LLM_TOOL_NAME_MAX];
    char arguments[LLM_TOOL_ARGS_MAX];
    size_t arguments_len;
} llm_tool_call_t;

/** 每收到一个文本分片就调用一次；返回 false 可中止当前流。 */
typedef bool (*llm_delta_cb_t)(const char *text, size_t length, void *user);

/** 先写入默认配置，再覆盖 NVS 中已保存的字段。 */
void llm_config_defaults(llm_config_t *out);

/** 读取持久化配置；缺失字段保持默认值。 */
esp_err_t llm_config_load(llm_config_t *out);

/** 持久化完整配置（四个文本字段与 max_tokens）。 */
esp_err_t llm_config_save(const llm_config_t *config);

/** API Key 已保存时返回 true，聊天页据此决定能否发起对话。 */
bool llm_config_ready(void);

/**
 * 将已序列化的 `body` POST 到 `config->base_url`，并流式读取 SSE 响应。
 *
 * - 每个 `delta.content` 文本片段都会交给 `on_delta`
 * - `delta.tool_calls` 分片会累积到 `tool_calls`，调用方必须提供
 *   LLM_MAX_TOOL_CALLS 个槽位
 * - 每次读取之间检查 `abort`，使网页可取消长时间回答
 *
 * 失败时向非空 `error` 写入简短、可读的错误原因。
 */
esp_err_t llm_stream_request(const llm_config_t *config, const char *body,
                             llm_delta_cb_t on_delta, void *user,
                             llm_tool_call_t *tool_calls, size_t *tool_count,
                             volatile bool *abort, char *error,
                             size_t error_size);
