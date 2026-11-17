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
    SPEAKER_ID_ERR_NO_PROFILE,  /* belum ada profil */
    SPEAKER_ID_ERR_FEW          /* suara bersuara terlalu sedikit */
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

/* ================= Warna suara (dikenali di tengah obrolan) =================
 *
 * Pendaftaran : bicara bebas ~20 detik lewat halaman /voice.
 * Saat ngobrol: 0,4 - 3 detik suara dibandingkan dengan "sidik warna suara" tiap orang.
 * Tidak peduli kalimatnya apa. Akurasi lebih rendah daripada pencocokan kata; bukan untuk keamanan.
 */
typedef struct {
    char  name[SPEAKER_ID_NAME_MAX + 1];
    float ratio;                            /* makin kecil makin mirip */
} speaker_vp_score_t;

typedef struct {
    bool known;
    bool ambiguous;
    char name[SPEAKER_ID_NAME_MAX + 1];
    float ratio;
    int  frames;                            /* jumlah frame bersuara yang dipakai */
    int  score_count;
    speaker_vp_score_t scores[SPEAKER_ID_MAX_PROFILES];
} speaker_vp_result_t;

bool  speaker_vp_has_profiles(void);
int   speaker_vp_profile_count(void);
bool  speaker_vp_get_profile(int index, char *name, size_t name_size);

float speaker_vp_get_tolerance(void);
bool  speaker_vp_set_tolerance(float tolerance);

speaker_id_status_t speaker_vp_identify(const int16_t *pcm, size_t samples, speaker_vp_result_t *out);
/* pcm = rekaman bicara bebas (sekitar 20 detik). Mengganti profil bernama sama. */
speaker_id_status_t speaker_vp_enroll(const char *name, const int16_t *pcm, size_t samples, int *frames_out);
bool  speaker_vp_delete(const char *name);

/* Rekam lewat jalur mikrofon percakapan (AFE). Jika kosong, jatuh ke jalur mentah. */
size_t speaker_id_record_conv(int16_t *dest, size_t max_samples, uint32_t timeout_ms);

/* Daftarkan halaman /voice ke HTTP server web_config. handle = httpd_handle_t. */
void speaker_id_web_register(void *httpd_handle);

#ifdef __cplusplus
}
#endif
