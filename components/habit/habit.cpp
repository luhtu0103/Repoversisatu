
#include "habit.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "HABIT";

#define HABIT_MAGIC        0x48414232u
#define HABIT_KINDS        7
#define HABIT_SLOTS        4
#define LESSON_MAX         6
#define LESSON_LEN         104          /* 103 karakter + NUL */
#define LESSONS_PER_SESSION 2
#define MIN_COUNT_FOR_HABIT 3
#define SAVE_INTERVAL_US   (10LL * 60 * 1000000)
#define WITA_OFFSET_SEC    (8 * 3600)
#define TIME_VALID_AFTER   1704067200   /* 1 Jan 2024 */

static const char *const KIND_NAMES[HABIT_KINDS] = {
    "radio", "lampu", "colokan", "kipas", "mp3", "cari", "alarm"
};
static const char *const KIND_TEXT[HABIT_KINDS] = {
    "mendengarkan radio", "menyalakan lampu", "memakai colokan listrik",
    "menyalakan kipas", "memakai MP3", "mencari info di web",
    "memasang alarm atau pengingat"
};
static const char *const SLOT_TEXT[HABIT_SLOTS] = { "pagi", "siang", "sore", "malam" };
static const char *const DAY_TEXT[7] = {
    "Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"
};

typedef struct {
    uint32_t magic;
    uint32_t first_day;      /* hari ke-berapa sejak 1970 (WITA) saat pertama dipakai */
    uint32_t sessions;
    uint32_t reserved;
    uint16_t counts[HABIT_KINDS][HABIT_SLOTS];
    uint8_t  lesson_next;
    uint8_t  pad[3];
    char     lessons[LESSON_MAX][LESSON_LEN];
} habit_data_t;

static habit_data_t s_data;
static bool s_loaded = false;
static int64_t s_last_save_us = 0;
static int s_lessons_this_session = 0;

/* ---------- kunci ---------- */
static SemaphoreHandle_t lock_get(void)
{
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t h = NULL;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    if (!h) {
        portENTER_CRITICAL(&mux);
        if (!h) h = xSemaphoreCreateMutexStatic(&buf);
        portEXIT_CRITICAL(&mux);
    }
    return h;
}

struct Guard {
    SemaphoreHandle_t h;
    Guard() : h(lock_get()) { xSemaphoreTake(h, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(h); }
};

/* ---------- penyimpanan ---------- */
static void load_locked(void)
{
    if (s_loaded) return;
    memset(&s_data, 0, sizeof(s_data));
    s_data.magic = HABIT_MAGIC;
    nvs_handle_t nvs;
    if (nvs_open("habit", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(s_data);
        habit_data_t tmp;
        if (nvs_get_blob(nvs, "data", &tmp, &size) == ESP_OK &&
            size == sizeof(tmp) && tmp.magic == HABIT_MAGIC) {
            s_data = tmp;
            ESP_LOGI(TAG, "Data kebiasaan dimuat: sesi=%u", (unsigned)s_data.sessions);
        }
        nvs_close(nvs);
    }
    /* jaga-jaga: pastikan setiap pelajaran berakhir dengan NUL */
    for (int i = 0; i < LESSON_MAX; ++i) s_data.lessons[i][LESSON_LEN - 1] = '\0';
    if (s_data.lesson_next >= LESSON_MAX) s_data.lesson_next = 0;
    s_loaded = true;
}

static void save_locked(void)
{
    nvs_handle_t nvs;
    if (nvs_open("habit", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS habit tidak bisa dibuka; data tidak disimpan");
        return;
    }
    if (nvs_set_blob(nvs, "data", &s_data, sizeof(s_data)) == ESP_OK)
        nvs_commit(nvs);
    nvs_close(nvs);
    s_last_save_us = esp_timer_get_time();
}

/* ---------- waktu ---------- */
static bool local_now(struct tm *out, uint32_t *day)
{
    time_t now = time(NULL);
    if (now < TIME_VALID_AFTER) return false;
    time_t local = now + WITA_OFFSET_SEC;
    gmtime_r(&local, out);
    *day = (uint32_t)(local / 86400);
    return true;
}

static int slot_of_hour(int hour)
{
    if (hour >= 4 && hour < 10) return 0;   /* pagi  */
    if (hour >= 10 && hour < 15) return 1;  /* siang */
    if (hour >= 15 && hour < 19) return 2;  /* sore  */
    return 3;                               /* malam */
}

/* ---------- teks ---------- */
static void append(char *out, size_t n, size_t *used, const char *fmt, ...)
{
    if (*used >= n) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *used, n - *used, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w >= n - *used) *used = n - 1;   /* terpotong */
    else *used += (size_t)w;
}

static const char *closeness_text(uint32_t days)
{
    if (days <= 2)  return "baru kenal (sopan, ramah, hangat)";
    if (days <= 14) return "mulai akrab (santai tetapi tetap sopan)";
    if (days <= 60) return "akrab (santai, sesekali bercanda ringan)";
    return "sangat akrab (seperti teman lama: santai, hangat, boleh bercanda)";
}

/* ---------- API ---------- */
void habit_note_event(const char *kind)
{
    if (!kind) return;
    int k = -1;
    for (int i = 0; i < HABIT_KINDS; ++i)
        if (strcmp(kind, KIND_NAMES[i]) == 0) { k = i; break; }
    if (k < 0) return;

    struct tm tm_now;
    uint32_t day = 0;
    if (!local_now(&tm_now, &day)) return;   /* tanpa jam, slot tidak diketahui */

    Guard g;
    load_locked();
    uint16_t *c = &s_data.counts[k][slot_of_hour(tm_now.tm_hour)];
    if (*c < 60000) (*c)++;
    save_locked();
}

size_t habit_build_context(char *out, size_t out_size)
{
    if (!out || out_size < 64) return 0;
    out[0] = '\0';

    struct tm tm_now;
    uint32_t day = 0;
    const bool have_time = local_now(&tm_now, &day);

    Guard g;
    load_locked();
    s_lessons_this_session = 0;
    s_data.sessions++;

    uint32_t days = 1;
    bool must_save = false;
    if (have_time) {
        if (s_data.first_day == 0 || s_data.first_day > day) {
            s_data.first_day = day;
            must_save = true;
        }
        days = day - s_data.first_day + 1;
    }
    /* Jangan tulis flash tiap sambung ulang; cukup tiap 10 menit. */
    if (must_save || esp_timer_get_time() - s_last_save_us > SAVE_INTERVAL_US)
        save_locked();

    size_t used = 0;
    append(out, out_size, &used,
           "Konteks pribadi (untuk membuat gaya bicaramu alami; jangan dibacakan mentah-mentah). ");
    const int cur_slot = have_time ? slot_of_hour(tm_now.tm_hour) : -1;
    if (have_time) {
        append(out, out_size, &used, "Sekarang hari %s %s, sekitar jam %02d WITA. ",
               DAY_TEXT[tm_now.tm_wday % 7], SLOT_TEXT[cur_slot], tm_now.tm_hour);
    }
    append(out, out_size, &used,
           "Kamu sudah menemani pengguna sekitar %u hari (%u percakapan). Tingkat kedekatan: %s.\n",
           (unsigned)days, (unsigned)s_data.sessions, closeness_text(days));

    /* tiga kebiasaan terkuat */
    bool used_pair[HABIT_KINDS][HABIT_SLOTS] = {};
    bool header = false;
    for (int round = 0; round < 3; ++round) {
        int bk = -1, bs = -1;
        uint16_t best = MIN_COUNT_FOR_HABIT - 1;
        for (int k = 0; k < HABIT_KINDS; ++k)
            for (int s = 0; s < HABIT_SLOTS; ++s)
                if (!used_pair[k][s] && s_data.counts[k][s] > best) {
                    best = s_data.counts[k][s]; bk = k; bs = s;
                }
        if (bk < 0) break;
        used_pair[bk][bs] = true;
        if (!header) { append(out, out_size, &used, "Kebiasaan pengguna yang sudah terlihat:\n"); header = true; }
        append(out, out_size, &used, "- sering %s di waktu %s%s\n",
               KIND_TEXT[bk], SLOT_TEXT[bs], bs == cur_slot ? " (cocok dengan waktu sekarang)" : "");
    }

    /* pelajaran yang Gemini simpan sendiri */
    bool lesson_header = false;
    for (int i = 0; i < LESSON_MAX; ++i) {
        if (!s_data.lessons[i][0]) continue;
        if (!lesson_header) { append(out, out_size, &used, "Hal yang sudah kamu pelajari tentang pengguna:\n"); lesson_header = true; }
        append(out, out_size, &used, "- %s\n", s_data.lessons[i]);
    }

    append(out, out_size, &used,
        "Aturan gaya: bicaralah seperti teman, dengan kalimat pendek; jangan kaku atau terdengar seperti membaca program. "
        "Variasikan sapaan dan jangan mengulang kalimat pembuka yang sama. Sesuaikan nada dengan waktu dan tingkat kedekatan. "
        "Jika ada kebiasaan yang cocok dengan waktu sekarang, boleh tawarkan SATU kali dengan nada santai "
        "(misalnya: mau kuputarkan radio seperti biasa?). Jangan melakukannya otomatis dan jangan memaksa; "
        "jika ditolak, jangan menawarkan lagi pada sesi ini. Jangan menyebut angka hitungan atau kata 'catatan kebiasaan'. "
        "Jika dalam percakapan kamu menangkap hal baru yang tidak sensitif tentang selera atau kebiasaan pengguna "
        "(misalnya stasiun radio favorit, jam tidur, panggilan yang disukai), panggil tool simpan_pelajaran dengan "
        "action simpan dan satu kalimat pendek, paling banyak dua kali per sesi, tanpa mengumumkannya. "
        "Jangan simpan soal kesehatan, uang, kata sandi, rahasia, atau data orang lain. "
        "Jika pengguna bertanya apa yang sudah kamu pelajari, pakai action baca. "
        "Jika pengguna minta kamu melupakan pelajaranmu, pakai action hapus_semua.");
    return used;
}

bool habit_lesson_add(const char *text, char *detail, size_t detail_size)
{
    if (detail && detail_size) detail[0] = '\0';
    if (!text) return false;

    /* bersihkan: buang spasi pinggir, ganti baris baru dengan spasi */
    char clean[LESSON_LEN + 8];
    size_t len = 0;
    while (*text == ' ' || *text == '\n' || *text == '\t') ++text;
    for (; *text && len < sizeof(clean) - 1; ++text)
        clean[len++] = (*text == '\n' || *text == '\r' || *text == '\t') ? ' ' : *text;
    clean[len] = '\0';
    while (len > 0 && clean[len - 1] == ' ') clean[--len] = '\0';

    if (len == 0) {
        if (detail) snprintf(detail, detail_size, "Gagal: kalimat kosong.");
        return false;
    }
    if (len > LESSON_LEN - 1) {
        if (detail) snprintf(detail, detail_size, "Gagal: terlalu panjang. Ringkas jadi satu kalimat pendek (maksimal 100 karakter).");
        return false;
    }

    Guard g;
    load_locked();
    if (s_lessons_this_session >= LESSONS_PER_SESSION) {
        if (detail) snprintf(detail, detail_size, "Cukup untuk sesi ini; simpan lagi di percakapan berikutnya.");
        return false;
    }
    for (int i = 0; i < LESSON_MAX; ++i) {
        if (strcmp(s_data.lessons[i], clean) == 0) {
            if (detail) snprintf(detail, detail_size, "Sudah pernah dipelajari; tidak perlu disimpan lagi.");
            return true;
        }
    }
    memcpy(s_data.lessons[s_data.lesson_next], clean, len + 1);
    s_data.lesson_next = (uint8_t)((s_data.lesson_next + 1) % LESSON_MAX);
    s_lessons_this_session++;
    save_locked();
    ESP_LOGI(TAG, "Pelajaran baru tersimpan (%u karakter)", (unsigned)len);
    if (detail) snprintf(detail, detail_size, "Tersimpan diam-diam. Jangan diumumkan; lanjutkan percakapan seperti biasa.");
    return true;
}

size_t habit_lesson_list(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    Guard g;
    load_locked();
    size_t used = 0;
    for (int i = 0; i < LESSON_MAX; ++i) {
        if (!s_data.lessons[i][0]) continue;
        append(out, out_size, &used, "- %s\n", s_data.lessons[i]);
    }
    return used;
}

bool habit_lesson_clear(void)
{
    Guard g;
    load_locked();
    memset(s_data.lessons, 0, sizeof(s_data.lessons));
    s_data.lesson_next = 0;
    save_locked();
    ESP_LOGI(TAG, "Semua pelajaran dihapus");
    return true;
}
