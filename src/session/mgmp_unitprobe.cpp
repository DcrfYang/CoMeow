#include "mgmp_unitprobe.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_ability.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

constexpr uintptr_t kChar_Entity   = 0x18;
constexpr uintptr_t kEnt_CompCount = 0x24;   // u32
constexpr uintptr_t kEnt_CompData  = 0x28;   // Component**
constexpr uintptr_t kComp_Enabled  = 0x0E, kComp_Destroyed = 0x0F, kComp_Alive = 0x10;
constexpr uint32_t  kMaxComps      = 512;

uintptr_t image_base() { static uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr); return b; }

// vtable slot 1, GetObjectType: a getter (returns the class's type id). SEH: a function of its own.
int32_t call_type_id(const void* comp) {
    void* const* vt = nullptr;
    if (!mem_read(comp, &vt, 8) || !vt) return -1;
    void* fn = nullptr;
    if (!mem_read(vt + 1, &fn, 8) || !fn) return -1;
    __try { return ((int32_t(__fastcall*)(const void*))fn)(comp); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void dump_ints(const char* what, const void* obj, uint32_t from, uint32_t to) {
    char line[1400]; int w = 0;
    line[0] = 0;
    for (uint32_t o = from; o < to && w < (int)sizeof(line) - 40; o += 4) {
        int32_t v = 0;
        if (!mem_read((const uint8_t*)obj + o, &v, 4)) break;
        if (v == 0) continue;
        w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " %X=%d", o, v);
    }
    log_line("UNITPROBE", "      %s ints:%s", what, line);
}

// std::strings among the first bytes of an object (MSVC layout: 16 bytes or a pointer, size, capacity >= 15)
void dump_strings(const char* what, const void* obj, uint32_t from, uint32_t to) {
    char line[1000]; int w = 0;
    line[0] = 0;
    for (uint32_t o = from; o + 32 <= to && w < (int)sizeof(line) - 80; o += 8) {
        uint64_t size = 0, cap = 0;
        if (!mem_read((const uint8_t*)obj + o + 16, &size, 8) || !mem_read((const uint8_t*)obj + o + 24, &cap, 8)) continue;
        if (cap < 15 || size == 0 || size > 60 || size > cap || cap > 4096) continue;
        char s[64] = {};
        if (!mem_read_std_string((const uint8_t*)obj + o, s, sizeof(s)) || !s[0]) continue;
        bool printable = true;
        for (const char* p = s; *p; ++p) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) { printable = false; break; }
        if (!printable) continue;
        w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, " %X='%s'", o, s);
    }
    if (w) log_line("UNITPROBE", "      %s strings:%s", what, line);
}

} // namespace

bool unit_rtti_name(const void* obj, char* out, unsigned cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    const uint8_t* vt = nullptr;
    if (!obj || !mem_read(obj, &vt, 8) || !vt) return false;
    const uint8_t* col = nullptr;
    if (!mem_read(vt - 8, &col, 8) || !col) return false;
    uint32_t sig = 0, td = 0;
    if (!mem_read(col, &sig, 4) || sig != 1 || !mem_read(col + 12, &td, 4) || !td) return false;
    char raw[128] = {};
    if (!mem_read((const void*)(image_base() + td + 16), raw, sizeof(raw) - 1)) return false;
    // ".?AVName@glaiel@@" or ".?AUName@..."
    const char* p = raw;
    if (strncmp(p, ".?AV", 4) == 0 || strncmp(p, ".?AU", 4) == 0) p += 4;
    unsigned n = 0;
    while (p[n] && p[n] != '@' && n + 1 < cap) { out[n] = p[n]; ++n; }
    out[n] = 0;
    return n > 0;
}

void unitprobe_log(uint32_t index, const void* chr, bool deep) {
    if (!chr) return;
    char cls[96] = {};
    unit_rtti_name(chr, cls, sizeof(cls));
    log_line("UNITPROBE", "== unit %u %p (%s)", index, chr, cls);
    // components
    const uint8_t* ent = nullptr;
    uint32_t n = 0; const void* const* data = nullptr;
    if (mem_read((const uint8_t*)chr + kChar_Entity, &ent, 8) && ent && mem_read(ent + kEnt_CompCount, &n, 4) && mem_read(ent + kEnt_CompData, &data, 8) && data && n <= kMaxComps) {
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* c = nullptr;
            if (!mem_read(data + i, &c, 8) || !c) continue;
            uint8_t fl[3] = {};
            mem_read(c + kComp_Enabled, fl, 3);
            char name[96] = {};
            unit_rtti_name(c, name, sizeof(name));
            const int32_t type = call_type_id(c);
            const void* owner = nullptr;
            mem_read(c + 0x38, &owner, 8);
            log_line("UNITPROBE", "   comp %u %s type %d en%u del%u alive%u%s", i, name[0] ? name : "?", type, fl[0], fl[1], fl[2], owner == chr ? " owner=unit" : "");
            if (deep && owner == chr) { dump_ints(name, c, 0x40, 0x140); dump_strings(name, c, 0x38, 0x140); }
        }
    } else log_line("UNITPROBE", "   (components not readable)");
    // abilities
    auto ab = [&](const char* slot, const void* a) {
        if (!a) return;
        char gon[64] = {}, an[96] = {};
        ability_gon_name(a, gon, sizeof(gon));
        unit_rtti_name(a, an, sizeof(an));
        log_line("UNITPROBE", "   ability %s '%s' (%s) %p", slot, gon, an, a);
        if (deep) { dump_ints(gon, a, 0x30, 0x140); dump_strings(gon, a, 0x30, 0x140); }
    };
    const void* p = nullptr;
    if (mem_read((const uint8_t*)chr + kCharAbilMove, &p, 8)) ab("move", p);
    if (mem_read((const uint8_t*)chr + kCharAbilAttack, &p, 8)) ab("attack", p);
    if (mem_read((const uint8_t*)chr + kCharAbilBonus, &p, 8)) ab("bonus", p);
    uint32_t sc = 0; const void* const* sd = nullptr;
    if (mem_read((const uint8_t*)chr + kCharSpellCount, &sc, 4) && mem_read((const uint8_t*)chr + kCharSpellData, &sd, 8) && sd && sc <= 64)
        for (uint32_t i = 0; i < sc; ++i) { char nm[16]; _snprintf_s(nm, sizeof(nm), _TRUNCATE, "spell%u", i); if (mem_read(sd + i, &p, 8)) ab(nm, p); }
    // a cat's item slots
    const uint8_t* cd = nullptr;
    if (mem_read((const uint8_t*)chr + kChar_CatData, &cd, 8) && cd) {
        for (int k = 0; k < 5; ++k) {
            const uint8_t* s = cd + kCatData_Items + k * 0x60;
            uint64_t id = 0; char nm[64] = {}, aux[64] = {}; int32_t dur = 0, ax = 0;
            mem_read(s, &id, 8); mem_read_std_string(s + 0x08, nm, sizeof(nm)); mem_read_std_string(s + 0x28, aux, sizeof(aux));
            mem_read(s + 0x48, &dur, 4); mem_read(s + 0x4C, &ax, 4);
            log_line("UNITPROBE", "   item %d id %llx '%s' durability %d aux %d str_aux '%s'", k, (unsigned long long)id, nm, dur, ax, aux);
            if (deep && nm[0]) dump_ints(nm, s, 0x00, 0x60);
        }
    }
    // a few Character fields the sync reads or may read
    if (deep) { dump_ints("character", chr, 0x340, 0x3A0); dump_ints("character", chr, 0x4A0, 0x620); dump_ints("character", chr, 0xC00, 0xD60); }
}

} // namespace mgmp
