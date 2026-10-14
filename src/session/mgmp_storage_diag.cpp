// Read-only diagnostics for the warehouse/storage regression investigation.
#include "mgmp_storage_diag.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_savefile.h"

namespace mgmp {
namespace {

constexpr uint8_t kPageHouse = 3;
constexpr uint8_t kPageCollar = 4;
constexpr uint8_t kPageEquipment = 5;
constexpr uintptr_t kBucketBytes = 64;
constexpr char kStorageProperty[] = "house_storage_upgrades";
constexpr int64_t kMissingProperty = INT64_MIN / 3;

struct GameStr {
    union { char buf[16]; char* ptr; };
    uint64_t size = 0;
    uint64_t cap = 0;
};
using fn_alloc = void* (__fastcall*)(size_t);
using fn_prop_int = int64_t (__fastcall*)(void*, GameStr*, int64_t);

uint8_t g_last_page = 0;
bool g_have_last_property = false;
int64_t g_last_local = 0;
int64_t g_last_answer = 0;
bool g_last_override = false;
bool g_internal_read = false;

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

const char* page_label(uint8_t p) {
    switch (p) {
        case kPageHouse: return "house";
        case kPageCollar: return "collar";
        case kPageEquipment: return "equipment";
        case 1: return "main_menu";
        case 2: return "save_slots";
        case 6: return "chapter";
        case 7: return "in_game";
        default: return "unknown";
    }
}

void utf8_path(const wchar_t* in, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!in || !in[0]) return;
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)cap, nullptr, nullptr);
    out[cap - 1] = 0;
}

bool make_key(GameStr& s, const char* text, fn_alloc alloc) {
    if (!text || !alloc) return false;
    const size_t n = strlen(text);
    memset(&s, 0, sizeof(s));
    if (n <= 15) {
        memcpy(s.buf, text, n + 1);
        s.size = n;
        s.cap = 15;
        return true;
    }
    size_t cap = n | 15;
    if (cap < 22) cap = 22;
    void* p = nullptr;
    __try { p = alloc(cap + 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!p) return false;
    memcpy(p, text, n + 1);
    s.ptr = (char*)p;
    s.size = n;
    s.cap = cap;
    return true;
}

bool director(void*& out) {
    out = nullptr;
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    return at && mem_read((const void*)at, &out, sizeof(out)) && out;
}

struct BucketInfo {
    bool ok = false;
    uint32_t count = 0;
    uint64_t header_hash = 0;
};

BucketInfo bucket(void* inv, uintptr_t off) {
    BucketInfo out;
    if (!inv) return out;
    uint8_t raw[kBucketBytes] = {};
    if (!mem_read((const uint8_t*)inv + off, raw, sizeof(raw))) return out;
    memcpy(&out.count, raw + kBucket_Count, sizeof(out.count));
    // The intrusive-list sentinel is an allocator address. Exclude it so the
    // digest remains useful across restarts; keep the count and remaining
    // bucket header fields so a clear/rebuild is visible.
    memset(raw + kBucket_Head, 0, sizeof(void*));
    out.header_hash = fnv1a(raw, sizeof(raw));
    out.ok = true;
    return out;
}

bool read_property(void* dir, int64_t& value) {
    value = 0;
    if (!dir) return false;
    auto get = (fn_prop_int)addr_of_call(C_PropGetInt);
    auto alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    if (!get || !alloc) return false;
    GameStr key;
    if (!make_key(key, kStorageProperty, alloc)) return false;
    bool ok = false;
    g_internal_read = true;
    __try {
        value = get((uint8_t*)dir + kDir_SaveProps, &key, kMissingProperty);
        ok = value != kMissingProperty;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_internal_read = false;
    return ok;
}

void emit_snapshot(const char* why, uint8_t page) {
    void* dir = nullptr;
    uint8_t slot = 0;
    const bool have_slot = savefile_selected_slot(&slot);
    wchar_t wpath[MAX_PATH] = {};
    char path[MAX_PATH * 3] = {};
    if (savefile_live_path(wpath, MAX_PATH)) utf8_path(wpath, path, sizeof(path));

    int64_t prop = 0;
    const bool prop_ok = director(dir) && read_property(dir, prop);
    void* inv = nullptr;
    if (dir) mem_read((uint8_t*)dir + kDir_Inventory, &inv, sizeof(inv));
    const BucketInfo backpack = bucket(inv, kInv_Backpack);
    const BucketInfo storage = bucket(inv, kInv_Storage);
    const BucketInfo trash = bucket(inv, kInv_Trash);
    log_line("STORAGE", "%s page=%s role=%s self=%u slot=%s%u path=%s property=%s%lld backpack=%s%u/%016llx storage=%s%u/%016llx trash=%s%u/%016llx",
             why ? why : "snapshot", page_label(page), net_role() == NetRole::Host ? "host" : "client", (unsigned)net_self(),
             have_slot ? "" : "?", have_slot ? (unsigned)slot : 0u, path[0] ? path : "-",
             prop_ok ? "" : "?", (long long)(prop_ok ? prop : 0),
             backpack.ok ? "" : "?", backpack.count, (unsigned long long)backpack.header_hash,
             storage.ok ? "" : "?", storage.count, (unsigned long long)storage.header_hash,
             trash.ok ? "" : "?", trash.count, (unsigned long long)trash.header_hash);
}

} // namespace

void storage_diag_page(uint8_t page) {
    if (page == g_last_page) return;
    const uint8_t old = g_last_page;
    g_last_page = page;
    const bool relevant = old == kPageHouse || old == kPageCollar || old == kPageEquipment ||
                          page == kPageHouse || page == kPageCollar || page == kPageEquipment;
    if (!relevant) return;
    log_line("STORAGE", "page transition %s -> %s", page_label(old), page_label(page));
    emit_snapshot(page == kPageHouse ? "house_enter" : page == kPageEquipment ? "equipment_enter" : "page_change", page);
}

void storage_diag_save_loaded() {
    emit_snapshot("save_loaded", g_last_page);
}

void storage_diag_property_get(const char* key, int64_t local, int64_t answer, bool overridden) {
    if (g_internal_read || !key || strcmp(key, kStorageProperty) != 0) return;
    if (!g_have_last_property || local != g_last_local || answer != g_last_answer || overridden != g_last_override) {
        g_have_last_property = true;
        g_last_local = local;
        g_last_answer = answer;
        g_last_override = overridden;
        log_line("STORAGE", "property_get key=%s local=%lld answer=%lld overridden=%d page=%s",
                 kStorageProperty, (long long)local, (long long)answer, overridden ? 1 : 0, page_label(g_last_page));
    }
}

void storage_diag_property_set(const char* key, int value) {
    if (!key || strcmp(key, kStorageProperty) != 0) return;
    log_line("STORAGE", "property_set key=%s value=%d page=%s", kStorageProperty, value, page_label(g_last_page));
    emit_snapshot("property_set", g_last_page);
}

} // namespace mgmp
