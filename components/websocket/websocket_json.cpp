#include "websocket_internal.h"
#include "web_config.h"
#include "display.h"
#include "audio_hal.h"
#include "uart_control.h"
#include "ai_memory.h"
#include "speaker_id.h"
#include "servo_head.h"

#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "WS_JSON";

/* v7.0.25: fixed PCM decode workspace. */
#define PCM_DECODE_WORKSPACE_SIZE (24 * 1024)
static uint8_t pcm_decode_buffer[PCM_DECODE_WORKSPACE_SIZE];

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
        "Mengontrol perangkat rumah melalui UART dan kepala robot. Untuk gerakan kepala gunakan servo_kiri, servo_kanan, servo_anguk, servo_geleng, atau servo_stop. Gunakan hanya jika pengguna meminta aksi perangkat. Setelah aksi berhasil, jawab natural dalam bahasa Indonesia.");

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
            "Kamu adalah asisten suara berbahasa Indonesia. " 
    "Sebelum menjawab setiap giliran ucapan pengguna, panggil speaker_identify satu kali. Jika cocok, gunakan nama profil secara natural; jika tidak cocok atau belum ada profil, gunakan sapaan netral dan jangan menebak nama. Untuk mendaftarkan suara, tunggu permintaan eksplisit pengguna yang memuat frasa daftarkan suara; panggil speaker_enroll dengan nama yang pengguna nyatakan. Setelah tool mengembalikan hasil, konfirmasikan dengan jujur, termasuk jika gagal. " 
    "Dalam sesi percakapan Gemini Live yang sedang aktif, gunakan konteks percakapan sebelumnya pada sesi yang sama untuk memahami rujukan dan pertanyaan lanjutan; jangan memperlakukan setiap turn sebagai percakapan baru. " 
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
    "Memori jangka panjang hanya boleh diubah jika transkrip pengguna menunjukkan permintaan eksplisit. Untuk menyimpan, pahami ungkapan alami bahasa Indonesia seperti \"catat ini\", \"tolong catat\", \"ingat bahwa\", \"tolong ingat\", \"jangan sampai lupa\", \"simpan ini\", \"tolong simpan\", \"masukkan ke memori\", atau \"jadikan catatan\"; frasa tidak harus persis sama. Jangan menyimpan jika pengguna melarang atau membatalkan pencatatan, dan jangan menganggap percakapan biasa sebagai izin. Untuk menghapus, tetap wajib ada frasa persis \"hapus catatan\". Untuk pencatatan, jangan meminta format, key, atau value. Tentukan key pendek dan stabil berdasarkan topik, dan value berisi fakta lengkap dalam bahasa Indonesia dengan detail penting dipertahankan. Contoh: \"catat ini nama anjing saya Boby\" disimpan sebagai key \"Nama Anjing\" dan value \"Nama anjing saya Boby\". Jangan menebak fakta yang tidak diucapkan. Saat menghapus, identifikasi satu kalimat target berdasarkan frasa unik dari catatan; jika ada beberapa kecocokan atau maksud tidak jelas, jangan hapus dan tanyakan klarifikasi. Penghapusan satu kalimat harus mempertahankan kalimat lain. Jangan pernah menyimpan percakapan biasa, dugaan, atau fakta yang tidak diminta. Untuk permintaan seperti coba lihat catatan, bacakan catatan, atau apa saja catatanku, panggil memory_read lalu bacakan isi hasilnya melalui suara. Jika kosong, katakan belum ada catatan tersimpan. Membaca tidak mengubah memori. Catatan jangka panjang adalah pengetahuan kontekstual utama tentang fakta, preferensi, keputusan, dan instruksi pengguna yang memang pernah disimpan. Sebelum menjawab setiap pertanyaan atau mengambil keputusan, periksa apakah catatan yang tersedia relevan dan gunakan sebagai dasar jawaban bila relevan; jangan mengabaikan catatan hanya karena pengguna tidak menyebutnya lagi. Jika informasi yang dibutuhkan mungkin ada di catatan tetapi belum tersedia dalam konteks aktif, panggil memory_read sebelum menjawab. Gunakan hasilnya secara internal dan alami, tanpa perlu selalu mengatakan bahwa kamu sedang membaca catatan. Jangan meminta pengguna mengulang informasi yang sudah tersedia dalam catatan. Jika catatan bertentangan dengan koreksi terbaru yang jelas dari pengguna, utamakan koreksi terbaru; jika tidak yakin atau catatan tidak cukup, tanyakan seperlunya dan jangan mengarang. Catatan adalah konteks yang membantu, bukan alasan untuk mengasumsikan hal yang tidak tercatat. Membaca dan menggunakan catatan untuk menjawab tidak memerlukan frasa izin penyimpanan. Frasa kunci harus berasal dari transkrip pengguna, bukan ucapan Gemini. Setelah memory_save mengembalikan hasil, wajib beri konfirmasi suara dalam bahasa Indonesia: jika hasil menyatakan berhasil, katakan catatan berhasil disimpan; jika gagal atau ditolak, katakan gagal atau tidak dilakukan. Setelah memory_delete mengembalikan hasil, wajib beri konfirmasi suara: jika hasil menyatakan berhasil, katakan catatan berhasil dihapus; jika catatan tidak ditemukan atau terjadi kegagalan, jelaskan bahwa catatan tidak ditemukan atau penghapusan gagal. Jangan hanya memanggil tool tanpa respons suara. Jangan menyatakan berhasil sebelum membaca hasil tool.");
    cJSON_AddItemToArray(system_parts, system_text);

    cJSON *memory_part = cJSON_CreateObject();
    /* Keep large context buffers off the ws_tx task stack to prevent overflow. */
    static char memory_context[4096] = {};
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
    /* Speaker enrollment and identification are explicit Gemini tools. */
    cJSON *speaker_tool = cJSON_CreateObject();
    cJSON *speaker_decls = cJSON_AddArrayToObject(speaker_tool, "functionDeclarations");
    cJSON *enroll = cJSON_CreateObject();
    cJSON_AddStringToObject(enroll, "name", "speaker_enroll");
    cJSON_AddStringToObject(enroll, "behavior", "BLOCKING");
    cJSON_AddStringToObject(enroll, "description", "Pendaftaran biometrik suara hanya setelah pengguna secara eksplisit mengatakan 'daftarkan suara'. Gunakan untuk permintaan seperti 'daftarkan suara: saya adalah Bos'. Ambil hanya nama profil yang pengguna nyatakan sebagai identitasnya; jangan mendaftarkan atas permintaan yang ambigu. Tool menyimpan fitur MFCC suara terbaru ke flash NVS dan mengembalikan hasil sebenarnya. Jangan pernah mengklaim berhasil jika hasil gagal.");
    cJSON *ep = cJSON_AddObjectToObject(enroll, "parameters"); cJSON_AddStringToObject(ep, "type", "OBJECT"); cJSON *eprops = cJSON_AddObjectToObject(ep, "properties"); cJSON *ename = cJSON_AddObjectToObject(eprops, "name"); cJSON_AddStringToObject(ename, "type", "STRING"); cJSON_AddStringToObject(ename, "description", "Nama profil singkat yang diucapkan pengguna, misalnya Bos; tanpa frasa 'saya adalah'."); cJSON *er = cJSON_AddArrayToObject(ep, "required"); cJSON_AddItemToArray(er, cJSON_CreateString("name")); cJSON_AddItemToArray(speaker_decls, enroll);
    cJSON *identify = cJSON_CreateObject(); cJSON_AddStringToObject(identify, "name", "speaker_identify"); cJSON_AddStringToObject(identify, "behavior", "BLOCKING"); cJSON_AddStringToObject(identify, "description", "Panggil sekali untuk setiap giliran ucapan pengguna sebelum menjawab, untuk membandingkan fitur MFCC-DTW terbaru dengan profil suara NVS. Gunakan nama hanya jika ada kecocokan; jika tidak, jangan menebak.");
    cJSON *ip = cJSON_AddObjectToObject(identify, "parameters"); cJSON_AddStringToObject(ip, "type", "OBJECT"); cJSON_AddItemToArray(speaker_decls, identify); cJSON_AddItemToArray(cJSON_GetObjectItem(setup, "tools"), speaker_tool);

    cJSON *realtime = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(realtime, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", false);
    cJSON_AddStringToObject(aad, "startOfSpeechSensitivity", "START_SENSITIVITY_HIGH");
    cJSON_AddNumberToObject(aad, "prefixPaddingMs", 200);
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
static bool pending_explicit_speaker_enroll = false;
/* Keep memory-read workspace off the WebSocket RX task stack. */
static char memory_read_context[4096];

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
    static const char *const speaker_negations[] = { "jangan daftarkan suara", "jangan daftarkan rekaman", "tidak jadi daftar suara", "batal daftarkan suara" };
    static const char *const speaker_words[] = { "daftarkan suara", "daftarkan rekaman suara", "daftarkan rekaman" };
    if (transcript_has_any(latest_user_transcript, speaker_negations, sizeof(speaker_negations) / sizeof(speaker_negations[0]))) {
        pending_explicit_speaker_enroll = false;
    } else if (transcript_has_any(latest_user_transcript, speaker_words, sizeof(speaker_words) / sizeof(speaker_words[0]))) {
        pending_explicit_speaker_enroll = true;
        ESP_LOGI(TAG, "SPEAKER_ID: izin pendaftaran eksplisit terdeteksi");
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

static bool explicit_speaker_enroll_request(void)
{
    bool allowed = pending_explicit_speaker_enroll;
    pending_explicit_speaker_enroll = false;
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
        } else if (strcmp(name->valuestring, "speaker_enroll") == 0) {
            cJSON *profile_name = cJSON_GetObjectItem(args, "name");
            if (!explicit_speaker_enroll_request()) {
                (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                    "Pendaftaran ditolak karena transkrip pengguna tidak memuat perintah eksplisit 'daftarkan suara'. Minta pengguna mengucapkannya terlebih dahulu.");
                continue;
            }
            char result[192] = {};
            if (cJSON_IsString(profile_name) && profile_name->valuestring)
                success = speaker_id_enroll_last(profile_name->valuestring, result, sizeof(result));
            else strlcpy(result, "Nama profil tidak diberikan.", sizeof(result));
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring, result);
            continue;
        } else if (strcmp(name->valuestring, "speaker_identify") == 0) {
            char speaker_name[SPEAKER_ID_NAME_MAX] = {};
            float distance = 0.0f;
            success = speaker_id_identify(speaker_name, sizeof(speaker_name), &distance);
            char result[192] = {};
            if (success) snprintf(result, sizeof(result), "Pembicara cocok dengan profil '%s' (jarak %.1f). Gunakan nama ini secara natural untuk menyapa; jangan menyatakan tingkat kepastian lebih tinggi dari hasil.", speaker_name, (double)distance);
            else if (speaker_id_profile_count() == 0) strlcpy(result, "Belum ada profil suara terdaftar; jangan menebak nama pengguna.", sizeof(result));
            else strlcpy(result, "Pembicara tidak cocok dengan profil terdaftar. Jangan menebak nama; gunakan sapaan netral.", sizeof(result));
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring, result);
            continue;
        } else if (strcmp(name->valuestring, "memory_read") == 0) {
            memory_read_context[0] = '\0';
            size_t memory_len = ai_memory_get_context(memory_read_context, sizeof(memory_read_context));
            (void)websocket_send_tool_response_text(id->valuestring, name->valuestring,
                memory_len ? memory_read_context : "Belum ada catatan jangka panjang yang tersimpan.");
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
