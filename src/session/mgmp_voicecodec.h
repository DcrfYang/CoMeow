// mgmp_voicecodec.h -- IMA ADPCM, 4 bits a sample (the voice chat's codec, mgmp_voice). Header only, no device and no network: the tests run it as it is.
//
// A frame is self-contained: it carries the predictor (the first sample, exact) and the step index the encoder had, so a lost or late frame costs nothing to the next one.
#pragma once

#include <cstdint>

namespace mgmp {

struct VoiceCodec { int pred = 0; int index = 0; };

namespace voice_detail {
constexpr int kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
constexpr int kIndexAdjust[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
inline int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
inline int clamp_index(int v) { return v < 0 ? 0 : v > 88 ? 88 : v; }
inline int decode_nibble(int nib, int& pred, int& index) {
    const int step = kStep[index];
    int diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    if (nib & 8) pred -= diff; else pred += diff;
    pred = clamp16(pred);
    index = clamp_index(index + kIndexAdjust[nib & 7]);
    return pred;
}
} // namespace voice_detail

// n samples (n >= 1) -> out; returns the bytes written ((n - 1 + 1) / 2). pcm[0] is NOT coded: the decoder starts from it (`st.pred` is set to it), the others follow.
inline uint32_t voice_adpcm_encode(const int16_t* pcm, uint32_t n, VoiceCodec& st, uint8_t* out) {
    using namespace voice_detail;
    if (!n) return 0;
    st.pred = pcm[0];
    st.index = clamp_index(st.index);
    int pred = st.pred, index = st.index;
    uint32_t bytes = 0;
    for (uint32_t i = 1; i < n; ++i) {
        const int step = kStep[index];
        int diff = pcm[i] - pred;
        int nib = 0;
        if (diff < 0) { nib = 8; diff = -diff; }
        int d = step >> 3;
        if (diff >= step) { nib |= 4; diff -= step; d += step; }
        if (diff >= (step >> 1)) { nib |= 2; diff -= step >> 1; d += step >> 1; }
        if (diff >= (step >> 2)) { nib |= 1; d += step >> 2; }
        if (nib & 8) pred -= d; else pred += d;
        pred = clamp16(pred);
        index = clamp_index(index + kIndexAdjust[nib & 7]);
        if (i & 1) out[(i - 1) >> 1] = (uint8_t)nib;               // low nibble first
        else       out[(i - 1) >> 1] |= (uint8_t)(nib << 4);
        bytes = ((i - 1) >> 1) + 1;
    }
    st.index = index;                                              // the step index carries into the next frame (its header says where it started)
    return bytes;
}

// The frame's `len` bytes -> n samples; pcm[0] = pred. False when `len` is too short for n samples.
inline bool voice_adpcm_decode(const uint8_t* data, uint32_t len, int pred, int index, uint32_t n, int16_t* pcm) {
    using namespace voice_detail;
    if (!n || (n - 1 + 1) / 2 > len) return false;
    pred = clamp16(pred); index = clamp_index(index);
    pcm[0] = (int16_t)pred;
    for (uint32_t i = 1; i < n; ++i) {
        const uint8_t b = data[(i - 1) >> 1];
        const int nib = (i & 1) ? (b & 0x0F) : (b >> 4);
        pcm[i] = (int16_t)decode_nibble(nib, pred, index);
    }
    return true;
}

} // namespace mgmp
