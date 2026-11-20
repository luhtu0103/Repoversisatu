// Pemutar radio streaming MP3/AAC untuk robot.
//  - radio_resolve(): cari stasiun (preset + direktori radio-browser.info)
//  - net_task: unduh stream HTTP/HTTPS ke buffer besar (PSRAM)
//  - play_task: decode MP3 (minimp3) atau AAC (ESP Audio Codec), lalu ke mono 24 kHz
#include "radio_player.h"
#include "radio_dsp.h"
#include "audio_hal.h"
#include "display.h"
#include "servo_head.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "minimp3.h"

extern "C" void oled2_face_set_speaker_level(uint8_t level);

static const char *TAG = "RADIO";

/* ---- Pengaturan yang boleh diubah ---- */
// Volume musik dalam persen. Sebelumnya 60 terlalu pelan di robot; 150 = 1,5x. Turunkan jika pecah.
#define RADIO_GAIN_PERCENT 150
// Volume khusus Menara FM (AAC). Siarannya pelan, jadi dikuatkan. Naikkan jika masih pelan
// (150 = 1,5x, 250 = 2,5x); turunkan jika suara pecah.
#define RADIO_GAIN_AAC_PERCENT 220
// Buffer unduhan (PSRAM). 128 KB = sekitar 8 detik pada 128 kbps.
#define RADIO_STREAM_BUFFER_BYTES (128 * 1024)
// Mulai memutar setelah buffer terisi segini (sekitar 3 detik pada 128 kbps).
#define RADIO_PREBUFFER_BYTES (48 * 1024)
// Berhenti jika tidak ada data baru selama ini.
#define RADIO_UNDERRUN_TIMEOUT_MS 10000
#define RADIO_CONNECT_ATTEMPTS 4
// Stack task pemutar (byte). Decoder MP3 sendiri butuh ~17 KB; sisanya untuk suara/wajah/log.
#define RADIO_PLAY_STACK 28672

/* Preset stream langsung. Menara FM memakai AAC-LC, bukan MP3. */
struct preset_t { const char *alias; const char *name; const char *url; };
static const preset_t kPresets[] = {
    {"beat", "The Beat Radio Bali", "https://c1.siar.us/radio/8030/stream"},
    {"menara", "Menara FM Bali", "https://c1.siar.us:10280/menarafmbali"},
};
static const char *const kApiHosts[] = {
    "de1.api.radio-browser.info", "at1.api.radio-browser.info", "nl1.api.radio-browser.info",
};

/* ---- Status bersama ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static radio_station_t s_pending = {};
static volatile bool s_has_pending = false;
static volatile int64_t s_pending_us = 0;

static volatile bool s_active = false;        // play_task hidup
static volatile bool s_net_alive = false;     // net_task hidup
static volatile bool s_stop = false;
static volatile bool s_net_done = false;
static volatile int s_net_fail = 0;           // 1 = gagal tersambung
static radio_station_t s_cur = {};

static volatile bool s_ended = false;
static char s_end_name[48] = {};
static int s_end_reason = 0;

static StreamBufferHandle_t s_sb = nullptr;
static uint8_t *s_sb_mem = nullptr;
static StaticStreamBuffer_t s_sb_struct;
static int s_users = 0;                       // jumlah task yang masih memakai s_sb

/* ================= Pencarian stasiun ================= */

struct body_t { char *data; size_t used; size_t cap; };

static esp_err_t body_event(esp_http_client_event_t *evt)
{
    body_t *b = (body_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && evt->data && evt->data_len > 0) {
        size_t n = (size_t)evt->data_len;
        if (n > b->cap - 1 - b->used) n = b->cap - 1 - b->used;
        if (n) {
            memcpy(b->data + b->used, evt->data, n);
            b->used += n;
            b->data[b->used] = '\0';
        }
    }
    return ESP_OK;
}

static bool http_get_body(const char *url, body_t *b)
{
    b->used = 0;
    b->data[0] = '\0';
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.keep_alive_enable = false;
    cfg.event_handler = body_event;
    cfg.user_data = b;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "Mozilla/5.0 (ESP32 Asisten Robot)";
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 512;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    esp_err_t err = esp_http_client_perform(h);
    const int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK || status != 200 || b->used == 0) {
        ESP_LOGW(TAG, "Pencarian radio gagal: err=%s HTTP=%d", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = '\0';
}

/* Ambil kata kunci inti: buang kata umum ("radio", "fm", "bali", ...). */
static void normalize_query(const char *in, char *out, size_t cap)
{
    static const char *const filler[] = {"radio", "fm", "stasiun", "bali", "di", "putar", "the", "dong", "ya"};
    char tmp[96];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && n + 1 < sizeof(tmp); ++p)
        tmp[n++] = (char)((*p < 0x80 && isalnum(*p)) ? tolower(*p) : ' ');
    tmp[n] = '\0';
    out[0] = '\0';
    size_t used = 0;
    char *save = nullptr;
    for (char *tok = strtok_r(tmp, " ", &save); tok; tok = strtok_r(nullptr, " ", &save)) {
        bool skip = false;
        for (const char *f : filler) if (strcmp(tok, f) == 0) { skip = true; break; }
        if (skip) continue;
        const size_t tl = strlen(tok);
        if (used + tl + 2 >= cap) break;
        if (used) out[used++] = ' ';
        memcpy(out + used, tok, tl);
        used += tl;
        out[used] = '\0';
    }
}

static void clean_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; ++p)
        if (*p >= 0x20 && *p < 0x7F && *p != '"' && *p != '\\') out[o++] = (char)*p;
    while (o > 0 && out[o - 1] == ' ') --o;
    out[o] = '\0';
    if (o == 0) snprintf(out, cap, "Radio");
}

/* Pilih stasiun terbaik dari hasil JSON (urut populer). */
static bool pick_station(const char *json, radio_station_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return false; }
    bool found = false;
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, root) {
        cJSON *url = cJSON_GetObjectItem(it, "url_resolved");
        cJSON *name = cJSON_GetObjectItem(it, "name");
        cJSON *ok = cJSON_GetObjectItem(it, "lastcheckok");
        cJSON *codec = cJSON_GetObjectItem(it, "codec");
        cJSON *br = cJSON_GetObjectItem(it, "bitrate");
        if (!cJSON_IsString(url) || !url->valuestring || !url->valuestring[0]) continue;
        if (strncmp(url->valuestring, "http", 4) != 0) continue;
        if (strlen(url->valuestring) >= sizeof(out->url)) continue;
        if (cJSON_IsNumber(ok) && ok->valueint != 1) continue;
        if (cJSON_IsString(codec) && codec->valuestring && !strcasestr(codec->valuestring, "mp3")) continue;
        if (cJSON_IsNumber(br) && br->valueint > 256) continue;   // terlalu besar untuk Wi-Fi robot
        strlcpy(out->url, url->valuestring, sizeof(out->url));
        clean_name(cJSON_IsString(name) && name->valuestring ? name->valuestring : "Radio",
                   out->name, sizeof(out->name));
        found = true;
        break;
    }
    cJSON_Delete(root);
    return found;
}

static bool api_search(const char *params, body_t *b, radio_station_t *out)
{
    for (const char *host : kApiHosts) {
        char url[400];
        snprintf(url, sizeof(url),
                 "https://%s/json/stations/search?%s&codec=MP3&hidebroken=true&order=clickcount&reverse=true&limit=10",
                 host, params);
        ESP_LOGI(TAG, "Cari radio: %s", url);
        if (!http_get_body(url, b)) continue;      // server ini gagal -> coba server lain
        return pick_station(b->data, out);         // server menjawab: hasilnya final
    }
    return false;
}

bool radio_resolve(const char *query, radio_station_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    char key[96];
    normalize_query(query, key, sizeof(key));
    ESP_LOGI(TAG, "Kata kunci radio: '%s' (asli: '%s')", key, query ? query : "");

    if (key[0]) {
        for (const preset_t &p : kPresets) {
            if (strstr(key, p.alias)) {
                strlcpy(out->name, p.name, sizeof(out->name));
                strlcpy(out->url, p.url, sizeof(out->url));
                ESP_LOGI(TAG, "Preset dipakai: %s", out->name);
                return true;
            }
        }
    }

    body_t b = {};
    b.cap = 24 * 1024;
    b.data = (char *)heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b.data) b.data = (char *)malloc(b.cap);
    if (!b.data) return false;

    char enc[160];
    char params[260];
    bool ok = false;
    if (key[0]) {
        url_encode(key, enc, sizeof(enc));
        snprintf(params, sizeof(params), "name=%s&state=bali", enc);
        ok = api_search(params, &b, out);
        if (!ok) {
            snprintf(params, sizeof(params), "name=%s&countrycode=ID", enc);
            ok = api_search(params, &b, out);
        }
        if (!ok) {
            snprintf(params, sizeof(params), "name=%s", enc);
            ok = api_search(params, &b, out);
        }
    } else {
        ok = api_search("state=bali&countrycode=ID", &b, out);
    }
    heap_caps_free(b.data);

    if (!ok && !key[0]) {   // permintaan umum "putar radio": pakai preset sebagai cadangan
        strlcpy(out->name, kPresets[0].name, sizeof(out->name));
        strlcpy(out->url, kPresets[0].url, sizeof(out->url));
        ok = true;
    }
    if (ok) ESP_LOGI(TAG, "Stasiun dipilih: %s -> %s", out->name, out->url);
    else ESP_LOGW(TAG, "Stasiun tidak ditemukan untuk '%s'", key);
    return ok;
}

/* ================= Status pending / ended ================= */

void radio_set_pending(const radio_station_t *st)
{
    if (!st) return;
    portENTER_CRITICAL(&s_mux);
    s_pending = *st;
    s_pending_us = esp_timer_get_time();
    s_has_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

bool radio_has_pending(void) { return s_has_pending; }

int64_t radio_pending_age_us(void)
{
    if (!s_has_pending) return 0;
    return esp_timer_get_time() - s_pending_us;
}

void radio_clear_pending(void)
{
    portENTER_CRITICAL(&s_mux);
    s_has_pending = false;
    portEXIT_CRITICAL(&s_mux);
}

bool radio_is_active(void) { return s_active; }
void radio_stop(void) { s_stop = true; }
bool radio_has_ended(void) { return s_ended && !s_active; }

bool radio_take_ended(char *name, size_t name_len, int *reason)
{
    bool r = false;
    portENTER_CRITICAL(&s_mux);
    if (s_ended && !s_active) {
        if (name && name_len) strlcpy(name, s_end_name, name_len);
        if (reason) *reason = s_end_reason;
        s_ended = false;
        r = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return r;
}

static void mark_ended(const char *name, int reason)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_end_name, name, sizeof(s_end_name));
    s_end_reason = reason;
    s_ended = true;
    portEXIT_CRITICAL(&s_mux);
}

/* Bebaskan buffer jika tidak ada task yang memakainya lagi. */
static void release_user(void)
{
    bool last = false;
    portENTER_CRITICAL(&s_mux);
    if (s_users > 0 && --s_users == 0) last = true;
    portEXIT_CRITICAL(&s_mux);
    if (last) {
        if (s_sb) { vStreamBufferDelete(s_sb); s_sb = nullptr; }
        if (s_sb_mem) { heap_caps_free(s_sb_mem); s_sb_mem = nullptr; }
    }
}

/* ================= Unduh stream ================= */

/* ---- Klien HTTP sederhana (socket) untuk alamat http://.
 * Server radio Shoutcast sering menjawab "ICY 200 OK" yang ditolak esp_http_client. ---- */
// 0 = berakhir setelah ada aliran, -1 = gagal, -2 = pindah alamat (url sudah diganti)
static int raw_http_stream(char *url, size_t url_cap, bool *got_data)
{
    char host[96] = {};
    char path[192] = "/";
    int port = 80;
    const char *p = url + 7;                       // lewati "http://"
    size_t hl = 0;
    while (*p && *p != ':' && *p != '/' && hl < sizeof(host) - 1) host[hl++] = *p++;
    host[hl] = '\0';
    if (*p == ':') { port = atoi(p + 1); while (*p && *p != '/') ++p; }
    if (*p == '/') strlcpy(path, p, sizeof(path));
    if (!host[0] || port <= 0 || port > 65535) { ESP_LOGW(TAG, "Alamat stream tidak valid: %s", url); return -1; }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "DNS gagal: %s", host);
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = {};
    tv.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 1;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const int rc = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        ESP_LOGW(TAG, "Gagal tersambung ke %s:%d (errno=%d)", host, port, errno);
        close(sock);
        return -1;
    }

    char req[384];
    const int rn = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: WinampMPEG/5.09\r\n"
        "Accept: */*\r\nIcy-MetaData: 0\r\nConnection: close\r\n\r\n", path, host, port);
    if (rn <= 0 || send(sock, req, (size_t)rn, 0) != rn) {
        ESP_LOGW(TAG, "Gagal mengirim permintaan stream");
        close(sock);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(2048);
    if (!buf) { close(sock); return -1; }
    size_t have = 0;
    int hdr_end = -1;
    int idle = 0;
    while (!s_stop && hdr_end < 0 && have < 2047) {
        const int n = recv(sock, buf + have, 2047 - have, 0);
        if (n > 0) {
            have += (size_t)n;
            buf[have] = '\0';
            const char *e = strstr((const char *)buf, "\r\n\r\n");
            if (e) hdr_end = (int)(e - (const char *)buf) + 4;
        } else if (n == 0) {
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (++idle > 6) break;
        } else {
            break;
        }
    }
    if (hdr_end < 0) {
        ESP_LOGW(TAG, "Server tidak mengirim header stream");
        free(buf); close(sock);
        return -1;
    }

    buf[have] = '\0';
    int status = 0;
    {
        const char *sp = strchr((const char *)buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    char first[80] = {};
    {
        size_t i = 0;
        while (i < sizeof(first) - 1 && buf[i] && buf[i] != '\r' && buf[i] != '\n') { first[i] = (char)buf[i]; ++i; }
    }
    ESP_LOGI(TAG, "Server: \"%s\" (kode %d)", first, status);

    if (status >= 300 && status < 400) {
        char loc[240] = {};
        for (const char *ln = (const char *)buf; ln && *ln; ln = strstr(ln, "\r\n")) {
            while (*ln == '\r' || *ln == '\n') ++ln;
            if (strncasecmp(ln, "Location:", 9) == 0) {
                const char *v = ln + 9;
                while (*v == ' ') ++v;
                size_t i = 0;
                while (v[i] && v[i] != '\r' && v[i] != '\n' && i < sizeof(loc) - 1) { loc[i] = v[i]; ++i; }
                loc[i] = '\0';
                break;
            }
        }
        free(buf); close(sock);
        if (!loc[0]) { ESP_LOGW(TAG, "Redirect tanpa Location"); return -1; }
        char next[640];   // cukup besar untuk host + port + Location; panjang akhir dicek di bawah
        if (strncmp(loc, "http", 4) == 0) strlcpy(next, loc, sizeof(next));
        else if (loc[0] == '/') snprintf(next, sizeof(next), "http://%s:%d%s", host, port, loc);
        else {
            // Alamat relatif tanpa garis miring (mis. "index.html?sid=1"): tempelkan ke folder alamat sekarang.
            char dir[192];
            strlcpy(dir, path, sizeof(dir));
            char *q = strchr(dir, '?');
            if (q) *q = '\0';
            char *sl = strrchr(dir, '/');
            if (sl) sl[1] = '\0'; else strlcpy(dir, "/", sizeof(dir));
            snprintf(next, sizeof(next), "http://%s:%d%s%s", host, port, dir, loc);
        }
        if (strlen(next) >= url_cap) { ESP_LOGW(TAG, "Alamat redirect terlalu panjang"); return -1; }
        ESP_LOGI(TAG, "Redirect -> %s", next);
        strlcpy(url, next, url_cap);
        return -2;
    }
    if (status != 200) {
        free(buf); close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Stream tersambung");
    // Sisa data setelah header adalah awal audio.
    if (have > (size_t)hdr_end) {
        size_t off = (size_t)hdr_end;
        *got_data = true;
        while (off < have && !s_stop)
            off += xStreamBufferSend(s_sb, buf + off, have - off, pdMS_TO_TICKS(300));
    }
    idle = 0;
    while (!s_stop) {
        const int n = recv(sock, buf, 2048, 0);
        if (n > 0) {
            idle = 0;
            *got_data = true;
            int off = 0;
            while (off < n && !s_stop)
                off += (int)xStreamBufferSend(s_sb, buf + off, (size_t)(n - off), pdMS_TO_TICKS(300));
        } else if (n == 0) {
            break;                                   // server menutup
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (++idle > 8) break;                   // 8 detik tanpa data
        } else {
            break;
        }
    }
    free(buf);
    close(sock);
    return 0;
}

// 0 = berakhir normal/putus setelah ada data, -1 = gagal tersambung
static int stream_once(const char *start_url, bool *got_data)
{
    char url[256];
    strlcpy(url, start_url, sizeof(url));
    for (int hop = 0; hop < 6 && !s_stop; ++hop) {
        if (strncmp(url, "http://", 7) == 0) {
            const int rr = raw_http_stream(url, sizeof(url), got_data);
            if (rr == -2) continue;                 // pindah alamat
            return rr;
        }
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 4000;
        cfg.keep_alive_enable = false;
        cfg.disable_auto_redirect = true;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.user_agent = "Mozilla/5.0 (ESP32 Asisten Robot)";
        cfg.buffer_size = 2048;
        cfg.buffer_size_tx = 1024;
        esp_http_client_handle_t h = esp_http_client_init(&cfg);
        if (!h) return -1;
        esp_http_client_set_header(h, "Accept", "*/*");
        esp_http_client_set_header(h, "Connection", "close");

        if (esp_http_client_open(h, 0) != ESP_OK) {
            ESP_LOGW(TAG, "Gagal membuka stream");
            esp_http_client_cleanup(h);
            return -1;
        }
        const int64_t hdr_len = esp_http_client_fetch_headers(h);
        if (hdr_len < 0) ESP_LOGW(TAG, "Header stream bermasalah (%lld)", (long long)hdr_len);
        const int status = esp_http_client_get_status_code(h);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char *loc = nullptr;
            esp_http_client_get_header(h, "Location", &loc);
            if (loc && loc[0] && strlen(loc) < sizeof(url) && strncmp(loc, "http", 4) == 0) {
                ESP_LOGI(TAG, "Redirect -> %s", loc);
                strlcpy(url, loc, sizeof(url));
                esp_http_client_close(h);
                esp_http_client_cleanup(h);
                continue;
            }
            ESP_LOGW(TAG, "Redirect (kode %d) tanpa alamat yang bisa dipakai", status);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            return -1;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "Stream menjawab HTTP %d", status);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            return -1;
        }

        uint8_t *rb = (uint8_t *)malloc(2048);
        if (!rb) { esp_http_client_close(h); esp_http_client_cleanup(h); return -1; }
        ESP_LOGI(TAG, "Stream tersambung");
        int zero_streak = 0;
        while (!s_stop) {
            const int n = esp_http_client_read(h, (char *)rb, 2048);
            if (n > 0) {
                zero_streak = 0;
                *got_data = true;
                int off = 0;
                while (off < n && !s_stop) {
                    off += (int)xStreamBufferSend(s_sb, rb + off, (size_t)(n - off), pdMS_TO_TICKS(300));
                }
            } else if (n == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                if (++zero_streak > 50) break;          // tidak ada data > 1 detik: anggap putus
#ifdef ESP_ERR_HTTP_EAGAIN
            } else if (n == -ESP_ERR_HTTP_EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(10));          // belum ada data (timeout kecil)
#endif
            } else {
                break;                                   // error
            }
        }
        free(rb);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return 0;
    }
    return -1;
}

static void net_task(void *)
{
    int fails = 0;
    for (int attempt = 0; attempt < RADIO_CONNECT_ATTEMPTS * 4 && !s_stop; ++attempt) {
        bool got = false;
        const int r = stream_once(s_cur.url, &got);
        if (s_stop) break;
        if (got) fails = 0;               // pernah mengalir: putus sebentar, coba sambung lagi
        else if (++fails >= RADIO_CONNECT_ATTEMPTS) break;
        (void)r;
        ESP_LOGW(TAG, "Stream putus/gagal (percobaan %d); sambung ulang", fails);
        for (int i = 0; i < 10 && !s_stop; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_stop && s_net_fail == 0) s_net_fail = 1;
    ESP_LOGI(TAG, "Unduh selesai; sisa stack unduh=%u byte", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_net_done = true;
    s_net_alive = false;
    release_user();
    vTaskDelete(nullptr);
}

/* ================= Putar ================= */

static inline uint8_t level_from_peak(int32_t peak)
{
    int32_t l = peak >> 8;
    return (uint8_t)(l > 255 ? 255 : l);
}

static void play_task(void *)
{
    constexpr size_t INBUF = 24 * 1024;
    int end_reason = 0;
    const bool is_aac = (strstr(s_cur.url, "menarafmbali") != nullptr);
    mp3dec_t *dec = is_aac ? nullptr : (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_audio_simple_dec_handle_t aac_dec = nullptr;
    bool aac_registered = false;
    bool aac_core_registered = false;
    uint8_t *in = (uint8_t *)heap_caps_malloc(INBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *mono = (int16_t *)heap_caps_malloc(1152 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *outbuf = (int16_t *)heap_caps_malloc(4096 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    RadioResampler *rs = new RadioResampler();

    if ((!is_aac && !dec) || !in || !pcm || !mono || !outbuf) {
        ESP_LOGE(TAG, "Memori tidak cukup untuk pemutar radio");
        end_reason = 2;
        goto finish;
    }
    if (is_aac) {
        // Dua langkah: (1) daftarkan decoder audio AAC-nya, (2) daftarkan pembungkus "simple decoder".
        // Tanpa langkah 1 muncul error "Decoder AAC not registered" (-7).
        const esp_audio_err_t core_err = esp_audio_dec_register_default();
        if (core_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Registrasi decoder audio gagal: %d", (int)core_err);
            end_reason = 2;
            goto finish;
        }
        aac_core_registered = true;
        const esp_audio_err_t reg_err = esp_audio_simple_dec_register_default();
        if (reg_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Registrasi decoder AAC gagal: %d", (int)reg_err);
            end_reason = 2;
            goto finish;
        }
        aac_registered = true;
        esp_audio_simple_dec_cfg_t cfg = {};
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        cfg.use_frame_dec = false;  // parser mencari batas frame AAC pada stream
        const esp_audio_err_t open_err = esp_audio_simple_dec_open(&cfg, &aac_dec);
        if (open_err != ESP_AUDIO_ERR_OK || !aac_dec) {
            ESP_LOGE(TAG, "Membuka decoder AAC gagal: %d", (int)open_err);
            end_reason = 2;
            goto finish;
        }
        ESP_LOGI(TAG, "Decoder AAC aktif untuk Menara FM");
    } else {
        mp3dec_init(dec);
    }

    {
        // Tunggu buffer terisi (prebuffer).
        const int64_t wait_start = esp_timer_get_time();
        while (!s_stop && !s_net_done &&
               xStreamBufferBytesAvailable(s_sb) < RADIO_PREBUFFER_BYTES) {
            if (esp_timer_get_time() - wait_start > 20LL * 1000000LL) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!s_stop && xStreamBufferBytesAvailable(s_sb) < 4096) {
            ESP_LOGW(TAG, "Tidak ada data stream");
            end_reason = (s_net_fail ? 2 : 1);
            goto finish;
        }
    }

    {
        audio_hal_set_mic_enabled(false);
        char line[64];
        snprintf(line, sizeof(line), "Radio: %s", s_cur.name);
        display_set_gemini_text(line);
        display_set_user_text("Sentuh sensor untuk berhenti");
        servo_head_set_speaking(true);        // kepala bergerak pelan seperti bicara
        face_set_state(FACE_SPEAKING);

        size_t rp = 0, valid = 0;
        uint32_t cur_hz = 0;
        int64_t last_data_us = esp_timer_get_time();
        int64_t last_face_us = 0;
        mp3dec_frame_info_t info;

        while (!s_stop) {
            // Isi ulang masukan.
            if (rp > 0 && (rp > INBUF / 2 || valid == 0)) {
                if (valid) memmove(in, in + rp, valid);
                rp = 0;
            }
            if (rp + valid < INBUF) {
                const TickType_t wait = (valid < 4096) ? pdMS_TO_TICKS(30) : 0;
                const size_t got = xStreamBufferReceive(s_sb, in + rp + valid, INBUF - rp - valid, wait);
                if (got) { valid += got; last_data_us = esp_timer_get_time(); }
            }
            if (valid == 0) {
                if (s_net_done) { end_reason = 1; break; }
                if (esp_timer_get_time() - last_data_us > (int64_t)RADIO_UNDERRUN_TIMEOUT_MS * 1000LL) {
                    ESP_LOGW(TAG, "Tidak ada data baru > %d detik", RADIO_UNDERRUN_TIMEOUT_MS / 1000);
                    end_reason = 1;
                    break;
                }
                continue;
            }

            int samples = 0;
            int channels = 0;
            uint32_t sample_hz = 0;
            int bitrate_kbps = 0;
            if (is_aac) {
                esp_audio_simple_dec_raw_t raw = {};
                raw.buffer = in + rp;
                raw.len = (uint32_t)valid;
                esp_audio_simple_dec_out_t frame = {};
                frame.buffer = (uint8_t *)pcm;
                frame.len = (uint32_t)(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
                const esp_audio_err_t decode_err = esp_audio_simple_dec_process(aac_dec, &raw, &frame);
                if (raw.consumed > valid) raw.consumed = (uint32_t)valid;
                rp += raw.consumed;
                valid -= raw.consumed;
                if (decode_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGE(TAG, "Buffer PCM AAC tidak cukup (perlu %lu byte)", (unsigned long)frame.needed_size);
                    end_reason = 2;
                    break;
                }
                if (decode_err != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "Frame AAC tidak valid: %d", (int)decode_err);
                    if (valid > 0) { ++rp; --valid; }
                    else vTaskDelay(1);
                    continue;
                }
                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t aac_info = {};
                    if (esp_audio_simple_dec_get_info(aac_dec, &aac_info) != ESP_AUDIO_ERR_OK ||
                        aac_info.bits_per_sample != 16 || aac_info.channel == 0) {
                        ESP_LOGW(TAG, "Info PCM AAC belum siap");
                        continue;
                    }
                    channels = (int)aac_info.channel;
                    sample_hz = aac_info.sample_rate;
                    samples = (int)(frame.decoded_size / (sizeof(int16_t) * (size_t)channels));
                    bitrate_kbps = (int)(aac_info.bitrate / 1000U);
                }
                if (samples <= 0) {
                    if (raw.consumed == 0 && valid >= INBUF - 16) { ++rp; --valid; }
                    else if (s_net_done && xStreamBufferBytesAvailable(s_sb) == 0 && valid == 0) { end_reason = 1; break; }
                    else if (raw.consumed == 0) vTaskDelay(1);
                    continue;
                }
            } else {
                memset(&info, 0, sizeof(info));
                samples = mp3dec_decode_frame(dec, in + rp, (int)valid, pcm, &info);
                if (info.frame_bytes > 0) {
                    const size_t fb = (size_t)info.frame_bytes > valid ? valid : (size_t)info.frame_bytes;
                    rp += fb;
                    valid -= fb;
                } else {
                    // Belum cukup data untuk satu frame. Jika buffer penuh, buang sebagian.
                    if (rp + valid >= INBUF - 16 && valid > 1024) { rp += 1024; valid -= 1024; }
                    else if (s_net_done && xStreamBufferBytesAvailable(s_sb) == 0) { end_reason = 1; break; }
                    else vTaskDelay(1);
                    continue;
                }
                channels = info.channels;
                sample_hz = (uint32_t)info.hz;
                bitrate_kbps = info.bitrate_kbps;
            }
            if (samples <= 0 || channels <= 0 || sample_hz == 0) continue;

            if (sample_hz != cur_hz) {
                cur_hz = sample_hz;
                rs->reset(cur_hz);
                ESP_LOGI(TAG, "%s: %u Hz, %d kanal, %d kbps", is_aac ? "AAC" : "MP3", (unsigned)cur_hz, channels, bitrate_kbps);
            }
            const int n = samples > 1152 ? 1152 : samples;
            if (channels == 2) {
                for (int i = 0; i < n; ++i) mono[i] = (int16_t)(((int32_t)pcm[2 * i] + (int32_t)pcm[2 * i + 1]) / 2);
            } else {
                memcpy(mono, pcm, (size_t)n * sizeof(int16_t));
            }
            const size_t out_n = rs->process(mono, (size_t)n, outbuf, 4096);

            int32_t peak = 0;
            for (size_t i = 0; i < out_n; ++i) {
                int32_t v = ((int32_t)outbuf[i] * (is_aac ? RADIO_GAIN_AAC_PERCENT : RADIO_GAIN_PERCENT)) / 100;
                if (v > 32767) v = 32767;          // cegah suara pecah/meluap
                else if (v < -32768) v = -32768;
                outbuf[i] = (int16_t)v;
                const int32_t a = v < 0 ? -v : v;
                if (a > peak) peak = a;
            }
            const uint8_t level = level_from_peak(peak);
            face_set_speech_level(level);
            oled2_face_set_speaker_level(level);

            (void)audio_write_speaker((const uint8_t *)outbuf, out_n * sizeof(int16_t));

            // Jaga wajah tetap "menyanyi" (state lain, mis. sentuhan, tidak boleh menimpa).
            const int64_t now = esp_timer_get_time();
            if (now - last_face_us > 700000) {
                last_face_us = now;
                if (face_get_state() != FACE_SPEAKING) face_set_state(FACE_SPEAKING);
            }
        }
    }

finish:
    face_set_speech_level(0);
    oled2_face_set_speaker_level(0);
    servo_head_set_speaking(false);
    face_set_state(FACE_THINKING);
    display_set_gemini_text("");
    display_set_user_text("");
    s_stop = true;                       // pastikan net_task ikut berhenti
    if (aac_dec) esp_audio_simple_dec_close(aac_dec);
    if (aac_registered) esp_audio_simple_dec_unregister_default();
    if (aac_core_registered) esp_audio_dec_unregister_default();
    if (dec) heap_caps_free(dec);
    if (in) heap_caps_free(in);
    if (pcm) heap_caps_free(pcm);
    if (mono) heap_caps_free(mono);
    if (outbuf) heap_caps_free(outbuf);
    delete rs;
    {
        // Hentikan = pengguna (0) bila tidak ada gangguan yang tercatat.
        const int reason = (end_reason != 0) ? end_reason : 0;
        mark_ended(s_cur.name, reason);
    }
    s_active = false;
    release_user();
    ESP_LOGI(TAG, "Radio selesai (alasan=%d); sisa stack pemutar=%u byte", end_reason,
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    vTaskDelete(nullptr);
}

bool radio_start_pending(void)
{
    if (s_active || !s_has_pending) return false;
    // Tunggu sisa net_task lama selesai (maks 3 detik).
    for (int i = 0; i < 30 && s_net_alive; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_net_alive) return false;

    portENTER_CRITICAL(&s_mux);
    s_cur = s_pending;
    s_has_pending = false;
    portEXIT_CRITICAL(&s_mux);

    s_sb_mem = (uint8_t *)heap_caps_malloc(RADIO_STREAM_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_sb_mem) {
        ESP_LOGE(TAG, "Alokasi buffer radio gagal");
        mark_ended(s_cur.name, 2);
        return false;
    }
    s_sb = xStreamBufferCreateStatic(RADIO_STREAM_BUFFER_BYTES, 1, s_sb_mem, &s_sb_struct);
    if (!s_sb) {
        heap_caps_free(s_sb_mem);
        s_sb_mem = nullptr;
        mark_ended(s_cur.name, 2);
        return false;
    }
    s_stop = false;
    s_net_done = false;
    s_net_fail = 0;
    s_users = 2;
    s_active = true;
    s_net_alive = true;

    if (xTaskCreatePinnedToCore(net_task, "radio_net", 10240, nullptr, 4, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat radio_net");
        s_active = false;
        s_net_alive = false;
        s_users = 0;
        vStreamBufferDelete(s_sb); s_sb = nullptr;
        heap_caps_free(s_sb_mem); s_sb_mem = nullptr;
        mark_ended(s_cur.name, 2);
        return false;
    }
    // Catatan: minimp3 butuh ~17 KB stack per frame (mp3dec_decode_frame = 17232 byte).
    // Stack 16 KB sebelumnya meluap dan merusak memori (crash StoreProhibited).
    if (xTaskCreatePinnedToCore(play_task, "radio_play", RADIO_PLAY_STACK, nullptr, 3, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat radio_play");
        s_stop = true;                 // hentikan net_task; ia akan melepas buffer
        s_active = false;
        portENTER_CRITICAL(&s_mux);
        s_users = 1;                   // hanya net_task yang masih memakai buffer
        portEXIT_CRITICAL(&s_mux);
        mark_ended(s_cur.name, 2);
        return false;
    }
    ESP_LOGI(TAG, "Radio dimulai: %s", s_cur.name);
    return true;
}
