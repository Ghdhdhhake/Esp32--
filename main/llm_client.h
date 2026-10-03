#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/**
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

/* One request can ask for at most this many tools, and each argument blob is
 * capped so a runaway model cannot exhaust the heap mid-stream. */
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
    char id[LLM_TOOL_ID_MAX];
    char name[LLM_TOOL_NAME_MAX];
    char arguments[LLM_TOOL_ARGS_MAX];
    size_t arguments_len;
} llm_tool_call_t;

/** Called for every content fragment. Return false to abort the stream. */
typedef bool (*llm_delta_cb_t)(const char *text, size_t length, void *user);

/** Fill `out` with defaults, then overlay whatever is stored in NVS. */
void llm_config_defaults(llm_config_t *out);

/** Read the persisted configuration. Missing keys keep their defaults. */
esp_err_t llm_config_load(llm_config_t *out);

/** Persist the configuration (all four fields plus max_tokens). */
esp_err_t llm_config_save(const llm_config_t *config);

/** True once an API key has been stored - the chat panel gates on this. */
bool llm_config_ready(void);

/**
 * POST `body` (already serialized JSON) to `config->base_url` and stream the
 * SSE response.
 *
 * - every `delta.content` fragment goes to `on_delta`
 * - `delta.tool_calls` fragments are accumulated into `tool_calls`, which the
 *   caller must size to LLM_MAX_TOOL_CALLS
 * - `abort` is polled between reads so the UI can cancel a long answer
 *
 * On failure `error` (if non-NULL) receives a short human-readable reason.
 */
esp_err_t llm_stream_request(const llm_config_t *config, const char *body,
                             llm_delta_cb_t on_delta, void *user,
                             llm_tool_call_t *tool_calls, size_t *tool_count,
                             volatile bool *abort, char *error,
                             size_t error_size);
