// mgmp_emote.cpp -- see mgmp_emote.h.
#include "mgmp_emote.h"

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

const char* const kNames[kEmoteCount] = {
    "Up", "Down", "Left", "Right", "Happy", "Sad", "ThumbsUp", "ThumbsDown", "Heart", "Bomb", "Coin", "Key",
    "TintedRock", "Reset", "Angel", "Devil", "Poop", "RaiseHand", "QuestionMark", "D6", "D20",
};

// The atlas (scratch/make_emote_atlas.py): 0..18 the pictures, 19 the D6, 20..25 its faces 1..6, 26..35 its roll,
// 36..41 the D20 (its first frame is the icon, the rest start a roll), 42..61 its faces 1..20, 62..71 its roll.
constexpr int kD6Icon = 19, kD6Face = 20, kD6Roll = 26, kD20Start = 36, kD20Face = 42, kD20Roll = 62;
constexpr unsigned kRollFrameMs = 70, kD20StartFrameMs = 34;

int faces(int pick) { return pick == kEmoteD6 ? 6 : pick == kEmoteD20 ? 20 : 0; }

} // namespace

const char* emote_name(int pick) { return pick >= 0 && pick < kEmoteCount ? kNames[pick] : nullptr; }

bool emote_token(const Emote& e, char* out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    const char* n = emote_name(e.pick);
    if (!n) return false;
    const int f = faces(e.pick);
    if (f ? (e.roll < 1 || e.roll > f) : e.roll != 0) return false;
    const int w = f ? _snprintf_s(out, cap, _TRUNCATE, "[:%s=%d:]", n, e.roll) : _snprintf_s(out, cap, _TRUNCATE, "[:%s:]", n);
    if (w <= 0) { out[0] = 0; return false; }
    return true;
}

bool emote_parse(const char* text, Emote& out) {
    out = Emote{};
    if (!text) return false;
    const size_t len = strlen(text);
    if (len < 5 || len > 24 || strncmp(text, "[:", 2) != 0 || strcmp(text + len - 2, ":]") != 0) return false;
    char body[32] = {};
    memcpy(body, text + 2, len - 4);
    char* eq = strchr(body, '=');
    int roll = 0;
    if (eq) {
        *eq = 0;
        const char* d = eq + 1;
        if (!*d || strlen(d) > 2) return false;
        for (const char* c = d; *c; ++c) { if (*c < '0' || *c > '9') return false; roll = roll * 10 + (*c - '0'); }
        if (d[0] == '0') return false;                       // "07": not how the token is written
    }
    for (int i = 0; i < kEmoteCount; ++i) {
        if (strcmp(body, kNames[i]) != 0) continue;
        const int f = faces(i);
        if (f ? (!eq || roll < 1 || roll > f) : eq != nullptr) return false;
        out.pick = i; out.roll = roll;
        return true;
    }
    return false;
}

int emote_cell(const Emote& e, uint64_t age_ms) {
    if (e.pick < 0 || e.pick >= kEmoteCount) return -1;
    if (e.pick == kEmoteD6) {
        if (e.roll < 1 || e.roll > 6) return kD6Icon;
        if (age_ms >= kEmoteRollMs) return kD6Face + e.roll - 1;
        return kD6Roll + (int)((age_ms / kRollFrameMs) % 10);
    }
    if (e.pick == kEmoteD20) {
        if (e.roll < 1 || e.roll > 20) return kD20Start;
        if (age_ms >= kEmoteRollMs) return kD20Face + e.roll - 1;
        if (age_ms < 6 * kD20StartFrameMs) return kD20Start + (int)(age_ms / kD20StartFrameMs);
        return kD20Roll + (int)(((age_ms - 6 * kD20StartFrameMs) / kRollFrameMs) % 10);
    }
    return e.pick;
}

int emote_icon_cell(int pick) {
    if (pick == kEmoteD6) return kD6Icon;
    if (pick == kEmoteD20) return kD20Start;
    return pick >= 0 && pick < kEmoteCount ? pick : -1;
}

} // namespace mgmp
