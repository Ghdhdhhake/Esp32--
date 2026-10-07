#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * RSS/Atom 新闻源接口：配置以多行 URL 保存，运行时返回已清洗的 JSON 条目。
 * 由调用者持有并释放成功返回的 cJSON 数组。
 *
 * RSS / Atom 新闻抓取 —— 不需要任何 API Key。
 *
 * 模型自己不会上网，板子在线也不等于模型知道今天的新闻。这个模块让板子
 * 去拉几个新闻源的 RSS，把标题和摘要喂回上下文，模型才有东西可总结。
 *
 * 只做够用的事：扫 <item> / <entry> 块，抠出标题、时间、正文，
 * 剥掉 HTML 标签和实体。不引入 XML 库 —— 一个完整的 expat 对这里
 * 是杀鸡用牛刀，而且 RSS 的形状足够规整。
 */

#define RSS_FEEDS_MAX 512
#define RSS_MAX_ITEMS 12
#define RSS_TITLE_MAX 160
#define RSS_TEXT_MAX 300

typedef struct {
    /* 一行一个 URL，# 开头当注释。 */
    char feeds[RSS_FEEDS_MAX];
} rss_config_t;

void rss_config_defaults(rss_config_t *out);
esp_err_t rss_config_load(rss_config_t *out);
esp_err_t rss_config_save(const rss_config_t *config);

/** 至少配置一个非注释新闻源 URL 时返回 true。 */
bool rss_config_ready(const rss_config_t *config);

/**
 * 依次抓取 feeds 里的每个源，最多返回 count 条。
 *
 * @param keyword 非空时只保留标题或正文里含该词（不区分大小写）的条目
 * @param out     成功时返回新建的 JSON 数组，调用方负责 cJSON_Delete
 */
esp_err_t rss_news_run(const char *feeds, int count, const char *keyword,
                       cJSON **out, char *error, size_t error_size);
