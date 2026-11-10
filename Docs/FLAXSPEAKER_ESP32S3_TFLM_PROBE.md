# FlaxSpeaker ESP32-S3 TFLite Micro Probe

## Tujuan

Probe ini memeriksa apakah model FlaxSpeaker dapat dimuat dan menjalankan satu inferensi pada ESP32-S3 dengan TFLite Micro. Workflow GitHub Actions mengaudit artefak model yang dipatok versinya, menyiapkan model INT8, lalu membangun aplikasi probe.

## Kontrak tensor

Firmware probe mengharapkan input `float32[1,180,80]` dan output `float32[1,192]`. Jika model tidak cocok, jangan mengubah pemeriksaan agar lolos; audit dulu kontrak model dan operatornya.

## Menjalankan

Buka tab **Actions** di GitHub, pilih **FlaxSpeaker ESP32-S3 TFLM Probe**, lalu pilih **Run workflow**. Build artifact dan manifest audit akan dilampirkan pada run jika berhasil dibuat.

## Batas pengujian

Tes ini memakai input nol. Lulus hanya berarti model berhasil dimuat, tensor dialokasikan, dan satu pemanggilan inferensi berhasil. Ini **belum** membuktikan akurasi identifikasi pembicara, kesetaraan frontend audio, atau performa dengan audio nyata. Jangan flash image probe ke robot utama; aplikasi ini terpisah dari firmware produksi.
