#include "websocket_internal.h"
#include "web_config.h"
#include "display.h"
#include "audio_hal.h"
#include "uart_control.h"
#include "ai_memory.h"
#include "servo_head.h"
#include "alarm.h"
#include "ota_update.h"
#include "habit.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "WS_JSON";

/* v7.0.25: fixed PCM decode workspace. Disimpan di PSRAM agar RAM internal hemat. */
#define PCM_DECODE_WORKSPACE_SIZE (24 * 1024)
static EXT_RAM_BSS_ATTR uint8_t pcm_decode_buffer[PCM_DECODE_WORKSPACE_SIZE];

/* Standby waits for Gemini's acknowledgement turn to finish. */
static volatile bool standby_after_turn_complete = false;

/* v7.0.28:
 * Large Gemini audio JSON contains a base64 string that can be 10-20 KB.
 * cJSON normally duplicates that string while building its tree, temporarily
 * consuming another large heap block. Before cJSON parses a large audio
 * message, decode the base64 directly from the RX slot into the persistent
 * PCM workspace, then remove only the base64 characters from the RX JSON.
 * The JSON structure is preserved, so the existing cJSON parser and handling
 * of turnComplete/sessionResumption/etc. remain unchanged, but cJSON no
 * longer needs to allocate a duplicate copy of the audio payload.
 */
static bool compact_large_audio_payload(char *json, size_t *io_len)
{
    if (!json || !io_len || *io_len == 0) return false;

    const size_t len = *io_len;
    const char *inline_key = strstr(json, "\"inlineData\"");
    if (!inline_key) return false;

    const char *data_key = strstr(inline_key, "\"data\"");
    if (!data_key || data_key >= json + len) return false;

    const char *p = data_key + strlen("\"data\"");
    const char *end = json + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p != ':') return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p != '\"') return false;
    ++p;

    char *b64 = (char *)p;
    char *q = b64;
    while (q < end) {
        if (*q == '\\') return false;
        if (*q == '\"') break;
        ++q;
    }
    if (q >= end) return false;

    size_t b64_len = (size_t)(q - b64);
    if (b64_len < 4096) return false;

    size_t pcm_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &pcm_len,
                                    (const unsigned char *)b64, b64_len);
    if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return false;
    if (pcm_len == 0) return false;

    if (pcm_len > PCM_DECODE_WORKSPACE_SIZE) {
        ESP_LOGW(TAG, "PCM audio melebihi fixed workspace: need=%u capacity=%u - audio dilewati",
                 (unsigned)pcm_len, (unsigned)PCM_DECODE_WORKSPACE_SIZE);
    } else {
        size_t decoded = pcm_len;
        ret = mbedtls_base64_decode(pcm_decode_buffer, sizeof(pcm_decode_buffer), &decoded,
                                    (const unsigned char *)b64, b64_len);
        if (ret != 0 || decoded == 0) return false;
        audio_chunks_received++;
        audio_bytes_received += decoded;
        ESP_LOGI(TAG, "AUDIO GEMINI: %u byte -> AUDIO BUFFER (PCM cap=%u, compact JSON)",
                 (unsigned)decoded, (unsigned)PCM_DECODE_WORKSPACE_SIZE);
        if (!queue_audio_pcm(pcm_decode_buffer, decoded))
            ESP_LOGE(TAG, "Gagal memasukkan audio ke ring buffer");
    }

    const size_t remove_len = b64_len;
    memmove(b64, q, (size_t)(end - q));
    *io_len = len - remove_len;
    return true;
}

void clear_session_handle(void)
{
    session_handle[0] = '\0';
    session_resumable = false;
}

bool store_session_handle(const char *handle)
{
    if (!handle || handle[0] == '\0') return false;
    size_t len = strlen(handle);
    if (len >= sizeof(session_handle)) {
        ESP_LOGE(TAG, "Session handle terlalu panjang: %u", (unsigned)len);
        return false;
    }
    memcpy(session_handle, handle, len + 1);
    session_resumable = true;
    ESP_LOGI(TAG, "Session resumption handle tersimpan: %u byte", (unsigned)len);
    return true;
}

static void add_device_control_tool(cJSON *setup)
{
    if (!setup) return;

    cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
    if (!tools) return;

    cJSON *tool = cJSON_CreateObject();
    cJSON *functions = cJSON_AddArrayToObject(tool, "functionDeclarations");
    cJSON *decl = cJSON_CreateObject();
    cJSON_AddStringToObject(decl, "name", "control_device");
    /* Gemini 3.8 Live defaults to async tool calls; preserve the existing synchronous device-control flow. */
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Mengontrol perangkat rumah melalui UART dan kepala robot. Untuk gerakan kepala gunakan servo_kiri, servo_kanan, servo_anguk, servo_geleng, atau servo_stop. Perintah mp3_* hanya untuk pemutar MP3 fisik terpisah, bukan radio streaming (untuk radio gunakan putar_radio). Gunakan hanya jika pengguna meminta aksi perangkat. Setelah aksi berhasil, jawab natural dalam bahasa Indonesia.");

    cJSON *parameters = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(parameters, "type", "OBJECT");
    cJSON *properties = cJSON_AddObjectToObject(parameters, "properties");
    cJSON *command = cJSON_AddObjectToObject(properties, "command");
    cJSON_AddStringToObject(command, "type", "STRING");
    cJSON_AddStringToObject(command, "description", "Command perangkat yang harus dijalankan sebagai satu kali tekan tombol fisik.");

    cJSON *enum_values = cJSON_AddArrayToObject(command, "enum");
    static const char *const commands[] = {
        "r1", "r2", "r3", "r4",
        "fan_pwr", "fan_speed", "fan_swing", "fan_mode",
        "mp3_mode", "mp3_play", "mp3_eq",
        "m_led", "m_mute", "m_musik", "m_cek",
        "cek_suhu", "cek_cahaya",
        "servo_kiri", "servo_kanan", "servo_anguk", "servo_geleng", "servo_stop",
        "standby"
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        cJSON_AddItemToArray(enum_values, cJSON_CreateString(commands[i]));

    cJSON *required = cJSON_AddArrayToObject(parameters, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToArray(functions, decl);
    cJSON_AddItemToArray(tools, tool);
}


static void add_memory_tools(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL;
    cJSON *function_declarations = NULL;
    cJSON_ArrayForEach(tool, tools) {
        function_declarations = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(function_declarations)) break;
        function_declarations = NULL;
    }
    if (!function_declarations) return;

    cJSON *decls[3] = { cJSON_CreateObject(), cJSON_CreateObject(), cJSON_CreateObject() };
    const char *names[3] = { "memory_save", "memory_delete", "memory_read" };
    const char *descriptions[3] = {
        "Simpan informasi hanya jika pengguna secara eksplisit meminta untuk menyimpannya. Pahami ungkapan alami bahasa Indonesia seperti 'catat ini', 'tolong catat', 'ingat bahwa', 'ingat ya', 'jangan sampai lupa', 'simpan ini', 'tolong simpan', 'masukkan ke memori', atau 'jadikan catatan'; frasa tidak harus persis sama. Jangan simpan jika pengguna menolak atau membatalkan pencatatan. Buat key singkat dan stabil sesuai topik (misalnya 'Preferensi Jawaban'), lalu value berupa fakta lengkap dan jelas. Jangan meminta pengguna menentukan key/value, mengarang detail, atau menimpa topik lain. Setelah tool mengembalikan hasil, sampaikan konfirmasi suara sesuai hasil tersebut; jangan klaim berhasil jika gagal.",
        "Hapus hanya catatan/kalimat yang diminta. Argumen key harus berisi frasa unik dari kalimat target, bukan nama topik tebakan. Jika ada beberapa kecocokan atau target tidak jelas, jangan panggil tool; minta pengguna memperjelas. Tool hanya menghapus jika transkrip pengguna memuat frasa persis hapus catatan. Setelah tool mengembalikan hasil, sampaikan statusnya dengan jujur.",
        "Baca catatan jangka panjang sebagai sumber konteks untuk menjawab. Panggil memory_read jika catatan yang relevan belum tersedia dalam konteks aktif atau pengguna meminta melihat/membaca catatan. Gunakan isi catatan secara alami untuk menjawab, jangan meminta pengguna mengulang fakta yang sudah tercatat, dan prioritaskan koreksi terbaru yang jelas dari pengguna. Jika pengguna secara khusus meminta melihat atau membaca catatan, bacakan isi hasilnya melalui suara bahasa Indonesia; jika kosong, katakan belum ada catatan. Membaca tidak mengubah atau menghapus memori."
    };
    for (int i = 0; i < 3; ++i) {
        cJSON *decl = decls[i];
        if (!decl) continue;
        cJSON_AddStringToObject(decl, "name", names[i]);
        cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
        cJSON_AddStringToObject(decl, "description", descriptions[i]);
        cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
        cJSON_AddStringToObject(params, "type", "OBJECT");
        cJSON *props = cJSON_AddObjectToObject(params, "properties");
        if (i == 0) {
            cJSON *key = cJSON_AddObjectToObject(props, "key");
            cJSON_AddStringToObject(key, "type", "STRING");
            cJSON_AddStringToObject(key, "description", "Buat nama topik singkat dan stabil dari informasi pengguna, misalnya “Nama Anjing”. Jangan meminta pengguna menentukan key.");
            cJSON *value = cJSON_AddObjectToObject(props, "value");
            cJSON_AddStringToObject(value, "type", "STRING");
            cJSON_AddStringToObject(value, "description", "Simpan fakta lengkap dalam bahasa alami dan pertahankan detail penting. Contoh ucapan “catat ini nama anjing saya Boby” menjadi value “Nama anjing saya Boby”. Jangan masukkan instruksi “catat ini” ke dalam value.");
            cJSON *required = cJSON_AddArrayToObject(params, "required");
            cJSON_AddItemToArray(required, cJSON_CreateString("key"));
            cJSON_AddItemToArray(required, cJSON_CreateString("value"));
        } else if (i == 1) {
            cJSON *key = cJSON_AddObjectToObject(props, "key");
            cJSON_AddStringToObject(key, "type", "STRING");
            cJSON_AddStringToObject(key, "description", "Frasa unik yang mengidentifikasi kalimat yang akan dihapus. Jika satu catatan berisi banyak kalimat, pilih kalimat yang dimaksud; jangan menebak dan jangan gunakan nama topik luas jika dapat mengenai beberapa kalimat.");
            cJSON *required = cJSON_AddArrayToObject(params, "required");
            cJSON_AddItemToArray(required, cJSON_CreateString("key"));
        }
        cJSON_AddItemToArray(function_declarations, decl);
    }
}

/* Pilihan pencarian web:
 *   0 = DuckDuckGo lewat robot (tool web_search, butuh websocket_search.cpp)  <- aman untuk akun gratis
 *   1 = Google Search bawaan Gemini (googleSearch). Di akun gratis bisa ditolak: "exceeded your current quota".
 */
#define WEB_SEARCH_GOOGLE 0

__attribute__((unused)) static void add_web_search_tool_ddg(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL, *decls = NULL;
    cJSON_ArrayForEach(tool, tools) {
        decls = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(decls)) break;
        decls = NULL;
    }
    if (!decls) return;
    cJSON *decl = cJSON_CreateObject();
    if (!decl) return;
    cJSON_AddStringToObject(decl, "name", "web_search");
    /* Search must finish before Gemini continues with an answer based on unverified facts. */
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Cari informasi terkini di web saat pengguna bertanya tentang berita, fakta yang perlu diverifikasi, atau informasi yang mungkin berubah. Gunakan pencarian ini sebelum menjawab pertanyaan web. Layanan pencarian terpisah ini gratis dan hasilnya dapat terbatas. Jangan mengklaim sudah mencari jika tool gagal. Query harus ringkas dan spesifik. Setelah hasil pencarian untuk pertanyaan yang sama diterima, gunakan hasil itu dan jangan panggil web_search lagi kecuali pengguna mengajukan pertanyaan lanjutan yang membutuhkan informasi berbeda.");
    cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(params, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *query = cJSON_AddObjectToObject(props, "query");
    cJSON_AddStringToObject(query, "type", "STRING");
    cJSON_AddStringToObject(query, "description", "Kata kunci pencarian web yang ringkas dan spesifik.");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("query"));
    cJSON_AddItemToArray(decls, decl);
}

/* Pencarian web memakai Google Search bawaan Gemini (dijalankan di server Google,
 * bukan di robot). Cukup didaftarkan sebagai tool tambahan di setup. */
__attribute__((unused)) static void add_web_search_tool_google(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *gs = cJSON_CreateObject();
    if (!gs) return;
    if (!cJSON_AddObjectToObject(gs, "googleSearch")) {
        cJSON_Delete(gs);
        return;
    }
    cJSON_AddItemToArray(tools, gs);
}

static void add_web_search_tool(cJSON *setup)
{
#if WEB_SEARCH_GOOGLE
    add_web_search_tool_google(setup);
#else
    add_web_search_tool_ddg(setup);
#endif
}

static void add_alarm_tool(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL, *decls = NULL;
    cJSON_ArrayForEach(tool, tools) {
        decls = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(decls)) break;
        decls = NULL;
    }
    if (!decls) return;
    cJSON *decl = cJSON_CreateObject();
    if (!decl) return;
    cJSON_AddStringToObject(decl, "name", "alarm_control");
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Kelola alarm bangun dan pengingat suara (maksimal 8 sekaligus). Panggil HANYA saat pengguna secara eksplisit meminta memasang alarm, meminta diingatkan sesuatu ('ingatkan aku ...'), melihat daftar alarm/pengingat, atau membatalkannya. Zona waktu selalu WITA (UTC+8). action=set memasang: kind=bangun untuk membangunkan, kind=pengingat untuk mengingatkan hal lain (minum obat, cek masakan, rapat). Tentukan waktu dengan salah satu cara: hour+minute (jam 0-23, menit 0-59) ATAU minutes_from_now untuk 'N menit lagi' (misal 15 menit lagi = 15; 1 jam 30 menit lagi = 90). repeat: sekali (bawaan), setiap_hari, atau hari_kerja (Senin sampai Jumat); pengulangan hanya untuk hour+minute. label berisi keterangan singkat apa yang diingatkan, misalnya 'minum obat'. Jangan mengarang waktu atau keterangan; jika waktu atau isi pengingat tidak jelas, tanyakan dulu. action=status membacakan semua alarm beserta nomornya. action=cancel membatalkan: isi id (nomor) untuk satu alarm, atau all=true untuk semua; jika pengguna hanya bilang 'batalkan alarm' dan ada lebih dari satu, tanyakan nomor mana atau apakah semuanya. Setelah tool memberi hasil, sampaikan konfirmasi suara berdasarkan hasil itu dan jangan mengaku berhasil jika gagal.");
    cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(params, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *action = cJSON_AddObjectToObject(props, "action");
    cJSON_AddStringToObject(action, "type", "STRING");
    cJSON_AddStringToObject(action, "description", "Aksi: set, status, atau cancel.");
    cJSON *action_enum = cJSON_AddArrayToObject(action, "enum");
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("set"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("status"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("cancel"));
    cJSON *hour = cJSON_AddObjectToObject(props, "hour");
    cJSON_AddStringToObject(hour, "type", "INTEGER");
    cJSON_AddStringToObject(hour, "description", "Jam WITA format 24 jam, 0 sampai 23; untuk action=set jika memakai jam tertentu.");
    cJSON *minute = cJSON_AddObjectToObject(props, "minute");
    cJSON_AddStringToObject(minute, "type", "INTEGER");
    cJSON_AddStringToObject(minute, "description", "Menit 0 sampai 59; untuk action=set jika memakai jam tertentu.");
    cJSON *rel = cJSON_AddObjectToObject(props, "minutes_from_now");
    cJSON_AddStringToObject(rel, "type", "INTEGER");
    cJSON_AddStringToObject(rel, "description", "Berapa menit lagi dari sekarang (1 sampai 1440), untuk 'ingatkan 15 menit lagi'. Jika diisi, hour dan minute tidak dipakai.");
    cJSON *label = cJSON_AddObjectToObject(props, "label");
    cJSON_AddStringToObject(label, "type", "STRING");
    cJSON_AddStringToObject(label, "description", "Keterangan singkat apa yang diingatkan atau tujuan alarm, misalnya 'minum obat' atau 'untuk membangunkan'. Wajib untuk action=set.");
    cJSON *kind = cJSON_AddObjectToObject(props, "kind");
    cJSON_AddStringToObject(kind, "type", "STRING");
    cJSON_AddStringToObject(kind, "description", "bangun = membangunkan pengguna; pengingat = mengingatkan hal lain.");
    cJSON *kind_enum = cJSON_AddArrayToObject(kind, "enum");
    cJSON_AddItemToArray(kind_enum, cJSON_CreateString("bangun"));
    cJSON_AddItemToArray(kind_enum, cJSON_CreateString("pengingat"));
    cJSON *repeat = cJSON_AddObjectToObject(props, "repeat");
    cJSON_AddStringToObject(repeat, "type", "STRING");
    cJSON_AddStringToObject(repeat, "description", "sekali, setiap_hari, atau hari_kerja. Bawaan sekali.");
    cJSON *repeat_enum = cJSON_AddArrayToObject(repeat, "enum");
    cJSON_AddItemToArray(repeat_enum, cJSON_CreateString("sekali"));
    cJSON_AddItemToArray(repeat_enum, cJSON_CreateString("setiap_hari"));
    cJSON_AddItemToArray(repeat_enum, cJSON_CreateString("hari_kerja"));
    cJSON *idp = cJSON_AddObjectToObject(props, "id");
    cJSON_AddStringToObject(idp, "type", "INTEGER");
    cJSON_AddStringToObject(idp, "description", "Nomor alarm yang dibatalkan (1 sampai 8), untuk action=cancel.");
    cJSON *allp = cJSON_AddObjectToObject(props, "all");
    cJSON_AddStringToObject(allp, "type", "BOOLEAN");
    cJSON_AddStringToObject(allp, "description", "true untuk membatalkan semua alarm dan pengingat (action=cancel).");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("action"));
    cJSON_AddItemToArray(decls, decl);
}

static void add_volume_tool(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL, *decls = NULL;
    cJSON_ArrayForEach(tool, tools) {
        decls = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(decls)) break;
        decls = NULL;
    }
    if (!decls) return;
    cJSON *decl = cJSON_CreateObject();
    if (!decl) return;
    cJSON_AddStringToObject(decl, "name", "atur_volume");
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Mengatur volume speaker robot (suara Gemini dan radio). Panggil HANYA saat pengguna meminta volume diubah atau ditanya, misalnya 'keraskan suaramu', 'pelankan', 'volume 50', 'suaranya terlalu keras', atau 'berapa volume sekarang'. action=naik atau turun mengubah satu tingkat (10 poin). action=set memakai level persen (10 sampai 120, 100 = normal). action=cek hanya melaporkan volume. Setelah tool menjawab, sampaikan hasilnya dengan SATU kalimat singkat; jangan mengaku berhasil jika gagal.");
    cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(params, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *action = cJSON_AddObjectToObject(props, "action");
    cJSON_AddStringToObject(action, "type", "STRING");
    cJSON_AddStringToObject(action, "description", "naik, turun, set, atau cek.");
    cJSON *action_enum = cJSON_AddArrayToObject(action, "enum");
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("naik"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("turun"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("set"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("cek"));
    cJSON *level = cJSON_AddObjectToObject(props, "level");
    cJSON_AddStringToObject(level, "type", "INTEGER");
    cJSON_AddStringToObject(level, "description", "Persen volume 10 sampai 120 (100 = normal); wajib untuk action=set.");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("action"));
    cJSON_AddItemToArray(decls, decl);
}

/* Tool "simpan_pelajaran": Gemini menyimpan kalimat pendek tentang kebiasaan/selera
 * pengguna. Terpisah dari ai_memory (yang hanya boleh diubah lewat perintah eksplisit). */
static void add_learn_tool(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL, *decls = NULL;
    cJSON_ArrayForEach(tool, tools) {
        decls = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(decls)) break;
        decls = NULL;
    }
    if (!decls) return;
    cJSON *decl = cJSON_CreateObject();
    if (!decl) return;
    cJSON_AddStringToObject(decl, "name", "simpan_pelajaran");
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Simpan satu kalimat pendek (maksimal 100 karakter) tentang selera atau kebiasaan pengguna yang tidak sensitif, agar kamu makin mengenalnya. "
        "Gunakan action simpan secara diam-diam tanpa mengumumkannya. Jangan simpan kesehatan, uang, kata sandi, rahasia, atau data orang lain. "
        "Gunakan action baca jika pengguna bertanya apa yang sudah kamu pelajari, lalu ceritakan dengan santai. "
        "Gunakan action hapus_semua hanya jika pengguna jelas meminta kamu melupakan pelajaranmu; setelah itu konfirmasi dengan suara.");
    cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(params, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *action = cJSON_AddObjectToObject(props, "action");
    cJSON_AddStringToObject(action, "type", "STRING");
    cJSON_AddStringToObject(action, "description", "simpan, baca, atau hapus_semua.");
    cJSON *action_enum = cJSON_AddArrayToObject(action, "enum");
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("simpan"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("baca"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("hapus_semua"));
    cJSON *teks = cJSON_AddObjectToObject(props, "teks");
    cJSON_AddStringToObject(teks, "type", "STRING");
    cJSON_AddStringToObject(teks, "description", "Kalimat pendek yang dipelajari; wajib untuk action=simpan.");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("action"));
    cJSON_AddItemToArray(decls, decl);
}

/* Tool "riwayat_obrolan": membaca pesan terakhir yang tersimpan di DevKit (lewat UART). */
static void add_history_tool(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL, *decls = NULL;
    cJSON_ArrayForEach(tool, tools) {
        decls = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(decls)) break;
        decls = NULL;
    }
    if (!decls) return;
    cJSON *decl = cJSON_CreateObject();
    if (!decl) return;
    cJSON_AddStringToObject(decl, "name", "riwayat_obrolan");
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Membaca riwayat obrolan antara pengguna dan kamu (ratusan pesan, tersimpan lintas hari). Panggil HANYA saat pengguna bertanya tentang obrolan sebelumnya, misalnya 'tadi kita bahas apa', 'kemarin ngobrol apa', 'lanjutkan yang tadi', atau 'tadi aku tanya apa'. Jika pengguna menyebut harinya (kemarin, dua hari lalu), isi hari_lalu; jika hanya 'tadi', biarkan kosong. Jangan dipanggil untuk pertanyaan biasa. Hasilnya berisi pesan dari paling lama ke paling baru beserta kapan terjadinya; pesan paling bawah biasanya pertanyaan pengguna barusan, jadi abaikan. Jawab santai dengan merangkum isi hasil itu, dan jangan mengarang isi obrolan yang tidak ada di hasil. Jika hasilnya kosong, katakan terus terang belum ada riwayat.");
    cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(params, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *jumlah = cJSON_AddObjectToObject(props, "jumlah");
    cJSON_AddStringToObject(jumlah, "type", "INTEGER");
    cJSON_AddStringToObject(jumlah, "description", "Berapa pesan yang dibaca, 1 sampai 12. Bawaan 6 (10 jika hari_lalu diisi).");
    cJSON *hari = cJSON_AddObjectToObject(props, "hari_lalu");
    cJSON_AddStringToObject(hari, "type", "INTEGER");
    cJSON_AddStringToObject(hari, "description", "Pilih satu hari saja: 0 = hari ini, 1 = kemarin, 2 = dua hari lalu, dst (maks 60). Kosongkan jika pengguna tidak menyebut hari; maka dibaca pesan terakhir apa pun harinya.");
    cJSON_AddItemToArray(decls, decl);
}

static void add_ota_tool(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL, *decls = NULL;
    cJSON_ArrayForEach(tool, tools) {
        decls = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(decls)) break;
        decls = NULL;
    }
    if (!decls) return;
    cJSON *decl = cJSON_CreateObject();
    if (!decl) return;
    cJSON_AddStringToObject(decl, "name", "update_firmware");
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Memeriksa atau memasang pembaruan program (firmware) robot ini lewat internet. Panggil HANYA jika pengguna jelas meminta cek update/pembaruan program robot (action=cek) atau jelas meminta memasang update (action=pasang). Untuk action=pasang, pengguna harus benar-benar memintanya; jika ragu, tanyakan dulu apakah dia yakin. Setelah tool menjawab, sampaikan hasilnya dengan singkat. Jika sedang memasang, robot akan restart sendiri; jangan mengaku sudah selesai.");
    cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(params, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *action = cJSON_AddObjectToObject(props, "action");
    cJSON_AddStringToObject(action, "type", "STRING");
    cJSON_AddStringToObject(action, "description", "cek = hanya periksa versi baru; pasang = unduh dan pasang versi baru.");
    cJSON *action_enum = cJSON_AddArrayToObject(action, "enum");
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("cek"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("pasang"));
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("action"));
    cJSON_AddItemToArray(decls, decl);
}

static void ota_tool_done(void *ctx, const char *text)
{
    char *id = (char *)ctx;
    if (!id) return;
    (void)websocket_send_tool_response_text(id, "update_firmware", (text && text[0]) ? text : "Selesai.");
    free(id);
}

static void add_radio_tool(cJSON *setup)
{
    if (!setup) return;
    cJSON *tools = cJSON_GetObjectItem(setup, "tools");
    if (!cJSON_IsArray(tools)) return;
    cJSON *tool = NULL, *decls = NULL;
    cJSON_ArrayForEach(tool, tools) {
        decls = cJSON_GetObjectItem(tool, "functionDeclarations");
        if (cJSON_IsArray(decls)) break;
        decls = NULL;
    }
    if (!decls) return;
    cJSON *decl = cJSON_CreateObject();
    if (!decl) return;
    cJSON_AddStringToObject(decl, "name", "putar_radio");
    cJSON_AddStringToObject(decl, "behavior", "BLOCKING");
    cJSON_AddStringToObject(decl, "description",
        "Memutar radio streaming internet lewat speaker robot. Panggil HANYA saat pengguna meminta memutar radio atau musik dari radio, misalnya 'putar radio Genta', 'putar Menara FM', atau 'putar radio Bali'. Ini BUKAN pemutar MP3 fisik (itu control_device mp3_*). Isi query dengan nama stasiun yang diucapkan pengguna; jika pengguna hanya bilang 'putar radio' atau 'radio Bali', isi 'Bali'. Setelah tool menjawab, ucapkan SATU kalimat singkat bahwa radio akan diputar dan pengguna dapat menyentuh sensor sentuh untuk menghentikannya, lalu berhenti bicara. Radio menyala otomatis setelah kamu selesai bicara dan percakapan dijeda sampai pengguna menyentuh sensor. Jika tool gagal, jangan mengaku berhasil.");
    cJSON *params = cJSON_AddObjectToObject(decl, "parameters");
    cJSON_AddStringToObject(params, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *query = cJSON_AddObjectToObject(props, "query");
    cJSON_AddStringToObject(query, "type", "STRING");
    cJSON_AddStringToObject(query, "description", "Nama stasiun radio, mis. 'Genta FM', 'Menara FM', 'Beat Radio', atau 'Bali' untuk radio Bali populer.");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("query"));
    cJSON_AddItemToArray(decls, decl);
}

bool build_gemini_setup(char **output, size_t *output_len)
{
    if (!output || !output_len) return false;
    *output = NULL;
    *output_len = 0;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON *generation_config = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = cJSON_AddArrayToObject(generation_config, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
    cJSON *speech_config = cJSON_AddObjectToObject(generation_config, "speechConfig");
    cJSON_AddStringToObject(speech_config, "languageCode", "id-ID");
    cJSON *voice_config = cJSON_AddObjectToObject(speech_config, "voiceConfig");
    cJSON *prebuilt = cJSON_AddObjectToObject(voice_config, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", "Kore");
    cJSON_AddStringToObject(setup, "model", "models/gemini-3.8-live");
    /* Bias input transcription toward Indonesian. speechConfig.languageCode only
     * selects the generated voice language; it does not pin input transcription. */
    cJSON *input_transcription = cJSON_AddObjectToObject(setup, "inputAudioTranscription");
    cJSON *input_language_codes = cJSON_AddArrayToObject(input_transcription, "languageCodes");
    cJSON_AddItemToArray(input_language_codes, cJSON_CreateString("id-ID"));

    cJSON *system_instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *system_parts = cJSON_AddArrayToObject(system_instruction, "parts");
    cJSON *system_text = cJSON_CreateObject();
    cJSON_AddStringToObject(system_text, "text",
            "Kamu adalah asisten suara berbahasa Indonesia. Nama panggilan perangkat ini adalah ESP (dibaca e-es-pe, bukan ISP). Jika pengguna menyapa dengan 'halo ESP', 'hai ESP', 'halo ISP', atau yang mirip, itu sapaan kepadamu. Jika audio terdengar seperti ucapan yang agak keliru atau tidak masuk akal, gunakan konteks percakapan dan perintah perangkat yang umum untuk menafsirkan maksud paling mungkin; terutama koreksi salah dengar yang masuk akal seperti 'ikan kipas' menjadi 'hidupkan kipas' hanya bila konteks mendukung. Jika maksud perintah alat masih tidak cukup jelas, tanyakan klarifikasi dan jangan jalankan aksi yang berisiko. Jika yang terdengar hanya gumaman, nyanyian, suara TV, musik, atau percakapan orang lain yang tidak ditujukan kepada robot, jangan memulai percakapan atau menjalankan tool; diam, atau jawab sangat singkat hanya jika perlu. Jangan menganggap setiap suara latar sebagai perintah pengguna. Untuk alarm, patuhi zona waktu WITA (UTC+8). Jika pengguna secara eksplisit meminta memasang, mengubah, melihat, atau membatalkan alarm atau pengingat (misalnya ingatkan aku jam delapan minum obat, atau ingatkan 15 menit lagi), gunakan tool alarm_control dan jangan hanya menjanjikan tindakan. Untuk membangunkan pakai kind bangun; untuk mengingatkan hal lain pakai kind pengingat. Jika waktu atau tujuan alarm tidak jelas, tanyakan klarifikasi sebelum memanggil tool. Alarm atau pengingat yang tiba akan memulai sesi Gemini otomatis; ucapkan sapaan yang alami sesuai jenis dan keterangannya, tanpa nada alarm. " 
    "Dalam sesi percakapan Gemini Live yang sedang aktif, gunakan konteks percakapan sebelumnya pada sesi yang sama untuk memahami rujukan dan pertanyaan lanjutan; jangan memperlakukan setiap turn sebagai percakapan baru. " 
    "Gaya bicara: rumah ini sering ramai oleh banyak orang. Bicaralah seperti teman dekat yang santai dan hangat, bukan seperti mesin. Jawab singkat, satu atau dua kalimat, kecuali pengguna meminta penjelasan panjang. Jangan membacakan daftar panjang, jangan mengulang pertanyaan pengguna, dan jangan membuka jawaban dengan basa-basi yang sama setiap kali. Sesekali boleh bertanya balik dengan singkat bila itu membantu. Tunggu sampai kalimat pengguna benar-benar selesai sebelum menjawab. Jika yang terdengar jelas bukan ditujukan kepadamu (orang lain mengobrol, TV, musik), tetap diam dan jangan menjawab. "
    "Aturan penting saat rumah ramai: jangan menanggapi ucapan yang sangat pendek atau tidak jelas ujungnya (misalnya iya, ibu, halo, oke, hmm, got it) dan jangan membalasnya dengan sapaan atau tawaran bantuan seperti 'ada yang bisa saya bantu'. Jangan bicara duluan kecuali sedang menjalankan alarm, pesan titipan, atau permintaan yang jelas. Jawab hanya jika ucapan itu jelas ditujukan kepadamu: pertanyaan atau perintah yang lengkap, atau lanjutan langsung dari obrolan kita barusan. Jika ragu apakah orang itu berbicara kepadamu atau kepada orang lain, tetap diam. Jangan menjawab dalam bahasa Inggris kecuali pengguna jelas memakai bahasa Inggris. "
    "Konteks sesi hanya berlaku untuk sesi aktif. Ketika perangkat memulai sesi baru tanpa session-resumption handle, perlakukan percakapan sebagai baru dan jangan menganggap kamu mengingat sesi sebelumnya. "
    "Jika pengguna meminta aksi pada perangkat, gunakan fungsi control_device dengan command tombol yang tepat. "
    "Semua aksi perangkat berarti menekan tombol fisik satu kali. Kamu tidak mengetahui dan tidak boleh menebak keadaan fisik perangkat, serta tidak menyimpan atau melacak status ON/OFF. "
    "Pahami perintah natural seperti \"tekan tombol play\", \"hidupkan kipas\", \"matikan kipas\", \"naikkan kecepatan kipas\", \"turunkan kecepatan kipas\", \"ubah mode kipas\", \"tekan tombol colokan harian\", \"tekan tombol colokan panjang\", \"tekan tombol saklar lampu\", dan \"tekan tombol power MP3\". "
    "Pemetaan tombol: colokan harian=r1, colokan panjang=r2, saklar lampu=r3, power MP3=r4, fan_pwr=tombol power kipas untuk mematikan kipas, fan_speed=tombol speed kipas untuk menghidupkan kipas dan mengubah kecepatan, fan_swing=ayunan kipas, fan_mode=mode kipas, mode MP3=mp3_mode, play=mp3_play, EQ=mp3_eq, LED=m_led, mute=m_mute, musik=m_musik, cek=m_cek. "
    "Jika pengguna mengatakan hidupkan kipas, gunakan fan_speed satu kali. Jika pengguna mengatakan matikan kipas, gunakan fan_pwr satu kali. Jika pengguna meminta naikkan atau turunkan kecepatan kipas, gunakan fan_speed satu kali. Jangan gunakan fan_pwr untuk menghidupkan kipas. "
    "Untuk relay, colokan, lampu, MP3, dan tombol lainnya, kata hidupkan, matikan, atau tekan tidak mengubah jumlah tekanan: selalu kirim command yang sesuai tepat satu kali. "
    "Setelah fungsi berhasil, respons mengikuti maksud pengguna secara natural. Jika pengguna mengatakan hidupkan, katakan bahwa sudah dihidupkan. Jika mengatakan matikan, katakan bahwa sudah dimatikan. Jika mengatakan tekan, katakan bahwa tombol sudah ditekan. Jangan mengklaim mengetahui status fisik perangkat. "
    "Jika pengguna meminta standby, selesai, atau asisten kembali menunggu kata Hi, ESP, gunakan command standby satu kali. Setelah standby, perangkat kembali menunggu wake word. "
    "Jangan membuat atau menggunakan command on/off berbasis status. Jangan mengarang command. "
    "Tunggu hasil fungsi sebelum menyatakan tombol berhasil ditekan. "
    "Jangan pernah mengucapkan nama command UART kepada pengguna. "
    "Memori jangka panjang hanya boleh diubah jika transkrip pengguna menunjukkan permintaan eksplisit. Untuk menyimpan, pahami ungkapan alami bahasa Indonesia seperti \"catat ini\", \"tolong catat\", \"ingat bahwa\", \"tolong ingat\", \"jangan sampai lupa\", \"simpan ini\", \"tolong simpan\", \"masukkan ke memori\", atau \"jadikan catatan\"; frasa tidak harus persis sama. Jangan menyimpan jika pengguna melarang atau membatalkan pencatatan, dan jangan menganggap percakapan biasa sebagai izin. Untuk menghapus, tetap wajib ada frasa persis \"hapus catatan\". Untuk pencatatan, jangan meminta format, key, atau value. Tentukan key pendek dan stabil berdasarkan topik, dan value berisi fakta lengkap dalam bahasa Indonesia dengan detail penting dipertahankan. Contoh: \"catat ini nama anjing saya Boby\" disimpan sebagai key \"Nama Anjing\" dan value \"Nama anjing saya Boby\". Jangan menebak fakta yang tidak diucapkan. Saat menghapus, identifikasi satu kalimat target berdasarkan frasa unik dari catatan; jika ada beberapa kecocokan atau maksud tidak jelas, jangan hapus dan tanyakan klarifikasi. Penghapusan satu kalimat harus mempertahankan kalimat lain. Jangan pernah menyimpan percakapan biasa, dugaan, atau fakta yang tidak diminta. Untuk permintaan seperti coba lihat catatan, bacakan catatan, atau apa saja catatanku, panggil memory_read lalu bacakan isi hasilnya melalui suara. Jika kosong, katakan belum ada catatan tersimpan. Membaca tidak mengubah memori. Catatan jangka panjang adalah pengetahuan kontekstual utama tentang fakta, preferensi, keputusan, dan instruksi pengguna yang memang pernah disimpan. Sebelum menjawab setiap pertanyaan atau mengambil keputusan, periksa apakah catatan yang tersedia relevan dan gunakan sebagai dasar jawaban bila relevan; jangan mengabaikan catatan hanya karena pengguna tidak menyebutnya lagi. Jika informasi yang dibutuhkan mungkin ada di catatan tetapi belum tersedia dalam konteks aktif, panggil memory_read sebelum menjawab. Gunakan hasilnya secara internal dan alami, tanpa perlu selalu mengatakan bahwa kamu sedang membaca catatan. Jangan meminta pengguna mengulang informasi yang sudah tersedia dalam catatan. Jika catatan bertentangan dengan koreksi terbaru yang jelas dari pengguna, utamakan koreksi terbaru; jika tidak yakin atau catatan tidak cukup, tanyakan seperlunya dan jangan mengarang. Catatan adalah konteks yang membantu, bukan alasan untuk mengasumsikan hal yang tidak tercatat. Membaca dan menggunakan catatan untuk menjawab tidak memerlukan frasa izin penyimpanan. Frasa kunci harus berasal dari transkrip pengguna, bukan ucapan Gemini. Setelah memory_save mengembalikan hasil, wajib beri konfirmasi suara dalam bahasa Indonesia: jika hasil menyatakan berhasil, katakan catatan berhasil disimpan; jika gagal atau ditolak, katakan gagal atau tidak dilakukan. Setelah memory_delete mengembalikan hasil, wajib beri konfirmasi suara: jika hasil menyatakan berhasil, katakan catatan berhasil dihapus; jika catatan tidak ditemukan atau terjadi kegagalan, jelaskan bahwa catatan tidak ditemukan atau penghapusan gagal. Jangan hanya memanggil tool tanpa respons suara. Jangan menyatakan berhasil sebelum membaca hasil tool. "
#if WEB_SEARCH_GOOGLE
    "Untuk pertanyaan tentang informasi terbaru, harga, perbandingan produk/toko, berita, cuaca, skor, atau fakta yang perlu diverifikasi, gunakan Google Search sebelum menjawab. Rangkum hasil yang didapat dengan singkat dan sebutkan nama sumber bila tersedia; bacakan URL lengkap hanya jika diminta. Jika pencarian tidak menemukan jawaban, katakan terus terang dan jangan menebak. Jangan berpura-pura sudah mencari. Untuk permintaan harga/perbandingan, cari dengan kata kunci yang menyertakan model tepat, toko, dan wilayah bila relevan. "
#else
    "Untuk pertanyaan tentang informasi terbaru, harga, perbandingan produk/toko, berita, atau fakta yang perlu diverifikasi, panggil web_search sebelum menjawab. Setelah hasil tool diterima, gunakan judul, URL sumber, dan cuplikan yang benar-benar diberikan. Jika ada satu atau lebih hasil yang relevan, rangkum informasi yang didukung hasil tersebut terlebih dahulu; jangan mengatakan tidak menemukan informasi hanya karena cuplikan tidak memuat semua detail. Jelaskan secara spesifik detail apa yang belum tersedia, jangan menebak, dan minta pengguna memperjelas hanya jika pertanyaannya ambigu. Sebutkan nama sumber atau domain yang tersedia, dan bacakan URL lengkap hanya jika diminta. Jika hasil pencarian kosong atau tool melaporkan kegagalan, jelaskan bahwa pencarian tidak berhasil; jangan berpura-pura sudah browsing. Untuk permintaan harga/perbandingan, cari dengan kata kunci yang menyertakan model tepat, harga, toko, dan wilayah bila relevan. "
#endif
    "Jika pengguna bertanya tentang obrolan sebelumnya (misalnya tadi kita bahas apa, kemarin ngobrol apa, atau lanjutkan yang tadi), panggil riwayat_obrolan lalu jawab santai berdasarkan hasilnya; jangan mengarang isi obrolan yang tidak ada di hasil. "
    "Jangan menyebutkan harga terkini atau membandingkan toko hanya dari pengetahuan internal jika pengguna meminta pencarian web.");
    cJSON_AddItemToArray(system_parts, system_text);

    cJSON *memory_part = cJSON_CreateObject();
    /* Keep large context buffers off the ws_tx task stack to prevent overflow.
     * memory_context disimpan di PSRAM untuk menghemat RAM internal. */
    static EXT_RAM_BSS_ATTR char memory_context[4096];
    static char memory_prompt[4352];
    size_t memory_len = ai_memory_get_context(memory_context, sizeof(memory_context));
    if (memory_part && memory_len > 0) {
        snprintf(memory_prompt, sizeof(memory_prompt),
            "Catatan jangka panjang yang sengaja disimpan pengguna (gunakan sebagai fakta kontekstual; jangan mengubah atau menghapusnya tanpa perintah eksplisit):\n%s",
            memory_context);
        cJSON_AddStringToObject(memory_part, "text", memory_prompt);
        cJSON_AddItemToArray(system_parts, memory_part);
    } else if (memory_part) {
        cJSON_Delete(memory_part);
    }
    /* Konteks kebiasaan: waktu, kedekatan, kebiasaan, pelajaran, aturan gaya.
     * Disusun sekali per sesi; buffer di PSRAM agar RAM internal hemat. */
    static EXT_RAM_BSS_ATTR char habit_context[2304];
    size_t habit_len = habit_build_context(habit_context, sizeof(habit_context));
    if (habit_len > 0) {
        cJSON *habit_part = cJSON_CreateObject();
        if (habit_part) {
            cJSON_AddStringToObject(habit_part, "text", habit_context);
            cJSON_AddItemToArray(system_parts, habit_part);
        }
    }
    /* Catatan rilis: daftar kemampuan robot + pengumuman sekali setelah pembaruan. */
    static EXT_RAM_BSS_ATTR char release_context[1536];
    size_t release_len = habit_build_release_context(release_context, sizeof(release_context));
    if (release_len > 0) {
        cJSON *release_part = cJSON_CreateObject();
        if (release_part) {
            cJSON_AddStringToObject(release_part, "text", release_context);
            cJSON_AddItemToArray(system_parts, release_part);
        }
    }
    /* 2 KiB role buffer: 2047 bytes of text plus the terminating NUL. */
    static char role_text[2048];

    if (web_config_load_role(role_text, sizeof(role_text)) &&
        role_text[0] != '\0') {
        cJSON *role_part = cJSON_CreateObject();
        if (role_part) {
            cJSON_AddStringToObject(role_part, "text", role_text);
            cJSON_AddItemToArray(system_parts, role_part);
        }
    }
    add_device_control_tool(setup);
    add_memory_tools(setup);
    add_web_search_tool(setup);
    add_alarm_tool(setup);
    add_radio_tool(setup);
    add_ota_tool(setup);
    add_volume_tool(setup);
    add_learn_tool(setup);
    add_history_tool(setup);

    cJSON *realtime = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(realtime, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", false);
    /* Gemini menyelesaikan jawabannya dulu; suara latar (TV, obrolan) tidak membatalkannya. */
    cJSON_AddStringToObject(realtime, "activityHandling", "NO_INTERRUPTION");
    /* Kepekaan menangkap suara. HIGH = mudah menangkap ucapan (lebih stabil mendengarkan).
     * LOW = lebih tahan suara ramai, tapi sering kelewatan ucapan pelan/jauh.
     * Kalau robot terlalu sering menjawab obrolan orang lain, ubah ke 0 (LOW). */
#define AAD_START_HIGH 1
#if AAD_START_HIGH
    cJSON_AddStringToObject(aad, "startOfSpeechSensitivity", "START_SENSITIVITY_HIGH");
#else
    cJSON_AddStringToObject(aad, "startOfSpeechSensitivity", "START_SENSITIVITY_LOW");
#endif
    cJSON_AddNumberToObject(aad, "prefixPaddingMs", 300);
    cJSON_AddStringToObject(aad, "endOfSpeechSensitivity", "END_SENSITIVITY_LOW");
    cJSON_AddNumberToObject(aad, "silenceDurationMs", 700);
    cJSON *resumption = cJSON_AddObjectToObject(setup, "sessionResumption");
    cJSON *compression = cJSON_AddObjectToObject(setup, "contextWindowCompression");
    if (compression)
        cJSON_AddObjectToObject(compression, "slidingWindow");
    if (session_resumable && session_handle[0] != '\0')
        cJSON_AddStringToObject(resumption, "handle", session_handle);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        ESP_LOGE(TAG, "Gagal serialize Gemini setup JSON");
        return false;
    }
    *output = json;
    *output_len = strlen(json);
    ESP_LOGI(TAG, "Gemini setup: 3.8 LIVE + AUDIO + input transcription language=id-ID + output voice=id-ID + AAD START HIGH + session resumption + context compression + BLOCKING UART TOOL");
    return true;
}

static cJSON *parse_json_with_diagnostics(const char *json, size_t len)
{
    if (!json || len == 0) return NULL;
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &parse_end, 0);
    if (root != NULL) return root;
    const char *error_ptr = cJSON_GetErrorPtr();
    size_t error_offset = 0;
    if (error_ptr && error_ptr >= json && error_ptr < json + len)
        error_offset = (size_t)(error_ptr - json);
    else if (parse_end && parse_end >= json && parse_end <= json + len)
        error_offset = (size_t)(parse_end - json);
    ESP_LOGW(TAG, "Payload bukan JSON valid: %u byte, error_offset=%u",
             (unsigned)len, (unsigned)error_offset);
    if (error_offset < len) {
        size_t start = error_offset > 24 ? error_offset - 24 : 0;
        size_t remaining = len - start;
        size_t preview_len = remaining < 96 ? remaining : 96;
        char preview[97];
        for (size_t i = 0; i < preview_len; ++i) {
            unsigned char c = (unsigned char)json[start + i];
            preview[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        preview[preview_len] = '\0';
        ESP_LOGW(TAG, "JSON sekitar error @%u: \"%s\"", (unsigned)error_offset, preview);
    }
    ESP_LOGW(TAG, "JSON RX diagnostic: heap=%u", (unsigned)esp_get_free_heap_size());
    return NULL;
}


static char latest_user_transcript[768];
static bool pending_explicit_memory_save = false;
static bool pending_explicit_memory_delete = false;
/* Keep memory-read workspace off the WebSocket RX task stack (disimpan di PSRAM). */
static EXT_RAM_BSS_ATTR char memory_read_context[4096];

static bool transcript_has_any(const char *text, const char *const *needles, size_t count)
{
    if (!text) return false;
    char lower[768];
    size_t n = strlen(text);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = text[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    for (size_t i = 0; i < count; ++i)
        if (strstr(lower, needles[i])) return true;
    return false;
}

static char history_last_user_sent[768];
static char history_assistant_turn[1200];
static size_t history_assistant_len = 0;

static void history_send_user_once(const char *text)
{
    if (!text || !text[0] || strcmp(history_last_user_sent, text) == 0) return;
    strlcpy(history_last_user_sent, text, sizeof(history_last_user_sent));
    uart_control_send_history("U", text);
}

static void history_append_assistant(const char *text)
{
    if (!text) return;
    while (*text && history_assistant_len < sizeof(history_assistant_turn) - 1) {
        char c = *text++;
        if (c == '\r' || c == '\n') c = ' ';
        history_assistant_turn[history_assistant_len++] = c;
    }
    history_assistant_turn[history_assistant_len] = '\0';
}
static void remember_user_transcript(const char *text)
{
    if (!text) return;
    size_t len = strlen(text);
    if (len >= sizeof(latest_user_transcript)) len = sizeof(latest_user_transcript) - 1;
    memcpy(latest_user_transcript, text, len);
    latest_user_transcript[len] = '\0';

    static const char *const save_negations[] = {
        "jangan catat", "jangan mencatat", "jangan simpan", "jangan menyimpan",
        "jangan ingat", "jangan diingat", "jangan masukkan ke memori",
        "jangan masukkan ke catatan", "tidak usah catat", "tidak usah simpan",
        "tidak perlu catat", "tidak perlu mencatat", "tidak perlu simpan",
        "tidak perlu menyimpan", "tak usah catat", "tak usah simpan",
        "tidak jadi catat", "batal mencatat", "jangan sampai tersimpan"
    };
    static const char *const delete_negations[] = {
        "jangan hapus catatan", "jangan menghapus catatan", "tidak usah hapus catatan"
    };
    /*
     * Natural Indonesian save requests. Keep these as explicit command-like
     * phrases; do not authorize saving from the bare words "ingat" or "simpan".
     */
    static const char *const save_words[] = {
        "catat ini", "tolong catat", "catat bahwa", "catat kalau", "catat ya",
        "ingat bahwa", "ingat kalau", "ingat ya", "tolong ingat",
        "saya ingin kamu ingat", "saya mau kamu ingat", "jangan sampai lupa",
        "simpan ini", "tolong simpan", "simpan informasi", "simpan bahwa",
        "simpan kalau", "simpan untuk nanti", "masukkan ke memori",
        "masukkan dalam memori", "masukkan ke catatan", "masukkan dalam catatan",
        "jadikan catatan", "rekam ini", "ingat untuk nanti"
    };
    static const char *const delete_words[] = { "hapus catatan" };
    bool save_negated = transcript_has_any(latest_user_transcript, save_negations,
                                           sizeof(save_negations) / sizeof(save_negations[0]));
    bool delete_negated = transcript_has_any(latest_user_transcript, delete_negations,
                                             sizeof(delete_negations) / sizeof(delete_negations[0]));
    /*
     * Transcription can arrive in multiple events. Keep explicit authorization
     * latched for the current user turn once its phrase is seen, rather than
     * losing it when a later transcript fragment omits the opening words.
     * Clear it on negation, after the tool consumes it, or at turnComplete.
     */
    if (save_negated) {
        pending_explicit_memory_save = false;
    } else if (transcript_has_any(latest_user_transcript, save_words,
                                  sizeof(save_words) / sizeof(save_words[0]))) {
        pending_explicit_memory_save = true;
        ESP_LOGI(TAG, "AI_MEMORY: izin simpan eksplisit terdeteksi pada transkrip pengguna");
    }
    if (delete_negated) {
        pending_explicit_memory_delete = false;
    } else if (transcript_has_any(latest_user_transcript, delete_words,
                                  sizeof(delete_words) / sizeof(delete_words[0]))) {
        pending_explicit_memory_delete = true;
        ESP_LOGI(TAG, "AI_MEMORY: izin hapus eksplisit terdeteksi pada transkrip pengguna");
    }
}

static bool explicit_memory_save_request(void)
{
    bool allowed = pending_explicit_memory_save;
    pending_explicit_memory_save = false;
    return allowed;
}

static bool explicit_memory_delete_request(void)
{
    bool allowed = pending_explicit_memory_delete;
    pending_explicit_memory_delete = false;
    return allowed;
}

static void process_gemini_tool_call(cJSON *tool_call)
{
    if (!cJSON_IsObject(tool_call)) return;

    cJSON *function_calls = cJSON_GetObjectItem(tool_call, "functionCalls");
    if (!cJSON_IsArray(function_calls)) return;

    cJSON *fc = NULL;
    cJSON_ArrayForEach(fc, function_calls) {
        if (!cJSON_IsObject(fc)) continue;

        cJSON *id = cJSON_GetObjectItem(fc, "id");
        cJSON *name = cJSON_GetObjectItem(fc, "name");
        cJSON *args = cJSON_GetObjectItem(fc, "args");
        if (!cJSON_IsString(id) || !id->valuestring ||
            !cJSON_IsString(name) || !name->valuestring ||
            !cJSON_IsObject(args)) {
            ESP_LOGW(TAG, "Tool call Gemini tidak lengkap");
            continue;
        }

        ESP_LOGI(TAG, "Gemini TOOL CALL: %s id=%s", name->valuestring, id->valuestring);

        bool success = false;
        if (strcmp(name->valuestring, "memory_save") == 0) {
            cJSON *key = cJSON_GetObjectItem(args, "key");
            cJSON *value = cJSON_GetObjectItem(args, "value");
            if (!explicit_memory_save_request()) {
                ESP_LOGW(TAG, "memory_save ditolak: tidak ada perintah eksplisit pada transkrip pengguna");
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    "Ditolak: belum terdeteksi perintah eksplisit untuk menyimpan catatan. Ucapkan misalnya 'tolong ingat bahwa ...' atau 'simpan ini ...'.");
                continue;
            }
            if (cJSON_IsString(key) && key->valuestring && cJSON_IsString(value) && value->valuestring) {
                ESP_LOGI(TAG, "memory_save diterima: key_len=%u value_len=%u",
                         (unsigned)strlen(key->valuestring), (unsigned)strlen(value->valuestring));
                success = ai_memory_save(key->valuestring, value->valuestring);
                ESP_LOGI(TAG, "memory_save hasil=%s", success ? "OK" : "GAGAL");
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    success ? "Catatan berhasil disimpan secara permanen." :
                    "Gagal menyimpan catatan. Periksa log AI_MEMORY untuk penyebab penyimpanan.");
                continue;
            }
        } else if (strcmp(name->valuestring, "memory_delete") == 0) {
            cJSON *key = cJSON_GetObjectItem(args, "key");
            if (!explicit_memory_delete_request()) {
                ESP_LOGW(TAG, "memory_delete ditolak: tidak ada perintah hapus eksplisit pada transkrip pengguna");
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    "Ditolak: catatan hanya boleh dihapus jika transkrip pengguna memuat frasa persis 'hapus catatan'.");
                continue;
            }
            if (cJSON_IsString(key) && key->valuestring) {
                char delete_detail[256] = {0};
                int delete_status = ai_memory_delete_target(key->valuestring, delete_detail, sizeof(delete_detail));
                success = (delete_status == 1);
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    delete_detail[0] ? delete_detail : "Penghapusan gagal; tidak ada perubahan yang dikonfirmasi.");
                continue;
            }
        } else if (strcmp(name->valuestring, "memory_read") == 0) {
            memory_read_context[0] = '\0';
            size_t memory_len = ai_memory_get_context(memory_read_context, sizeof(memory_read_context));
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                memory_len ? memory_read_context : "Belum ada catatan jangka panjang yang tersimpan.");
            continue;
#if !WEB_SEARCH_GOOGLE
        } else if (strcmp(name->valuestring, "web_search") == 0) {
            cJSON *query = cJSON_GetObjectItem(args, "query");
            if (!cJSON_IsString(query) || !query->valuestring || !query->valuestring[0]) {
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    "Pencarian gagal: query kosong.");
            } else {
                habit_note_event("cari");
                websocket_set_mic_uplink_paused(true);
                if (!websocket_search_start(id->valuestring, query->valuestring)) {
                    websocket_set_mic_uplink_paused(false);
                    (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                        "Pencarian web belum bisa dimulai karena memori atau task tidak tersedia. Coba lagi.");
                }
            }
            continue;
#endif
        } else if (strcmp(name->valuestring, "putar_radio") == 0) {
            cJSON *query = cJSON_GetObjectItem(args, "query");
            const char *qs = (cJSON_IsString(query) && query->valuestring) ? query->valuestring : "";
            habit_note_event("radio");
            /* Tahan mic selagi mencari stasiun; dibuka lagi setelah balasan Gemini selesai. */
            websocket_set_mic_uplink_paused(true);
            if (!websocket_radio_start(id->valuestring, qs)) {
                websocket_set_mic_uplink_paused(false);
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    "Radio belum bisa dimulai karena memori atau task tidak tersedia. Coba lagi.");
            }
            continue;
        } else if (strcmp(name->valuestring, "atur_volume") == 0) {
            cJSON *action = cJSON_GetObjectItem(args, "action");
            cJSON *level = cJSON_GetObjectItem(args, "level");
            const char *act = (cJSON_IsString(action) && action->valuestring) ? action->valuestring : "";
            const int before = audio_hal_get_volume();
            char detail[160] = {};
            if (strcmp(act, "cek") == 0) {
                snprintf(detail, sizeof(detail), "Volume sekarang %d persen (100 = normal, batas 10 sampai 120).", before);
            } else {
                int target = before;
                bool valid = true;
                if (strcmp(act, "naik") == 0) target = before + 10;
                else if (strcmp(act, "turun") == 0) target = before - 10;
                else if (strcmp(act, "set") == 0 && cJSON_IsNumber(level)) target = level->valueint;
                else valid = false;
                if (!valid) {
                    snprintf(detail, sizeof(detail), "Gagal: aksi volume tidak valid atau level belum disebut.");
                } else {
                    const int now_vol = audio_hal_set_volume(target);
                    if (now_vol == before && (target > before || target < before)) {
                        snprintf(detail, sizeof(detail), "Volume sudah di batas %s, tetap %d persen.",
                                 target > before ? "paling keras" : "paling pelan", now_vol);
                    } else {
                        snprintf(detail, sizeof(detail), "Volume diubah dari %d ke %d persen.", before, now_vol);
                    }
                }
            }
            ESP_LOGI(TAG, "VOLUME TOOL action=%s -> %s", act, detail);
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring, detail);
            continue;
        } else if (strcmp(name->valuestring, "riwayat_obrolan") == 0) {
            cJSON *jumlah = cJSON_GetObjectItem(args, "jumlah");
            cJSON *hari_lalu = cJSON_GetObjectItem(args, "hari_lalu");
            const int days = cJSON_IsNumber(hari_lalu) ? hari_lalu->valueint : -1;
            int n = cJSON_IsNumber(jumlah) ? jumlah->valueint : (days >= 0 ? 10 : 6);
            static EXT_RAM_BSS_ATTR char hist_buf[3584];   /* static di PSRAM: tidak membebani stack */
            static const char hist_header[] =
                "Riwayat obrolan terakhir (paling lama di atas, paling baru di bawah). "
                "Baris paling bawah biasanya pertanyaan pengguna barusan; abaikan.\n";
            const size_t header_len = sizeof(hist_header) - 1;
            memcpy(hist_buf, hist_header, header_len + 1);
            const size_t got = uart_control_request_history(n, days, hist_buf + header_len,
                                                            sizeof(hist_buf) - header_len, 2500);
            ESP_LOGI(TAG, "RIWAYAT TOOL jumlah=%d hari_lalu=%d hasil=%u byte", n, days, (unsigned)got);
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                got > 0 ? hist_buf
                        : "Belum ada riwayat obrolan yang tersimpan, atau perangkat penyimpan riwayat (DevKit) tidak menjawab. Katakan terus terang kamu belum bisa melihat obrolan sebelumnya.");
            continue;
        } else if (strcmp(name->valuestring, "simpan_pelajaran") == 0) {
            cJSON *action = cJSON_GetObjectItem(args, "action");
            cJSON *teks = cJSON_GetObjectItem(args, "teks");
            const char *act = (cJSON_IsString(action) && action->valuestring) ? action->valuestring : "";
            static char learn_buf[640];   /* static: tidak membebani stack task WebSocket */
            learn_buf[0] = '\0';
            if (strcmp(act, "simpan") == 0) {
                if (!cJSON_IsString(teks) || !teks->valuestring || !teks->valuestring[0]) {
                    snprintf(learn_buf, sizeof(learn_buf), "Gagal: kalimat pelajaran kosong.");
                } else {
                    (void)habit_lesson_add(teks->valuestring, learn_buf, sizeof(learn_buf));
                }
            } else if (strcmp(act, "baca") == 0) {
                if (habit_lesson_list(learn_buf, sizeof(learn_buf)) == 0)
                    snprintf(learn_buf, sizeof(learn_buf), "Belum ada yang kamu pelajari tentang pengguna.");
            } else if (strcmp(act, "hapus_semua") == 0) {
                static const char *const forget_terms[] = {
                    "lupakan", "hapus pelajaran", "hapus semua pelajaran", "reset pelajaran"
                };
                if (!transcript_has_any(latest_user_transcript, forget_terms,
                        sizeof(forget_terms) / sizeof(forget_terms[0]))) {
                    snprintf(learn_buf, sizeof(learn_buf),
                             "Ditolak: pengguna belum jelas meminta kamu melupakan pelajaranmu.");
                } else {
                    snprintf(learn_buf, sizeof(learn_buf), "%s",
                             habit_lesson_clear() ? "Semua pelajaran sudah dihapus. Konfirmasikan dengan suara."
                                                  : "Gagal menghapus pelajaran.");
                }
            } else {
                snprintf(learn_buf, sizeof(learn_buf), "Gagal: aksi tidak dikenal.");
            }
            ESP_LOGI(TAG, "PELAJARAN TOOL action=%s", act);
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring, learn_buf);
            continue;
        } else if (strcmp(name->valuestring, "update_firmware") == 0) {
            cJSON *action = cJSON_GetObjectItem(args, "action");
            bool install = cJSON_IsString(action) && action->valuestring &&
                           strcmp(action->valuestring, "pasang") == 0;
            char *id_copy = strdup(id->valuestring);
            if (!id_copy || !ota_update_run_async(install, ota_tool_done, id_copy)) {
                free(id_copy);
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    "Update belum bisa dimulai karena sedang sibuk atau memori kurang. Coba lagi nanti.");
            }
            continue;
        } else if (strcmp(name->valuestring, "alarm_control") == 0) {
            cJSON *action = cJSON_GetObjectItem(args, "action");
            if (!cJSON_IsString(action) || !action->valuestring) {
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring, "Gagal: aksi alarm tidak valid.");
                continue;
            }
            static char detail[900];   /* static: tidak membebani stack task WebSocket */
            detail[0] = '\0';
            static const char *const wakeup_terms[] = {
                "membangunkan", "bangunkan", "bangun pagi", "waktu bangun", "bangun"
            };
            if (strcmp(action->valuestring, "set") == 0) {
                cJSON *hour = cJSON_GetObjectItem(args, "hour");
                cJSON *minute = cJSON_GetObjectItem(args, "minute");
                cJSON *rel = cJSON_GetObjectItem(args, "minutes_from_now");
                cJSON *label = cJSON_GetObjectItem(args, "label");
                cJSON *kind_j = cJSON_GetObjectItem(args, "kind");
                cJSON *repeat_j = cJSON_GetObjectItem(args, "repeat");
                const bool relative = cJSON_IsNumber(rel) && rel->valueint > 0;
                if ((!relative && (!cJSON_IsNumber(hour) || !cJSON_IsNumber(minute))) ||
                    !cJSON_IsString(label) || !label->valuestring || !label->valuestring[0]) {
                    (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                        "Gagal: waktu (jam dan menit, atau berapa menit lagi) dan keterangan harus lengkap.");
                    continue;
                }
                const char *effective_label = label->valuestring;
                int kind = -1;
                if (cJSON_IsString(kind_j) && kind_j->valuestring) {
                    if (strcmp(kind_j->valuestring, "bangun") == 0) kind = ALARM_KIND_WAKE;
                    else if (strcmp(kind_j->valuestring, "pengingat") == 0) kind = ALARM_KIND_REMINDER;
                }
                /* Gemini kadang meringkas tujuan menjadi label generik "Alarm". Pulihkan
                 * tujuan membangunkan dari transkrip asli pengguna. */
                if (strcmp(effective_label, "Alarm") == 0 ||
                    strcmp(effective_label, "alarm") == 0 ||
                    strcmp(effective_label, "pengingat") == 0) {
                    if (transcript_has_any(latest_user_transcript, wakeup_terms,
                            sizeof(wakeup_terms) / sizeof(wakeup_terms[0]))) {
                        effective_label = "untuk membangunkan";
                        kind = ALARM_KIND_WAKE;
                        ESP_LOGW(TAG, "ALARM: label generik diperbaiki dari transkrip menjadi '%s'",
                                 effective_label);
                    }
                }
                if (kind < 0)
                    kind = transcript_has_any(effective_label, wakeup_terms,
                               sizeof(wakeup_terms) / sizeof(wakeup_terms[0]))
                           ? ALARM_KIND_WAKE : ALARM_KIND_REMINDER;
                int repeat = ALARM_REPEAT_ONCE;
                if (cJSON_IsString(repeat_j) && repeat_j->valuestring) {
                    if (strcmp(repeat_j->valuestring, "setiap_hari") == 0) repeat = ALARM_REPEAT_DAILY;
                    else if (strcmp(repeat_j->valuestring, "hari_kerja") == 0) repeat = ALARM_REPEAT_WEEKDAYS;
                }
                success = alarm_add(relative ? 0 : hour->valueint, relative ? 0 : minute->valueint,
                                    relative ? rel->valueint : 0, kind, repeat,
                                    effective_label, detail, sizeof(detail));
                if (success) habit_note_event("alarm");
            } else if (strcmp(action->valuestring, "cancel") == 0) {
                cJSON *id_j = cJSON_GetObjectItem(args, "id");
                cJSON *all_j = cJSON_GetObjectItem(args, "all");
                if (cJSON_IsTrue(all_j)) {
                    success = alarm_cancel_all(detail, sizeof(detail));
                } else if (cJSON_IsNumber(id_j)) {
                    success = alarm_cancel_id(id_j->valueint, detail, sizeof(detail));
                } else {
                    snprintf(detail, sizeof(detail),
                             "Gagal: sebutkan nomor alarm yang dibatalkan, atau minta batalkan semua.");
                }
            } else if (strcmp(action->valuestring, "status") == 0) {
                (void)alarm_describe_all(detail, sizeof(detail));
                success = true;
            } else {
                snprintf(detail, sizeof(detail), "Gagal: aksi alarm tidak dikenal.");
            }
            ESP_LOGI(TAG, "ALARM TOOL action=%s result=%s", action->valuestring, success ? "OK" : "GAGAL");
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                detail[0] ? detail : (success ? "Alarm berhasil diproses." : "Alarm gagal diproses."));
            continue;
        } else if (strcmp(name->valuestring, "control_device") == 0) {
            cJSON *command = cJSON_GetObjectItem(args, "command");

            if (cJSON_IsString(command) && command->valuestring) {
                const char *cmd = command->valuestring;

                if (strcmp(cmd, "standby") == 0) {
                    success = true;
                }
                else if (strcmp(cmd, "servo_kiri") == 0 ||
                         strcmp(cmd, "servo_kanan") == 0 ||
                         strcmp(cmd, "servo_anguk") == 0 ||
                         strcmp(cmd, "servo_geleng") == 0 ||
                         strcmp(cmd, "servo_stop") == 0) {
                    success = servo_head_command(cmd);
                    ESP_LOGI(TAG, "SERVO TOOL command=%s result=%s", cmd, success ? "OK" : "REJECTED");
                }
                else {
                    success = uart_control_execute_command(cmd);
                    ESP_LOGI(TAG, "DEVICE TOOL command=%s result=%s", cmd, success ? "OK" : "REJECTED");
                    if (success) {
                        if (strcmp(cmd, "r3") == 0) habit_note_event("lampu");
                        else if (strcmp(cmd, "r1") == 0 || strcmp(cmd, "r2") == 0) habit_note_event("colokan");
                        else if (strncmp(cmd, "fan_", 4) == 0) habit_note_event("kipas");
                        else if (strncmp(cmd, "mp3_", 4) == 0 || strcmp(cmd, "r4") == 0) habit_note_event("mp3");
                    }
                }
            } else {
                ESP_LOGW(TAG, "control_device tanpa argument command");
            }
        }

        if (!websocket_send_tool_response(id->valuestring, name->valuestring, success))
            ESP_LOGW(TAG, "Gagal mengirim toolResponse ke Gemini");
        if (success && strcmp(name->valuestring, "control_device") == 0) {
            cJSON *command = cJSON_GetObjectItem(args, "command");
            if (cJSON_IsString(command) && command->valuestring &&
                strcmp(command->valuestring, "standby") == 0) {
                /* Do not disconnect immediately after toolResponse: Gemini still
                 * needs to generate the spoken acknowledgement. */
                standby_after_turn_complete = true;
                ESP_LOGI(TAG, "STANDBY: toolResponse terkirim; menunggu turnComplete balasan Gemini");
            }
        }
    }
}

void process_gemini_message(const char *json, size_t len)
{
    if (!json || len == 0) return;
    size_t parse_len = len;
    (void)compact_large_audio_payload((char *)json, &parse_len);
    cJSON *root = parse_json_with_diagnostics(json, parse_len);
    if (!root) return;

    cJSON *setup_complete_obj = cJSON_GetObjectItem(root, "setupComplete");
    if (cJSON_IsObject(setup_complete_obj)) {
        setup_complete = true;
        ESP_LOGI(TAG, "Gemini setupComplete: SESI SIAP");
        display_status("AI terhubung");
        // Gemini is ready for microphone input; finish the wake reaction.
        face_set_state(FACE_THINKING);
        cJSON_Delete(root);
        return;
    }

    cJSON *input_transcription = cJSON_GetObjectItem(root, "inputTranscription");
    if (cJSON_IsObject(input_transcription)) {
        cJSON *text = cJSON_GetObjectItem(input_transcription, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            remember_user_transcript(text->valuestring);
            ESP_LOGI(TAG, "USER: %s", text->valuestring);
            display_set_user_text(text->valuestring);
            history_send_user_once(text->valuestring);
            // Some Gemini messages deliver transcription at the root level.
            if (text->valuestring[0] != '\0' &&
                face_get_state() == FACE_THINKING)
                face_set_state(FACE_THINKING);
        }
    }

    /* A final input transcript may be nested in serverContent beside toolCall.
     * Capture it before dispatching tools, while keeping device-tool turnComplete timing. */
    cJSON *early_server = cJSON_GetObjectItem(root, "serverContent");
    cJSON *early_input = NULL;
    if (cJSON_IsObject(early_server)) {
        early_input = cJSON_GetObjectItem(early_server, "inputTranscription");
        if (cJSON_IsObject(early_input)) {
            cJSON *early_text = cJSON_GetObjectItem(early_input, "text");
            if (cJSON_IsString(early_text) && early_text->valuestring)
                remember_user_transcript(early_text->valuestring);
        }
    }
    cJSON *tool_call = cJSON_GetObjectItem(root, "toolCall");
    if (cJSON_IsObject(tool_call))
        process_gemini_tool_call(tool_call);

    cJSON *output_transcription = cJSON_GetObjectItem(root, "outputTranscription");
    // Prefer the standard serverContent.outputTranscription stream when present.
    // Processing both forms in one JSON message caused duplicated/misaligned text.
    const bool has_server_output_transcription =
        cJSON_IsObject(early_server) &&
        cJSON_IsObject(cJSON_GetObjectItem(early_server, "outputTranscription"));
    if (!has_server_output_transcription && cJSON_IsObject(output_transcription)) {
        cJSON *text = cJSON_GetObjectItem(output_transcription, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            ESP_LOGI(TAG, "GEMINI TEXT (root): %s", text->valuestring);
            display_set_gemini_text(text->valuestring);
            history_append_assistant(text->valuestring);
        }
    }

    cJSON *server = cJSON_GetObjectItem(root, "serverContent");
    if (cJSON_IsObject(server)) {
        cJSON *interim_input_transcription = cJSON_GetObjectItem(server, "interimInputTranscription");
        if (cJSON_IsObject(interim_input_transcription)) {
            cJSON *text = cJSON_GetObjectItem(interim_input_transcription, "text");
            if (cJSON_IsString(text) && text->valuestring)
                display_set_user_text(text->valuestring);
        }

        cJSON *server_input_transcription = cJSON_GetObjectItem(server, "inputTranscription");
        if (cJSON_IsObject(server_input_transcription)) {
            cJSON *text = cJSON_GetObjectItem(server_input_transcription, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                /* Tool-call messages may already have consumed authorization from this same transcript. */
                cJSON *same_early_text = NULL;
                if (cJSON_IsObject(early_input))
                    same_early_text = cJSON_GetObjectItem(early_input, "text");
                bool already_captured = cJSON_IsString(same_early_text) &&
                    strcmp(same_early_text->valuestring, text->valuestring) == 0 &&
                    cJSON_IsObject(tool_call);
                if (!already_captured) remember_user_transcript(text->valuestring);
                ESP_LOGI(TAG, "USER: %s", text->valuestring);
                display_set_user_text(text->valuestring);
                history_send_user_once(text->valuestring);
                // Final user transcript marks the hand-off only while awaiting a response.
                // Ignore delayed transcripts after audio playback has started.
                if (text->valuestring[0] != '\0' &&
                    face_get_state() == FACE_THINKING)
                    face_set_state(FACE_THINKING);
            }
        }

        cJSON *server_output_transcription = cJSON_GetObjectItem(server, "outputTranscription");
        if (cJSON_IsObject(server_output_transcription)) {
            cJSON *text = cJSON_GetObjectItem(server_output_transcription, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                ESP_LOGI(TAG, "GEMINI TEXT: %s", text->valuestring);
                display_append_gemini_text(text->valuestring);
                history_append_assistant(text->valuestring);
            }
        }

        cJSON *turn = cJSON_GetObjectItem(server, "modelTurn");
        if (cJSON_IsObject(turn)) {
            // Start thinking on the first model response even if input transcription
            // is disabled. Never overwrite SPEAKING during streamed audio playback.
            if (face_get_state() == FACE_THINKING)
                face_set_state(FACE_THINKING);
            cJSON *parts = cJSON_GetObjectItem(turn, "parts");
            if (cJSON_IsArray(parts)) {
                cJSON *part = NULL;
                cJSON_ArrayForEach(part, parts) {
                    cJSON *inlineData = cJSON_GetObjectItem(part, "inlineData");
                    if (!cJSON_IsObject(inlineData)) continue;
                    cJSON *mimeType = cJSON_GetObjectItem(inlineData, "mimeType");
                    if (cJSON_IsString(mimeType) && mimeType->valuestring)
                        ESP_LOGD(TAG, "Gemini audio MIME: %s", mimeType->valuestring);
                    cJSON *audio = cJSON_GetObjectItem(inlineData, "data");
                    if (!cJSON_IsString(audio) || !audio->valuestring) continue;
                    const char *b64 = audio->valuestring;
                    size_t b64_len = strlen(b64);
                    if (b64_len == 0) continue;
                    if (b64_len > (WS_RX_MAX_PAYLOAD_SIZE * 2)) {
                        ESP_LOGE(TAG, "Base64 audio terlalu besar: %u", (unsigned)b64_len);
                        continue;
                    }
                    size_t pcm_len = 0;
                    int ret = mbedtls_base64_decode(NULL, 0, &pcm_len,
                                                    (const unsigned char *)b64, b64_len);
                    if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) continue;
                    if (pcm_len == 0 || pcm_len > PCM_DECODE_WORKSPACE_SIZE) continue;
                    size_t decoded = pcm_len;
                    ret = mbedtls_base64_decode(pcm_decode_buffer, sizeof(pcm_decode_buffer),
                                                &decoded, (const unsigned char *)b64, b64_len);
                    if (ret == 0 && decoded > 0) {
                        audio_chunks_received++;
                        audio_bytes_received += decoded;
                        ESP_LOGI(TAG, "AUDIO GEMINI: %u byte -> AUDIO BUFFER (PCM cap=%u)",
                                 (unsigned)decoded, (unsigned)PCM_DECODE_WORKSPACE_SIZE);
                        if (!queue_audio_pcm(pcm_decode_buffer, decoded))
                            ESP_LOGE(TAG, "Gagal memasukkan audio ke ring buffer");
                    }
                }
            }
        }

        cJSON *generation_complete = cJSON_GetObjectItem(server, "generationComplete");
        if (cJSON_IsTrue(generation_complete)) ESP_LOGI(TAG, "Gemini: GENERATION COMPLETE");

        cJSON *turn_complete = cJSON_GetObjectItem(server, "turnComplete");
        if (cJSON_IsTrue(turn_complete)) {
            if (history_assistant_len > 0) {
                uart_control_send_history("A", history_assistant_turn);
                history_assistant_len = 0;
                history_assistant_turn[0] = '\0';
            }
            history_last_user_sent[0] = '\0';
            habit_release_mark_announced();   /* pengumuman pembaruan sudah terucap; jangan diulang */
            /* Do not carry memory authorization into a later user turn. */
            pending_explicit_memory_save = false;
            pending_explicit_memory_delete = false;
            if (standby_after_turn_complete) {
                standby_after_turn_complete = false;
                ESP_LOGI(TAG, "STANDBY: balasan Gemini selesai dibuat; menunggu audio selesai diputar");
                websocket_request_standby();
            }
            audio_turn_complete_pending = true;
            size_t pending = get_audio_pending_bytes();
            ESP_LOGI(TAG, "Gemini: TURN COMPLETE - menunggu audio drain");
            ESP_LOGI(TAG, "AUDIO SUMMARY: chunks=%u received=%llu played=%llu pending=%u write_calls=%u",
                     (unsigned)audio_chunks_received, (unsigned long long)audio_bytes_received,
                     (unsigned long long)audio_bytes_played, (unsigned)pending,
                     (unsigned)audio_write_calls);
        }

    }

    cJSON *resume = cJSON_GetObjectItem(root, "sessionResumptionUpdate");
    if (cJSON_IsObject(resume)) {
        cJSON *handle = cJSON_GetObjectItem(resume, "newHandle");
        cJSON *resumable = cJSON_GetObjectItem(resume, "resumable");
        bool can_resume = cJSON_IsTrue(resumable);
        if (can_resume && cJSON_IsString(handle) && handle->valuestring && handle->valuestring[0] != '\0') {
            if (store_session_handle(handle->valuestring))
                ESP_LOGI(TAG, "Session resumption: resumable=true, handle updated");
        } else {
            /* Match RepoV3: a non-resumable update does not invalidate the last
             * known-good handle; replace it only when a new valid handle arrives. */
            ESP_LOGI(TAG, "Session resumption: update not resumable; last valid handle retained");
        }
    }

    cJSON *go_away = cJSON_GetObjectItem(root, "goAway");
    if (cJSON_IsObject(go_away)) {
        cJSON *time_left = cJSON_GetObjectItem(go_away, "timeLeft");
        if (cJSON_IsString(time_left) && time_left->valuestring) {
            ESP_LOGW(TAG, "Gemini GO AWAY diterima; timeLeft=%s",
                     time_left->valuestring);
        } else {
            ESP_LOGW(TAG, "Gemini GO AWAY diterima");
        }

        /* GOAWAY is advisory while the current transport is usable. If a valid
         * resumption handle exists, rotate the socket and continue the same Gemini
         * conversation; otherwise leave the current socket alone. */
        websocket_request_resume_reconnect();

    }

    cJSON_Delete(root);
}
