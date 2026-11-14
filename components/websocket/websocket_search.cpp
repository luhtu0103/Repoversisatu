#include "websocket_internal.h"
#include "uart_control.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "WS_SEARCH";
#define SEARCH_HTTP_MAX 24000
#define SEARCH_QUERY_MAX 240
#define SEARCH_RESULT_MAX 3000

typedef struct {
    char id[96];
    char query[SEARCH_QUERY_MAX + 1];
} search_job_t;

typedef struct {
    char data[SEARCH_HTTP_MAX + 1];
    size_t used;
    bool truncated;
} search_http_buffer_t;

static esp_err_t search_http_event(esp_http_client_event_t *evt)
{
    search_http_buffer_t *b = (search_http_buffer_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && evt->data && evt->data_len > 0) {
        size_t n = (size_t)evt->data_len;
        if (n > SEARCH_HTTP_MAX - b->used) {
            n = SEARCH_HTTP_MAX - b->used;
            b->truncated = true;
        }
        if (n == 0 && evt->data_len > 0) b->truncated = true;
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
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
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

/* Parse both DuckDuckGo Lite (result-link) and HTML (result__a) anchors.
 * Walk complete <a> tags so href works regardless of attribute order. */
static unsigned parse_ddg_results(const char *html, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = html;
    unsigned count = 0;
    while (p && *p && count < 4) {
        const char *a = strstr(p, "<a");
        if (!a) break;
        const char *gt = strchr(a, '>');
        if (!gt) break;
        const char *end = strstr(gt + 1, "</a>");
        if (!end) break;
        const char *class1 = strstr(a, "result-link");
        const char *class2 = strstr(a, "result__a");
        bool is_result = (class1 && class1 < gt) || (class2 && class2 < gt);
        if (is_result) {
            char title[240] = {0};
            append_text(title, sizeof(title), gt + 1, (size_t)(end - gt - 1));
            char url[500] = {0};
            const char *href = strstr(a, "href=");
            if (href && href < gt) {
                const char *u = href + 5;
                while (u < gt && (*u == ' ' || *u == '\t')) ++u;
                if (u < gt && (*u == '"' || *u == '\'')) {
                    char quote = *u++;
                    const char *ue = (const char *)memchr(u, quote, (size_t)(gt - u));
                    if (ue && (size_t)(ue - u) < sizeof(url)) {
                        size_t n = (size_t)(ue - u);
                        memcpy(url, u, n); url[n] = '\0';
                    }
                }
            }
            char snippet[360] = {0};
            const char *sn = strstr(end + 4, "result-snippet");
            if (!sn) sn = strstr(end + 4, "result__snippet");
            if (sn) {
                const char *sgt = strchr(sn, '>');
                const char *se = sgt ? strchr(sgt + 1, '<') : NULL;
                if (sgt && se) append_text(snippet, sizeof(snippet), sgt + 1, (size_t)(se - sgt - 1));
            }
            if (title[0] && url[0]) {
                size_t used = strlen(out);
                if (used + 4 < cap) {
                    int n = snprintf(out + used, cap - used,
                        "HASIL %u\nJudul: %s\nURL sumber: %s\nCuplikan: %s\n\n",
                        count + 1, title, url, snippet[0] ? snippet : "(cuplikan tidak tersedia)");
                    if (n > 0 && (size_t)n < cap - used) ++count;
                }
            }
        }
        p = end + 4;
    }
    return count;
}

static void web_search_task(void *arg)
{
    search_job_t *job = (search_job_t *)arg;
    /* Avoid overlapping the search TLS handshake with the first audio burst. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "TLS mulai: free_heap=%u min_free_heap=%u stack_hwm=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    char encoded[SEARCH_QUERY_MAX * 3 + 1] = {0};
    char result[SEARCH_RESULT_MAX] = {0};
    search_http_buffer_t *body = (search_http_buffer_t *)calloc(1, sizeof(search_http_buffer_t));
    bool ok = body && url_encode(job->query, encoded, sizeof(encoded));
    if (ok) {
        /* DuckDuckGo Lite first: /html has returned HTTP 202 challenge pages on ESP32. */
        char url[1024];
        snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", encoded);
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 15000;
        cfg.keep_alive_enable = false;
        cfg.event_handler = search_http_event;
        cfg.user_data = body;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.buffer_size = 2048;
        cfg.buffer_size_tx = 512;
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        if (http) {
            esp_http_client_set_header(http, "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131.0 Safari/537.36");
            esp_http_client_set_header(http, "Accept", "text/html,application/xhtml+xml");
            esp_http_client_set_header(http, "Accept-Language", "id-ID,id;q=0.9,en;q=0.8");
            esp_err_t err = esp_http_client_perform(http);
            int status = esp_http_client_get_status_code(http);
            if (err == ESP_OK && status == 200 && body->used > 0) {
                unsigned parsed = parse_ddg_results(body->data, result, sizeof(result));
                ESP_LOGI(TAG, "HTTP %d byte=%u truncated=%d parsed=%u result_len=%u",
                         status, (unsigned)body->used, body->truncated ? 1 : 0,
                         parsed, (unsigned)strlen(result));
                ESP_LOGI(TAG, "Hasil ringkas untuk debug: %.*s",
                         (int)(strlen(result) > 220 ? 220 : strlen(result)), result);
                if (parsed == 0) {
                    ESP_LOGW(TAG, "HTTP 200 tetapi hasil tidak dikenali; prefix=%.*s", 100, body->data);
                    snprintf(result, sizeof(result),
                        "Pencarian web menerima HTTP 200 tetapi tidak menemukan hasil yang dapat dibaca. Format halaman mungkin berubah.");
                }
            } else {
                ESP_LOGW(TAG, "HTTP pencarian gagal: status=%d err=%s body=%u",
                         status, esp_err_to_name(err), (unsigned)body->used);
                snprintf(result, sizeof(result), "Pencarian web gagal (HTTP %d, err=%s). Coba lagi nanti.",
                         status, esp_err_to_name(err));
            }
            esp_http_client_cleanup(http);
        } else {
            snprintf(result, sizeof(result), "Pencarian web gagal: tidak bisa membuat koneksi HTTP.");
        }
    } else {
        snprintf(result, sizeof(result), "Pencarian web gagal: memori tidak cukup atau query terlalu panjang.");
    }

    if (body) free(body);
    uart_control_send_web_search(result);
    if (!websocket_send_tool_response_text(job->id, "web_search", result))
        ESP_LOGW(TAG, "Hasil pencarian tidak terkirim ke Gemini (id=%s)", job->id);
    else
        ESP_LOGI(TAG, "Hasil pencarian dikirim ke Gemini (id=%s)", job->id);
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
    BaseType_t created = xTaskCreate(web_search_task, "web_search", 8192, job, 2, NULL);
    if (created != pdPASS) {
        free(job);
        ESP_LOGW(TAG, "Tidak bisa membuat task pencarian");
        return false;
    }
    ESP_LOGI(TAG, "Pencarian web dijadwalkan (query_len=%u)", (unsigned)qlen);
    return true;
}
