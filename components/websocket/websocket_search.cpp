#include "websocket_internal.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "WS_SEARCH";
#define SEARCH_HTTP_MAX 10000
#define SEARCH_QUERY_MAX 240
#define SEARCH_RESULT_MAX 1800

typedef struct {
    char id[96];
    char query[SEARCH_QUERY_MAX + 1];
} search_job_t;

typedef struct {
    char data[SEARCH_HTTP_MAX + 1];
    size_t used;
} search_http_buffer_t;

static esp_err_t search_http_event(esp_http_client_event_t *evt)
{
    search_http_buffer_t *b = (search_http_buffer_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && evt->data && evt->data_len > 0) {
        size_t n = (size_t)evt->data_len;
        if (n > SEARCH_HTTP_MAX - b->used) n = SEARCH_HTTP_MAX - b->used;
        if (n) {
            memcpy(b->data + b->used, evt->data, n);
            b->used += n;
            b->data[b->used] = '\0';
        }
    }
    return ESP_OK;
}

static void append_text(char *out, size_t cap, const char *s, size_t n)
{
    size_t used = strlen(out);
    for (size_t i = 0; i < n && used + 1 < cap; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '<') {
            while (i < n && s[i] != '>') ++i;
        } else if (c == '&') {
            if (i + 4 < n && strncmp(s + i, "&amp;", 5) == 0) { c = '&'; i += 4; }
            else if (i + 3 < n && strncmp(s + i, "&lt;", 4) == 0) { c = '<'; i += 3; }
            else if (i + 3 < n && strncmp(s + i, "&gt;", 4) == 0) { c = '>'; i += 3; }
            else if (i + 5 < n && strncmp(s + i, "&quot;", 6) == 0) { c = '"'; i += 5; }
            else if (i + 5 < n && strncmp(s + i, "&#x27;", 6) == 0) { c = '\''; i += 5; }
            else c = ' ';
        }
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c >= 32) out[used++] = (char)c;
    }
    out[used] = '\0';
}

static bool url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (used + 1 >= cap) return false;
            out[used++] = (char)c;
        } else if (c == ' ') {
            if (used + 1 >= cap) return false;
            out[used++] = '+';
        } else {
            if (used + 3 >= cap) return false;
            out[used++] = '%'; out[used++] = hex[c >> 4]; out[used++] = hex[c & 15];
        }
    }
    out[used] = '\0';
    return true;
}

static void parse_ddg_results(const char *html, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = html;
    unsigned count = 0;
    while (p && *p && count < 4) {
        const char *a = strstr(p, "class=\"result__a\"");
        if (!a) a = strstr(p, "class='result__a'");
        if (!a) break;
        const char *gt = strchr(a, '>');
        if (!gt) break;
        const char *end = strstr(gt + 1, "</a>");
        if (!end) break;
        char title[240] = {0};
        append_text(title, sizeof(title), gt + 1, (size_t)(end - gt - 1));
        const char *href = strstr(a, "href=\"");
        if (!href) href = strstr(a, "href='");
        char url[360] = {0};
        if (href) {
            char quote = href[5];
            const char *u = href + 6;
            const char *ue = strchr(u, quote);
            if (ue && ue - u < (ptrdiff_t)sizeof(url)) {
                size_t n = (size_t)(ue - u);
                memcpy(url, u, n); url[n] = '\0';
            }
        }
        const char *snippet_start = strstr(end + 4, "result__snippet");
        char snippet[360] = {0};
        if (snippet_start) {
            const char *sgt = strchr(snippet_start, '>');
            const char *se = sgt ? strchr(sgt + 1, '<') : NULL;
            if (sgt && se) append_text(snippet, sizeof(snippet), sgt + 1, (size_t)(se - sgt - 1));
        }
        size_t used = strlen(out);
        if (used + 4 < cap) {
            snprintf(out + used, cap - used, "%u. %s\n%s\n%s\n", count + 1, title, url, snippet);
            count++;
        }
        p = end + 4;
    }
    if (!count) snprintf(out, cap, "Pencarian tidak menghasilkan cuplikan yang bisa dibaca. Situs pencarian mungkin mengubah format atau membatasi permintaan.");
}

static void web_search_task(void *arg)
{
    search_job_t *job = (search_job_t *)arg;
    char encoded[SEARCH_QUERY_MAX * 3 + 1] = {0};
    char result[SEARCH_RESULT_MAX] = {0};
    search_http_buffer_t *body = (search_http_buffer_t *)calloc(1, sizeof(search_http_buffer_t));
    bool ok = body && url_encode(job->query, encoded, sizeof(encoded));
    if (ok) {
        char url[1024];
        snprintf(url, sizeof(url), "https://html.duckduckgo.com/html/?q=%s", encoded);
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 9000;
        cfg.event_handler = search_http_event;
        cfg.user_data = body;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.buffer_size = 1024;
        cfg.buffer_size_tx = 512;
        esp_http_client_handle_t client_http = esp_http_client_init(&cfg);
        if (client_http) {
            esp_http_client_set_header(client_http, "User-Agent", "Mozilla/5.0 (compatible; ESP32WebSearch/1.0)");
            esp_err_t err = esp_http_client_perform(client_http);
            int status = esp_http_client_get_status_code(client_http);
            if (err == ESP_OK && status == 200 && body->used > 0) {
                parse_ddg_results(body->data, result, sizeof(result));
                ESP_LOGI(TAG, "Pencarian selesai: HTTP %d, %u byte", status, (unsigned)body->used);
            } else {
                snprintf(result, sizeof(result), "Pencarian web gagal (HTTP %d, err=%s). Coba lagi nanti.", status, esp_err_to_name(err));
                ESP_LOGW(TAG, "Pencarian gagal: HTTP=%d err=%s", status, esp_err_to_name(err));
            }
            esp_http_client_cleanup(client_http);
        } else snprintf(result, sizeof(result), "Pencarian web gagal: tidak bisa membuat koneksi HTTP.");
    } else snprintf(result, sizeof(result), "Pencarian web gagal: memori tidak cukup.");
    if (body) free(body);
    /* Tool responses are placed on the existing TX queue; never write the socket from this worker. */
    if (!websocket_send_tool_response_text(job->id, "web_search", result))
        ESP_LOGW(TAG, "Hasil pencarian tidak terkirim ke Gemini");
    free(job);
    vTaskDelete(NULL);
}

bool websocket_search_start(const char *id, const char *query)
{
    if (!id || !query || !query[0] || strlen(id) >= 96) return false;
    size_t qlen = strlen(query);
    if (qlen > SEARCH_QUERY_MAX) qlen = SEARCH_QUERY_MAX;
    search_job_t *job = (search_job_t *)calloc(1, sizeof(search_job_t));
    if (!job) return false;
    snprintf(job->id, sizeof(job->id), "%s", id);
    memcpy(job->query, query, qlen);
    job->query[qlen] = '\0';
    BaseType_t created = xTaskCreate(web_search_task, "web_search", 6144, job, 3, NULL);
    if (created != pdPASS) {
        free(job);
        ESP_LOGW(TAG, "Tidak bisa membuat task pencarian");
        return false;
    }
    ESP_LOGI(TAG, "Pencarian web dijadwalkan (query_len=%u)", (unsigned)qlen);
    return true;
}
