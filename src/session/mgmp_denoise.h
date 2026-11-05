// mgmp_denoise.h -- THE MICROPHONE'S NOISE SUPPRESSOR (0.1.137; testers: "the voice is smooth now, but noisy").
//
// A spectral suppressor of the classic kind, on the 16 kHz mono capture before the gate and the encoder:
//   * short-time spectrum: 256-sample frames (16 ms), hop 128, sqrt-Hann analysis and synthesis windows (overlap-add reconstructs the input exactly at gain 1);
//   * a noise spectrum per bin, learned all the time: it follows the signal DOWN at once and creeps UP slowly (minimum tracking), and adapts faster while the
//     caller says nobody is talking (the gate's verdict);
//   * a Wiener gain from the decision-directed a-priori SNR (smooth, so no "musical" tones), floored at kFloor so the voice never sounds gated;
//   * everything under ~90 Hz (the first bins: hum, desk thumps) taken down to the floor.
// Latency: one hop (8 ms) plus the frame. Header-only and allocation-free, so the tests drive it without the devices.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mgmp {

class Denoiser {
public:
    static constexpr int kN = 256, kHop = 128, kBins = kN / 2 + 1;
    static constexpr float kFloor = 0.12f;          // -18 dB at most: deep enough to quiet a fan, shallow enough to keep the voice natural
    static constexpr float kAlpha = 0.96f;          // decision-directed smoothing

    Denoiser() { reset(); }

    void reset() {
        memset(in_, 0, sizeof(in_)); memset(out_, 0, sizeof(out_)); memset(ola_, 0, sizeof(ola_));
        in_n_ = 0; q_head_ = 0; q_count_ = kHop;       // one hop of silence primes the output queue: the output is the input kN samples (16 ms) late
        for (int k = 0; k < kBins; ++k) { noise_[k] = 0; smooth_[k] = 0; gain_prev_[k] = 1; post_prev_[k] = 1; }
        frames_ = 0;
        for (int i = 0; i < kN; ++i) win_[i] = (float)std::sqrt(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / kN));   // sqrt-Hann (periodic)
        gain_sum_ = 0; gain_frames_ = 0;
    }

    // Processes `n` samples in place (any n; the output is the input kN samples late). `quiet`: nobody talks (the gate is closed) -- the noise learns faster.
    void process(int16_t* pcm, int n, bool quiet) {
        for (int i = 0; i < n; ++i) {
            in_[in_n_++] = pcm[i];
            if (in_n_ == kHop) { frame(quiet); in_n_ = 0; }          // adds kHop samples to the queue before this one is taken
            pcm[i] = out_[q_head_];
            q_head_ = (q_head_ + 1) % kQ; --q_count_;
        }
    }

    bool enabled = true;
    // statistics for the log
    float noise_db() const { float s = 0; for (int k = 4; k < kBins; ++k) s += noise_[k]; return s > 0 ? 10.0f * std::log10(s / (kBins - 4)) : -99.0f; }
    float mean_gain_and_clear() { const float g = gain_frames_ ? (float)(gain_sum_ / gain_frames_) : 1.0f; gain_sum_ = 0; gain_frames_ = 0; return g; }

private:
    float win_[kN];
    float hist_[kN] = {};                  // the last kN input samples (the frame)
    int16_t in_[kHop];
    int in_n_ = 0;
    float ola_[kN];                        // overlap-add accumulator
    static constexpr int kQ = kHop * 2;
    int16_t out_[kQ] = {};
    int q_head_ = 0, q_count_ = 0;
    float noise_[kBins], smooth_[kBins], gain_prev_[kBins], post_prev_[kBins];
    uint64_t frames_ = 0;
    double gain_sum_ = 0; uint64_t gain_frames_ = 0;

    static void fft(float* re, float* im, int n, bool inverse) {
        for (int i = 1, j = 0; i < n; ++i) {                 // bit reversal
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const double ang = 2.0 * 3.14159265358979323846 / len * (inverse ? 1 : -1);
            const float wr = (float)std::cos(ang), wi = (float)std::sin(ang);
            for (int i = 0; i < n; i += len) {
                float cr = 1, ci = 0;
                for (int k = 0; k < len / 2; ++k) {
                    const int a = i + k, b = a + len / 2;
                    const float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                    const float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
                }
            }
        }
        if (inverse) for (int i = 0; i < n; ++i) { re[i] /= n; im[i] /= n; }
    }

    void frame(bool quiet) {
        memmove(hist_, hist_ + kHop, sizeof(float) * (kN - kHop));
        for (int i = 0; i < kHop; ++i) hist_[kN - kHop + i] = (float)in_[i];
        float re[kN], im[kN];
        for (int i = 0; i < kN; ++i) { re[i] = hist_[i] * win_[i]; im[i] = 0; }
        fft(re, im, kN, false);
        ++frames_;
        double gsum = 0;
        for (int k = 0; k < kBins; ++k) {
            const float p = re[k] * re[k] + im[k] * im[k];
            float g = 1.0f;
            if (enabled) {
                smooth_[k] = frames_ == 1 ? p : smooth_[k] * 0.7f + p * 0.3f;
                if (frames_ <= 12) noise_[k] = frames_ == 1 ? smooth_[k] : noise_[k] * 0.8f + smooth_[k] * 0.2f;   // the first ~100 ms: whatever is there is the room
                else if (smooth_[k] < noise_[k]) noise_[k] = smooth_[k];                                            // down at once
                else noise_[k] = noise_[k] * (quiet ? 0.95f : 0.9995f) + smooth_[k] * (quiet ? 0.05f : 0.0005f);  // up slowly (fast while nobody talks)
                const float n = noise_[k] > 1e-3f ? noise_[k] : 1e-3f;
                const float post = p / n;
                const float prior = kAlpha * gain_prev_[k] * gain_prev_[k] * post_prev_[k] + (1 - kAlpha) * (post - 1 > 0 ? post - 1 : 0);
                g = prior / (1 + prior);
                if (k < 2) g = 0;                                                 // under ~90 Hz: hum and thumps
                if (g < kFloor) g = kFloor;
                gain_prev_[k] = g; post_prev_[k] = post;
            }
            gsum += g;
            re[k] *= g; im[k] *= g;
            if (k > 0 && k < kN / 2) { re[kN - k] = re[k]; im[kN - k] = -im[k]; }   // keep the spectrum Hermitian
        }
        gain_sum_ += gsum / kBins; ++gain_frames_;
        fft(re, im, kN, true);
        for (int i = 0; i < kN; ++i) ola_[i] += re[i] * win_[i];
        // the first hop of the accumulator is complete: out
        for (int i = 0; i < kHop; ++i) {
            float v = ola_[i];
            v = v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v;
            out_[(q_head_ + q_count_ + i) % kQ] = (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
        }
        q_count_ += kHop;
        memmove(ola_, ola_ + kHop, sizeof(float) * (kN - kHop));
        memset(ola_ + kN - kHop, 0, sizeof(float) * kHop);
    }
};

} // namespace mgmp
