#include "speaker_id.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "SPEAKER_ID";
static constexpr int SAMPLE_RATE = 16000;
static constexpr int FFT_N = 512;
static constexpr int FRAME_N = 400;
static constexpr int HOP_N = 160;
static constexpr int MFCC_N = 13;
static constexpr int MEL_N = 26;
static constexpr int SEQ_N = 160;
static constexpr int PROFILE_N = 4;
static constexpr int DOWNSAMPLED_N = 80;
static constexpr float PI_F = 3.14159265358979323846f;
static constexpr uint32_t PROFILE_MAGIC = 0x53504944U;

typedef struct {
    uint32_t magic;
    uint16_t count;
    char name[SPEAKER_ID_NAME_MAX];
    int16_t feat[SEQ_N][MFCC_N];
} profile_t;

static profile_t profiles[PROFILE_N];
static profile_t profile_staging;
static size_t profile_count = 0;

static int16_t frame_buf[FRAME_N];
static size_t frame_fill = 0;
static int16_t ring_feat[SEQ_N][MFCC_N];
static size_t ring_write = 0;
static size_t ring_count = 0;
static size_t silence_samples = 0;
static bool was_speech = false;
static bool initialized = false;

static constexpr size_t AUDIO_QUEUE_DEPTH = 3;
static constexpr size_t AUDIO_FRAME_MAX_SAMPLES = 1024;
typedef struct {
    uint16_t samples;
    int16_t pcm[AUDIO_FRAME_MAX_SAMPLES];
} speaker_audio_frame_t;
static QueueHandle_t audio_queue = nullptr;
static QueueHandle_t audio_free_queue = nullptr;
static speaker_audio_frame_t audio_frames[AUDIO_QUEUE_DEPTH];
static TaskHandle_t audio_worker_handle = nullptr;

static float fft_re[FFT_N];
static float fft_im[FFT_N];
static float mel_edges[MEL_N + 2];
static int16_t current_work[SEQ_N][MFCC_N];
static int16_t down_work[DOWNSAMPLED_N][MFCC_N];
static int16_t ref_work[DOWNSAMPLED_N][MFCC_N];

static const char *profile_key(size_t index)
{
    static const char *const keys[PROFILE_N] = {"p0", "p1", "p2", "p3"};
    return index < PROFILE_N ? keys[index] : nullptr;
}

static float hz_to_mel(float hz)
{
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel)
{
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

static void fft512(void)
{
    for (int i = 1, j = 0; i < FFT_N; ++i) {
        int bit = FFT_N >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float temp = fft_re[i];
            fft_re[i] = fft_re[j];
            fft_re[j] = temp;
            temp = fft_im[i];
            fft_im[i] = fft_im[j];
            fft_im[j] = temp;
        }
    }

    for (int len = 2; len <= FFT_N; len <<= 1) {
        const float angle = -2.0f * PI_F / (float)len;
        const float wr0 = cosf(angle);
        const float wi0 = sinf(angle);
        for (int i = 0; i < FFT_N; i += len) {
            float wr = 1.0f;
            float wi = 0.0f;
            for (int j = 0; j < len / 2; ++j) {
                const int a = i + j;
                const int b = a + len / 2;
                const float tr = wr * fft_re[b] - wi * fft_im[b];
                const float ti = wr * fft_im[b] + wi * fft_re[b];
                fft_re[b] = fft_re[a] - tr;
                fft_im[b] = fft_im[a] - ti;
                fft_re[a] += tr;
                fft_im[a] += ti;
                const float next_wr = wr * wr0 - wi * wi0;
                wi = wr * wi0 + wi * wr0;
                wr = next_wr;
            }
        }
    }
}

static void compute_mfcc(const int16_t *pcm, int16_t out[MFCC_N])
{
    for (int i = 0; i < FFT_N; ++i) {
        if (i < FRAME_N) {
            const float window = 0.54f - 0.46f * cosf(
                2.0f * PI_F * (float)i / (float)(FRAME_N - 1));
            fft_re[i] = (float)pcm[i] * window / 32768.0f;
        } else {
            fft_re[i] = 0.0f;
        }
        fft_im[i] = 0.0f;
    }

    fft512();

    float power[FFT_N / 2 + 1];
    for (int i = 0; i <= FFT_N / 2; ++i) {
        power[i] = fft_re[i] * fft_re[i] + fft_im[i] * fft_im[i];
    }

    float mel[MEL_N];
    for (int m = 0; m < MEL_N; ++m) {
        float sum = 0.0f;
        const float left = mel_edges[m];
        const float center = mel_edges[m + 1];
        const float right = mel_edges[m + 2];
        for (int k = 0; k <= FFT_N / 2; ++k) {
            const float hz = (float)k * (float)SAMPLE_RATE / (float)FFT_N;
            if (hz >= left && hz <= center && center > left) {
                sum += power[k] * (hz - left) / (center - left);
            } else if (hz > center && hz <= right && right > center) {
                sum += power[k] * (right - hz) / (right - center);
            }
        }
        mel[m] = logf(sum + 1e-10f);
    }

    for (int c = 0; c < MFCC_N; ++c) {
        float sum = 0.0f;
        for (int m = 0; m < MEL_N; ++m) {
            sum += mel[m] * cosf(PI_F * (float)c * ((float)m + 0.5f) / (float)MEL_N);
        }
        float scaled = sum * 100.0f;
        if (scaled > 30000.0f) {
            scaled = 30000.0f;
        }
        if (scaled < -30000.0f) {
            scaled = -30000.0f;
        }
        out[c] = (int16_t)scaled;
    }
}

static void push_feature(const int16_t *pcm)
{
    int16_t feature[MFCC_N];
    compute_mfcc(pcm, feature);
    memcpy(ring_feat[ring_write], feature, sizeof(feature));
    ring_write = (ring_write + 1) % SEQ_N;
    if (ring_count < SEQ_N) {
        ++ring_count;
    }
}

static void load_profiles(void)
{
    nvs_handle_t handle;
    if (nvs_open("speaker_id", NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "NVS speaker belum ada; mulai tanpa profil");
        return;
    }

    profile_count = 0;
    for (size_t i = 0; i < PROFILE_N; ++i) {
        profile_t profile = {};
        size_t length = sizeof(profile);
        if (nvs_get_blob(handle, profile_key(i), &profile, &length) == ESP_OK &&
            length == sizeof(profile) &&
            profile.magic == PROFILE_MAGIC &&
            profile.count > 0 &&
            profile.count <= SEQ_N &&
            profile.name[0] != '\0') {
            profiles[profile_count++] = profile;
        }
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Profil suara dimuat dari NVS: %u", (unsigned)profile_count);
}

static void speaker_id_audio_worker(void *arg);

extern "C" void speaker_id_init(void)
{
    if (initialized) {
        return;
    }
    initialized = true;

    const float max_mel = hz_to_mel((float)SAMPLE_RATE / 2.0f);
    for (int i = 0; i < MEL_N + 2; ++i) {
        const float mel = (float)i * max_mel / (float)(MEL_N + 1);
        mel_edges[i] = mel_to_hz(mel);
    }
    load_profiles();

    audio_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(speaker_audio_frame_t *));
    audio_free_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(speaker_audio_frame_t *));
    if (audio_queue == nullptr || audio_free_queue == nullptr) {
        ESP_LOGE(TAG, "Gagal membuat antrean audio; pengenalan suara dinonaktifkan");
        if (audio_queue != nullptr) vQueueDelete(audio_queue);
        if (audio_free_queue != nullptr) vQueueDelete(audio_free_queue);
        audio_queue = nullptr;
        audio_free_queue = nullptr;
        return;
    }
    for (size_t i = 0; i < AUDIO_QUEUE_DEPTH; ++i) {
        speaker_audio_frame_t *frame = &audio_frames[i];
        if (xQueueSend(audio_free_queue, &frame, 0) != pdTRUE) {
            ESP_LOGE(TAG, "Gagal menyiapkan pool audio speaker ID");
            vQueueDelete(audio_queue);
            vQueueDelete(audio_free_queue);
            audio_queue = nullptr;
            audio_free_queue = nullptr;
            return;
        }
    }
    const BaseType_t task_result = xTaskCreatePinnedToCore(
        speaker_id_audio_worker, "speaker_id", 6144, nullptr, 3,
        &audio_worker_handle, 1);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat task speaker ID; pengenalan suara dinonaktifkan");
        vQueueDelete(audio_queue);
        vQueueDelete(audio_free_queue);
        audio_queue = nullptr;
        audio_free_queue = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Worker MFCC-DTW aktif di task terpisah");
}

static void process_pcm_internal(const int16_t *pcm, size_t samples)
{
    if (pcm == nullptr || samples == 0) {
        return;
    }
    uint64_t energy = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t value = pcm[i];
        energy += (uint64_t)(value * value);
    }
    const float rms = sqrtf((float)energy / (float)samples);

    if (rms < 220.0f) {
        silence_samples += samples;
        if (silence_samples > 2400) {
            was_speech = false;
        }
        return;
    }

    if (!was_speech && silence_samples > 2400) {
        ring_count = 0;
        ring_write = 0;
        frame_fill = 0;
    }
    was_speech = true;
    silence_samples = 0;

    for (size_t i = 0; i < samples; ++i) {
        frame_buf[frame_fill++] = pcm[i];
        if (frame_fill == FRAME_N) {
            push_feature(frame_buf);
            memmove(frame_buf, frame_buf + HOP_N, (FRAME_N - HOP_N) * sizeof(int16_t));
            frame_fill = FRAME_N - HOP_N;
        }
    }
}

static void speaker_id_audio_worker(void *arg)
{
    (void)arg;
    speaker_audio_frame_t *frame = nullptr;
    for (;;) {
        if (xQueueReceive(audio_queue, &frame, portMAX_DELAY) == pdTRUE && frame != nullptr) {
            if (frame->samples > 0 && frame->samples <= AUDIO_FRAME_MAX_SAMPLES) {
                process_pcm_internal(frame->pcm, frame->samples);
            }
            (void)xQueueSend(audio_free_queue, &frame, portMAX_DELAY);
        }
    }
}

extern "C" void speaker_id_process_pcm(const int16_t *pcm, size_t samples)
{
    if (pcm == nullptr || samples == 0) {
        return;
    }
    /* Start the MFCC worker only when real microphone PCM is available,
     * not during early boot before Wi-Fi/cache initialization. */
    if (audio_queue == nullptr) {
        speaker_id_init();
    }
    if (audio_queue == nullptr) {
        return;
    }
    if (samples > AUDIO_FRAME_MAX_SAMPLES) {
        pcm += samples - AUDIO_FRAME_MAX_SAMPLES;
        samples = AUDIO_FRAME_MAX_SAMPLES;
    }

    speaker_audio_frame_t *frame = nullptr;
    if (xQueueReceive(audio_free_queue, &frame, 0) != pdTRUE || frame == nullptr) {
        return;
    }
    frame->samples = static_cast<uint16_t>(samples);
    memcpy(frame->pcm, pcm, samples * sizeof(int16_t));
    if (xQueueSend(audio_queue, &frame, 0) != pdTRUE) {
        (void)xQueueSend(audio_free_queue, &frame, 0);
    }
}

static void copy_recent(int16_t dst[SEQ_N][MFCC_N], size_t *count)
{
    *count = ring_count;
    const size_t start = (ring_write + SEQ_N - ring_count) % SEQ_N;
    for (size_t i = 0; i < ring_count; ++i) {
        memcpy(dst[i], ring_feat[(start + i) % SEQ_N], sizeof(dst[i]));
    }
}

extern "C" bool speaker_id_enroll_last(const char *name, char *result, size_t result_len)
{
    if (result != nullptr && result_len > 0) {
        result[0] = '\0';
    }
    if (name == nullptr || name[0] == '\0' || strlen(name) >= SPEAKER_ID_NAME_MAX) {
        if (result != nullptr && result_len > 0) {
            snprintf(result, result_len, "Nama profil tidak valid (maksimum %d karakter).",
                     SPEAKER_ID_NAME_MAX - 1);
        }
        return false;
    }
    if (!initialized) {
        speaker_id_init();
    }
    if (ring_count < 45) {
        if (result != nullptr && result_len > 0) {
            strlcpy(result, "Rekaman terlalu singkat; ucapkan nama dan kalimat sekitar 2 detik, lalu daftar ulang.",
                    result_len);
        }
        return false;
    }

    int index = -1;
    for (size_t i = 0; i < profile_count; ++i) {
        if (strcmp(profiles[i].name, name) == 0) {
            index = (int)i;
            break;
        }
    }
    if (index < 0) {
        if (profile_count >= PROFILE_N) {
            if (result != nullptr && result_len > 0) {
                snprintf(result, result_len, "Penyimpanan penuh; maksimum %d profil suara.", PROFILE_N);
            }
            return false;
        }
        index = (int)profile_count;
    }

    profile_t &profile = profile_staging;
    memset(&profile, 0, sizeof(profile));
    profile.magic = PROFILE_MAGIC;
    strlcpy(profile.name, name, sizeof(profile.name));
    size_t feature_count = 0;
    copy_recent(profile.feat, &feature_count);
    profile.count = (uint16_t)feature_count;

    nvs_handle_t handle;
    if (nvs_open("speaker_id", NVS_READWRITE, &handle) != ESP_OK) {
        if (result != nullptr && result_len > 0) {
            strlcpy(result, "NVS tidak bisa dibuka.", result_len);
        }
        return false;
    }

    const esp_err_t write_error = nvs_set_blob(handle, profile_key((size_t)index),
                                                &profile, sizeof(profile));
    esp_err_t commit_error = write_error;
    if (write_error == ESP_OK) {
        commit_error = nvs_commit(handle);
    }
    nvs_close(handle);

    if (commit_error != ESP_OK) {
        if (result != nullptr && result_len > 0) {
            snprintf(result, result_len, "Gagal menyimpan profil ke NVS (kode %d).", (int)commit_error);
        }
        return false;
    }

    profiles[index] = profile;
    if ((size_t)(index + 1) > profile_count) {
        profile_count = (size_t)(index + 1);
    }
    if (result != nullptr && result_len > 0) {
        snprintf(result, result_len, "Profil suara '%s' berhasil disimpan di NVS.", name);
    }
    ESP_LOGI(TAG, "Profil tersimpan: %s, frames=%u", name, (unsigned)profile.count);
    return true;
}

static float dtw_distance(const int16_t a[SEQ_N][MFCC_N], size_t count_a,
                          const int16_t b[SEQ_N][MFCC_N], size_t count_b)
{
    float previous[SEQ_N + 1];
    float current[SEQ_N + 1];
    for (int j = 0; j <= SEQ_N; ++j) {
        previous[j] = 1e30f;
    }
    previous[0] = 0.0f;

    for (size_t i = 1; i <= count_a; ++i) {
        current[0] = 1e30f;
        for (size_t j = 1; j <= count_b; ++j) {
            float distance = 0.0f;
            for (int c = 1; c < MFCC_N; ++c) {
                const float delta = (float)a[i - 1][c] - (float)b[j - 1][c];
                distance += delta * delta;
            }
            distance = sqrtf(distance / (float)(MFCC_N - 1));
            float best = previous[j];
            if (current[j - 1] < best) {
                best = current[j - 1];
            }
            if (previous[j - 1] < best) {
                best = previous[j - 1];
            }
            current[j] = distance + best;
        }
        memcpy(previous, current, sizeof(previous));
    }
    return previous[count_b] / (float)(count_a + count_b);
}

extern "C" bool speaker_id_identify(char *name, size_t name_len, float *distance_out)
{
    if (name != nullptr && name_len > 0) {
        name[0] = '\0';
    }
    if (distance_out != nullptr) {
        *distance_out = 0.0f;
    }
    if (!initialized) {
        speaker_id_init();
    }
    if (ring_count < 45 || profile_count == 0) {
        return false;
    }

    size_t current_count = 0;
    copy_recent(current_work, &current_count);
    float best_distance = 1e30f;
    int winner = -1;

    /* Compare every second frame to bound DTW cost on the ESP32-S3. */
    size_t down_count = 0;
    for (size_t i = 0; i < current_count; i += 2) {
        memcpy(down_work[down_count++], current_work[i], sizeof(down_work[0]));
    }

    for (size_t p = 0; p < profile_count; ++p) {
        size_t reference_count = 0;
        for (size_t i = 0; i < profiles[p].count; i += 2) {
            if (reference_count < DOWNSAMPLED_N) {
                memcpy(ref_work[reference_count++], profiles[p].feat[i], sizeof(ref_work[0]));
            }
        }
        if (reference_count < 10 || down_count < 10) {
            continue;
        }
        const float distance = dtw_distance(
            (const int16_t (*)[MFCC_N])down_work, down_count,
            (const int16_t (*)[MFCC_N])ref_work, reference_count);
        if (distance < best_distance) {
            best_distance = distance;
            winner = (int)p;
        }
    }

    if (distance_out != nullptr) {
        *distance_out = best_distance;
    }
    /* Initial threshold requires calibration with actual device recordings. */
    if (winner < 0 || best_distance > 1800.0f) {
        return false;
    }
    if (name != nullptr && name_len > 0) {
        strlcpy(name, profiles[winner].name, name_len);
    }
    return true;
}

extern "C" size_t speaker_id_profile_count(void)
{
    if (!initialized) {
        speaker_id_init();
    }
    return profile_count;
}
