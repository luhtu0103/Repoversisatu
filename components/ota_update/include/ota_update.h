#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Dipanggil sekali dengan teks hasil (untuk dibacakan Gemini). Boleh NULL.
typedef void (*ota_result_cb_t)(void *ctx, const char *text);

// Tandai program yang sedang jalan sebagai "sehat". Wajib dipanggil setelah
// robot terbukti hidup normal. Kalau tidak dipanggil dan robot restart,
// bootloader akan kembali ke program lama (rollback).
void ota_update_mark_valid(void);

// Jalankan di task sendiri (butuh internet, tidak boleh memblokir WebSocket).
//   install = false : hanya cek apakah ada versi baru.
//   install = true  : cek, lalu kalau ada versi baru langsung unduh + pasang + restart.
// cb dipanggil tepat satu kali SEBELUM unduhan dimulai. Return false kalau
// sedang sibuk atau memori tidak cukup.
bool ota_update_run_async(bool install, ota_result_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
