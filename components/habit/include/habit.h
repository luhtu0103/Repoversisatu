#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Catat bahwa pengguna memakai sebuah fitur pada jam sekarang.
 * kind: "radio", "lampu", "colokan", "kipas", "mp3", "cari", "alarm".
 * Kind lain diabaikan. Murah dan aman dipanggil dari task mana pun. */
void habit_note_event(const char *kind);

/* Dipanggil sekali tiap sesi Gemini dibuat. Menyusun teks konteks
 * (waktu, hari ke-berapa, tingkat kedekatan, kebiasaan, pelajaran, aturan gaya).
 * Mengembalikan panjang teks; 0 jika tidak ada. */
size_t habit_build_context(char *out, size_t out_size);

/* "Pelajaran" = kalimat pendek yang Gemini simpan sendiri tentang kebiasaan
 * atau selera pengguna. Terpisah dari ai_memory (yang hanya boleh diubah
 * lewat perintah eksplisit pengguna). */
bool habit_lesson_add(const char *text, char *detail, size_t detail_size);
size_t habit_lesson_list(char *out, size_t out_size);   /* isi teks, 0 jika kosong */
bool habit_lesson_clear(void);

#ifdef __cplusplus
}
#endif
