# Speaker ID — MFCC + DTW

Komponen identifikasi pembicara lokal untuk ESP32-S3. Menggunakan MFCC untuk ekstraksi fitur suara dan Dynamic Time Warping (DTW) untuk membandingkan suara dengan template yang disimpan di NVS.

Input: PCM 16-bit mono, 16 kHz. Enroll template melalui API komponen, lalu bandingkan audio berikutnya dengan template yang tersimpan.

Lihat deklarasi API di `include/speaker_id.h`.
