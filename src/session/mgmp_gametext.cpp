// mgmp_gametext.cpp -- see mgmp_gametext.h.
#include "mgmp_gametext.h"

#include <windows.h>
#include <cstring>

#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"

namespace mgmp {
namespace {

// sub_1409616D0(Texts* this, std::wstring* out, const std::string* key, bool localize): `out` is a constructed (empty) std::wstring, the std::string is only read.
typedef void* (__fastcall* fn_texts_get)(void* texts, void* out, const void* key, bool localize);
typedef void  (__fastcall* fn_wstr_tidy)(void* ws);

// "Fighter" ... "Jester": kClassKeys' order (net/mgmp_catbrief.h), as the game's CAT_CLASS_<NAME>_NAME keys write them.
const char* const kClassTextKeys[14] = {
    "CAT_CLASS_FIGHTER_NAME", "CAT_CLASS_HUNTER_NAME", "CAT_CLASS_MAGE_NAME", "CAT_CLASS_MEDIC_NAME", "CAT_CLASS_TANK_NAME", "CAT_CLASS_THIEF_NAME", "CAT_CLASS_COLORLESS_NAME",
    "CAT_CLASS_MONK_NAME", "CAT_CLASS_BUTCHER_NAME", "CAT_CLASS_DRUID_NAME", "CAT_CLASS_TINKERER_NAME", "CAT_CLASS_NECROMANCER_NAME", "CAT_CLASS_PSYCHIC_NAME", "CAT_CLASS_JESTER_NAME",
};

// A std::wstring as UTF-8, without the zero-width characters (U+200B..U+200D, U+FEFF) the game's Chinese words carry between characters as break points -- the mod's font draws them as boxes.
bool wide_to_utf8(const void* ws, char* out, size_t cap) {
    out[0] = 0;
    uint64_t size = 0, wcap = 0;
    if (!mem_read((const uint8_t*)ws + 16, &size, 8) || !mem_read((const uint8_t*)ws + 24, &wcap, 8) || wcap < 7 || size > wcap) return false;
    if (!size || size > 400) return size == 0;
    const void* data = ws;
    if (wcap > 7 && (!mem_read(ws, &data, sizeof(data)) || !data)) return false;
    wchar_t chars[420];
    if (!mem_read(data, chars, (size_t)size * 2)) return false;
    size_t n = 0;
    for (size_t i = 0; i < size; ++i)
        if (!((chars[i] >= 0x200B && chars[i] <= 0x200D) || chars[i] == 0xFEFF)) chars[n++] = chars[i];
    if (!n) return false;
    const int w = WideCharToMultiByte(CP_UTF8, 0, chars, (int)n, out, (int)cap - 1, nullptr, nullptr);
    if (w <= 0) { out[0] = 0; return false; }
    out[w] = 0;
    return true;
}

struct Cache {
    char       text[14][64] = {};
    bool       have[14] = {};
    ULONGLONG  read_at = 0;
    bool       tried = false, said = false;
} g_cache;

} // namespace

bool gametext_lookup(const char* key, char* out, size_t cap) {
    if (!out || cap < 8) return false;
    out[0] = 0;
    const uintptr_t get = addr_of_call(C_TextsGet), tidy = addr_of_call(C_WStrTidy), texts = addr_of_data(D_TextsObject);
    if (!key || !get || !tidy || !texts) return false;
    const size_t n = strlen(key);
    if (!n || n > 200) return false;
    alignas(16) uint8_t k[32] = {};                                   // an MSVC std::string: 16 bytes inline or a pointer, size at +0x10, capacity at +0x18
    char heap[256];
    if (n <= 15) memcpy(k, key, n + 1);
    else { memcpy(heap, key, n + 1); const char* p = heap; memcpy(k, &p, sizeof(p)); }
    const uint64_t size = n, capacity = n <= 15 ? 15 : n;
    memcpy(k + 0x10, &size, 8); memcpy(k + 0x18, &capacity, 8);
    alignas(16) uint8_t ws[32] = {};
    ws[0x18] = 7;                                                      // an empty std::wstring (capacity 7, inline)
    bool ok = false;
    __try {
        ((fn_texts_get)get)((void*)texts, ws, k, true);
        ok = wide_to_utf8(ws, out, cap);
        ((fn_wstr_tidy)tidy)(ws);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; out[0] = 0; }
    if (ok && (!out[0] || strcmp(out, key) == 0 || out[0] == '[')) { out[0] = 0; return false; }      // no text for the key: the game answers with the key
    return ok;
}

const char* gametext_class_name(uint8_t klass) {
    if (klass >= 14) return nullptr;
    const ULONGLONG now = GetTickCount64();
    if (!g_cache.read_at || now - g_cache.read_at > 3000) {
        g_cache.read_at = now;
        unsigned got = 0;
        char buf[64];
        for (int i = 0; i < 14; ++i) {
            const bool ok = gametext_lookup(kClassTextKeys[i], buf, sizeof(buf));
            if (ok) { if (strcmp(buf, g_cache.text[i]) != 0) { strncpy_s(g_cache.text[i], sizeof(g_cache.text[i]), buf, _TRUNCATE); g_cache.said = false; } ++got; }
            g_cache.have[i] = ok;
        }
        if (!g_cache.said) {
            g_cache.said = true;
            if (got == 14) log_line("GAMETEXT", "class names from the game's own text: %s %s %s %s %s %s %s %s %s %s %s %s %s %s", g_cache.text[0], g_cache.text[1], g_cache.text[2], g_cache.text[3],
                                    g_cache.text[4], g_cache.text[5], g_cache.text[6], g_cache.text[7], g_cache.text[8], g_cache.text[9], g_cache.text[10], g_cache.text[11], g_cache.text[12], g_cache.text[13]);
            else if (!g_cache.tried) log_line_lvl(LogLevel::Warn, "GAMETEXT", "!! the game's text table answered for %u of 14 class names -- the mod's own words stand in for the rest", got);
            g_cache.tried = true;
        }
    }
    return g_cache.have[klass] ? g_cache.text[klass] : nullptr;
}

} // namespace mgmp
