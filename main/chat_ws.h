#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Web chat panel.
 *
 * Registers four routes on an already running server:
 *
 *   GET  /chat              the chat page
 *   GET  /api/llm/config    current endpoint / model (never the key itself)
 *   POST /api/llm/config    store endpoint / key / model in NVS
 *   GET  /ws/chat           WebSocket carrying the conversation
 *
 * The board is a gateway, not a model host: it owns the API key, holds the
 * conversation history and executes the tool calls the model asks for, while
 * the browser only renders tokens.
 */
esp_err_t chat_register(httpd_handle_t server);
