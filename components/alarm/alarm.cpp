#include "alarm.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ALARM";
static const char *NS = "alarmcfg";

#define STORE_MAGIC 0xA1A20002u
#define LATE_LIMIT_SECONDS 300   // terlambat lebih dari 5 menit (mis. robot mati): jangan bunyi

typedef struct {
    uint32_t magic;
    alarm_info_t slots[ALARM_MAX];
} alarm_store_t;

static alarm_info_t slots[ALARM_MAX];
static alarm_info_t due_info;
static bool due_pending = false;
static bool clock_ready = false;
static portMUX_TYPE alarm_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *kind_text(int kind)
{
    return kind == ALARM_KIND_WAKE ? "alarm bangun" : "pengingat";
}

static const char *repeat_text(int repeat)
{
    switch (repeat) {
        case ALARM_REPEAT_DAILY: return "setiap hari";
        case ALARM_REPEAT_WEEKDAYS: return "hari kerja (Senin sampai Jumat)";
        default: return "sekali saja";
    }
}

static bool persist_alarms(void)
{
    alarm_store_t st;
    memset(&st, 0, sizeof(st));
    st.magic = STORE_MAGIC;
    portENTER_CRITICAL(&alarm_mux);
    memcpy(st.slots, slots, sizeof(slots));
    portEXIT_CRITICAL(&alarm_mux);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, "slots", &st, sizeof(st));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

/* Waktu berikutnya untuk jam:menit, selalu SETELAH "after". Hari kerja melewati Sabtu/Minggu. */
static time_t next_target(int hour, int minute, int repeat, time_t after)
{
    struct tm local = {};
    localtime_r(&after, &local);
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    time_t t = mktime(&local);
    for (int i = 0; i < 9; ++i) {
        bool ok = t > after;
        if (ok && repeat == ALARM_REPEAT_WEEKDAYS) {
            struct tm w = {};
            localtime_r(&t, &w);
            if (w.tm_wday == 0 || w.tm_wday == 6) ok = false;
        }
        if (ok) return t;
        local.tm_mday += 1;
        local.tm_isdst = -1;
        t = mktime(&local);
    }
    return t;
}

void alarm_set_time_ready(bool ready)
{
    portENTER_CRITICAL(&alarm_mux);
    clock_ready = ready;
    portEXIT_CRITICAL(&alarm_mux);
    ESP_LOGI(TAG, "Clock readiness=%s", ready ? "NTP valid" : "NTP unavailable; alarm firing disabled");
}

bool alarm_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS alarm belum tersedia; alarm dimulai kosong");
        return false;
    }
    alarm_store_t st;
    size_t len = sizeof(st);
    bool loaded = false;
    if (nvs_get_blob(h, "slots", &st, &len) == ESP_OK && len == sizeof(st) && st.magic == STORE_MAGIC) {
        portENTER_CRITICAL(&alarm_mux);
        memcpy(slots, st.slots, sizeof(slots));
        portEXIT_CRITICAL(&alarm_mux);
        loaded = true;
    }
    bool migrated = false;
    if (!loaded) {
        /* Format lama: satu alarm. Pindahkan ke nomor 1 sebagai alarm bangun sekali. */
        uint8_t active = 0;
        int32_t hour = 0, minute = 0;
        int64_t target = 0;
        char label[96] = "Alarm";
        size_t label_len = sizeof(label);
        (void)nvs_get_u8(h, "active", &active);
        (void)nvs_get_i32(h, "hour", &hour);
        (void)nvs_get_i32(h, "minute", &minute);
        (void)nvs_get_i64(h, "target", &target);
        if (nvs_get_str(h, "label", label, &label_len) != ESP_OK) strlcpy(label, "Alarm", sizeof(label));
        if (active != 0 && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && target > 0) {
            portENTER_CRITICAL(&alarm_mux);
            slots[0].active = true;
            slots[0].kind = ALARM_KIND_WAKE;
            slots[0].repeat = ALARM_REPEAT_ONCE;
            slots[0].hour = (int)hour;
            slots[0].minute = (int)minute;
            slots[0].target_epoch = target;
            strlcpy(slots[0].label, label, sizeof(slots[0].label));
            portEXIT_CRITICAL(&alarm_mux);
            migrated = true;
        }
    }
    nvs_close(h);
    portENTER_CRITICAL(&alarm_mux);
    for (int i = 0; i < ALARM_MAX; ++i) slots[i].id = i + 1;
    portEXIT_CRITICAL(&alarm_mux);
    if (migrated) (void)persist_alarms();
    int count = 0;
    for (int i = 0; i < ALARM_MAX; ++i) if (slots[i].active) ++count;
    ESP_LOGI(TAG, "Alarm NVS: %d alarm aktif%s", count, migrated ? " (dipindah dari format lama)" : "");
    return true;
}

bool alarm_add(int hour, int minute, int minutes_from_now, int kind, int repeat,
               const char *label, char *result, size_t result_size)
{
    const bool relative = minutes_from_now > 0;
    if (!label || !label[0]) {
        if (result && result_size) snprintf(result, result_size, "Gagal: keterangan alarm tidak boleh kosong.");
        return false;
    }
    if (kind != ALARM_KIND_WAKE && kind != ALARM_KIND_REMINDER) kind = ALARM_KIND_REMINDER;
    if (repeat < ALARM_REPEAT_ONCE || repeat > ALARM_REPEAT_WEEKDAYS) repeat = ALARM_REPEAT_ONCE;
    if (relative) {
        if (minutes_from_now > 1440) {
            if (result && result_size) snprintf(result, result_size, "Gagal: pengingat relatif maksimal 24 jam (1440 menit). Sebutkan jamnya saja.");
            return false;
        }
        repeat = ALARM_REPEAT_ONCE;
    } else if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        if (result && result_size) snprintf(result, result_size, "Gagal: jam atau menit tidak valid.");
        return false;
    }
    if (!clock_ready) {
        if (result && result_size) snprintf(result, result_size, "Gagal: waktu NTP belum tersinkronisasi. Alarm tidak disetel agar tidak meleset.");
        return false;
    }
    const time_t now = time(NULL);
    struct tm nowtm = {};
    localtime_r(&now, &nowtm);
    if (nowtm.tm_year < (2024 - 1900)) {
        if (result && result_size) snprintf(result, result_size, "Gagal: waktu perangkat belum tersinkronisasi. Coba lagi setelah internet dan NTP siap.");
        return false;
    }

    time_t target;
    if (relative) {
        target = now + (time_t)minutes_from_now * 60;
        struct tm t = {};
        localtime_r(&target, &t);
        hour = t.tm_hour;
        minute = t.tm_min;
    } else {
        target = next_target(hour, minute, repeat, now);
    }

    int slot = -1;
    bool updated = false;
    bool full = false;
    alarm_info_t before = {};
    portENTER_CRITICAL(&alarm_mux);
    if (!relative) {
        for (int i = 0; i < ALARM_MAX; ++i) {
            if (slots[i].active && slots[i].hour == hour && slots[i].minute == minute &&
                slots[i].repeat == repeat && slots[i].kind == kind) {
                slot = i;
                updated = true;
                break;
            }
        }
    }
    if (slot < 0) {
        for (int i = 0; i < ALARM_MAX; ++i) {
            if (!slots[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) {
        full = true;
    } else {
        before = slots[slot];
        slots[slot].id = slot + 1;
        slots[slot].active = true;
        slots[slot].kind = (uint8_t)kind;
        slots[slot].repeat = (uint8_t)repeat;
        slots[slot].hour = hour;
        slots[slot].minute = minute;
        slots[slot].target_epoch = (int64_t)target;
        strlcpy(slots[slot].label, label, sizeof(slots[slot].label));
    }
    portEXIT_CRITICAL(&alarm_mux);
    if (full) {
        if (result && result_size) snprintf(result, result_size,
            "Gagal: sudah ada %d alarm, itu batas maksimal. Batalkan salah satu dulu.", ALARM_MAX);
        return false;
    }
    if (!persist_alarms()) {
        portENTER_CRITICAL(&alarm_mux);
        slots[slot] = before;
        portEXIT_CRITICAL(&alarm_mux);
        if (result && result_size) snprintf(result, result_size, "Gagal menyimpan alarm ke NVS.");
        return false;
    }
    ESP_LOGI(TAG, "Alarm #%d %s %02d:%02d WITA (%s) label=%s target=%lld",
             slot + 1, kind_text(kind), hour, minute, repeat_text(repeat), label, (long long)target);
    if (result && result_size) {
        if (relative) {
            snprintf(result, result_size,
                "%s nomor %d aktif %d menit lagi (pukul %02d:%02d WITA). Keterangan: %s.",
                kind == ALARM_KIND_WAKE ? "Alarm bangun" : "Pengingat", slot + 1,
                minutes_from_now, hour, minute, label);
        } else {
            snprintf(result, result_size,
                "%s nomor %d %s pukul %02d:%02d WITA, %s. Keterangan: %s.",
                kind == ALARM_KIND_WAKE ? "Alarm bangun" : "Pengingat", slot + 1,
                updated ? "diperbarui" : "aktif", hour, minute, repeat_text(repeat), label);
        }
    }
    return true;
}

bool alarm_set(int hour, int minute, const char *label, char *result, size_t result_size)
{
    return alarm_add(hour, minute, 0, ALARM_KIND_WAKE, ALARM_REPEAT_ONCE, label, result, result_size);
}

bool alarm_cancel_id(int id, char *result, size_t result_size)
{
    if (id < 1 || id > ALARM_MAX) {
        if (result && result_size) snprintf(result, result_size, "Gagal: nomor alarm harus 1 sampai %d.", ALARM_MAX);
        return false;
    }
    bool was_active = false;
    portENTER_CRITICAL(&alarm_mux);
    was_active = slots[id - 1].active;
    slots[id - 1].active = false;
    portEXIT_CRITICAL(&alarm_mux);
    if (!was_active) {
        if (result && result_size) snprintf(result, result_size, "Alarm nomor %d memang tidak aktif.", id);
        return false;
    }
    bool ok = persist_alarms();
    if (result && result_size) snprintf(result, result_size, ok ? "Alarm nomor %d berhasil dibatalkan." :
                                        "Alarm nomor %d dibatalkan tapi gagal disimpan.", id);
    return ok;
}

bool alarm_cancel_all(char *result, size_t result_size)
{
    portENTER_CRITICAL(&alarm_mux);
    for (int i = 0; i < ALARM_MAX; ++i) slots[i].active = false;
    due_pending = false;
    portEXIT_CRITICAL(&alarm_mux);
    bool ok = persist_alarms();
    if (result && result_size) snprintf(result, result_size, "%s",
        ok ? "Semua alarm berhasil dibatalkan." : "Gagal menyimpan pembatalan alarm.");
    return ok;
}

bool alarm_cancel(char *result, size_t result_size)
{
    return alarm_cancel_all(result, result_size);
}

int alarm_list(alarm_info_t *out, int max_items)
{
    if (!out || max_items <= 0) return 0;
    alarm_info_t tmp[ALARM_MAX];
    portENTER_CRITICAL(&alarm_mux);
    memcpy(tmp, slots, sizeof(tmp));
    portEXIT_CRITICAL(&alarm_mux);
    int n = 0;
    for (int i = 0; i < ALARM_MAX && n < max_items; ++i) {
        if (!tmp[i].active) continue;
        int j = n++;
        while (j > 0 && out[j - 1].target_epoch > tmp[i].target_epoch) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = tmp[i];
    }
    return n;
}

int alarm_describe_all(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    alarm_info_t list[ALARM_MAX];
    const int n = alarm_list(list, ALARM_MAX);
    if (n == 0) {
        snprintf(out, out_size, "Saat ini tidak ada alarm atau pengingat aktif.");
        return 0;
    }
    size_t used = (size_t)snprintf(out, out_size, "Ada %d alarm/pengingat aktif. ", n);
    for (int i = 0; i < n && used < out_size; ++i) {
        const int w = snprintf(out + used, out_size - used,
            "Nomor %d: %s pukul %02d:%02d WITA, %s, keterangan %s. ",
            list[i].id, kind_text(list[i].kind), list[i].hour, list[i].minute,
            repeat_text(list[i].repeat), list[i].label);
        if (w < 0) break;
        used += (size_t)w;
    }
    return n;
}

bool alarm_take_due(alarm_info_t *info)
{
    bool taken = false;
    portENTER_CRITICAL(&alarm_mux);
    if (due_pending) {
        if (info) *info = due_info;
        due_pending = false;
        taken = true;
    }
    portEXIT_CRITICAL(&alarm_mux);
    return taken;
}

/* Dipanggil sering dari audio_task; tidak butuh task FreeRTOS tambahan.
 * Perhitungan waktu (mktime) dilakukan DI LUAR critical section. */
bool alarm_poll_due_internal(void)
{
    if (!clock_ready) return false;
    const time_t now = time(NULL);
    alarm_info_t snap[ALARM_MAX];
    bool busy;
    portENTER_CRITICAL(&alarm_mux);
    memcpy(snap, slots, sizeof(snap));
    busy = due_pending;
    portEXIT_CRITICAL(&alarm_mux);
    if (busy) return false;

    bool any_changed = false;
    bool fired = false;
    for (int i = 0; i < ALARM_MAX && !fired; ++i) {
        const alarm_info_t *a = &snap[i];
        if (!a->active || a->target_epoch <= 0 || now < (time_t)a->target_epoch) continue;

        const bool late = (now - (time_t)a->target_epoch) > LATE_LIMIT_SECONDS;
        const bool repeating = a->repeat != ALARM_REPEAT_ONCE;
        const int64_t new_target = repeating
            ? (int64_t)next_target(a->hour, a->minute, a->repeat, now) : 0;

        bool applied = false;
        portENTER_CRITICAL(&alarm_mux);
        if (slots[i].active && slots[i].target_epoch == a->target_epoch) {   // belum diubah task lain
            if (repeating) slots[i].target_epoch = new_target;
            else slots[i].active = false;
            applied = true;
            if (!late) {
                due_info = *a;
                due_pending = true;
            }
        }
        portEXIT_CRITICAL(&alarm_mux);
        if (!applied) continue;
        any_changed = true;
        if (late) {
            ESP_LOGW(TAG, "Alarm #%d terlewat (lebih dari 5 menit): %02d:%02d WITA - %s",
                     a->id, a->hour, a->minute, a->label);
        } else {
            fired = true;
            ESP_LOGW(TAG, "ALARM DUE #%d (%s): %02d:%02d WITA - %s",
                     a->id, kind_text(a->kind), a->hour, a->minute, a->label);
        }
    }
    if (any_changed && !persist_alarms()) ESP_LOGE(TAG, "Gagal menyimpan status alarm setelah terpicu");
    return fired;
}
