#pragma once
// mgmp_catblob -- read the room-panel facts out of a cat image, i.e. the bytes
// glaiel::SerializeCatData writes (mgmp_catsync's serialize_cat). Pure: no game
// calls, so it is unit-tested against layouts measured from real saves.
//
// THE LAYOUT is the community's ImHex pattern (research/external/analysis/cat.hexpat,
// "version 19"), which this build's saves match: measured 2026-09-30 by decoding the
// `cats` table of a real save (LZ4 blob, u32 length in front) -- all 70-odd cats came
// out with a sane name, class, level, and exactly 111 bytes left over for the
// zone/injury tail that this file does not read. Nothing here is guessed; what is not
// needed is SKIPPED by its measured width:
//
//   u32 version(=19)   u64 entropy   wstring name(u64 char count + UTF-16)
//   string nameplate   u32 sex, u32 sex      u8[8] flags
//   string unknown     u32 unknown           f64 x8  (libido .. fertility)
//   i32 texture, i32 fur palette, i32 collar palette,
//   14 body-part descriptors of 5 x i32, 2 x i32, string voice, f64 pitch
//   CatStats base(7 x i32), CatStats from levelling, CatStats from injuries
//   string last-injury stat
//   i32 hp   u8 dead   u8 ?   i32 ?   { u32 n; n x {string, u32} } stat modifiers
//   string x 2 basic, x 4 accessible, x 4 inherited actives
//   { string, i32 } x 2 passives, x 2 mutations
//   5 x equipment { u32 version; u8 present; if present { string, string, 4 x i32, 2 x u8 } }
//   string collar (= the class)   i32 level
//
// A string is u64 byte length + bytes. Every read is bounds-checked and a failure
// leaves `out` untouched: an image that stops making sense is skipped, never guessed.

#include <cstdint>
#include <cstring>

#include "mgmp_catbrief.h"

namespace mgmp {
namespace catblob {

struct Cur {
    const uint8_t* p;
    uint32_t       n;
    uint32_t       pos = 0;
    bool           ok  = true;

    Cur(const uint8_t* b, uint32_t len) : p(b), n(len) {}

    bool take(uint32_t k) {
        if (!ok || k > n - pos) { ok = false; return false; }
        return true;
    }
    void skip(uint32_t k) { if (take(k)) pos += k; }
    uint8_t u8()  { uint8_t v = 0;  if (take(1)) { v = p[pos]; pos += 1; } return v; }
    uint32_t u32() { uint32_t v = 0; if (take(4)) { memcpy(&v, p + pos, 4); pos += 4; } return v; }
    int32_t  i32() { return (int32_t)u32(); }
    uint64_t u64() { uint64_t v = 0; if (take(8)) { memcpy(&v, p + pos, 8); pos += 8; } return v; }

    // u64 length + bytes. Copies at most cap-1 bytes when `out` is given.
    void str(char* out = nullptr, uint32_t cap = 0) {
        const uint64_t len = u64();
        if (!ok || len > 0xFFFF || !take((uint32_t)len)) { ok = false; return; }
        if (out && cap) {
            const uint32_t c = len < cap - 1 ? (uint32_t)len : cap - 1;
            memcpy(out, p + pos, c);
            out[c] = 0;
        }
        pos += (uint32_t)len;
    }

    // u64 UTF-16 code-unit count + units -> UTF-8, truncated at a character boundary.
    void wstr_utf8(char* out, uint32_t cap) {
        const uint64_t units = u64();
        if (!ok || units > 0x1000 || !take((uint32_t)units * 2)) { ok = false; return; }
        uint32_t o = 0;
        auto put = [&](uint32_t cp) {
            uint8_t b[4]; uint32_t k;
            if (cp < 0x80)         { b[0] = (uint8_t)cp; k = 1; }
            else if (cp < 0x800)   { b[0] = (uint8_t)(0xC0 | cp >> 6); b[1] = (uint8_t)(0x80 | (cp & 63)); k = 2; }
            else if (cp < 0x10000) { b[0] = (uint8_t)(0xE0 | cp >> 12); b[1] = (uint8_t)(0x80 | ((cp >> 6) & 63));
                                     b[2] = (uint8_t)(0x80 | (cp & 63)); k = 3; }
            else                   { b[0] = (uint8_t)(0xF0 | cp >> 18); b[1] = (uint8_t)(0x80 | ((cp >> 12) & 63));
                                     b[2] = (uint8_t)(0x80 | ((cp >> 6) & 63)); b[3] = (uint8_t)(0x80 | (cp & 63)); k = 4; }
            if (o + k >= cap) return false;
            memcpy(out + o, b, k); o += k;
            return true;
        };
        for (uint32_t i = 0; i < units; ++i) {
            uint16_t u; memcpy(&u, p + pos + i * 2, 2);
            uint32_t cp = u;
            if (u >= 0xD800 && u < 0xDC00 && i + 1 < units) {
                uint16_t v; memcpy(&v, p + pos + (i + 1) * 2, 2);
                if (v >= 0xDC00 && v < 0xE000) { cp = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00); ++i; }
            }
            if (!put(cp)) break;
        }
        out[o] = 0;
        pos += (uint32_t)units * 2;
    }
};

} // namespace catblob

// Health has no stored maximum: it is four per point of CON (the image's own, its
// levelling and its injuries, plus the class's stat_mods) and then whatever items and
// passives add, which this file cannot see. So `maxhp` is a FLOOR, and a caller that
// shows a bar must not let the current value exceed it (measured: a Tank at 48 hp where
// the formula says 44).
inline bool catblob_parse(const uint8_t* img, uint32_t len, CatBrief& out) {
    if (!img || len < 64) return false;
    catblob::Cur c(img, len);
    CatBrief b;

    if (c.u32() != 19) return false;              // the layout above is version 19's
    c.u64();                                       // entropy
    c.wstr_utf8(b.name, sizeof(b.name));
    c.str();                                       // nameplate
    c.skip(8 + 8);                                 // sex x2, flags
    c.str(); c.u32();
    c.skip(8 * 8);                                 // libido .. fertility
    b.tex  = c.i32();
    b.fur  = c.i32();
    b.coat = c.i32();
    c.skip(14 * 5 * 4 + 2 * 4);                    // body parts + two unknown words
    c.str(); c.skip(8);                            // voice, pitch
    int32_t stats[3][7] = {};
    for (auto& block : stats) for (int32_t& s : block) s = c.i32();
    c.str();                                       // last injury's stat
    b.hp = c.i32();
    const bool dead = c.u8() != 0;
    c.skip(1 + 4);
    const uint32_t mods = c.u32();
    if (mods > 256) return false;
    for (uint32_t i = 0; i < mods && c.ok; ++i) { c.str(); c.u32(); }
    for (int i = 0; i < 2 + 4 + 4 && c.ok; ++i) c.str();
    for (int i = 0; i < 4 && c.ok; ++i) { c.str(); c.i32(); }
    for (int i = 0; i < 5 && c.ok; ++i) {
        c.u32();
        if (c.u8()) { c.str(); c.str(); c.skip(4 * 4 + 2); }
    }
    char collar[24] = {};
    c.str(collar, sizeof(collar));
    b.level = c.i32();
    if (!c.ok) return false;

    if (dead) b.hp = 0;
    b.klass = brief_class_from_key(collar);
    int con = stats[0][2] + stats[1][2] + stats[2][2];
    if (b.klass < kClassCount) con += kClassConMod[b.klass];
    b.maxhp = con > 0 ? con * 4 : 0;
    out = b;
    return true;
}

} // namespace mgmp
