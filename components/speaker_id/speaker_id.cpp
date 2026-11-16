#include "speaker_id.h"
#include "ai_memory.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fungsi dari audio_hal.h (C++ linkage, tanpa extern "C"). Dideklarasikan di sini
 * supaya komponen ini tidak perlu bergantung pada header driver I2S. */
size_t audio_read_mic_raw(uint8_t *dest, size_t max_len);
void audio_hal_set_mic_enabled(bool enabled);
bool audio_hal_is_mic_enabled(void);

static const char *TAG = "SPEAKER_ID";

/* ---------- Pengaturan MFCC ---------- */
#define SAMPLE_RATE        SPEAKER_ID_SAMPLE_RATE
#define FRAME_LEN          400      /* 25 ms */
#define FRAME_HOP          160      /* 10 ms */
#define FFT_SIZE           512
#define NUM_MEL            26
#define NUM_CEP            12       /* c1..c12 (c0 dibuang) */
#define MAX_FRAMES         150      /* maks 1,5 detik suara */
#define MIN_FRAMES         25       /* min 0,25 detik suara */
#define MAX_INPUT_SAMPLES  48000    /* maks 3 detik masukan */
#define MAX_INPUT_FRAMES   300
#define MIN_PEAK_RMS       60.0f    /* di bawah ini dianggap terlalu pelan */
#define MIN_FRAME_ENERGY   900.0f   /* RMS 30 ^ 2 */
#define FEAT_SCALE         64.0f    /* float -> int16 */
#define INTRA_FLOOR        0.5f     /* jarak acuan minimum */
#define DEFAULT_TOLERANCE  1.5f
#define AMBIGUOUS_MARGIN   1.10f    /* nilai terbaik harus >10% lebih baik dari yang kedua */

/* ---------- Penyimpanan ---------- */
#define FILE_PATH   "/spiffs/voiceid.bin"
#define FILE_TMP    "/spiffs/voiceid.tmp"
#define FILE_MAGIC  0x44495053u     /* 'SPID' */
#define FILE_VERSION 1

typedef struct {
    uint16_t frames;
    int16_t  data[MAX_FRAMES * NUM_CEP];
} template_t;

typedef struct {
    char       name[SPEAKER_ID_NAME_MAX + 1];
    uint8_t    count;
    uint16_t   intra_x100;   /* jarak rata-rata antar 3 rekaman pendaftaran x100 */
    template_t t[SPEAKER_ID_SAMPLES_PER_PROFILE];
} profile_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t tol_x100;
    uint32_t count;
} file_header_t;

typedef struct {
    float energy[MAX_INPUT_FRAMES];
    float fft[FFT_SIZE * 2];
    float feat[MAX_FRAMES * NUM_CEP];    /* fitur terakhir hasil extract_features */
    float query[MAX_FRAMES * NUM_CEP];   /* salinan float sebuah template */
    float dtw_a[MAX_FRAMES + 1];
    float dtw_b[MAX_FRAMES + 1];
} work_t;

static float s_window[FRAME_LEN];
static float s_cos[FFT_SIZE / 2];
static float s_sin[FFT_SIZE / 2];
static int   s_mel_bin[NUM_MEL + 2];
static float s_dct[NUM_CEP][NUM_MEL];
static bool  s_tables_ready = false;

static profile_t  *s_profiles = nullptr;
static int         s_count = 0;
static float       s_tol = DEFAULT_TOLERANCE;
static work_t     *s_work = nullptr;
static template_t *s_pend = nullptr;
static char        s_pend_name[SPEAKER_ID_NAME_MAX + 1] = {0};
static int         s_pend_count = 0;
static SemaphoreHandle_t s_mutex = nullptr;
static bool        s_ready = false;

const char *speaker_id_status_text(speaker_id_status_t status)
{
    switch (status) {
        case SPEAKER_ID_OK:             return "OK";
        case SPEAKER_ID_ERR_NOT_READY:  return "Penyimpanan belum siap";
        case SPEAKER_ID_ERR_QUIET:      return "Suara terlalu pelan atau tidak terdengar. Dekatkan ke mic dan ulangi.";
        case SPEAKER_ID_ERR_SHORT:      return "Suara terlalu pendek. Ucapkan Hi ESP dengan jelas dan ulangi.";
        case SPEAKER_ID_ERR_NAME:       return "Nama tidak valid (1-24 huruf, tanpa tanda kutip atau garis miring).";
        case SPEAKER_ID_ERR_FULL:       return "Profil sudah penuh. Hapus salah satu dulu.";
        case SPEAKER_ID_ERR_STEP:       return "Urutan rekaman salah. Mulai lagi dari rekaman 1.";
        case SPEAKER_ID_ERR_STORAGE:    return "Gagal menyimpan ke memori.";
        case SPEAKER_ID_ERR_NOMEM:      return "Memori tidak cukup.";
        case SPEAKER_ID_ERR_NO_PROFILE: return "Belum ada profil suara.";
    }
    return "Error";
}

/* ---------- Tabel MFCC ---------- */
static float hz_to_mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
static float mel_to_hz(float mel) { return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f); }

static void init_tables(void)
{
    if (s_tables_ready) return;
    const float two_pi = 6.28318530718f;
    for (int n = 0; n < FRAME_LEN; ++n)
        s_window[n] = 0.54f - 0.46f * cosf(two_pi * (float)n / (float)(FRAME_LEN - 1));
    for (int k = 0; k < FFT_SIZE / 2; ++k) {
        const float a = two_pi * (float)k / (float)FFT_SIZE;
        s_cos[k] = cosf(a);
        s_sin[k] = sinf(a);
    }
    const float mel_min = hz_to_mel(100.0f);
    const float mel_max = hz_to_mel(7600.0f);
    for (int i = 0; i < NUM_MEL + 2; ++i) {
        const float mel = mel_min + (mel_max - mel_min) * (float)i / (float)(NUM_MEL + 1);
        int bin = (int)floorf((float)(FFT_SIZE + 1) * mel_to_hz(mel) / (float)SAMPLE_RATE);
        if (bin < 0) bin = 0;
        if (bin > FFT_SIZE / 2) bin = FFT_SIZE / 2;
        s_mel_bin[i] = bin;
    }
    const float dct_scale = sqrtf(2.0f / (float)NUM_MEL);
    for (int j = 0; j < NUM_CEP; ++j)
        for (int m = 0; m < NUM_MEL; ++m)
            s_dct[j][m] = dct_scale * cosf(3.14159265359f * (float)(j + 1) * ((float)m + 0.5f) / (float)NUM_MEL);
    s_tables_ready = true;
}

/* FFT radix-2 di tempat. x berisi pasangan (real, imajiner) sepanjang FFT_SIZE. */
static void fft512(float *x)
{
    unsigned j = 0;
    for (unsigned i = 0; i < FFT_SIZE - 1; ++i) {
        if (i < j) {
            float tr = x[2 * i], ti = x[2 * i + 1];
            x[2 * i] = x[2 * j];
            x[2 * i + 1] = x[2 * j + 1];
            x[2 * j] = tr;
            x[2 * j + 1] = ti;
        }
        unsigned k = FFT_SIZE >> 1;
        while (k <= j) { j -= k; k >>= 1; }
        j += k;
    }
    for (unsigned len = 2; len <= FFT_SIZE; len <<= 1) {
        const unsigned half = len >> 1;
        const unsigned step = FFT_SIZE / len;
        for (unsigned i = 0; i < FFT_SIZE; i += len) {
            for (unsigned k = 0; k < half; ++k) {
                const float wr = s_cos[k * step];
                const float wi = -s_sin[k * step];
                const unsigned a = i + k;
                const unsigned b = a + half;
                const float tr = x[2 * b] * wr - x[2 * b + 1] * wi;
                const float ti = x[2 * b] * wi + x[2 * b + 1] * wr;
                x[2 * b]     = x[2 * a] - tr;
                x[2 * b + 1] = x[2 * a + 1] - ti;
                x[2 * a]     += tr;
                x[2 * a + 1] += ti;
            }
        }
    }
}

/* MFCC satu frame -> NUM_CEP angka. prev = sampel sebelum frame (untuk pre-emphasis). */
static void mfcc_frame(const int16_t *x, int16_t prev, float *out)
{
    float *f = s_work->fft;
    float last = (float)prev;
    for (int i = 0; i < FRAME_LEN; ++i) {
        const float cur = (float)x[i];
        f[2 * i] = (cur - 0.97f * last) * s_window[i];
        f[2 * i + 1] = 0.0f;
        last = cur;
    }
    for (int i = FRAME_LEN; i < FFT_SIZE; ++i) {
        f[2 * i] = 0.0f;
        f[2 * i + 1] = 0.0f;
    }
    fft512(f);

    float mel[NUM_MEL];
    for (int m = 0; m < NUM_MEL; ++m) {
        const int left = s_mel_bin[m], center = s_mel_bin[m + 1], right = s_mel_bin[m + 2];
        float sum = 0.0f;
        for (int k = left; k < center; ++k) {
            const float p = f[2 * k] * f[2 * k] + f[2 * k + 1] * f[2 * k + 1];
            sum += p * ((float)(k - left) / (float)(center - left));
        }
        for (int k = center; k <= right; ++k) {
            const float p = f[2 * k] * f[2 * k] + f[2 * k + 1] * f[2 * k + 1];
            const float w = (right > center) ? (float)(right - k) / (float)(right - center) : 1.0f;
            sum += p * w;
        }
        mel[m] = logf(sum + 1.0f);
    }
    for (int j = 0; j < NUM_CEP; ++j) {
        float c = 0.0f;
        for (int m = 0; m < NUM_MEL; ++m) c += mel[m] * s_dct[j][m];
        out[j] = c;
    }
}

/* Potong diam di awal/akhir, hitung MFCC, kurangi rata-rata (CMS).
 * Hasil ada di s_work->feat. */
static speaker_id_status_t extract_features(const int16_t *pcm, size_t n, uint16_t *frames_out)
{
    if (!pcm || n < FRAME_LEN) return SPEAKER_ID_ERR_SHORT;
    if (n > MAX_INPUT_SAMPLES) {          /* pakai 3 detik terakhir */
        pcm += n - MAX_INPUT_SAMPLES;
        n = MAX_INPUT_SAMPLES;
    }
    int nf = (int)((n - FRAME_LEN) / FRAME_HOP) + 1;
    if (nf > MAX_INPUT_FRAMES) nf = MAX_INPUT_FRAMES;

    float max_e = 0.0f;
    for (int t = 0; t < nf; ++t) {
        const int16_t *x = pcm + (size_t)t * FRAME_HOP;
        float sum = 0.0f;
        for (int i = 0; i < FRAME_LEN; ++i) sum += (float)x[i] * (float)x[i];
        const float e = sum / (float)FRAME_LEN;
        s_work->energy[t] = e;
        if (e > max_e) max_e = e;
    }
    if (sqrtf(max_e) < MIN_PEAK_RMS) return SPEAKER_ID_ERR_QUIET;

    float thr = max_e * 0.02f;           /* 17 dB di bawah puncak */
    if (thr < MIN_FRAME_ENERGY) thr = MIN_FRAME_ENERGY;
    int start = -1, end = -1;
    for (int t = 0; t + 2 < nf; ++t) {
        if (s_work->energy[t] > thr && s_work->energy[t + 1] > thr && s_work->energy[t + 2] > thr) { start = t; break; }
    }
    for (int t = nf - 1; t >= 2; --t) {
        if (s_work->energy[t] > thr && s_work->energy[t - 1] > thr && s_work->energy[t - 2] > thr) { end = t; break; }
    }
    if (start < 0 || end < 0 || end < start) return SPEAKER_ID_ERR_QUIET;
    start = start >= 5 ? start - 5 : 0;
    end = (end + 5 < nf) ? end + 5 : nf - 1;
    int len = end - start + 1;
    if (len < MIN_FRAMES) return SPEAKER_ID_ERR_SHORT;
    if (len > MAX_FRAMES) {              /* ambil jendela dengan energi terbesar */
        float sum = 0.0f;
        for (int i = 0; i < MAX_FRAMES; ++i) sum += s_work->energy[start + i];
        float best = sum;
        int best_start = start;
        for (int s = start + 1; s + MAX_FRAMES - 1 <= end; ++s) {
            sum += s_work->energy[s + MAX_FRAMES - 1] - s_work->energy[s - 1];
            if (sum > best) { best = sum; best_start = s; }
        }
        start = best_start;
        len = MAX_FRAMES;
    }

    for (int t = 0; t < len; ++t) {
        const int16_t *x = pcm + (size_t)(start + t) * FRAME_HOP;
        const int16_t prev = (x > pcm) ? x[-1] : 0;
        mfcc_frame(x, prev, s_work->feat + (size_t)t * NUM_CEP);
    }

    /* CMS: kurangi rata-rata tiap dimensi supaya tidak peka pada mic/jarak. */
    for (int j = 0; j < NUM_CEP; ++j) {
        float mean = 0.0f;
        for (int t = 0; t < len; ++t) mean += s_work->feat[(size_t)t * NUM_CEP + j];
        mean /= (float)len;
        for (int t = 0; t < len; ++t) s_work->feat[(size_t)t * NUM_CEP + j] -= mean;
    }
    if (frames_out) *frames_out = (uint16_t)len;
    return SPEAKER_ID_OK;
}

static int16_t quantize(float v)
{
    float s = v * FEAT_SCALE;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (int16_t)lrintf(s);
}

/* DTW: q = fitur float (nq frame), t = template int16 (nt frame). Hasil dinormalisasi. */
static float dtw_distance(const float *q, int nq, const int16_t *t, int nt)
{
    const float INF = 1.0e30f;
    const float inv = 1.0f / FEAT_SCALE;
    int diff = nq > nt ? nq - nt : nt - nq;
    int band = (nq > nt ? nq : nt) / 3;
    if (band < diff) band = diff;
    if (band < 10) band = 10;

    float *prev = s_work->dtw_a;
    float *cur = s_work->dtw_b;
    for (int j = 0; j <= nt; ++j) prev[j] = INF;
    prev[0] = 0.0f;

    for (int i = 1; i <= nq; ++i) {
        for (int j = 0; j <= nt; ++j) cur[j] = INF;
        int lo = i - band; if (lo < 1) lo = 1;
        int hi = i + band; if (hi > nt) hi = nt;
        const float *qi = q + (size_t)(i - 1) * NUM_CEP;
        for (int j = lo; j <= hi; ++j) {
            const int16_t *tj = t + (size_t)(j - 1) * NUM_CEP;
            float d2 = 0.0f;
            for (int k = 0; k < NUM_CEP; ++k) {
                const float d = qi[k] - (float)tj[k] * inv;
                d2 += d * d;
            }
            float best = prev[j];
            if (cur[j - 1] < best) best = cur[j - 1];
            if (prev[j - 1] < best) best = prev[j - 1];
            cur[j] = sqrtf(d2) + best;
        }
        float *tmp = prev; prev = cur; cur = tmp;
    }
    const float total = prev[nt];
    if (total >= INF * 0.5f) return 1.0e9f;
    return total / (float)(nq + nt);
}

/* ---------- Nama ---------- */
static char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool names_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower_ascii(*a) != lower_ascii(*b)) return false;
        ++a; ++b;
    }
    return *a == *b;
}

static bool sanitize_name(const char *in, char *out)
{
    if (!in) return false;
    while (*in == ' ') ++in;
    size_t len = strlen(in);
    while (len > 0 && in[len - 1] == ' ') --len;
    if (len == 0 || len > SPEAKER_ID_NAME_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '\\') return false;
        out[i] = (char)c;
    }
    out[len] = '\0';
    return true;
}

static int find_profile(const char *name)
{
    for (int i = 0; i < s_count; ++i)
        if (names_equal(s_profiles[i].name, name)) return i;
    return -1;
}

/* ---------- File ---------- */
static bool save_file(void)
{
    FILE *f = fopen(FILE_TMP, "wb");
    if (!f) { ESP_LOGE(TAG, "Tidak bisa membuat %s", FILE_TMP); return false; }
    file_header_t h = {};
    h.magic = FILE_MAGIC;
    h.version = FILE_VERSION;
    h.tol_x100 = (uint16_t)(s_tol * 100.0f + 0.5f);
    h.count = (uint32_t)s_count;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    for (int i = 0; ok && i < s_count; ++i)
        ok = fwrite(&s_profiles[i], sizeof(profile_t), 1, f) == 1;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(FILE_TMP); ESP_LOGE(TAG, "Tulis file profil gagal"); return false; }
    remove(FILE_PATH);                       /* SPIFFS tidak menimpa lewat rename */
    if (rename(FILE_TMP, FILE_PATH) != 0) { ESP_LOGE(TAG, "Rename file profil gagal"); return false; }
    return true;
}

static void load_file(void)
{
    s_count = 0;
    FILE *f = fopen(FILE_PATH, "rb");
    if (!f) { ESP_LOGI(TAG, "Belum ada file profil suara"); return; }
    file_header_t h = {};
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != FILE_MAGIC || h.version != FILE_VERSION ||
        h.count > SPEAKER_ID_MAX_PROFILES) {
        fclose(f);
        ESP_LOGW(TAG, "File profil tidak valid; diabaikan");
        return;
    }
    float tol = (float)h.tol_x100 / 100.0f;
    if (tol >= 1.0f && tol <= 3.0f) s_tol = tol;
    int loaded = 0;
    for (uint32_t i = 0; i < h.count; ++i) {
        if (fread(&s_profiles[loaded], sizeof(profile_t), 1, f) != 1) { loaded = 0; break; }
        const profile_t *p = &s_profiles[loaded];
        bool valid = p->count > 0 && p->count <= SPEAKER_ID_SAMPLES_PER_PROFILE;
        for (int k = 0; valid && k < p->count; ++k)
            valid = p->t[k].frames >= MIN_FRAMES && p->t[k].frames <= MAX_FRAMES;
        if (!valid) { loaded = 0; break; }
        ++loaded;
    }
    fclose(f);
    s_count = loaded;
    if (loaded == 0 && h.count > 0) ESP_LOGW(TAG, "Isi file profil rusak; semua profil diabaikan");
}

/* ---------- API ---------- */
bool speaker_id_init(void)
{
    if (s_ready) return true;
    if (!ai_memory_init()) {
        ESP_LOGE(TAG, "SPIFFS belum siap; speaker_id dimatikan");
        return false;
    }
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_profiles = (profile_t *)heap_caps_calloc(SPEAKER_ID_MAX_PROFILES, sizeof(profile_t), caps);
    s_pend = (template_t *)heap_caps_calloc(SPEAKER_ID_SAMPLES_PER_PROFILE, sizeof(template_t), caps);
    s_work = (work_t *)heap_caps_calloc(1, sizeof(work_t), caps);
    if (!s_profiles || !s_pend || !s_work) {
        ESP_LOGE(TAG, "Alokasi PSRAM gagal (profil=%u pend=%u work=%u)",
                 (unsigned)(SPEAKER_ID_MAX_PROFILES * sizeof(profile_t)),
                 (unsigned)(SPEAKER_ID_SAMPLES_PER_PROFILE * sizeof(template_t)),
                 (unsigned)sizeof(work_t));
        heap_caps_free(s_profiles); s_profiles = nullptr;
        heap_caps_free(s_pend);     s_pend = nullptr;
        heap_caps_free(s_work);     s_work = nullptr;
        return false;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;
    init_tables();
    load_file();
    s_ready = true;
    ESP_LOGI(TAG, "Pengenal suara siap: profil=%d toleransi=%.2f", s_count, (double)s_tol);
    return true;
}

bool speaker_id_is_ready(void) { return s_ready; }
int  speaker_id_profile_count(void) { return s_ready ? s_count : 0; }
bool speaker_id_has_profiles(void) { return s_ready && s_count > 0; }

bool speaker_id_get_profile(int index, char *name, size_t name_size, int *samples)
{
    if (!s_ready || index < 0 || index >= s_count) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (name && name_size) strlcpy(name, s_profiles[index].name, name_size);
    if (samples) *samples = s_profiles[index].count;
    xSemaphoreGive(s_mutex);
    return true;
}

float speaker_id_get_tolerance(void) { return s_tol; }

bool speaker_id_set_tolerance(float tolerance)
{
    if (!s_ready || tolerance < 1.0f || tolerance > 3.0f) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_tol = tolerance;
    bool ok = save_file();
    xSemaphoreGive(s_mutex);
    return ok;
}

speaker_id_status_t speaker_id_identify(const int16_t *pcm, size_t samples, speaker_id_result_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!s_ready) return SPEAKER_ID_ERR_NOT_READY;
    if (s_count == 0) return SPEAKER_ID_ERR_NO_PROFILE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const int64_t t0 = esp_timer_get_time();
    uint16_t nq = 0;
    speaker_id_status_t st = extract_features(pcm, samples, &nq);
    if (st != SPEAKER_ID_OK) {
        xSemaphoreGive(s_mutex);
        return st;
    }

    int best = -1;
    float best_ratio = 1.0e9f;
    float second_ratio = 1.0e9f;
    for (int p = 0; p < s_count; ++p) {
        const profile_t *prof = &s_profiles[p];
        float d[SPEAKER_ID_SAMPLES_PER_PROFILE];
        int nd = 0;
        for (int k = 0; k < prof->count; ++k)
            d[nd++] = dtw_distance(s_work->feat, nq, prof->t[k].data, prof->t[k].frames);
        for (int a = 0; a < nd; ++a)            /* urutkan kecil -> besar */
            for (int b = a + 1; b < nd; ++b)
                if (d[b] < d[a]) { float tmp = d[a]; d[a] = d[b]; d[b] = tmp; }
        const float dist = nd >= 2 ? (d[0] + d[1]) * 0.5f : d[0];
        float ref = (float)prof->intra_x100 / 100.0f;
        if (ref < INTRA_FLOOR) ref = INTRA_FLOOR;
        const float ratio = dist / ref;
        if (out && p < SPEAKER_ID_MAX_PROFILES) {
            strlcpy(out->scores[p].name, prof->name, sizeof(out->scores[p].name));
            out->scores[p].distance = dist;
            out->scores[p].ratio = ratio;
            out->score_count = p + 1;
        }
        if (ratio < best_ratio) {
            second_ratio = best_ratio;
            best_ratio = ratio;
            best = p;
        } else if (ratio < second_ratio) {
            second_ratio = ratio;
        }
    }
    /* Jika dua profil sama-sama mirip (selisih < 10%), jangan menebak nama:
     * salah menyebut nama lebih buruk daripada tidak menyapa. */
    const bool ambiguous = s_count >= 2 && second_ratio < best_ratio * AMBIGUOUS_MARGIN;
    const bool known = best >= 0 && best_ratio <= s_tol && !ambiguous;
    if (out) {
        out->known = known;
        out->ambiguous = ambiguous && best_ratio <= s_tol;
        if (known) strlcpy(out->name, s_profiles[best].name, sizeof(out->name));
    }
    ESP_LOGI(TAG, "Identifikasi: frame=%u terbaik=%s rasio=%.2f kedua=%.2f toleransi=%.2f -> %s%s (%lld ms)",
             (unsigned)nq, best >= 0 ? s_profiles[best].name : "-", (double)best_ratio,
             s_count >= 2 ? (double)second_ratio : 0.0, (double)s_tol,
             known ? "DIKENAL" : "TIDAK DIKENAL",
             (ambiguous && best_ratio <= s_tol) ? " (ragu: dua profil mirip)" : "",
             (long long)((esp_timer_get_time() - t0) / 1000));
    xSemaphoreGive(s_mutex);
    return SPEAKER_ID_OK;
}

speaker_id_status_t speaker_id_enroll_sample(const char *name, int step,
                                             const int16_t *pcm, size_t samples,
                                             int *count_out, bool *done_out)
{
    if (count_out) *count_out = 0;
    if (done_out) *done_out = false;
    if (!s_ready) return SPEAKER_ID_ERR_NOT_READY;

    char clean[SPEAKER_ID_NAME_MAX + 1];
    if (!sanitize_name(name, clean)) return SPEAKER_ID_ERR_NAME;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    speaker_id_status_t result = SPEAKER_ID_OK;

    if (step == 1) {
        if (find_profile(clean) < 0 && s_count >= SPEAKER_ID_MAX_PROFILES) {
            xSemaphoreGive(s_mutex);
            return SPEAKER_ID_ERR_FULL;
        }
        strlcpy(s_pend_name, clean, sizeof(s_pend_name));
        s_pend_count = 0;
    } else if (!names_equal(s_pend_name, clean) || step != s_pend_count + 1) {
        xSemaphoreGive(s_mutex);
        return SPEAKER_ID_ERR_STEP;
    }

    uint16_t frames = 0;
    result = extract_features(pcm, samples, &frames);
    if (result != SPEAKER_ID_OK) {
        xSemaphoreGive(s_mutex);
        return result;
    }

    template_t *slot = &s_pend[s_pend_count];
    slot->frames = frames;
    for (size_t i = 0; i < (size_t)frames * NUM_CEP; ++i) slot->data[i] = quantize(s_work->feat[i]);
    ++s_pend_count;
    if (count_out) *count_out = s_pend_count;

    if (s_pend_count < SPEAKER_ID_SAMPLES_PER_PROFILE) {
        xSemaphoreGive(s_mutex);
        return SPEAKER_ID_OK;
    }

    /* Rekaman ke-3 masuk: hitung jarak acuan (rata-rata 3 pasangan). */
    float intra = 0.0f;
    int pairs = 0;
    for (int a = 0; a < SPEAKER_ID_SAMPLES_PER_PROFILE; ++a) {
        for (int i = 0; i < s_pend[a].frames * NUM_CEP; ++i)
            s_work->query[i] = (float)s_pend[a].data[i] / FEAT_SCALE;
        for (int b = a + 1; b < SPEAKER_ID_SAMPLES_PER_PROFILE; ++b) {
            intra += dtw_distance(s_work->query, s_pend[a].frames, s_pend[b].data, s_pend[b].frames);
            ++pairs;
        }
    }
    intra /= (float)pairs;

    int idx = find_profile(clean);
    const bool is_new = idx < 0;
    if (is_new) idx = s_count;
    profile_t *prof = &s_profiles[idx];
    memset(prof, 0, sizeof(*prof));
    strlcpy(prof->name, clean, sizeof(prof->name));
    prof->count = SPEAKER_ID_SAMPLES_PER_PROFILE;
    float intra_x100 = intra * 100.0f;
    if (intra_x100 > 65000.0f) intra_x100 = 65000.0f;
    prof->intra_x100 = (uint16_t)intra_x100;
    memcpy(prof->t, s_pend, sizeof(prof->t));
    if (is_new) ++s_count;

    const bool saved = save_file();
    if (!saved && is_new) --s_count;
    s_pend_name[0] = '\0';
    s_pend_count = 0;
    if (done_out) *done_out = saved;
    ESP_LOGI(TAG, "Profil '%s' %s: acuan=%.2f", clean, saved ? "tersimpan" : "GAGAL disimpan", (double)intra);
    xSemaphoreGive(s_mutex);
    return saved ? SPEAKER_ID_OK : SPEAKER_ID_ERR_STORAGE;
}

bool speaker_id_delete(const char *name)
{
    if (!s_ready || !name) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = find_profile(name);
    bool ok = false;
    if (idx >= 0) {
        for (int i = idx; i + 1 < s_count; ++i) memcpy(&s_profiles[i], &s_profiles[i + 1], sizeof(profile_t));
        --s_count;
        ok = save_file();
        ESP_LOGI(TAG, "Profil '%s' dihapus (%s)", name, ok ? "OK" : "file gagal ditulis");
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

size_t speaker_id_record(int16_t *dest, size_t max_samples, uint32_t timeout_ms)
{
    if (!dest || max_samples == 0) return 0;
    const bool was_enabled = audio_hal_is_mic_enabled();
    audio_hal_set_mic_enabled(true);

    size_t got = 0;
    size_t discard = 3200;                      /* buang 200 ms awal (sisa antrean lama) */
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (got < max_samples && esp_timer_get_time() < deadline) {
        const size_t bytes = audio_read_mic_raw((uint8_t *)(dest + got), (max_samples - got) * sizeof(int16_t));
        const size_t n = bytes / sizeof(int16_t);
        if (n == 0) { vTaskDelay(1); continue; }
        if (discard > 0) {
            if (n <= discard) { discard -= n; continue; }
            memmove(dest, dest + discard, (n - discard) * sizeof(int16_t));
            got = n - discard;
            discard = 0;
            continue;
        }
        got += n;
    }
    if (!was_enabled) audio_hal_set_mic_enabled(false);
    return got;
}
