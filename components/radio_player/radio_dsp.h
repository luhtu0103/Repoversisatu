#pragma once
// Pengubah kecepatan suara (resampler) mono ke 24000 Hz untuk pemutar radio.
// Hanya fungsi hitung biasa, tanpa ESP-IDF, jadi bisa diuji di komputer.
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#define RADIO_OUT_HZ 24000U

struct RadioResampler {
    uint32_t in_hz = 0;
    double pos = 0.0;      // posisi sampel keluaran berikutnya, relatif terhadap awal blok masuk
    double step = 1.0;     // in_hz / 24000
    bool smooth = false;   // filter kecil saat kecepatan diturunkan (kurangi desis/aliasing)
    static constexpr int TAPS = 9;      // filter penurun kecepatan 9 titik (sinc berjendela Hann)
    float h[TAPS] = {};
    int32_t hist[TAPS] = {};            // sampel masukan terakhir (hist[0] = terbaru)
    int32_t y_prev = 0;      // sampel tersaring sebelumnya

    void reset(uint32_t hz)
    {
        in_hz = hz;
        step = (double)hz / (double)RADIO_OUT_HZ;
        smooth = step > 1.05;
        pos = 0.0;
        for (int i = 0; i < TAPS; ++i) hist[i] = 0;
        y_prev = 0;
        if (smooth) {
            // Potong di ~0,45 x kecepatan keluaran supaya nada tinggi yang tak muat tidak berubah jadi desis.
            const double fc = 0.45 * (double)RADIO_OUT_HZ / (double)hz;   // 0..0,5 (relatif kecepatan masuk)
            const double mid = (TAPS - 1) / 2.0;
            double sum = 0.0;
            for (int i = 0; i < TAPS; ++i) {
                const double t = (double)i - mid;
                const double sinc = (t == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * t) / (M_PI * t);
                const double win = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(TAPS - 1));
                const double c = sinc * win;
                h[i] = (float)c;
                sum += c;
            }
            for (int i = 0; i < TAPS; ++i) h[i] = (float)((double)h[i] / sum);   // penguatan DC = 1
        }
    }

    // in: n sampel mono 16-bit. out: kapasitas cap. Mengembalikan jumlah sampel keluaran.
    size_t process(const int16_t *in, size_t n, int16_t *out, size_t cap)
    {
        size_t o = 0;
        for (size_t k = 0; k < n && o < cap; ++k) {
            const int32_t x0 = in[k];
            for (int i = TAPS - 1; i > 0; --i) hist[i] = hist[i - 1];
            hist[0] = x0;
            int32_t y = x0;
            if (smooth) {
                float acc = 0.0f;
                for (int i = 0; i < TAPS; ++i) acc += h[i] * (float)hist[i];
                y = (int32_t)acc;
                if (y > 32767) y = 32767;
                if (y < -32768) y = -32768;
            }
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

// Pembatas halus: di bawah 24000 suara tidak diubah; di atasnya puncak dilunakkan perlahan menuju 32767
// (bukan dipotong rata, yang terdengar pecah). Masukan boleh melebihi 16-bit (hasil penguatan).
static inline int16_t radio_soft_limit(int32_t v)
{
    constexpr int32_t T = 24000, R = 32767 - T;
    const int32_t a = v < 0 ? -v : v;
    if (a <= T) return (int16_t)v;
    int32_t over = a - T;
    if (over > 200000) over = 200000;
    const int32_t o = T + (int32_t)(((int64_t)over * R) / ((int64_t)over + R));
    return (int16_t)(v < 0 ? -o : o);
}
