#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mulai melapor kondisi robot ke DevKit lewat UART (halaman /robot di web DevKit).
 * Panggil sekali di app_main setelah Wi-Fi siap. Aman dipanggil dua kali. */
void robot_stats_start(void);

/* Catat status koneksi Gemini (true = tersambung). */
void robot_stats_ws_state(bool connected);

/* Catat bahwa koneksi Gemini putus (dikirim ke DevKit sebagai kejadian).
 * src: "err" (error jaringan), "dc" (terputus), "cl" (ditutup). code: errno soket jika ada, jika tidak 0. */
void robot_stats_note_ws_drop(const char *src, int code);

/* Simpan kode penutupan dari server Gemini (mis. 1008, 1011) untuk ikut dilaporkan saat putus. */
void robot_stats_note_close_code(int code);

/* Ukur suara mic yang dikirim ke Gemini (puncak dan jumlah sampel pecah/clipping).
 * Dipanggil dari audio_task tiap potongan suara saat percakapan. Murah: satu loop pendek. */
void robot_stats_mic_feed(const int16_t *samples, size_t count);

/* Kirim kejadian bebas, mis. "restart_macet". Hanya huruf, angka, garis bawah. */
void robot_stats_event(const char *kind);

#ifdef __cplusplus
}
#endif
