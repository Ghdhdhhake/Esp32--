#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "rss_news.h"

static const char *TAG = "rss";

#define NVS_NAMESPACE "news_cfg"
#define NVS_KEY_FEEDS "feeds"

/* Two verified 中新网 channels: 要闻导读 for the headlines, 即时新闻 for
 * whatever just broke.  Both return ~30 items and update continuously. */
#define DEFAULT_FEEDS                                                     \
    "https://www.chinanews.com.cn/rss/importnews.xml\n"                   \
    "https://www.chinanews.com.cn/rss/scroll-news.xml"

#define FEED_TIMEOUT_MS 15000
#define FEED_HTTP_BUF 1024
/* A 中新网 feed is ~16 KB.  Truncating mid-feed is harmless here because we
 * scan for complete <item> blocks, so the first few items still parse. */
#define FEED_BODY_MAX 24576

/* ------------------------------------------------------------------------- */
/* 配置                                                                      */
/* ------------------------------------------------------------------------- */

void rss_config_defaults(rss_config_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->feeds, DEFAULT_FEEDS, sizeof(out->feeds));
}

esp_err_t rss_config_load(rss_config_t *out)
{
    rss_config_defaults(out);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t length = sizeof(out->feeds);
    (void)nvs_get_str(handle, NVS_KEY_FEEDS, out->feeds, &length);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t rss_config_save(const rss_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_FEEDS, config->feeds);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool rss_config_ready(const rss_config_t *config)
{
    return config->feeds[0] != '\0' && config->feeds[0] != '#';
}

/* ------------------------------------------------------------------------- */
/* XML 小工具                                                                */
/* ------------------------------------------------------------------------- */

/* Copy `length` bytes of `in` into `out`, dropping markup and decoding the
 * entities that actually show up in feeds.  Whitespace is collapsed because
 * the raw text carries indentation and the model does not need it. */
static void sanitize(const char *in, size_t length, char *out, size_t out_size,
                     size_t limit)
{
    if (out_size == 0) {
        return;
    }

    size_t written = 0;
    bool inside_tag = false;

    for (size_t i = 0; i < length && written + 1 < out_size && written < limit;
         i++) {
        char character = in[i];

        if (character == '<') {
            /* CDATA is content, not markup. */
            if (strncmp(in + i, "<![CDATA[", 9) == 0) {
                i += 8;
                continue;
            }
            if (strncmp(in + i, "]]>", 3) == 0) {
                i += 2;
                continue;
            }
            inside_tag = true;
            continue;
        }
        if (character == '>') {
            inside_tag = false;
            continue;
        }
        if (inside_tag) {
            continue;
        }

        if (character == '&') {
            if (strncmp(in + i, "&amp;", 5) == 0) {
                character = '&';
                i += 4;
            } else if (strncmp(in + i, "&lt;", 4) == 0) {
                character = '<';
                i += 3;
            } else if (strncmp(in + i, "&gt;", 4) == 0) {
                character = '>';
                i += 3;
            } else if (strncmp(in + i, "&quot;", 6) == 0) {
                character = '"';
                i += 5;
            } else if (strncmp(in + i, "&apos;", 6) == 0) {
                character = '\'';
                i += 5;
            } else if (strncmp(in + i, "&#39;", 5) == 0) {
                character = '\'';
                i += 4;
            } else if (strncmp(in + i, "&nbsp;", 6) == 0) {
                character = ' ';
                i += 5;
            } else if (strncmp(in + i, "&#", 2) == 0) {
                /* Numeric entities are rare in Chinese feeds; drop the whole
                 * escape rather than emit half of it. */
                const char *semicolon = strchr(in + i, ';');
                if (semicolon != NULL && semicolon - (in + i) < 12) {
                    i = (size_t)(semicolon - in);
                }
                continue;
            }
        }

        if (character == '\n' || character == '\r' || character == '\t') {
            character = ' ';
        }
        if (character == ' ' && written > 0 && out[written - 1] == ' ') {
            continue;
        }
        out[written++] = character;
    }

    /* Trim trailing space that the collapse above can leave behind. */
    while (written > 0 && out[written - 1] == ' ') {
        written--;
    }
    out[written] = '\0';
}

/* Pull the text of `<tag ...>...</tag>` out of `block`. */
static bool extract_tag(const char *block, const char *tag, char *out,
                        size_t out_size, size_t limit)
{
    char opener[24];
    char closer[24];
    snprintf(opener, sizeof(opener), "<%s", tag);
    snprintf(closer, sizeof(closer), "</%s>", tag);

    const char *start = strstr(block, opener);
    if (start == NULL) {
        return false;
    }
    const char *opener_end = strchr(start, '>');
    if (opener_end == NULL) {
        return false;
    }
    start = opener_end + 1;

    const char *end = strstr(start, closer);
    if (end == NULL) {
        return false;
    }

    sanitize(start, (size_t)(end - start), out, out_size, limit);
    return out[0] != '\0';
}

/* ------------------------------------------------------------------------- */
/* 抓取                                                                      */
/* ------------------------------------------------------------------------- */

static esp_err_t fetch(const char *url, char **body_out)
{
    *body_out = NULL;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = FEED_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = FEED_HTTP_BUF,
        .buffer_size_tx = FEED_HTTP_BUF,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;
    char *body = NULL;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        goto cleanup;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        goto cleanup;
    }
    if (esp_http_client_get_status_code(client) != 200) {
        goto cleanup;
    }

    body = malloc(FEED_BODY_MAX);
    if (body == NULL) {
        goto cleanup;
    }

    size_t total = 0;
    while (total < FEED_BODY_MAX - 1) {
        int received =
            esp_http_client_read(client, body + total, FEED_BODY_MAX - 1 - total);
        if (received <= 0) {
            break;
        }
        total += (size_t)received;
    }
    body[total] = '\0';

    if (total == 0) {
        goto cleanup;
    }

    *body_out = body;
    body = NULL;
    result = ESP_OK;

cleanup:
    free(body);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static bool contains_ci(const char *haystack, const char *needle)
{
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    size_t needle_length = strlen(needle);
    for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
        size_t matched = 0;
        while (matched < needle_length) {
            char a = cursor[matched];
            char b = needle[matched];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b || a == '\0') {
                break;
            }
            matched++;
        }
        if (matched == needle_length) {
            return true;
        }
    }
    return false;
}

/* Append up to `remaining` items from one feed. */
static int harvest(const char *xml, const char *source, const char *keyword,
                   cJSON *out, int remaining)
{
    static const char *const BLOCKS[] = {"item", "entry"};
    int added = 0;

    for (size_t kind = 0; kind < sizeof(BLOCKS) / sizeof(BLOCKS[0]); kind++) {
        char opener[16];
        char closer[16];
        snprintf(opener, sizeof(opener), "<%s>", BLOCKS[kind]);
        snprintf(closer, sizeof(closer), "</%s>", BLOCKS[kind]);
        size_t opener_length = strlen(opener);

        const char *cursor = xml;
        while (added < remaining) {
            const char *start = strstr(cursor, opener);
            if (start == NULL) {
                break;
            }
            start += opener_length;
            const char *end = strstr(start, closer);
            if (end == NULL) {
                break;
            }
            cursor = end + strlen(closer);

            /* extract_tag works on a NUL-terminated string, so copy the block
             * out first. */
            size_t block_length = (size_t)(end - start);
            char *block = malloc(block_length + 1);
            if (block == NULL) {
                return added;
            }
            memcpy(block, start, block_length);
            block[block_length] = '\0';

            char title[RSS_TITLE_MAX];
            char text[RSS_TEXT_MAX];
            char date[48];
            char link[160];

            bool has_title = extract_tag(block, "title", title, sizeof(title),
                                         RSS_TITLE_MAX - 1);
            bool has_text = extract_tag(block, "description", text,
                                        sizeof(text), RSS_TEXT_MAX - 1);
            if (!has_text) {
                has_text = extract_tag(block, "summary", text, sizeof(text),
                                       RSS_TEXT_MAX - 1);
            }
            bool has_date = extract_tag(block, "pubDate", date, sizeof(date),
                                        sizeof(date) - 1);
            if (!has_date) {
                has_date = extract_tag(block, "updated", date, sizeof(date),
                                       sizeof(date) - 1);
            }
            bool has_link = extract_tag(block, "link", link, sizeof(link),
                                        sizeof(link) - 1);
            free(block);

            if (!has_title) {
                continue;
            }
            if (keyword != NULL && keyword[0] != '\0' &&
                !contains_ci(title, keyword) &&
                !(has_text && contains_ci(text, keyword))) {
                continue;
            }

            cJSON *entry = cJSON_CreateObject();
            if (entry == NULL) {
                return added;
            }
            cJSON_AddStringToObject(entry, "source", source);
            cJSON_AddStringToObject(entry, "title", title);
            if (has_date) {
                cJSON_AddStringToObject(entry, "date", date);
            }
            if (has_text) {
                cJSON_AddStringToObject(entry, "text", text);
            }
            if (has_link) {
                cJSON_AddStringToObject(entry, "link", link);
            }
            cJSON_AddItemToArray(out, entry);
            added++;
        }
    }
    return added;
}

/* Feed title, used to label which source an item came from. */
static void feed_title(const char *xml, char *out, size_t out_size)
{
    /* The channel's <title> is the first one in the document. */
    if (!extract_tag(xml, "title", out, out_size, out_size - 1)) {
        strlcpy(out, "新闻源", out_size);
    }
}

esp_err_t rss_news_run(const char *feeds, int count, const char *keyword,
                       cJSON **out, char *error, size_t error_size)
{
    *out = NULL;

    if (feeds == NULL || feeds[0] == '\0') {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "还没有配置新闻源");
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (count < 1) {
        count = 6;
    }
    if (count > RSS_MAX_ITEMS) {
        count = RSS_MAX_ITEMS;
    }

    cJSON *results = cJSON_CreateArray();
    if (results == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "内存不足");
        }
        return ESP_ERR_NO_MEM;
    }

    int failures = 0;
    int feed_index = 0;
    int last_error = 0;

    char *copy = strdup(feeds);
    if (copy == NULL) {
        cJSON_Delete(results);
        return ESP_ERR_NO_MEM;
    }

    for (char *line = strtok(copy, "\r\n"); line != NULL;
         line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == ' ' || line[length - 1] == '\t')) {
            line[--length] = '\0';
        }
        if (length == 0 || line[0] == '#') {
            continue;
        }

        int remaining = count - cJSON_GetArraySize(results);
        if (remaining <= 0) {
            break;
        }

        char *xml = NULL;
        esp_err_t err = fetch(line, &xml);
        if (err != ESP_OK || xml == NULL) {
            failures++;
            last_error = err;
            ESP_LOGW(TAG, "feed %d failed: %s", feed_index,
                     esp_err_to_name(err));
            free(xml);
            feed_index++;
            continue;
        }

        char source[64];
        feed_title(xml, source, sizeof(source));
        int added = harvest(xml, source, keyword, results, remaining);
        ESP_LOGI(TAG, "%s -> %d item(s)", source, added);
        free(xml);
        feed_index++;
    }

    free(copy);

    if (cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(results);
        if (error != NULL && error_size > 0) {
            if (failures > 0) {
                snprintf(error, error_size,
                         "新闻源都抓取失败（%d 个，最后一个 %s）", failures,
                         esp_err_to_name(last_error));
            } else if (keyword != NULL && keyword[0] != '\0') {
                snprintf(error, error_size, "新闻源里没有含「%s」的条目",
                         keyword);
            } else {
                snprintf(error, error_size, "新闻源里没有可用的条目");
            }
        }
        return ESP_FAIL;
    }

    *out = results;
    return ESP_OK;
}
