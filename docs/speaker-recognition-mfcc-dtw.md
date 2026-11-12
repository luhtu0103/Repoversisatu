# Desain Pengenalan Pembicara MFCC + DTW

Status: implementasi awal sudah ditambahkan di branch `copy-repo5`; masih perlu validasi build CI dan uji perangkat nyata. Ambang DTW belum dikalibrasi dengan dataset perangkat.

## Hasil inspeksi branch `copy-repo5`

- `main/main.cpp` memulai AFE, lalu `audio_task` membaca audio saat sesi Gemini aktif.
- `audio_read_mic_raw()` memasok PCM16 mentah 16 kHz mono untuk WakeNet.
- `audio_read_mic()` memproses frame mikrofon melalui AFE bila siap, kemudian mengembalikan PCM16 yang sudah diproses untuk dikirim ke Gemini.
- Gemini menerima frame PCM melalui `websocket_send_audio_data()` pada jalur WebSocket yang sudah ada.
- AFE menggunakan NS, AGC, dan VAD; AEC sengaja nonaktif.
- Belum ditemukan komponen MFCC/DTW atau penyimpanan profil pembicara yang terintegrasi.

## Arsitektur yang dituju

```text
INMP441 / I2S
  ├── PCM mentah ───────────────> WakeNet (tetap seperti sekarang)
  └── capture PCM ──> AFE ──────> buffer audio Gemini ──> WebSocket
                       └────────> tap PCM AFE ke speaker_id
                                  -> segmentasi energi
                                  -> MFCC
                                  -> DTW ke template NVS
                                  -> hasil speaker_identify tool
                                  -> konteks identitas untuk Gemini
```

Implementasi saat ini menyalin/memproses PCM keluaran AFE langsung pada jalur `audio_read_mic()` tanpa mengambil frame dari antrean audio lain. Ini menghindari persaingan konsumen dengan Gemini/WakeNet, tetapi menambah beban CPU di jalur audio; ukur latency dan underrun di perangkat nyata.

## Penyimpanan di flash

Implementasi awal memakai namespace NVS `speaker_id` dan blob profil dengan magic/version marker, nama maksimum 23 byte, dan hingga 160 frame MFCC. Batas saat ini 4 profil. Penghapusan profil belum dibuat.

Inisialisasi NVS yang sudah ada di `app_main()` harus tetap menjadi prasyarat. Jangan menyimpan audio mentah kecuali benar-benar diperlukan; utamakan fitur template dan sediakan cara menghapus pendaftaran.

## Pipeline MFCC + DTW yang diterapkan (awal)

- Input: PCM16 mono 16 kHz dari keluaran AFE.
- Segmentasi: energi/VAD dengan ambang dan durasi minimum; abaikan segmen terlalu pendek atau terlalu senyap.
- MFCC: pre-emphasis, window Hamming, FFT, filter-bank mel, log-energy, DCT; mulai dengan 13 koefisien dan frame 25 ms / hop 10 ms.
- DTW: jarak fitur antar-frame dengan batas warping agar waktu komputasi terkontrol.
- Keputusan: bandingkan skor terbaik dengan ambang yang dikalibrasi; jika tidak lolos atau margin antarprofil terlalu kecil, hasilnya `unknown`.
- Keluaran: API `speaker_id_enroll_last()` dan `speaker_id_identify()`; tool Gemini `speaker_enroll` dan `speaker_identify` mengembalikan hasil sebenarnya untuk konfirmasi/penyapaan. Pendaftaran hanya diizinkan jika transkrip memuat perintah eksplisit seperti “daftarkan suara”.

## Batasan penting

MFCC + DTW membandingkan rangkaian fitur akustik, tetapi bukan metode identifikasi pembicara yang kuat untuk kalimat bebas. DTW sangat dipengaruhi isi fonetik; template satu kalimat tidak menjamin identifikasi akurat saat orang mengucapkan kalimat berbeda. Untuk prototipe MFCC + DTW, daftarkan beberapa contoh dan gunakan frasa pendaftaran yang konsisten saat evaluasi. Jika syarat produk adalah identifikasi orang pada kalimat apa pun, ukur akurasinya secara nyata dan pertimbangkan speaker embedding sebagai peningkatan berikutnya.

## Batas perubahan

- Jangan ubah `main/main.cpp`.
- Jangan ubah algoritme atau pemicu WakeNet.
- Jangan ubah format maupun pengiriman PCM Gemini.
- Integrasi harus opt-in dan gagal secara aman: jika NVS/profil/AFE tidak tersedia, Gemini dan WakeNet tetap bekerja seperti semula.
- Sebelum mengklaim selesai, pastikan build firmware CI berhasil dan uji enrollment, restart/persistensi NVS, pembicara dikenal, pembicara tak dikenal, serta beban audio Gemini.
