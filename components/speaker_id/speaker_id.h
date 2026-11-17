#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pengenal suara sederhana: MFCC + DTW.
 *
 * Cara kerja:
 *  - Daftar : ucapkan "Hi ESP" 3 kali -> 3 template MFCC disimpan di SPIFFS.
 *  - Kenali : saat wake word, rekaman singkat dibandingkan dengan semua template.
 *
 * Cocok untuk kalimat yang SAMA (wake word). Bukan untuk keamanan.
 */

#define SPEAKER_ID_MAX_PROFILES      5
#define SPEAKER_ID_SAMPLES_PER_PROFILE 3
#define SPEAKER_ID_NAME_MAX          24
#define SPEAKER_ID_SAMPLE_RATE       16000

typedef enum {
    SPEAKER_ID_OK = 0,
    SPEAKER_ID_ERR_NOT_READY,   /* penyimpanan / memori belum siap */
    SPEAKER_ID_ERR_QUIET,       /* suara terlalu pelan */
    SPEAKER_ID_ERR_SHORT,       /* suara terlalu pendek */
    SPEAKER_ID_ERR_NAME,        /* nama tidak valid */
    SPEAKER_ID_ERR_FULL,        /* profil sudah penuh */
    SPEAKER_ID_ERR_STEP,        /* urutan rekaman salah */
    SPEAKER_ID_ERR_STORAGE,     /* gagal menulis file */
    SPEAKER_ID_ERR_NOMEM,       /* memori tidak cukup */
    SPEAKER_ID_ERR_NO_PROFILE   /* belum ada profil */
} speaker_id_status_t;

typedef struct {
    char  name[SPEAKER_ID_NAME_MAX + 1];
    float distance;   /* jarak DTW mentah */
    float ratio;      /* jarak dibagi jarak acuan profil (makin kecil makin mirip) */
} speaker_id_score_t;

typedef struct {
    bool known;                         /* true jika ada profil yang cocok */
    bool ambiguous;                     /* mirip lebih dari satu profil -> dianggap tidak dikenal */
    char name[SPEAKER_ID_NAME_MAX + 1]; /* nama profil terbaik jika known */
    int  score_count;
    speaker_id_score_t scores[SPEAKER_ID_MAX_PROFILES];
} speaker_id_result_t;

/* Pesan singkat berbahasa Indonesia untuk status. */
const char *speaker_id_status_text(speaker_id_status_t status);

/* Mount penyimpanan, alokasi memori (PSRAM), baca profil dari file. */
bool speaker_id_init(void);
bool speaker_id_is_ready(void);

int   speaker_id_profile_count(void);
bool  speaker_id_has_profiles(void);
bool  speaker_id_get_profile(int index, char *name, size_t name_size, int *samples);

/* Toleransi 1.0 - 3.0. Makin besar makin mudah dikenali (tapi makin mudah salah). */
float speaker_id_get_tolerance(void);
bool  speaker_id_set_tolerance(float tolerance);

/* Bandingkan suara (PCM16 mono 16 kHz) dengan semua profil. */
speaker_id_status_t speaker_id_identify(const int16_t *pcm, size_t samples,
                                        speaker_id_result_t *out);

/* Pendaftaran. step=1 memulai ulang. Setelah rekaman ke-3 profil otomatis tersimpan. */
speaker_id_status_t speaker_id_enroll_sample(const char *name, int step,
                                             const int16_t *pcm, size_t samples,
                                             int *count_out, bool *done_out);

bool speaker_id_delete(const char *name);

/* Rekam dari mikrofon robot (jalur mentah yang sama dengan wake word). */
size_t speaker_id_record(int16_t *dest, size_t max_samples, uint32_t timeout_ms);

/* ================= Kata harian (dikenali di tengah obrolan) =================
 *
 * Pendaftaran : tiap kata diucapkan SPEAKER_WORD_SAMPLES kali lewat halaman /voice.
 * Saat ngobrol: ucapan terakhir dicari apakah memuat salah satu kata terdaftar
 *               (DTW subsequence). Jika cocok -> pembicara dikenali.
 * Rekaman pakai jalur mikrofon yang SAMA dengan percakapan (audio_read_mic).
 */
#define SPEAKER_WORD_MAX_PER_PROFILE 16
#define SPEAKER_WORD_SAMPLES         2
#define SPEAKER_WORD_NAME_MAX        15

typedef struct {
    char  name[SPEAKER_ID_NAME_MAX + 1];
    char  word[SPEAKER_WORD_NAME_MAX + 1];  /* kata terbaik milik profil ini */
    float ratio;                            /* makin kecil makin mirip */
} speaker_word_score_t;

typedef struct {
    bool known;
    bool ambiguous;
    char name[SPEAKER_ID_NAME_MAX + 1];
    char word[SPEAKER_WORD_NAME_MAX + 1];
    float ratio;
    int  score_count;
    speaker_word_score_t scores[SPEAKER_ID_MAX_PROFILES];
} speaker_word_result_t;

bool  speaker_id_has_words(void);
int   speaker_id_word_profile_count(void);
/* Ambil nama profil ke-index dan jumlah katanya. */
bool  speaker_id_word_get_profile(int index, char *name, size_t name_size, int *word_count);
/* Ambil kata ke-widx milik profil ke-pidx. */
bool  speaker_id_word_get_word(int pidx, int widx, char *word, size_t word_size);

float speaker_id_word_get_tolerance(void);
bool  speaker_id_word_set_tolerance(float tolerance);

/* Cari kata terdaftar di dalam ucapan (PCM16 mono 16 kHz, maks 3 detik terakhir dipakai). */
speaker_id_status_t speaker_id_word_identify(const int16_t *pcm, size_t samples,
                                             speaker_word_result_t *out);

/* step=1 memulai kata baru; setelah rekaman ke-SPEAKER_WORD_SAMPLES kata tersimpan. */
speaker_id_status_t speaker_id_word_enroll(const char *name, const char *word, int step,
                                           const int16_t *pcm, size_t samples,
                                           int *count_out, bool *done_out);

/* word == NULL -> hapus semua kata milik name. */
bool  speaker_id_word_delete(const char *name, const char *word);

/* Rekam lewat jalur mikrofon percakapan (AFE). Jika kosong, jatuh ke jalur mentah. */
size_t speaker_id_record_conv(int16_t *dest, size_t max_samples, uint32_t timeout_ms);

/* Daftarkan halaman /voice ke HTTP server web_config. handle = httpd_handle_t. */
void speaker_id_web_register(void *httpd_handle);

#ifdef __cplusplus
}
#endif
