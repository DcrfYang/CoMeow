// test_denoise.cpp -- the microphone's noise suppressor (mgmp_denoise.h, 0.1.137).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "mgmp_denoise.h"

using namespace mgmp;
static int g_checks = 0, g_fail = 0;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static uint32_t g_rng = 12345;
static float noise() { g_rng = g_rng * 1664525u + 1013904223u; return ((g_rng >> 8) / 16777216.0f) * 2.0f - 1.0f; }

static double power(const std::vector<int16_t>& v, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b; ++i) s += (double)v[i] * v[i]; return s / (double)(b - a); }
// the amplitude of a 1 kHz sine in [a, b) (projection onto sin and cos)
static double tone_amp(const std::vector<int16_t>& v, size_t a, size_t b, double f) {
    double s = 0, c = 0;
    for (size_t i = a; i < b; ++i) { const double ph = 2 * 3.14159265358979 * f * i / 16000.0; s += v[i] * std::sin(ph); c += v[i] * std::cos(ph); }
    return 2 * std::sqrt(s * s + c * c) / (double)(b - a);
}

int main() {
    // 1. switched off: the input comes out exactly, kN samples late (the windows reconstruct it)
    {
        Denoiser d; d.enabled = false;
        std::vector<int16_t> in(16000), out;
        for (size_t i = 0; i < in.size(); ++i) in[i] = (int16_t)(3000 * std::sin(i * 0.07) + 500 * noise());
        out = in;
        for (size_t i = 0; i < out.size(); i += 320) d.process(&out[i], 320, false);
        int worst = 0;
        for (size_t i = Denoiser::kN; i < out.size(); ++i) { const int e = std::abs(out[i] - in[i - Denoiser::kN]); if (e > worst) worst = e; }
        std::printf("off: worst sample error %d\n", worst);
        CHECK(worst <= 2);
    }
    // 2. a fan (white noise, 2 s alone, then under a 1 kHz tone for 2 s, then alone again): the noise goes down, the tone stays
    {
        Denoiser d;
        const size_t n = 16000 * 6;
        std::vector<int16_t> in(n), out;
        for (size_t i = 0; i < n; ++i) {
            double v = 400 * noise();
            if (i >= 32000 && i < 64000) v += 3000 * std::sin(2 * 3.14159265358979 * 1000.0 * i / 16000.0);
            in[i] = (int16_t)v;
        }
        out = in;
        for (size_t i = 0; i < n; i += 320) d.process(&out[i], 320, i < 32000 || i >= 64000);
        const size_t L = Denoiser::kN;
        const double pin = power(in, 16000, 32000), pout = power(out, 16000 + L, 32000 + L);
        const double red = 10 * std::log10(pin / (pout > 1 ? pout : 1));
        const double ain = tone_amp(in, 40000, 60000, 1000), aout = tone_amp(out, 40000 + L, 60000 + L, 1000);
        const double keep = 20 * std::log10(aout / ain);
        const double pin2 = power(in, 80000, 96000), pout2 = power(out, 80000 + L, 96000 - 1);
        const double red2 = 10 * std::log10(pin2 / (pout2 > 1 ? pout2 : 1));
        std::printf("fan: noise down %.1f dB before the voice, %.1f dB after it; the 1 kHz tone %.2f dB; noise floor %.1f dB, mean gain %.2f\n", red, red2, keep, d.noise_db(), d.mean_gain_and_clear());
        CHECK(red >= 12.0);
        CHECK(red2 >= 12.0);
        CHECK(keep > -2.0 && keep < 1.0);
    }
    // 3. hum under 90 Hz is taken down
    {
        Denoiser d;
        std::vector<int16_t> in(32000), out;
        for (size_t i = 0; i < in.size(); ++i) in[i] = (int16_t)(4000 * std::sin(2 * 3.14159265358979 * 50.0 * i / 16000.0));
        out = in;
        for (size_t i = 0; i < in.size(); i += 320) d.process(&out[i], 320, false);
        const double down = 20 * std::log10(tone_amp(out, 16000, 32000, 50) / tone_amp(in, 16000, 32000, 50));
        std::printf("hum 50 Hz: %.1f dB\n", down);
        CHECK(down < -12.0);
    }
    std::printf("test_denoise: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
