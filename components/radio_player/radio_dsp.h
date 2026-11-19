#pragma once
// Pengubah kecepatan suara (resampler) mono ke 24000 Hz untuk pemutar radio.
// Hanya fungsi hitung biasa, tanpa ESP-IDF, jadi bisa diuji di komputer.
#include <stddef.h>
#include <stdint.h>

#define RADIO_OUT_HZ 24000U

struct RadioResampler {
    uint32_t in_hz = 0;
    double pos = 0.0;      // posisi sampel keluaran berikutnya, relatif terhadap awal blok masuk
    double step = 1.0;     // in_hz / 24000
    bool smooth = false;   // filter kecil saat kecepatan diturunkan (kurangi desis/aliasing)
    int32_t x1 = 0, x2 = 0;  // dua sampel masukan sebelumnya
    int32_t y_prev = 0;      // sampel tersaring sebelumnya

    void reset(uint32_t hz)
    {
        in_hz = hz;
        step = (double)hz / (double)RADIO_OUT_HZ;
        smooth = step > 1.2;
        pos = 0.0;
        x1 = x2 = 0;
        y_prev = 0;
    }

    // in: n sampel mono 16-bit. out: kapasitas cap. Mengembalikan jumlah sampel keluaran.
    size_t process(const int16_t *in, size_t n, int16_t *out, size_t cap)
    {
        size_t o = 0;
        for (size_t k = 0; k < n && o < cap; ++k) {
            const int32_t x0 = in[k];
            const int32_t y = smooth ? (x0 + 2 * x1 + x2) / 4 : x0;
            x2 = x1;
            x1 = x0;
            while (pos <= (double)k && o < cap) {
                const double frac = pos - ((double)k - 1.0);   // 0..1 antara y_prev dan y
                double v = (double)y_prev + ((double)y - (double)y_prev) * frac;
                if (v > 32767.0) v = 32767.0;
                if (v < -32768.0) v = -32768.0;
                out[o++] = (int16_t)v;
                pos += step;
            }
            y_prev = y;
        }
        pos -= (double)n;
        return o;
    }
};
