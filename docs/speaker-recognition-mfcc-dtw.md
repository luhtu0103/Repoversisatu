# Desain Pengenalan Pembicara MFCC + DTW

Status: rancangan integrasi; belum mengaktifkan pengenalan pembicara pada firmware.

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
                       └────────> salinan frame -> queue terbatas
                                             -> task speaker_id
                                             -> VAD/segmentasi suara
                                             -> MFCC
                                             -> DTW ke template terdaftar
                                             -> identitas / unknown
                                             -> konteks identitas untuk Gemini
```

Pengenal pembicara harus menerima salinan frame, bukan mengambil buffer yang sama untuk dimiliki sendiri. Pemrosesan MFCC/DTW berjalan di task terpisah dengan antrean terbatas agar tidak menahan pengiriman audio Gemini. Jika antrean penuh, frame pengenal dibuang dan dihitung; jalur Gemini tidak boleh terhambat.

## Penyimpanan di flash

Gunakan NVS, bukan menulis langsung ke alamat flash. Simpan profil sebagai blob berversi dengan:
- ID profil dan nama yang dibatasi panjangnya;
- parameter MFCC/model yang digunakan;
- beberapa template fitur per pembicara;
- checksum atau validasi panjang dan versi;
- batas jumlah profil serta fungsi hapus profil.

Inisialisasi NVS yang sudah ada di `app_main()` harus tetap menjadi prasyarat. Jangan menyimpan audio mentah kecuali benar-benar diperlukan; utamakan fitur template dan sediakan cara menghapus pendaftaran.

## Pipeline MFCC + DTW yang diusulkan

- Input: PCM16 mono 16 kHz dari keluaran AFE.
- Segmentasi: energi/VAD dengan ambang dan durasi minimum; abaikan segmen terlalu pendek atau terlalu senyap.
- MFCC: pre-emphasis, window Hamming, FFT, filter-bank mel, log-energy, DCT; mulai dengan 13 koefisien dan frame 25 ms / hop 10 ms.
- DTW: jarak fitur antar-frame dengan batas warping agar waktu komputasi terkontrol.
- Keputusan: bandingkan skor terbaik dengan ambang yang dikalibrasi; jika tidak lolos atau margin antarprofil terlalu kecil, hasilnya `unknown`.
- Keluaran: API lokal untuk status, nama, skor, serta API pendaftaran dan penghapusan profil.

## Batasan penting

MFCC + DTW membandingkan rangkaian fitur akustik, tetapi bukan metode identifikasi pembicara yang kuat untuk kalimat bebas. DTW sangat dipengaruhi isi fonetik; template satu kalimat tidak menjamin identifikasi akurat saat orang mengucapkan kalimat berbeda. Untuk prototipe MFCC + DTW, daftarkan beberapa contoh dan gunakan frasa pendaftaran yang konsisten saat evaluasi. Jika syarat produk adalah identifikasi orang pada kalimat apa pun, ukur akurasinya secara nyata dan pertimbangkan speaker embedding sebagai peningkatan berikutnya.

## Batas perubahan

- Jangan ubah `main/main.cpp`.
- Jangan ubah algoritme atau pemicu WakeNet.
- Jangan ubah format maupun pengiriman PCM Gemini.
- Integrasi harus opt-in dan gagal secara aman: jika NVS/profil/AFE tidak tersedia, Gemini dan WakeNet tetap bekerja seperti semula.
- Sebelum mengklaim selesai, build firmware di CI dan uji enrollment, restart/persistensi NVS, pembicara dikenal, pembicara tak dikenal, dan beban audio Gemini.
