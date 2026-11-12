#include "speaker_id.h"

#include <math.h>
#include <float.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "SPEAKER_ID";

namespace {
constexpr int kSampleRate = 16000;
constexpr int kFrame = 400;       // 25 ms
constexpr int kHop = 160;         // 10 ms
constexpr int kBins = kFrame / 2 + 1;
constexpr int kMelFilters = 26;
constexpr int kCoeffs = 13;
constexpr int kMaxFrames = 240; // up to 2.4 s at 16 kHz with 10 ms hop
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinRmsPcm = 120.0f;

float window_fn[kFrame];
float cos_step[kBins];
float sin_step[kBins];
float mel_weights[kMelFilters][kBins];
bool initialized = false;

float reference_features[kMaxFrames][kCoeffs];
int reference_count = 0;
constexpr uint32_t kTemplateMagic = 0x53494431U;
struct PersistedTemplateHeader { uint32_t magic; uint32_t count; char name[32]; };
constexpr size_t kTemplateHeaderSize = sizeof(PersistedTemplateHeader);
constexpr size_t template_blob_size(size_t count) {
    return kTemplateHeaderSize + count * kCoeffs * sizeof(float);
}
char enrolled_name[32] = {};
volatile int request_state = 0;
char request_name[32] = {};
volatile esp_err_t request_result = ESP_ERR_INVALID_STATE;
volatile float verification_distance = 0.0f;

float current_features[kMaxFrames][kCoeffs];
float power_spectrum[kBins];
float mel_energy[kMelFilters];
float dtw_prev[kMaxFrames + 1];
float dtw_curr[kMaxFrames + 1];

float hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}
float mel_to_hz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

void initialize_tables() {
    for (int n = 0; n < kFrame; ++n) {
        window_fn[n] = 0.5f - 0.5f *
            cosf(2.0f * kPi * n / (kFrame - 1));
    }

    for (int k = 0; k < kBins; ++k) {
        const float angle = 2.0f * kPi * k / kFrame;
        cos_step[k] = cosf(angle);
        sin_step[k] = sinf(angle);
    }

    const float mel_min = hz_to_mel(80.0f);
    const float mel_max = hz_to_mel(7600.0f);
    float edge[kMelFilters + 2];

    for (int i = 0; i < kMelFilters + 2; ++i) {
        const float mel = mel_min +
            (mel_max - mel_min) * i / (kMelFilters + 1);
        edge[i] = mel_to_hz(mel);
    }

    memset(mel_weights, 0, sizeof(mel_weights));
    for (int m = 0; m < kMelFilters; ++m) {
        for (int k = 0; k < kBins; ++k) {
            const float hz = (float)k * kSampleRate / kFrame;
            if (hz >= edge[m] && hz <= edge[m + 1] &&
                edge[m + 1] > edge[m]) {
                mel_weights[m][k] =
                    (hz - edge[m]) / (edge[m + 1] - edge[m]);
            } else if (hz > edge[m + 1] && hz <= edge[m + 2] &&
                       edge[m + 2] > edge[m + 1]) {
                mel_weights[m][k] =
                    (edge[m + 2] - hz) / (edge[m + 2] - edge[m + 1]);
            }
        }
    }
    initialized = true;
}

void compute_mfcc(const int16_t *pcm, float out[kCoeffs]) {
    memset(power_spectrum, 0, sizeof(power_spectrum));
    memset(mel_energy, 0, sizeof(mel_energy));

    // DFT recurrence avoids calling sin/cos for every audio sample.
    for (int k = 0; k < kBins; ++k) {
        float re = 0.0f, im = 0.0f;
        float c = 1.0f, s = 0.0f;
        const float cs = cos_step[k];
        const float ss = sin_step[k];

        for (int n = 0; n < kFrame; ++n) {
            const float x = ((float)pcm[n] / 32768.0f) * window_fn[n];
            re += x * c;
            im -= x * s;
            const float next_c = c * cs - s * ss;
            s = s * cs + c * ss;
            c = next_c;
        }
        power_spectrum[k] = (re * re + im * im) / kFrame;
    }

    for (int m = 0; m < kMelFilters; ++m) {
        float e = 0.0f;
        for (int k = 0; k < kBins; ++k)
            e += power_spectrum[k] * mel_weights[m][k];
        mel_energy[m] = logf(e + 1e-10f);
    }

    for (int c = 0; c < kCoeffs; ++c) {
        float sum = 0.0f;
        for (int m = 0; m < kMelFilters; ++m) {
            sum += mel_energy[m] *
                cosf(kPi * c * (m + 0.5f) / kMelFilters);
        }
        out[c] = sum;
    }
}

esp_err_t extract_features(const int16_t *pcm, size_t samples,
                           float out[kMaxFrames][kCoeffs], int *out_count) {
    if (!pcm || !out_count || samples < (size_t)kFrame)
        return ESP_ERR_INVALID_ARG;

    int frames = 0;
    double sum_sq = 0.0;
    size_t used = 0;
    for (size_t i = 0; i < samples; ++i) {
        const double v = pcm[i];
        sum_sq += v * v;
        ++used;
    }
    const float rms = (used > 0) ? (float)sqrt(sum_sq / used) : 0.0f;
    if (rms < kMinRmsPcm) {
        ESP_LOGW(TAG, "Audio terlalu senyap (PCM RMS %.1f)", (double)rms);
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t start = 0;
         start + kFrame <= samples && frames < kMaxFrames;
         start += kHop) {
        compute_mfcc(pcm + start, out[frames]);
        ++frames;
    }

    if (frames < 5) return ESP_ERR_INVALID_SIZE;
    *out_count = frames;
    return ESP_OK;
}

float dtw_distance(float a[kMaxFrames][kCoeffs], int na,
                   float b[kMaxFrames][kCoeffs], int nb) {
    for (int j = 0; j <= nb; ++j) dtw_prev[j] = FLT_MAX;
    dtw_prev[0] = 0.0f;

    for (int i = 1; i <= na; ++i) {
        dtw_curr[0] = FLT_MAX;
        for (int j = 1; j <= nb; ++j) {
            float d2 = 0.0f;
            // Exclude c0 (overall log energy) to reduce loudness sensitivity.
            for (int c = 1; c < kCoeffs; ++c) {
                const float d = a[i - 1][c] - b[j - 1][c];
                d2 += d * d;
            }
            const float local = sqrtf(d2 / (kCoeffs - 1));
            const float best = fminf(dtw_prev[j],
                                     fminf(dtw_curr[j - 1],
                                           dtw_prev[j - 1]));
            dtw_curr[j] = local + best;
        }
        memcpy(dtw_prev, dtw_curr, (nb + 1) * sizeof(float));
    }

    return dtw_prev[nb] / (na + nb);
}
} // namespace

extern "C" esp_err_t speaker_id_init(void) {
    if (!initialized) initialize_tables();
    nvs_handle_t h;
    if (nvs_open("speaker_id", NVS_READONLY, &h) == ESP_OK) {
        size_t blob_size = 0;
        esp_err_t e = nvs_get_blob(h, "template", nullptr, &blob_size);
        if (e == ESP_OK && blob_size >= kTemplateHeaderSize) {
            PersistedTemplateHeader header = {};
            size_t header_size = sizeof(header);
            e = nvs_get_blob(h, "template", &header, &header_size);
            const size_t expected_size = template_blob_size(header.count);
            if (e == ESP_OK && header.magic == kTemplateMagic &&
                header.count > 0 && header.count <= kMaxFrames &&
                header.name[0] && blob_size == expected_size) {
                float *features = (float *)heap_caps_malloc(
                    header.count * kCoeffs * sizeof(float),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!features) features = (float *)malloc(
                    header.count * kCoeffs * sizeof(float));
                if (features) {
                    // Read only the feature payload; the header was validated above.
                    e = nvs_get_blob(h, "template", nullptr, &blob_size);
                    uint8_t *blob = (uint8_t *)malloc(blob_size);
                    if (blob) {
                        e = nvs_get_blob(h, "template", blob, &blob_size);
                        if (e == ESP_OK) {
                            memcpy(features, blob + kTemplateHeaderSize,
                                   header.count * kCoeffs * sizeof(float));
                            reference_count = (int)header.count;
                            memcpy(reference_features, features,
                                   reference_count * kCoeffs * sizeof(float));
                            strlcpy(enrolled_name, header.name, sizeof(enrolled_name));
                            ESP_LOGI(TAG, "Template NVS dimuat: %s (%d frame, %u byte)",
                                     enrolled_name, reference_count, (unsigned)blob_size);
                        }
                        free(blob);
                    }
                    heap_caps_free(features);
                } else {
                    ESP_LOGW(TAG, "RAM tidak cukup untuk memuat template speaker (%u byte)",
                             (unsigned)(header.count * kCoeffs * sizeof(float)));
                }
            } else {
                ESP_LOGW(TAG, "Template NVS tidak valid: err=%s size=%u count=%u",
                         esp_err_to_name(e), (unsigned)blob_size, (unsigned)header.count);
            }
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Gagal membaca ukuran template NVS: %s", esp_err_to_name(e));
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "MFCC + DTW siap; input PCM16 mono 16 kHz");
    return ESP_OK;
}

extern "C" esp_err_t speaker_id_enroll_pcm16(const int16_t *pcm, size_t samples) {
    return speaker_id_enroll_named_pcm16(pcm, samples, "Pengguna");
}
extern "C" esp_err_t speaker_id_enroll_named_pcm16(const int16_t *pcm, size_t samples, const char *name) {
    if (!pcm || !name || !name[0] || strlen(name) >= sizeof(enrolled_name)) return ESP_ERR_INVALID_ARG;
    if (!initialized) speaker_id_init();
    int count = 0;
    esp_err_t err = extract_features(pcm, samples, current_features, &count);
    if (err != ESP_OK) return err;
    const size_t blob_size = template_blob_size((size_t)count);
    uint8_t *blob = (uint8_t *)heap_caps_calloc(
        1, blob_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) blob = (uint8_t *)calloc(1, blob_size);
    if (!blob) return ESP_ERR_NO_MEM;
    PersistedTemplateHeader header = {};
    header.magic = kTemplateMagic;
    header.count = (uint32_t)count;
    strlcpy(header.name, name, sizeof(header.name));
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + kTemplateHeaderSize, current_features,
           count * kCoeffs * sizeof(float));
    nvs_handle_t h;
    err = nvs_open("speaker_id", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "template", blob, blob_size);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) {
        memcpy(reference_features, current_features, count * kCoeffs * sizeof(float));
        reference_count = count;
        strlcpy(enrolled_name, name, sizeof(enrolled_name));
        ESP_LOGI(TAG, "Template suara '%s' tersimpan di NVS (%d frame, %u byte)",
                 name, count, (unsigned)blob_size);
    } else ESP_LOGE(TAG, "Gagal menyimpan template NVS: %s", esp_err_to_name(err));
    heap_caps_free(blob);
    return err;
}
extern "C" esp_err_t speaker_id_request_enrollment(const char *name) {
    if (!name || !name[0] || strlen(name) >= sizeof(request_name)) return ESP_ERR_INVALID_ARG;
    // A previous caller may have timed out while capture was already in progress.
    // If that capture later completed, discard its unclaimed completion before
    // accepting a new request so the enrollment API cannot remain stuck in state 3.
    if (request_state == 3 || request_state == 6) request_state = 0;
    if (request_state != 0) return ESP_ERR_INVALID_STATE;
    strlcpy(request_name, name, sizeof(request_name));
    request_result = ESP_ERR_INVALID_STATE;
    request_state = 1;
    TickType_t start = xTaskGetTickCount(), timeout = pdMS_TO_TICKS(30000);
    while ((xTaskGetTickCount() - start) < timeout) {
        if (request_state == 3) { esp_err_t result = request_result; request_state = 0; return result; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (request_state == 1) request_state = 0;
    return ESP_ERR_TIMEOUT;
}
extern "C" int speaker_id_take_enrollment_request(char *out, size_t size) {
    if (!out || !size || request_state != 1) return 0;
    strlcpy(out, request_name, size); request_state = 2; return 1;
}
extern "C" void speaker_id_complete_enrollment(esp_err_t result) {
    request_result = result; request_state = 3;
}
extern "C" esp_err_t speaker_id_compare_pcm16(
    const int16_t *pcm, size_t samples, float *out_distance) {
    if (!out_distance || !pcm) return ESP_ERR_INVALID_ARG;
    if (!initialized) speaker_id_init();
    if (reference_count == 0) return ESP_ERR_INVALID_STATE;

    int count = 0;
    ESP_LOGI(TAG, "Verifikasi dimulai: samples=%u template_frames=%d",
             (unsigned)samples, reference_count);
    esp_err_t err = extract_features(pcm, samples, current_features, &count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Verifikasi gagal ekstraksi MFCC: %s", esp_err_to_name(err));
        return err;
    }

    *out_distance = dtw_distance(reference_features, reference_count,
                                 current_features, count);
    ESP_LOGI(TAG, "MFCC/DTW selesai: input_frames=%d template_frames=%d distance=%.5f (ambang belum dikalibrasi)",
             count, reference_count, (double)*out_distance);
    return ESP_OK;
}

extern "C" esp_err_t speaker_id_request_verification(float *out_distance) {
    if (!out_distance) return ESP_ERR_INVALID_ARG;
    if (reference_count == 0) return ESP_ERR_INVALID_STATE;
    if (request_state == 3 || request_state == 6) request_state = 0;
    if (request_state != 0) return ESP_ERR_INVALID_STATE;
    request_result = ESP_ERR_INVALID_STATE;
    verification_distance = 0.0f;
    request_state = 4;
    TickType_t start = xTaskGetTickCount(), timeout = pdMS_TO_TICKS(30000);
    while ((xTaskGetTickCount() - start) < timeout) {
        if (request_state == 6) {
            esp_err_t result = request_result;
            *out_distance = verification_distance;
            request_state = 0;
            return result;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (request_state == 4) request_state = 0;
    return ESP_ERR_TIMEOUT;
}

extern "C" int speaker_id_take_verification_request(void) {
    if (request_state != 4) return 0;
    request_state = 5;
    return 1;
}

extern "C" void speaker_id_complete_verification(esp_err_t result, float distance) {
    request_result = result;
    verification_distance = distance;
    request_state = 6;
}

extern "C" int speaker_id_has_template(void) {
    return reference_count > 0 ? 1 : 0;
}
