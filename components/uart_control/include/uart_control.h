#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void uart_control_init(void);
void uart_control_send(const char *cmd);
void uart_control_send_history(const char *role, const char *text);
// Send web-search results to the secondary DevKit using WSRCH framed lines.
void uart_control_send_web_search(const char *text);
int uart_control_read(char *buf, size_t max_len);

// Minta DevKit mengirim 'count' (1..12) pesan riwayat terakhir lewat UART (HISTREQ/HISTRES).
// days_ago < 0: pesan terakhir apa pun harinya; 0 = hanya hari ini; 1 = kemarin; dst (maks 60).
// Hasil berupa teks siap baca (satu pesan per baris, paling lama di atas) di 'out'.
// Mengembalikan panjang teks; 0 jika riwayat kosong, DevKit tidak menjawab, atau timeout.
size_t uart_control_request_history(int count, int days_ago, char *out, size_t out_size, uint32_t timeout_ms);

// Validate and execute one Gemini device command over UART.
bool uart_control_execute_command(const char *command);

// Execute a sensor command and wait for its one-line response.
bool uart_control_execute_sensor_command(const char *command, char *response, size_t response_len, uint32_t timeout_ms);

// Returns the latest successful sensor response and clears it from the shared slot.
const char *uart_control_take_last_sensor_response(void);

// ---- Perintah jarak jauh dari DevKit (web) ----
typedef enum { UART_REMOTE_NONE = 0, UART_REMOTE_WAKE, UART_REMOTE_SAY, UART_REMOTE_READ } uart_remote_type_t;
typedef struct {
    uart_remote_type_t type;
    uint32_t id;
    char text[320];
} uart_remote_cmd_t;

// Mulai penerima baris "RMT|..." (panggil sekali setelah uart_control_init).
void uart_control_start_remote(void);
// Ambil satu perintah yang menunggu. true jika ada.
bool uart_control_take_remote(uart_remote_cmd_t *out);
// Balas ke DevKit: RMT|ACK|<id>|<kode>.
void uart_control_remote_ack(uint32_t id, const char *code);

// Legacy text-action helper retained for compatibility.
bool uart_control_process_action_text(const char *text);

#ifdef __cplusplus
}
#endif
