#pragma once
// Pemutar radio streaming MP3 (HTTP/HTTPS) lewat speaker robot.
// Alur: radio_resolve() cari stasiun -> radio_set_pending() -> radio_start_pending() -> main
// menunggu radio_is_active() false (sentuh sensor -> radio_stop()).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[48];
    char url[256];
} radio_station_t;

// Cari stasiun radio dari kata kunci (mis. "Genta", "Menara", "Bali"). BLOCKING (1-8 detik,
// pakai internet). Jangan dipanggil dari task RX WebSocket. Mengembalikan false jika tidak ada.
bool radio_resolve(const char *query, radio_station_t *out);

// Simpan stasiun yang akan diputar nanti (setelah Gemini selesai bicara).
void radio_set_pending(const radio_station_t *station);
bool radio_has_pending(void);
// Umur (mikrodetik) sejak radio_set_pending(). 0 jika tidak ada.
int64_t radio_pending_age_us(void);
void radio_clear_pending(void);

// Mulai memutar stasiun pending. Mengembalikan false jika gagal memulai.
bool radio_start_pending(void);

// true selama radio sedang menyambung atau memutar.
bool radio_is_active(void);

// Minta radio berhenti (aman dipanggil dari task mana pun, termasuk sentuh sensor).
void radio_stop(void);

// Setelah radio berhenti: true sekali, mengisi nama stasiun dan alasan:
// 0 = dihentikan pengguna, 1 = gangguan jaringan/stream, 2 = gagal dimulai.
bool radio_has_ended(void);
bool radio_take_ended(char *name, size_t name_len, int *reason);

#ifdef __cplusplus
}
#endif
