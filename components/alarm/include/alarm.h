#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Maksimal alarm/pengingat yang tersimpan bersamaan. Nomor alarm = 1..ALARM_MAX.
#define ALARM_MAX 8

// Jenis: bangun (sapaan membangunkan) atau pengingat biasa.
#define ALARM_KIND_WAKE     0
#define ALARM_KIND_REMINDER 1

// Pengulangan.
#define ALARM_REPEAT_ONCE     0   // sekali saja
#define ALARM_REPEAT_DAILY    1   // setiap hari
#define ALARM_REPEAT_WEEKDAYS 2   // Senin sampai Jumat

typedef struct {
    int id;                 // nomor alarm 1..ALARM_MAX
    bool active;
    uint8_t kind;           // ALARM_KIND_*
    uint8_t repeat;         // ALARM_REPEAT_*
    int hour;
    int minute;
    int64_t target_epoch;   // kapan berbunyi berikutnya
    char label[96];
} alarm_info_t;

bool alarm_init(void);
void alarm_set_time_ready(bool ready);

// Tambah alarm. Pilih salah satu cara menentukan waktu:
//   minutes_from_now > 0 : "N menit lagi" (hour/minute diabaikan, selalu sekali)
//   minutes_from_now = 0 : jam:menit WITA; repeat menentukan pengulangan
// Alarm dengan jam, menit, jenis, dan pengulangan yang sama hanya diperbarui keterangannya.
bool alarm_add(int hour, int minute, int minutes_from_now, int kind, int repeat,
               const char *label, char *result, size_t result_size);

// Kompatibel dengan kode lama: alarm bangun sekali pada jam:menit.
bool alarm_set(int hour, int minute, const char *label, char *result, size_t result_size);

bool alarm_cancel_id(int id, char *result, size_t result_size);
bool alarm_cancel_all(char *result, size_t result_size);
// Kompatibel dengan kode lama: sama dengan alarm_cancel_all.
bool alarm_cancel(char *result, size_t result_size);

// Salin alarm aktif (urut dari yang paling dekat). Mengembalikan jumlahnya.
int alarm_list(alarm_info_t *out, int max_items);
// Tulis ringkasan semua alarm aktif dalam bahasa Indonesia. Mengembalikan jumlah alarm.
int alarm_describe_all(char *out, size_t out_size);

bool alarm_take_due(alarm_info_t *info);
// Dipanggil sering dari audio_task. Hanya memicu satu alarm per panggilan,
// dan tidak memicu lagi sebelum alarm_take_due() mengambil yang sebelumnya.
bool alarm_poll_due_internal(void);

#ifdef __cplusplus
}
#endif
