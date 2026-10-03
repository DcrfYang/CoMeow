#include "mgmp_catsync.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_bytestream.h"
#include "mgmp_catblob.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_lockstep.h"   // lockstep_in_battle
#include "mgmp_roster.h"     // roster_add_ids -- the 4+4 import puts ids in the party
                             // list through the module that owns that write
#include "mgmp_rng.h"
#if defined(MGMP_WITH_SETUP)
#include "mgmp_setup.h"
#endif

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace mgmp {
namespace {

// The ByteStream's layout now lives in mgmp_bytestream.h -- mgmp_runhist drives
// the same struct through a different serializer, and one copy of an offset
// table is the only number of copies worth having.

typedef void  (__fastcall* fn_serialize_cat)(void* cat, void* stream, bool flag);
typedef void  (__fastcall* fn_bs_dtor)(void* stream);
typedef void* (__fastcall* fn_ofstream_ctor)(void* self);
typedef void* (__fastcall* fn_cat_by_id)(void* registry, uint64_t id);
typedef int   (__fastcall* fn_cat_max_health)(void* cat, const void* filter);

struct State {
    uintptr_t base = 0;
    bool      resolved = false;   // every call target verified

    fn_serialize_cat serialize = nullptr;
    fn_bs_dtor       bs_dtor   = nullptr;
    fn_ofstream_ctor of_ctor   = nullptr;
    fn_cat_by_id     by_id     = nullptr;
    fn_cat_max_health max_health = nullptr;   // optional: the panel falls back to a floor without it
    const void**     director_slot = nullptr;
    void* (__fastcall* allocate)(size_t) = nullptr;
    void* (__fastcall* construct)(void*) = nullptr;
    void (__fastcall* post_load)(void*) = nullptr;
    void* (__fastcall* register_cat)(void*, void*, const uint64_t*) = nullptr;
    void (__fastcall* register_parents)(void*, uint64_t, uint64_t, uint64_t) = nullptr;
    void (__fastcall* append_id)(void*, const uint64_t*) = nullptr;
    void (__fastcall* status_range_dtor)(void*, void*) = nullptr;
    uint64_t* id_counter = nullptr;
    bool import_ready = false;

    bool settling = false;
    bool on        = false;
    bool is_client = false;
    bool announced = false;
    // One line per battle, not one per cat: the host pushes the whole run on a
    // reconnect and this would otherwise be a dozen identical lines.
    bool declined_in_battle = false;
    bool held_in_battle     = false;   // same, for the deferring path
    // THE 4+4 IMPORT, ONCE PER RUN (2026-09-23). Cleared by catsync_forget so a
    // session that starts over can import again; see import_file.
    bool import_tried       = false;
    // WHAT THE IMPORT FILE SAID, kept so the run can be CHECKED against it. The peer's
    // agreed list can arrive after the import and write the run back to what it was --
    // measured in the first B-group run -- and this peer's next publish then tells the
    // peer the shorter list, which destroys the peer's own memory of the longer one.
    // Neither side can recover from that on its own; the file can.
    uint64_t import_id[64]  = {};
    uint32_t import_n       = 0;
    uint32_t reassert_ticks = 0;

    // WHAT THE TWO PEERS LAST AGREED ON, per cat. Flat and small: a run is a few
    // dozen cats, and a linear scan of 64 u64s is not worth a hash map on a path
    // that runs once per map node.
    //
    // IT IS A BASELINE, NOT A SEND LOG, and that is the whole of the bidirectional
    // change (2026-09-22). It used to be "the bytes I last sent", which is the same
    // thing while only one peer ever writes -- and is exactly wrong once both do:
    //
    //   - SET ON SEND, as before. An unchanged cat costs nothing on the wire.
    //
    //   - SET ON APPLY, which is new. A cat that arrived from the peer has just
    //     become the state both peers hold, so this peer must NOT turn around and
    //     publish its own copy at the next node -- which is precisely what "last
    //     sent" means for a cat this peer never sent. Without this, the client
    //     echoed the host's own cats back at it and undid the host's equips.
    //
    //   - A CLIENT PUBLISHES ONLY A CAT IT HAS A BASELINE FOR. An empty entry means
    //     "the host has never told me about this cat", so this peer's copy is not
    //     known to be anyone's -- and the client's copy of a run the host owns can
    //     be a version the host has already replaced in the House or a shop, not
    //     yet pushed. Sending it would revert the host's change. The host has the
    //     mirror rule: an empty entry THERE means the client has never seen the
    //     cat, which is exactly when it must be sent.
    //
    // A true simultaneous write to one cat -- both players editing the same cat
    // before either message arrives -- resolves to the arriving message, because
    // holding a local change back forever is worse than losing one equip. It is
    // logged when it happens; the design it comes from is one cat, one editor.
    static constexpr uint32_t kMaxCats = 64;
    uint64_t base_id[kMaxCats]   = {};
    uint64_t base_hash[kMaxCats] = {};
    uint32_t base_count = 0;

    // The map tick's publish is throttled: deciding whether anything changed means
    // serializing every cat, and that tick runs at frame rate.
    uint32_t map_ticks = 0;
    uint32_t held_pub  = 0;   // publishes deferred because cats were held

    // Cats that arrived at a moment this peer must not write them, held until
    // the map-follow tick. Coalesced by id -- a CATDATA is whole state, not a
    // delta, so a newer one for the same cat makes the older one worthless.
    uint64_t pend_id[kMaxCats]   = {};
    uint64_t pend_hash[kMaxCats] = {};
    uint8_t* pend_data[kMaxCats] = {};
    uint32_t pend_size[kMaxCats] = {};
    uint32_t pend_count = 0;

    uint32_t pushed = 0, applied = 0, skipped = 0, deferred = 0, coalesced = 0;

    // THE DIGEST (2026-09-29). The baseline above says what this peer believes both
    // agreed on; nothing ever CHECKED that belief against the peer's actual bytes
    // between nodes, so a push that was lost, refused or reverted stayed diverged until
    // a battle hash halted on it. See catsync_on_digest in the header.
    static constexpr uint32_t kDigestPeers = 8;
    uint32_t    digest_ticks = 0;
    uint64_t    digest_sig   = 0;     // signature of the last digest this peer sent
    CatDigestMsg peer_digest[kDigestPeers] = {};
    bool        peer_fresh[kDigestPeers]   = {};
    uint64_t    peer_members[kDigestPeers] = {};   // last membership difference said
    struct Heal {
        uint8_t  peer = 0;
        uint64_t id = 0, theirs = 0, mine = 0;
        uint32_t seen = 0, tries = 0;
        bool     said = false;
    };
    Heal     heal[kMaxCats] = {};
    uint32_t heal_count = 0;
    uint32_t digests_sent = 0, digests_seen = 0, mismatches = 0;
    uint32_t heals_sent = 0, heals_ok = 0, heal_stuck = 0, dumps = 0;
};

State g;

// --- SEH shims --------------------------------------------------------------
//
// Every one of these calls into the game with a pointer whose meaning is
// recovered rather than declared. The project's rule is that a wrong guess
// produces a bad log line and never a crash in the game process, so each raw
// call lives in its own function with no C++ objects in scope and a __try around
// it. They return false instead of propagating.

bool safe_ofstream_ctor(void* p) {
    __try { g.of_ctor(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_bs_dtor(void* p) {
    __try { g.bs_dtor(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_serialize(void* cat, void* stream) {
    // `true` at every observed call site (GlobalProgressionData, the two save
    // paths, the winning-teams writer); none passes false.
    __try { g.serialize(cat, stream, true); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_by_id(void* registry, uint64_t id, void** out) {
    __try { *out = g.by_id(registry, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The game's own maximum health for a cat: 4 x effective CON + base health, at least 1. The filter
// is what the game's callers pass -- 0xFF x 32 -- and the callee reads it with MOVAPS.
int safe_max_health(void* cat) {
    if (!g.max_health) return 0;
    alignas(16) static uint8_t filter[32];
    memset(filter, 0xFF, sizeof(filter));
    __try { const int v = g.max_health(cat, filter); return v > 0 && v < 100000 ? v : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool clear_saved_statuses(void* cat) {
    if (!g.status_range_dtor) return false;
    __try {
        auto* vector = (uintptr_t*)((uint8_t*)cat + kCatData_StatusEffects);
        const uintptr_t begin = vector[0], end = vector[1], capacity = vector[2];
        if (end < begin || capacity < end || (!begin && capacity) ||
            (end - begin) % kCatStatusEffectSize ||
            (capacity - begin) % kCatStatusEffectSize) return false;
        // RVA 0x22F301 appends; use the game's range destructor before replacing saved effects.
        g.status_range_dtor((void*)begin, (void*)end);
        vector[1] = begin;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool invalidate_saved_items(void* cat) {
    // Item reader RVA 0x22D46B returns immediately for an absent item. It was
    // written for fresh CatData; on an existing cat that retains the old item.
    // Its validity test (0x22D41F) requires id != -1 AND a nonempty name.
    // Invalidate just the id: retain the owned string buffers for the game's
    // reader/destructor to reuse/free. A present item gets a new id at 0x22D48C.
    // Do not memset these records: they contain two owning std::strings.
    const uint64_t empty = UINT64_MAX;
    for (uint32_t i = 0; i < kCatItemCount; ++i)
        if (!mem_write((uint8_t*)cat + kCatData_Items + i * kCatItemSize,
                       &empty, sizeof(empty))) return false;
    return true;
}

void* construct_cat() {
    __try {
        void* cat = g.allocate(0xC58);
        if (!cat) return nullptr;
        memset(cat, 0, 0xC58);
        return g.construct(cat);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// PUTS A CAT THAT ALREADY EXISTS BACK IN THE STATE A NEW ONE STARTS IN (2026-10-01, the 3-player halt).
//
// A save the mod has run on carries last session's session cats (ids 0x70000001.., 0x71000001..), so on the
// next run some peers find their session ids already registered and read the new image INTO that old object,
// while a peer that never had them builds a new one. The two do not end up the same: five human cats had a
// different max HP on the host (which built everything new) than on the clients (which read into old
// objects) from byte-identical images -- an object read into keeps whatever the image does not carry, and
// the post-load pass that a new cat gets never ran on it. Setup-time cats are rebuilt the same way on every
// peer from here on: zeroed, constructed, read, post-loaded -- exactly what construct_cat + register_cat do.
// (The old object's own heap pieces are left to leak; it is a handful of strings, once per run.)
bool reset_cat(void* cat) {
    __try {
        memset(cat, 0, 0xC58);
        return g.construct(cat) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool post_load_cat(void* cat) {
    __try { g.post_load((uint8_t*)cat + 0x60); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool is_session_range(uint64_t id) { return id >= 0x70000000ull && id < 0x80000000ull; }

bool register_cat(void* registry, void* cat, uint64_t id) {
    __try {
        g.post_load((uint8_t*)cat + 0x60);
        struct Result { void* node; uint64_t inserted; } result{};
        g.register_cat((uint8_t*)registry + 0xE8, &result, &id);
        if (!result.node || !result.inserted) return false;
        *(void**)((uint8_t*)result.node + 0x18) = cat;
        g.register_parents((uint8_t*)registry + 0x38, id, UINT64_MAX, UINT64_MAX);
        if (!is_session_range(id) && *g.id_counter < id) *g.id_counter = id;   // see repair_id_counter
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool replace_familiars(void* vector, const uint64_t* ids, uint32_t count) {
    __try {
        *(uint32_t*)((uint8_t*)vector + 4) = 0;
        for (uint32_t i = 0; i < count; ++i) g.append_id(vector, &ids[i]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// --- stream lifecycle -------------------------------------------------------

bool bs_construct(uint8_t* bs) {
    memset(bs, 0, kByteStreamSize);
    if (!safe_ofstream_ctor(bs + kBS_Ofstream)) return false;
    // Both endian words stay 0, so read() compares 0 != 0 and never byte-swaps.
    // This is the element-size cap that guards the swap it will not do.
    *(uint32_t*)(bs + kBS_SwapElem) = 8;
    return true;
}

// --- the run's cats ---------------------------------------------------------
//
// director+1432 is the id -> CatData* registry, +1468/+1472 the run's id list
// (count, data). All three read off sub_1403B2060, which is what the inventory
// screen itself walks. We read the ids directly rather than calling that
// function because it allocates a CustomVector we would only free again.
struct RunCats {
    const void* registry = nullptr;
    const uint64_t* ids  = nullptr;
    uint32_t count       = 0;
    uint64_t all_ids[64] = {};
};

bool run_cats(RunCats& out) {
    if (!g.director_slot) return false;
    const void* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return false;

    const uint8_t* d = (const uint8_t*)dir;
    if (!mem_read(d + kDir_CatRegistry, &out.registry, sizeof(out.registry))) return false;
    if (!mem_read(d + kDir_CatIdCount,  &out.count,    sizeof(out.count)))    return false;
    if (!mem_read(d + kDir_CatIdData,   &out.ids,      sizeof(out.ids)))      return false;

    // Refuse anything implausible rather than walking it. A run has a few dozen
    // cats; a five-digit count means the offsets moved and we are reading some
    // other object, which is exactly when NOT to start calling game functions
    // with the results.
    if (!out.registry || !out.ids || !out.count || out.count > 64) return false;
    if (!mem_read(out.ids, out.all_ids, out.count * sizeof(uint64_t))) return false;
    uint32_t familiar_count = 0, capacity = 0;
    const uint64_t* familiars = nullptr;
    if (!mem_read(d + kDir_CatFamiliars, &capacity, 4) ||
        !mem_read(d + kDir_CatFamiliars + 4, &familiar_count, 4) ||
        !mem_read(d + kDir_CatFamiliars + 8, &familiars, 8) ||
        familiar_count > capacity || familiar_count > 64 - out.count) return false;
    if (familiar_count && (!familiars || !mem_read(familiars, out.all_ids + out.count,
                                                 familiar_count * sizeof(uint64_t)))) return false;
    out.count += familiar_count;
    for (uint32_t i = 0; i < out.count; ++i) {
        if (!out.all_ids[i] || out.all_ids[i] == UINT64_MAX) return false;
        for (uint32_t j = 0; j < i; ++j) if (out.all_ids[j] == out.all_ids[i]) return false;
    }
    out.ids = out.all_ids;
    return true;
}

// Setup/import owner notes exist before the first battle split. Without this
// fallback every peer can publish the other peers' pre-battle copies.
bool cat_is_mine(uint64_t id, bool& mine) {
    if(lockstep_cat_is_mine(id,mine))return true;
    uint8_t owner=kNoPeer;
    if(!lockstep_owner_pos(id,owner)||owner>=net_peer_count())return false;
    mine=owner==net_peer_pos();return true;
}

uint64_t fnv1a(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

// WHERE two images of one cat differ, as values (2026-09-29). "hash A -> hash B" says that
// a replacement happened and nothing about what changed; the first differing offset and
// sixteen bytes either side of it are usually enough to tell HP from an item from a status
// list without a second tool. Prints the differing byte count too, so "one field" and
// "the whole image moved" are distinguishable at a glance.
void describe_diff(const uint8_t* mine, uint32_t mn, const uint8_t* theirs, uint32_t tn,
                   char* out, size_t cap) {
    const uint32_t common = mn < tn ? mn : tn;
    uint32_t first = UINT32_MAX, differing = 0;
    for (uint32_t i = 0; i < common; ++i)
        if (mine[i] != theirs[i]) { if (first == UINT32_MAX) first = i; ++differing; }
    if (first == UINT32_MAX) {
        if (mn == tn) { _snprintf_s(out, cap, _TRUNCATE, "identical"); return; }
        first = common;
    }
    const uint32_t from = first >= 8 ? first - 8 : 0;
    char a[80] = {}, b[80] = {};
    int ao = 0, bo = 0;
    for (uint32_t i = from; i < from + 24; ++i) {
        if (i < mn) ao += _snprintf_s(a + ao, sizeof(a) - ao, _TRUNCATE, "%02X", mine[i]);
        if (i < tn) bo += _snprintf_s(b + bo, sizeof(b) - bo, _TRUNCATE, "%02X", theirs[i]);
    }
    _snprintf_s(out, cap, _TRUNCATE,
                "sizes %u/%u, %u differing byte(s) in the common part, first at +0x%X; "
                "bytes from +0x%X: here %s | incoming %s",
                mn, tn, differing, first, from, a, b);
}

// A MEMORY SNAPSHOT OF ONE CAT (2026-09-29): the serialized image this peer holds, written
// beside the log the first time the digest shows that cat differing, so the two peers' files
// can be diffed byte for byte (tools/save_dump.py reads the same decompressed record). Capped
// per session; silent when no log path is configured (the unit tests).
void dump_cat_image(uint64_t id, const uint8_t* bytes, uint32_t n, const char* why) {
    if (g.dumps >= 24) return;
    const wchar_t* log = config().log_path;
    if (!log || !log[0]) return;
    wchar_t path[600] = {};
    wcsncpy_s(path, log, _TRUNCATE);
    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t* slash2 = wcsrchr(path, L'/');
    if (slash2 > slash) slash = slash2;
    if (!slash) return;
    swprintf_s(slash + 1, 600 - (slash + 1 - path), L"mgmp_catdump_%s%u_%016llX_%016llX.bin",
               g.is_client ? L"client" : L"host", (unsigned)net_peer_pos(),
               (unsigned long long)id, (unsigned long long)fnv1a(bytes, n));
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    const bool ok = fwrite(bytes, 1, n, f) == n;
    fclose(f);
    if (!ok) return;
    ++g.dumps;
    log_line_lvl(LogLevel::Warn, "CATSYNC", "cat %016llx image (%u bytes) written to %ls (%s)",
                 (unsigned long long)id, n, path, why);
}

// (Both moved out of this anonymous namespace on 2026-09-23: see the note beside their
// definitions at the bottom of the namespace block.)

// The baseline entry for one cat, created empty if this is the first time either
// peer has mentioned it. Null only when the table is full.
uint64_t* baseline(uint64_t id) {
    for (uint32_t i = 0; i < g.base_count; ++i)
        if (g.base_id[i] == id) return &g.base_hash[i];
    if (g.base_count >= State::kMaxCats) return nullptr;
    g.base_id[g.base_count] = id;
    g.base_hash[g.base_count] = 0;
    return &g.base_hash[g.base_count++];
}

// Fill the baseline from the run this peer is holding, once. See the call site.
void seed_client_baseline(const RunCats& rc) {
    uint32_t seeded = 0;
    for (uint32_t i = 0; i < rc.count; ++i) {
        uint64_t id = 0;
        if (!mem_read(&rc.ids[i], &id, sizeof(id)) || !id) continue;

        void* cat = nullptr;
        if (!safe_by_id((void*)rc.registry, id, &cat) || !cat) continue;

        uint8_t* bytes = nullptr;
        uint32_t n = serialize_cat(cat, &bytes);
        if (!n) continue;

        uint64_t* base = baseline(id);
        if (base) { *base = fnv1a(bytes, n); ++seeded; }
        free(bytes);
    }
    if (seeded)
        log_line("CATSYNC", "seeded the baseline from the run this peer holds (%u "
                            "cat(s)) -- a client's run CAME from the host's save, so "
                            "at load time the two agreed on exactly these bytes, and "
                            "this is what this peer's own edits are measured against",
                 seeded);
}

// Read from the CONFIG rather than the live session, for the same reason
// mgmp_savefile does: the host publishes at its first map node, which can be
// before a peer has finished connecting.
//
// NOT LATCHED -- see the note on mgmp_savefile's ensure_state. The panel's
// connect buttons can set the role after this has already run once, and a
// latched "off" is a module that never arms for the rest of the process.
void ensure_state() {
    const Config& c = config();
    bool host   = _stricmp(c.net_role, "host")   == 0;
    bool client = _stricmp(c.net_role, "client") == 0;
    g.is_client = client;
    g.on = tune::kCatSync && (host || client) && g.resolved;

    static bool said = false;
    if (!g.on && tune::kCatSync && (host || client) && !said) {
        said = true;
        log_line("CATSYNC", "!! disabled: the game functions it calls did not "
                            "verify against this build");
    }
}

} // namespace

// WHY THESE TWO LIVE OUT HERE, OUTSIDE THE ANONYMOUS NAMESPACE (2026-09-23). They are the only
// two functions of this module that another module calls by name -- mgmp_choice's level-up path
// uses serialize_cat to take this peer's cat's image and catsync_deserialize_into to lay it over
// the pending level-up's copy. A definition inside the anonymous namespace has INTERNAL linkage,
// so the header's declaration compiles and the link then fails with
//
//   LNK2019: unresolved external symbol "unsigned int __cdecl serialize_cat(void*, ...)"
//
// which is a mistake this file has now made twice (the note at the bottom of the old probe block
// records the first time). Everything they use -- bs_construct, safe_serialize, safe_bs_dtor and
// the kBS_* offsets -- is still visible from here, because an anonymous namespace's names are
// usable in the namespace that encloses it.
uint32_t serialize_cat(void* cat, uint8_t** out) {
    *out = nullptr;
    uint8_t bs[kByteStreamSize];
    if (!bs_construct(bs)) return 0;

    *(uint32_t*)(bs + kBS_Mode) = 1;             // write, growing the buffer
    uint32_t n = 0;
    if (safe_serialize(cat, bs)) {
        const void* buf = *(const void**)(bs + kBS_WriteBuf);
        uint32_t    len = *(uint32_t*)(bs + kBS_WriteLen);
        if (buf && len && len <= kMaxCatBytes) {
            uint8_t* copy = (uint8_t*)malloc(len);
            // Copy out before the destructor runs: the buffer belongs to the
            // GAME's heap and the destructor frees it there.
            if (copy && mem_read(buf, copy, len)) { *out = copy; n = len; }
            else if (copy) free(copy);
        }
    }
    safe_bs_dtor(bs);
    return n;
}

// See the declaration in the header: the cat-sync apply path's read-mode recipe, against an
// address the caller names rather than one the run resolves. Same two checks afterwards, so a
// truncated image cannot be acknowledged as successfully loaded.
bool catsync_deserialize_into(void* cat, const uint8_t* image, uint32_t len) {
    if (!cat || !image || !len || len > kMaxCatBytes) return false;

    uint8_t bs[kByteStreamSize];
    if (!bs_construct(bs)) return false;

    *(uint32_t*)(bs + kBS_Mode)       = 0;             // read
    *(const void**)(bs + kBS_ReadBuf) = image;         // BORROWED, see kBS_ReadOwns
    *(uint8_t*)(bs + kBS_ReadOwns)    = 0;             // ...so the game must not free it
    *(uint32_t*)(bs + kBS_ReadLen)    = len;
    *(uint32_t*)(bs + kBS_ReadPos)    = 0;

    bool ok = clear_saved_statuses(cat);
    if (!ok)
        log_line_lvl(LogLevel::Error, "CATSYNC", "!! saved-status cleanup failed -- cat image not loaded");
    if (ok && !invalidate_saved_items(cat)) {
        log_line_lvl(LogLevel::Error, "CATSYNC", "!! saved-item cleanup failed -- cat image not loaded");
        ok = false;
    }
    if (ok) ok = safe_serialize(cat, bs);
    ok = ok && *(uint32_t*)(bs + kBS_ReadPos) == len;
    safe_bs_dtor(bs);
    return ok;
}

static bool cat_data_perished(const void* cat) {
    uint8_t killed = 0; uint64_t flags = 0;
    if (!cat) return false;
    if (mem_read((const uint8_t*)cat + kCatData_Killed, &killed, 1) && killed) return true;
    return mem_read((const uint8_t*)cat + kCatData_Flags, &flags, sizeof(flags)) && (flags & kCatFlag_Perished) != 0;
}

uint64_t catsync_image_hash(void* cat) {
    ensure_state();
    if (!cat) return 0;
    uint8_t* bytes = nullptr;
    const uint32_t n = serialize_cat(cat, &bytes);
    const uint64_t h = n && bytes ? fnv1a(bytes, n) : 0;
    free(bytes);
    return h;
}

bool catsync_cat_exists(uint64_t id) {
    ensure_state();
    if (!g.resolved || !g.director_slot) return false;
    const uint8_t* dir = nullptr;
    const void* registry = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir || !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry) return false;
    void* cat = nullptr;
    return safe_by_id((void*)registry, id, &cat) && cat;
}

bool catsync_cat_perished(uint64_t id) {
    ensure_state();
    if (!g.resolved || !g.director_slot) return false;
    const uint8_t* dir = nullptr;
    const void* registry = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir || !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry) return false;
    void* cat = nullptr;
    return safe_by_id((void*)registry, id, &cat) && cat && cat_data_perished(cat);
}

bool catsync_local_briefs(CatBrief* out, uint32_t max, uint32_t& n) {
    n = 0;
    if (!out || !max || !g.resolved || !g.director_slot) return false;
    RunCats rc{};
    if (!run_cats(rc)) return false;   // implausible or mid-swap: the caller keeps what it had

    // Is this a shared run? Its cats carry session ids. Before that the party is just this player's pick.
    bool shared = false;
    for (uint32_t i = 0; i < rc.count; ++i)
        if (session_cat_owner(rc.ids[i]) >= 0) { shared = true; break; }
    const int me = net_peer_count() >= 2 ? (int)net_peer_pos() : 0;

    struct Pick { uint64_t id; uint32_t order; };
    Pick picks[64]; uint32_t np = 0;
    // The party list alone, before there is a shared run: the familiar list then holds leftovers of an
    // earlier run, not somebody else's cats.
    uint32_t party_count = 0;
    if (!shared) {
        const uint8_t* d = nullptr;
        if (mem_read(g.director_slot, &d, sizeof(d)) && d && mem_read(d + kDir_CatIdCount, &party_count, 4) &&
            party_count <= rc.count) { /* first party_count ids of all_ids are the party */ }
        else party_count = rc.count;
    }
    for (uint32_t i = 0; i < rc.count && np < 64; ++i) {
        const uint64_t id = rc.ids[i];
        int owner = session_cat_owner(id);
        if (owner < 0) {
            uint8_t pos = 0;
            if (lockstep_owner_pos(id, pos)) owner = pos;
            else if (shared) owner = 0;                 // nobody claimed it: the host is the authority
        }
        if (!shared) { if (i >= party_count) continue; }   // pre-shared: own picks only
        else if (owner != me) continue;
        picks[np++] = { id, i };
    }
    // Stable: by id when every pick is a session id (ids are the identity and survive re-lists), else list order.
    bool all_session = np > 0;
    for (uint32_t i = 0; i < np; ++i) if (session_cat_owner(picks[i].id) < 0) all_session = false;
    if (all_session)
        for (uint32_t i = 1; i < np; ++i)
            for (uint32_t j = i; j > 0 && picks[j].id < picks[j - 1].id; --j) { Pick t = picks[j]; picks[j] = picks[j - 1]; picks[j - 1] = t; }

    for (uint32_t i = 0; i < np && n < max; ++i) {
        void* cat = nullptr;
        if (!safe_by_id((void*)rc.registry, picks[i].id, &cat) || !cat) continue;
        // A cat whose corpse was just destroyed is gone for good: the panel drops it at that moment, not when the battle ends and the
        // game finally takes the id out of the run's lists. (A cat that only fell, corpse intact, comes back and stays at hp 0.)
        if (cat_data_perished(cat)) continue;
        uint8_t* image = nullptr;
        const uint32_t size = serialize_cat(cat, &image);
        if (!size || !image) continue;
        CatBrief b;
        if (catblob_parse(image, size, b)) {
            const int mh = safe_max_health(cat);
            if (mh > 0) {
                b.maxhp = mh;
                if (b.hp > 0 && b.hp >= mh) b.hp = kBriefFullHp;   // the game clamps to max: at max = unhurt
            }
            // Inside a fight the damage is on the Character, not yet on the CatData.
            int32_t lh = 0, lm = 0;
            if (lockstep_live_health(picks[i].id, lh, lm)) {
                b.maxhp = lm;
                b.hp = lh >= lm ? kBriefFullHp : lh;
            }
            out[n++] = b;
        }
        free(image);
    }
    return true;
}

bool catsync_export_party_setup(SetupMsg& out) {
    ensure_state();
    out = SetupMsg{};
    if (!g.on || !g.director_slot) return false;

    const void* dir = nullptr;
    const void* registry = nullptr;
    const uint64_t* ids = nullptr;
    uint32_t count = 0;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir ||
        !mem_read((const uint8_t*)dir + kDir_CatRegistry, &registry, sizeof(registry)) ||
        !mem_read((const uint8_t*)dir + kDir_CatIdCount, &count, sizeof(count)) ||
        !mem_read((const uint8_t*)dir + kDir_CatIdData, &ids, sizeof(ids)) ||
        !registry || !ids || count == 0 || count > kPartyMaxCats)
        return false;

    uint64_t party[kSetupMaxCats] = {};
    if (!mem_read(ids, party, count * sizeof(uint64_t))) return false;
    auto fail = [&out]() { for(auto& c:out.cats) free(c.data); out=SetupMsg{}; return false; };
    for (uint32_t i = 0; i < count; ++i) {
        if (!party[i] || party[i] == UINT64_MAX) return fail();
        for (uint32_t j = 0; j < i; ++j) if (party[i] == party[j]) return fail();
        void* cat = nullptr;
        if (!safe_by_id((void*)registry, party[i], &cat) || !cat) return fail();
        uint8_t* image = nullptr;
        uint32_t size = serialize_cat(cat, &image);
        if (!size || !image) {
            for (uint32_t j = 0; j < out.count; ++j) free(out.cats[j].data);
            out = SetupMsg{};
            return false;
        }
        out.cats[i].id = party[i];
        out.cats[i].size = size;
        out.cats[i].hash = fnv1a(image, size);
        out.cats[i].data = image;
        out.count = (uint8_t)(i + 1);
    }
    log_line("CATSYNC", "captured %u locally selected cat(s) for setup exchange", out.count);
    return out.count >= 1 && out.count <= kPartyMaxCats;
}

// A saved client map can show its local party or the canonical host party.
// Ownership comes from the established session namespace, never list position.
bool catsync_export_resume(SetupMsg& out, uint8_t owner) {
    ensure_state();
    out = SetupMsg{};
    RunCats rc{};
    if (!g.on || owner >= net_peer_count() || !run_cats(rc) || rc.count > kSetupMaxCats) return false;
    unsigned masks[kMaxPeers]{};
    for (uint32_t i=0;i<rc.count;++i) {
        const auto id=rc.ids[i]; const int p=session_cat_owner(id);
        void* cat=nullptr;
        if(p<0 || p>=net_peer_count() || !safe_by_id((void*)rc.registry,id,&cat) || !cat) return false;
        masks[p] |= 1u << unsigned((id & 0xFFFFFF)-1);
    }
    for(unsigned p=0;p<net_peer_count();++p) if(!masks[p]) return false;
    for (uint8_t i = 0; i < kPartyMaxCats; ++i) {
        if(!(masks[owner] & (1u<<i))) continue;
        const uint64_t id = 0x70000001ull + ((uint64_t)owner << 24) + i;
        void* cat = nullptr;
        uint8_t* bytes = nullptr;
        const uint32_t n = safe_by_id((void*)rc.registry, id, &cat) && cat
                         ? serialize_cat(cat, &bytes) : 0;
        if (!n || !bytes) {
            free(bytes);
            for (auto& c : out.cats) free(c.data);
            out = SetupMsg{};
            return false;
        }
        out.cats[out.count] = {id, n, fnv1a(bytes, n), bytes};
        ++out.count;
    }
    return true;
}

void catsync_setup_sent(const SetupMsg& snapshot) {
    // Incoming host cats populate the baseline before the client's first map
    // tick. Explicitly seed OUR exported cats too, or they stay unpublished.
    for (uint8_t i = 0; i < snapshot.count; ++i)
        if (uint64_t* b = baseline(snapshot.cats[i].id)) *b = snapshot.cats[i].hash;
}

// Native 3B5430 returns both +5B8 (party) and +640 (familiars) to
// the House. Its settlement loops traverse those vectors; removing the peer
// IDs before the call restricts the normal game writeback to this owner.
// THE GAME'S OWN PERMADEATH, as read from the disassembly (2026-10-02). A cat whose corpse is destroyed goes through a routine that
// (1) appends a 24-byte DEATH RECORD {cat id, .., kind at +0xC, flag at +0x10} to the director's list (cap +0x5D8, count +0x5DC,
// data +0x5E0) and (2) removes that id from the run's party / mirror / familiar vectors. The settlement (ENDFINAL 0x3B5430 and the
// defeat twin 0x3B4750) then walks the record list: every recorded cat gets +0xBF8 |= 0x10 (dead) and the on-adventure bit cleared.
// For a session clone that is exactly the state the merge writes back over the original -- so the death should reach the save
// by itself. This only prints the list so the next full run can confirm it.
static void log_death_records(const uint8_t* dir, const char* why) {
    uint32_t n = 0;
    const uint8_t* data = nullptr;
    if (!dir || !mem_read(dir + 0x5DC, &n, 4) || n > 64) return;
    char text[400] = {};
    int off = 0;
    if (n && mem_read(dir + 0x5E0, &data, sizeof(data)) && data) {
        for (uint32_t i = 0; i < n && off < 350; ++i) {
            uint64_t id = 0; int32_t kind = 0; uint8_t flag = 0;
            mem_read(data + i * 24, &id, 8); mem_read(data + i * 24 + 0xC, &kind, 4); mem_read(data + i * 24 + 0x10, &flag, 1);
            off += _snprintf_s(text + off, sizeof(text) - off, _TRUNCATE, " %llx(kind %d flag %u)", (unsigned long long)id, kind, (unsigned)flag);
        }
    }
    log_line("CATSYNC", "death records (%s): %u%s", why, n, n ? text : "");
}

bool catsync_prepare_settlement(void* director) {
    ensure_state();
    if (!g.on || g.settling || net_peer_pos() >= kMaxPeers) return false;
    const void* live = nullptr;
    if (!g.director_slot || !mem_read(g.director_slot, &live, sizeof(live)) ||
        !director || live != director) return false;
    const uint8_t owner = net_peer_pos();
    log_death_records((const uint8_t*)director, "before the native settlement");
    RunCats rc{};
    if (!run_cats(rc) || rc.count > kSetupMaxCats) return false;
    unsigned seen = 0, own_n=0, masks[kMaxPeers]{};
    uint64_t own[4] = {};
    for (uint32_t i = 0; i < rc.count; ++i) {
        const uint64_t id = rc.ids[i];
        const int pos=session_cat_owner(id);
        // A CAT THE GAME BROUGHT IN DURING THE RUN (the gain_cat_familiar event) has an ordinary id, not a session one. This used to
        // fail the whole check below and hold the home door shut for ever. Such a cat is the HOST's (events and loot are the host's
        // call): the host settles it with its own party -- it is a real cat of the host's house, not a clone -- and a client leaves it
        // out like any other copy of somebody else's cat.
        if (pos < 0 && id < 0x70000000ull) {
            void* gained = nullptr;
            if (owner == 0 && own_n < 4 && safe_by_id((void*)rc.registry, id, &gained) && gained) {
                own[own_n++] = id;
                log_line("SETTLE", "cat %016llx was gained during the run: settled with the host's own party", (unsigned long long)id);
            } else {
                log_line("SETTLE", "cat %016llx was gained during the run: %s", (unsigned long long)id,
                         owner == 0 ? "not settled with the party (no room / not registered)" : "the host's -- left out of this peer's settlement");
            }
            continue;
        }
        const int slot=pos<0 ? -1 : int((id & 0xFFFFFF)-1);
        void* cat = nullptr;
        uint8_t noted = 0xFF;
        // THE OWNERSHIP SOURCE IS THE SESSION NOTE TABLE, NOT THE BATTLE SPLIT
        // (2026-09-28, measured live). lockstep_cat_is_mine answers from the
        // battle split's table, which only fills when a battle's TurnControl is
        // built -- a session resumed onto the map can walk an event node
        // straight into the home node (today's run: special event -> home, the
        // probe never armed, no TurnControl ever existed), so that table was
        // empty, every is_mine call failed, and the finalize was held on BOTH
        // peers with the game frozen at the door. The owner-note table is
        // session-absolute wire truth -- noted by the roster install at setup
        // or resume, and imported from the checkpoint journal since disk
        // version 2 -- and it needs no battle. The 0x70/0x71 namespace already
        // names the position; the note must agree with it. The split table is
        // still consulted when it has the cat: a battle that assigned the cat
        // the other way is a real desync and still refuses the settlement.
        if (slot < 0 || pos>=net_peer_count() || !lockstep_owner_pos(id, noted) || noted != (uint8_t)pos ||
            !safe_by_id((void*)rc.registry, id, &cat) || !cat ||
            (seen & (1u << (pos * 4 + slot)))) return false;
        seen |= 1u << (pos * 4 + slot);
        bool mine = false;
        if (lockstep_cat_is_mine(id, mine) && mine != (pos == owner)) return false;
        masks[pos] |= 1u<<slot;
        if ((uint8_t)pos == owner) own[own_n++] = id;
    }
    if(!own_n || own_n>4) return false;
    for(unsigned p=0;p<net_peer_count();++p) if(!masks[p]) return false;

    // Validate all destinations BEFORE the first write. Snapshot bytes, not
    // containers: native allocations/capacities and owning objects remain intact.
    struct Edit { void* at; size_t n; uint8_t before[32], after[32]; } edits[7]{};
    unsigned used = 0;
    auto add = [&](void* at, const void* value, size_t n) {
        if (!at || n > 32 || used >= 7) return false;
        auto& e = edits[used]; e.at = at; e.n = n;
        if (!mem_read(at, e.before, n)) return false;
        memcpy(e.after, value, n); ++used; return true;
    };
    auto* d = (uint8_t*)director;
    uint32_t party_n=0;
    if(!mem_read(d+kDir_CatIdCount,&party_n,4)) return false;
    // Ensure own party/mirror capacity via the engine allocator before transactional writes.
    for (const uintptr_t offset : {kDir_CatIdCap, kDir_MirrorCap, kDir_CatFamiliars}) {
        uint32_t cap=0,count=0; uint64_t* data=nullptr;
        if(!mem_read(d+offset,&cap,4) || !mem_read(d+offset+4,&count,4) ||
           !mem_read(d+offset+8,&data,8) || count>cap || count>kSetupMaxCats || (count&&!data)) return false;
        uint64_t mirror_ids[kPartyMaxCats]{};
        if(offset==kDir_MirrorCap && (count>kPartyMaxCats || count!=party_n || !mem_read(data,mirror_ids,count*8) || memcmp(mirror_ids,rc.ids,count*8))) return false;
        if(offset==kDir_CatFamiliars) { const uint32_t zero=0; if(!add(d+offset+4,&zero,4)) return false; }
        else {
            // Setup reserves room for four in both party vectors even for one-cat teams.
            if(cap<own_n || !data || !add(data,own,own_n*8) || !add(d+offset+4,&own_n,4)) return false;
        }
    }
    // Peer registry objects are intentionally left untouched. Their CatData
    // flags are not the save-file ownership bits (the native finalizer derives
    // those while traversing the party/familiar vectors); guessing at +0xBF8
    // here would corrupt unrelated status flags. Removing peer IDs from both
    // native settlement vectors is the ownership boundary.
    for (unsigned i = 0; i < used; ++i) {
        uint8_t check[32] = {};
        auto& e = edits[i];
        if (!mem_write(e.at, e.after, e.n) || !mem_read(e.at, check, e.n) || memcmp(check, e.after, e.n)) {
            bool rollback = true;
            for (int j = (int)i; j >= 0; --j) {
                auto& old = edits[j];
                const bool ok = mem_write(old.at, old.before, old.n) &&
                    mem_read(old.at, check, old.n) && !memcmp(check, old.before, old.n);
                rollback = rollback && ok;
            }
            log_line_lvl(LogLevel::Error, "SETTLE", "!! ownership filter write failed; rollback=%s; native settlement NOT called",
                         rollback ? "verified" : "FAILED");
            return false;
        }
    }
    g.settling = true;
    roster_setup_set_ready(false); // no deferred map/gear swap may reinsert peer cats
    log_line("SETTLE", "native home settlement owner %u: party=[%llx %llx %llx %llx], familiars=0; "
                      "peer copies excluded; own cat/items/currency handled by game",
             owner, own[0], own[1], own[2], own[3]);
    return true;
}

// --- the clones' way home (2026-10-01, finding F1) ---------------------------------------------------------------
namespace {
// How many clone slots a player has (the settlement's ownership masks and prepare_party_setup both use 1..4). Session
// ids with a higher slot are leftovers of older builds: they are never a target of the next run's cloning.
constexpr uint32_t kCloneSlots = 4;

struct RegNode { const uint8_t* next; const uint8_t* prev; uint64_t key; void* value; };

// Walk the registry's hash list (table = registry+0xE8; head sentinel = *(table+8); node = {next, prev, key, CatData*}).
template <class F> bool registry_walk(const void* registry, F&& visit) {
    const uint8_t* table = (const uint8_t*)registry + 0xE8;
    const uint8_t* head = nullptr;
    const uint8_t* n = nullptr;
    if (!mem_read(table + 8, &head, sizeof(head)) || !head || !mem_read(head, &n, sizeof(n))) return false;
    for (unsigned guard = 0; n && n != head && guard < 20000; ++guard) {
        RegNode node{};
        if (!mem_read(n, &node, sizeof(node))) return false;
        if (node.value) visit(node.key, node.value);
        n = node.next;
    }
    return true;
}

// Clones already kept as new cats and not yet re-used for a new selection. The game was seen to put the flags of a
// retired clone back (the departure box starts from the party/mirror lists), and a second merge then kept the SAME
// clone as yet another new cat; a clone in this set is only retired again.
uint64_t g_adopted[64] = {};
uint32_t g_adopted_n = 0;
bool adopted_has(uint64_t id) { for (uint32_t i = 0; i < g_adopted_n; ++i) if (g_adopted[i] == id) return true; return false; }
void adopted_note(uint64_t id) { if (!adopted_has(id) && g_adopted_n < 64) g_adopted[g_adopted_n++] = id; }
void adopted_clear(uint64_t id) {
    for (uint32_t i = 0; i < g_adopted_n; ++i)
        if (g_adopted[i] == id) { g_adopted[i] = g_adopted[--g_adopted_n]; return; }
}

struct Origin { uint64_t id = 0; void* cat = nullptr; bool legacy = false; unsigned ordinary_away = 0, ordinary_home = 0, legacy_away = 0; void* home_cat = nullptr; };

// Clone id -> the original it was swapped into (swap_identity), kept so the House's entities -- which carry their OWN id, written to house_state -- can be put right.
struct SwapNote { uint64_t clone = 0, original = 0; };
SwapNote g_swaps[32];
uint32_t g_swaps_n = 0;
void swap_note(uint64_t clone, uint64_t original) {
    for (uint32_t i = 0; i < g_swaps_n; ++i) if (g_swaps[i].clone == clone) { g_swaps[i].original = original; return; }
    if (g_swaps_n < 32) g_swaps[g_swaps_n++] = { clone, original };
    else { memmove(g_swaps, g_swaps + 1, sizeof(g_swaps) - sizeof(SwapNote)); g_swaps[31] = { clone, original }; }
}

// The registry's list node of an id ({next, prev, key, CatData*}); null when the id is not there.
uint8_t* registry_node(const void* registry, uint64_t id) {
    const uint8_t* table = (const uint8_t*)registry + 0xE8;
    const uint8_t* head = nullptr;
    const uint8_t* n = nullptr;
    if (!mem_read(table + 8, &head, sizeof(head)) || !head || !mem_read(head, &n, sizeof(n))) return nullptr;
    for (unsigned guard = 0; n && n != head && guard < 20000; ++guard) {
        RegNode node{};
        if (!mem_read(n, &node, sizeof(node))) return nullptr;
        if (node.key == id) return (uint8_t*)n;
        n = node.next;
    }
    return nullptr;
}

// THE HOUSE KEEPS THE CLONE (2026-10-03). The native settlement puts the SETTLED CLONES into the House (and so into the save's house_state, by id); the
// originals stay out of the House. Copying the clone's progress onto the original and zeroing the clone's flags (the way this used to work) left the House
// holding a clone with flags 0: the next load put it back as a live cat (flags 1, NOT retired -- it could go out again, a twin of the retired original), and
// the original, absent from house_state, was not loaded at all. So the two cats swap IDENTITIES instead: the registry entries and the saved ids are
// exchanged, the object the House already holds (the clone, with everything the run did to it, flags 3) becomes the original's id, and the old original
// object becomes the clone slot (retired afterwards by the caller). The House, the save and the registry then agree, as after a solo run.
bool swap_identity(const void* registry, uint64_t clone_id, void* clone, uint64_t orig_id, void* orig) {
    uint8_t* nc = registry_node(registry, clone_id);
    uint8_t* no = registry_node(registry, orig_id);
    if (!nc || !no || nc == no) return false;
    void* vc = nullptr; void* vo = nullptr;
    if (!mem_read(nc + 0x18, &vc, sizeof(vc)) || !mem_read(no + 0x18, &vo, sizeof(vo)) || vc != clone || vo != orig) return false;
    uint64_t sc = 0, so = 0;
    if (!mem_read((uint8_t*)clone + kCatData_SaveId, &sc, sizeof(sc)) || !mem_read((uint8_t*)orig + kCatData_SaveId, &so, sizeof(so)) || sc != clone_id || so != orig_id) return false;
    const bool ok = mem_write(nc + 0x18, &orig, sizeof(orig)) && mem_write(no + 0x18, &clone, sizeof(clone)) &&
                    mem_write((uint8_t*)clone + kCatData_SaveId, &orig_id, sizeof(orig_id)) && mem_write((uint8_t*)orig + kCatData_SaveId, &clone_id, sizeof(clone_id));
    if (ok) return true;
    mem_write(nc + 0x18, &clone, sizeof(clone)); mem_write(no + 0x18, &orig, sizeof(orig));          // put everything back
    mem_write((uint8_t*)clone + kCatData_SaveId, &clone_id, sizeof(clone_id)); mem_write((uint8_t*)orig + kCatData_SaveId, &orig_id, sizeof(orig_id));
    return false;
}

// The cat a clone was made from, found by its seed (a clone is a byte copy, so the seed is the same; it is unique among
// ordinary cats). An ORDINARY cat is preferred; failing that, a leftover session cat of an older build (slot above
// kCloneSlots) that is still out on adventure -- the source when a clone was itself made from such a cat. Ids in `busy`
// (the run's own party) never qualify. False for none or for more than one of the winning kind.
bool find_origin_by_seed(const void* registry, uint64_t seed, const uint64_t* busy, uint32_t nbusy, Origin& out) {
    out = Origin{};
    if (!seed) return false;
    Origin o_ord, o_leg;
    const bool walked = registry_walk(registry, [&](uint64_t key, void* cat) {
        for (uint32_t i = 0; i < nbusy; ++i) if (busy[i] == key) return;
        uint64_t s = 0, flags = 0;
        if (!mem_read(cat, &s, sizeof(s)) || s != seed) return;
        if (!mem_read((uint8_t*)cat + kCatData_Flags, &flags, sizeof(flags))) return;
        const bool away = (flags & kCatFlag_OnAdventure) != 0;
        // Only a cat that is OUT on adventure can be the original of a clone: a cat at home with the same seed is a
        // copy (an adopted clone) or a cat somebody played on, and must never be written over or make the match ambiguous.
        if (!is_session_range(key)) { if (away) { ++out.ordinary_away; o_ord = {key, cat, false}; } else { ++out.ordinary_home; out.home_cat = cat; } return; }
        if ((key & 0xFFFFFF) > kCloneSlots && away) { ++out.legacy_away; o_leg = {key, cat, true}; }
    });
    if (!walked) return false;
    const unsigned oa = out.ordinary_away, oh = out.ordinary_home, la = out.legacy_away;
    void* const hc = out.home_cat;
    if (oa) { if (oa != 1) return false; out = o_ord; out.ordinary_away = oa; out.ordinary_home = oh; out.legacy_away = la; out.home_cat = hc; return true; }
    if (la != 1) return false;
    out = o_leg; out.ordinary_away = oa; out.ordinary_home = oh; out.legacy_away = la; out.home_cat = hc; return true;
}

// The highest id among ORDINARY cats (0 when the walk fails).
uint64_t max_ordinary_id(const void* registry) {
    uint64_t best = 0;
    if (!registry_walk(registry, [&](uint64_t key, void*) { if (!is_session_range(key) && key > best) best = key; })) return 0;
    return best;
}

// THE GAME NUMBERS NEW CATS FROM A COUNTER, and registering a session id used to drag it up to that id: every cat the
// game made afterwards (a recruit, a kitten, a found cat) got an id INSIDE the session range, where the mod reads it as
// somebody's clone -- and a cat with a peer's namespace in an adventure fails the settlement's ownership check and
// freezes the home door. register_cat no longer raises the counter for session ids; this puts a counter that an older
// build already pushed up back to just above the highest ordinary cat.
void repair_id_counter(const void* registry) {
    if (!g.id_counter || *g.id_counter < 0x70000000ull) return;
    const uint64_t top = max_ordinary_id(registry);
    if (!top || top >= 0x70000000ull) return;
    log_line("CATSYNC", "the cat id counter was %llx (inside the session range) -- put back to %llx",
             (unsigned long long)*g.id_counter, (unsigned long long)top);
    *g.id_counter = top;
}

// A settled clone with no usable original becomes an ordinary cat of its own, under the next free ordinary id, so the
// progress it carries survives the next run's re-use of the clone slot.
bool adopt_as_new_cat(const void* registry, const uint8_t* image, uint32_t size, uint64_t& new_id) {
    if (!g.id_counter || !image || !size) return false;
    const uint64_t id = *g.id_counter + 1;
    if (!id || is_session_range(id)) return false;
    void* cat = construct_cat();
    if (!cat || !catsync_deserialize_into(cat, image, size)) return false;
    if (!mem_write((uint8_t*)cat + kCatData_SaveId, &id, sizeof(id)) || !register_cat((void*)registry, cat, id)) return false;
    new_id = id;
    return true;
}
} // namespace


// --- clone -> original, remembered (see catsync.h) ------------------------------------------------------------------------------
namespace {
CloneOriginNote g_origin[32];
uint32_t g_origin_n = 0;
}

void catsync_note_origin(uint64_t clone, uint64_t original) {
    if (!clone || !original || original >= 0x70000000ull) return;          // an ordinary cat of the save is the only thing worth recording
    for (uint32_t i = 0; i < g_origin_n; ++i)
        if (g_origin[i].clone == clone) { g_origin[i].original = original; return; }
    if (g_origin_n < 32) g_origin[g_origin_n++] = { clone, original };
}

uint64_t catsync_origin_of(uint64_t clone) {
    for (uint32_t i = 0; i < g_origin_n; ++i) if (g_origin[i].clone == clone) return g_origin[i].original;
    return 0;
}

void catsync_origin_clear() { for (uint32_t i = 0; i < g_origin_n; ++i) g_origin[i] = {}; g_origin_n = 0; }

static void origin_forget(uint64_t clone) {
    for (uint32_t i = 0; i < g_origin_n; ++i)
        if (g_origin[i].clone == clone) { g_origin[i] = g_origin[--g_origin_n]; g_origin[g_origin_n] = {}; return; }
}

uint32_t catsync_origin_export(CloneOriginNote* out, uint32_t max) {
    if (!out || !max) return 0;
    const uint32_t n = g_origin_n < max ? g_origin_n : max;
    for (uint32_t i = 0; i < n; ++i) out[i] = g_origin[i];
    return n;
}

void catsync_origin_import(const CloneOriginNote* in, uint32_t count) {
    if (!in) return;
    for (uint32_t i = 0; i < count; ++i) catsync_note_origin(in[i].clone, in[i].original);
}

// WHAT IS LEFT OUT AFTER THE MERGE (2026-10-02): a cat whose clone died in the run has no settled clone to come back from, so its
// original stays flagged "out on adventure" for good (hidden, never deleted). This only LOOKS and says so in the log -- which
// ordinary cats are still out, which of this player's session cats still exist and in what state -- so the comparison "the cats
// that left vs the cats that came back" can be read after a real run before anything is written about it.
static void settle_audit(const void* registry, const uint8_t* dir, const char* why) {
    log_death_records(dir, why);
    const uint64_t base = 0x70000000ull + ((uint64_t)net_peer_pos() << 24);
    char away[400] = {}, mine[400] = {};
    int ao = 0, mo = 0;
    unsigned n_away = 0, n_mine = 0;
    registry_walk(registry, [&](uint64_t key, void* cat) {
        uint64_t flags = 0, seed = 0;
        if (!mem_read((uint8_t*)cat + kCatData_Flags, &flags, sizeof(flags)) || !mem_read(cat, &seed, sizeof(seed))) return;
        if (!is_session_range(key)) {
            if (!(flags & kCatFlag_OnAdventure)) return;
            ++n_away;
            if (ao < 340) ao += _snprintf_s(away + ao, sizeof(away) - ao, _TRUNCATE, " %llx(flags %llx seed %llx)", (unsigned long long)key, (unsigned long long)flags, (unsigned long long)seed);
        } else if (key > base && key <= base + 32) {
            ++n_mine;
            if (mo < 340) mo += _snprintf_s(mine + mo, sizeof(mine) - mo, _TRUNCATE, " %llx(flags %llx seed %llx)", (unsigned long long)key, (unsigned long long)flags, (unsigned long long)seed);
        }
    });
    if (n_away || n_mine)
        log_line("CATSYNC", "settle audit (%s): %u ordinary cat(s) still out on adventure:%s | %u of this player's session cat(s) still registered:%s",
                 why, n_away, away[0] ? away : " -", n_mine, mine[0] ? mine : " -");
}

namespace {
// POD-only (SEH).
bool call_save_game(uintptr_t fn, const void* director) {
    __try { ((void(__fastcall*)(const void*))fn)(director); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool catsync_save_game(const char* why) {
    ensure_state();
    const uintptr_t fn = addr_of_call(C_SaveGame);
    const void* director = nullptr;
    if (!fn || !g.director_slot || !mem_read(g.director_slot, &director, sizeof(director)) || !director) {
        log_line_lvl(LogLevel::Warn, "CATSYNC", "!! save (%s): MewDirector::SaveGame is not available -- the file on disk keeps the settlement's own save", why ? why : "");
        return false;
    }
    const bool ok = call_save_game(fn, director);
    log_line_lvl(ok ? LogLevel::Info : LogLevel::Error, "CATSYNC", "save (%s): the game's own save was written again after the clone merge (%s)", why ? why : "", ok ? "ok" : "FAILED");
    return ok;
}

void catsync_house_save(void* house) {
    if (!g_swaps_n || !house) return;
    // The writer (sub_1401E6810) reads the entity list from ((house+0x18)->+8)->+0x20 -> +0x4480: a vector {.. count at +0xC, element pointers at +0x10}; each entity's id is at +0x80.
    const uint8_t* a = nullptr; const uint8_t* b = nullptr; const uint8_t* c = nullptr; const uint8_t* vec = nullptr;
    if (!mem_read((const uint8_t*)house + 0x18, &a, sizeof(a)) || !a || !mem_read(a + 8, &b, sizeof(b)) || !b ||
        !mem_read(b + 0x20, &c, sizeof(c)) || !c || !mem_read(c + 0x4480, &vec, sizeof(vec)) || !vec) return;
    uint32_t count = 0; const uint8_t* const* data = nullptr;
    if (!mem_read(vec + 0xC, &count, sizeof(count)) || !count || count > 4096 || !mem_read(vec + 0x10, &data, sizeof(data)) || !data) return;
    unsigned fixed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* ent = nullptr; uint64_t id = 0;
        if (!mem_read(data + i, &ent, sizeof(ent)) || !ent || !mem_read(ent + 0x80, &id, sizeof(id)) || !is_session_range(id)) continue;
        for (uint32_t k = 0; k < g_swaps_n; ++k) {
            if (g_swaps[k].clone != id) continue;
            if (mem_write((uint8_t*)ent + 0x80, &g_swaps[k].original, sizeof(uint64_t))) {
                ++fixed;
                log_line("CATSYNC", "house save: the House's cat entity %016llx is written as %016llx (the cat it was swapped into)", (unsigned long long)id, (unsigned long long)g_swaps[k].original);
            }
            break;
        }
    }
    (void)fixed;
}

unsigned catsync_merge_session_cats(const char* why) {
    ensure_state();
    if (!g.on || !g.import_ready || !g.director_slot || net_peer_pos() >= kMaxPeers) return 0;
    const uint8_t* dir = nullptr;
    const void* registry = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir ||
        !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry) return 0;
    repair_id_counter(registry);

    // The run's own party (and familiars) are never an origin to write over: a selected original is in there.
    uint64_t busy[64] = {};
    uint32_t nbusy = 0;
    {
        RunCats rc{};
        if (run_cats(rc)) { nbusy = rc.count < 64 ? rc.count : 64; memcpy(busy, rc.all_ids, nbusy * sizeof(uint64_t)); }
    }

    unsigned merged = 0, by_record = 0, by_seed = 0, as_new = 0, dead_n = 0;
    uint64_t from_id[32] = {}, to_id[32] = {};
    const uint64_t base = 0x70000000ull + ((uint64_t)net_peer_pos() << 24);
    for (uint32_t slot = 1; slot <= 32; ++slot) {
        const uint64_t id = base + slot;
        void* clone = nullptr;
        if (!safe_by_id((void*)registry, id, &clone) || !clone) continue;
        uint64_t flags = 0, seed = 0;
        if (!mem_read((uint8_t*)clone + kCatData_Flags, &flags, sizeof(flags)) ||
            !mem_read(clone, &seed, sizeof(seed))) continue;
        // Still flagged "out on a run" normally means the run is not over for that clone. NOT when the game has marked it dead for
        // good and the run no longer lists it: a cat whose corpse was destroyed left the lists, so the settlement never walked it and
        // never cleared the flag -- merging it is how the death reaches the original (and the departure bit is cleared below).
        const bool clone_dead = cat_data_perished(clone);
        bool in_run = false;
        for (uint32_t b = 0; b < nbusy; ++b) if (busy[b] == id) in_run = true;
        if (!flags || ((flags & kCatFlag_OnAdventure) && !(clone_dead && !in_run))) continue;     // retired already / still out on a run
        log_line("CATSYNC", "merge (%s): session cat %016llx settled with flags %llx (bit 0x10/0x20 = the game's \"dead\" marks, 0x1/0x2 = alive)",
                 why, (unsigned long long)id, (unsigned long long)flags);
        if (adopted_has(id)) {                                      // kept as a new cat before; its flags came back
            const uint64_t retired_again = 0;
            mem_write((uint8_t*)clone + kCatData_Flags, &retired_again, sizeof(retired_again));
            log_line("CATSYNC", "merge (%s): session cat %016llx was already kept as a new cat -- retired again, not kept twice",
                     why, (unsigned long long)id);
            continue;
        }

        Origin o;
        uint64_t oflags = 0;
        // 1. The recorded origin (setup noted it; a restored journal brought it back). Trusted only if that cat is still there, is not
        //    in the run, and still carries the clone's seed -- the seed is a byte copy at cloning time and never changes afterwards.
        bool have_origin = false, recorded = false;
        {
            const uint64_t oid = catsync_origin_of(id);
            void* oc = nullptr; uint64_t os = 0;
            bool busy_o = false;
            for (uint32_t b = 0; b < nbusy; ++b) if (busy[b] == oid) busy_o = true;
            if (oid && !is_session_range(oid) && !busy_o && safe_by_id((void*)registry, oid, &oc) && oc && mem_read(oc, &os, sizeof(os)) &&
                os == seed && mem_read((uint8_t*)oc + kCatData_Flags, &oflags, sizeof(oflags))) {
                o = Origin{ oid, oc, false };
                have_origin = true; recorded = true;
            } else if (oid) {
                log_line("CATSYNC", "merge (%s): session cat %016llx has a recorded origin %016llx that is gone, in the run or no longer the same cat -- searching by seed",
                         why, (unsigned long long)id, (unsigned long long)oid);
            }
        }
        // 2. Fallback: the cat with this seed that is out on adventure (the way it always worked).
        if (!have_origin)
            have_origin = find_origin_by_seed(registry, seed, busy, nbusy, o) &&
                          mem_read((uint8_t*)o.cat + kCatData_Flags, &oflags, sizeof(oflags)) && (oflags & kCatFlag_OnAdventure);
        if (!have_origin && slot > kCloneSlots) {                   // a leftover of an older build: it is its own cat
            log_line("CATSYNC", "merge (%s): session cat %016llx (seed %llx) has no usable original (out on adventure: %u ordinary, %u older clone(s); at home with that seed: %u) -- left as the cat it is",
                     why, (unsigned long long)id, (unsigned long long)seed, o.ordinary_away, o.legacy_away, o.ordinary_home);
            continue;
        }
        if (!have_origin)
            log_line("CATSYNC", "merge (%s): session cat %016llx (seed %llx): originals out on adventure: %u ordinary, %u older clone(s); at home with that seed: %u",
                     why, (unsigned long long)id, (unsigned long long)seed, o.ordinary_away, o.legacy_away, o.ordinary_home);
        // A live clone whose original is AT HOME already (the merge returned it before, and the game put the clone's flags back -- or an older build
        // left it live) is a DUPLICATE of a cat that exists: keeping it as a new cat gave a second, identical cat WITHOUT the retired mark (flags 1, the
        // original's 3), which the adventure box accepts although the cat has been on its adventure (2026-10-03 live: "returned cats are not retired and
        // can go out again"). It is only retired again.
        bool twin = false;                                            // exactly ONE cat at home with this seed, and it is the clone's image but for the flags
        if (!have_origin && o.ordinary_home == 1 && o.home_cat) {
            uint64_t cf = 0, hf = 0;
            uint8_t* a = nullptr; uint8_t* b = nullptr;
            if (mem_read((uint8_t*)clone + kCatData_Flags, &cf, sizeof(cf)) && mem_read((uint8_t*)o.home_cat + kCatData_Flags, &hf, sizeof(hf)) &&
                mem_write((uint8_t*)clone + kCatData_Flags, &hf, sizeof(hf))) {              // same flags on both for the comparison, put back right after
                const uint32_t sa = serialize_cat(clone, &a);
                mem_write((uint8_t*)clone + kCatData_Flags, &cf, sizeof(cf));
                const uint32_t sb = serialize_cat(o.home_cat, &b);
                twin = sa && sa == sb && a && b && memcmp(a, b, sa) == 0;
            }
            free(a); free(b);
        }
        if (twin) {
            const uint64_t zero = 0;
            mem_write((uint8_t*)clone + kCatData_Flags, &zero, sizeof(zero));
            origin_forget(id);
            log_line_lvl(LogLevel::Warn, "CATSYNC", "!! merge (%s): session cat %016llx (flags %llx) is a live duplicate of %u cat(s) already at home -- retired again, NOT kept as a new cat",
                         why, (unsigned long long)id, (unsigned long long)flags, o.ordinary_home);
            continue;
        }

        uint8_t* image = nullptr;
        const uint32_t size = serialize_cat(clone, &image);
        if (!size || !image) { free(image); continue; }
        uint64_t target = 0;
        bool ok = false;
        bool swapped = false;
        if (have_origin) {
            uint8_t* backup = nullptr;
            // A cat that survived: the clone the House holds BECOMES the original (see swap_identity). A dead one has nothing in the House: its image is copied.
            if (!clone_dead && swap_identity(registry, id, clone, o.id, o.cat)) {
                swapped = true; ok = true;
                swap_note(id, o.id);
                std::swap(clone, o.cat);                            // from here `o.cat` is the object that carries the progress, `clone` the old original (retired below)
            }
            const uint32_t bsize = swapped ? 0 : serialize_cat(o.cat, &backup);   // so a failed write can be undone
            if (!swapped)
            ok = bsize && backup && reset_cat(o.cat) && catsync_deserialize_into(o.cat, image, size) &&
                 mem_write((uint8_t*)o.cat + kCatData_SaveId, &o.id, sizeof(o.id)) && post_load_cat(o.cat);
            if (!ok && bsize && backup) {                           // put the original back as it was
                const bool restored = reset_cat(o.cat) && catsync_deserialize_into(o.cat, backup, bsize) &&
                                      mem_write((uint8_t*)o.cat + kCatData_SaveId, &o.id, sizeof(o.id)) && post_load_cat(o.cat);
                log_line_lvl(LogLevel::Error, "CATSYNC", "!! merge (%s): writing session cat %016llx over %016llx FAILED; original %s",
                             why, (unsigned long long)id, (unsigned long long)o.id, restored ? "restored" : "NOT RESTORED");
            }
            free(backup);
            if (ok) {
                target = o.id;
                uint64_t now = 0;                                   // the image already carries the settled flags;
                if (mem_read((uint8_t*)o.cat + kCatData_Flags, &now, sizeof(now)) && (now & kCatFlag_OnAdventure)) {
                    now &= ~kCatFlag_OnAdventure;                   // ... make sure the departure bit is gone
                    mem_write((uint8_t*)o.cat + kCatData_Flags, &now, sizeof(now));
                }
                // A cat that died for good must come home DEAD in the way the game itself marks it: 0x20 beside the 0x40000 the
                // corpse destruction wrote (the settlement sets 0x20 on a killed cat, 0x10 on one that merely left).
                if (clone_dead) {
                    if (!(now & 0x20)) { now |= 0x20; mem_write((uint8_t*)o.cat + kCatData_Flags, &now, sizeof(now)); }
                    ++dead_n;
                }
                // Read it back: the original must carry the clone's seed and no departure bit now.
                {
                    uint64_t chk_seed = 0, chk_flags = 0;
                    if (!mem_read(o.cat, &chk_seed, sizeof(chk_seed)) || chk_seed != seed || !mem_read((uint8_t*)o.cat + kCatData_Flags, &chk_flags, sizeof(chk_flags)) ||
                        (chk_flags & kCatFlag_OnAdventure))
                        log_line_lvl(LogLevel::Error, "CATSYNC", "!! merge (%s): the original %016llx did not read back as expected (seed %llx, flags %llx)",
                                     why, (unsigned long long)o.id, (unsigned long long)chk_seed, (unsigned long long)chk_flags);
                }
                if (recorded) ++by_record; else ++by_seed;
                origin_forget(id);
                log_line("CATSYNC", "merge (%s): session cat %016llx returned to %016llx%s%s%s%s (flags %llx -> %llx), clone retired",
                         why, (unsigned long long)id, (unsigned long long)o.id, o.legacy ? " (an older build's clone)" : "",
                         recorded ? " [by the recorded origin]" : " [by seed]", clone_dead ? " [DEAD]" : "", swapped ? " [identities swapped: the House's cat is now the original]" : "",
                         (unsigned long long)oflags, (unsigned long long)now);
            }
        } else {
            ok = adopt_as_new_cat(registry, image, size, target);
            if (ok) { adopted_note(id); ++as_new; origin_forget(id); }
            if (ok && !clone_dead) {
                // THE HOUSE HOLDS THE CLONE HERE TOO (2026-10-03, host: one cat lost its retired mark after the reload). The adopted copy is a new object nobody shows, and the
                // House (and house_state) still name the clone's id: on the next load the clone slot came back alive, not retired. So the same identity swap as for an original:
                // the object the House holds becomes the new cat, the fresh copy takes the clone slot (and is retired below).
                void* adopted = nullptr;
                if (safe_by_id((void*)registry, target, &adopted) && adopted && swap_identity(registry, id, clone, target, adopted)) {
                    swap_note(id, target);
                    std::swap(clone, adopted);
                    log_line("CATSYNC", "merge (%s): session cat %016llx is now the new cat %016llx [identities swapped: the House's cat is the new cat]",
                             why, (unsigned long long)id, (unsigned long long)target);
                }
            }
            if (ok) {
                void* nc = nullptr; uint64_t nf = 0;
                if (safe_by_id((void*)registry, target, &nc) && nc && mem_read((uint8_t*)nc + kCatData_Flags, &nf, sizeof(nf))) {
                    // A cat that comes out of a run has been on its adventure: it is RETIRED (flag 2, set by the native settlement; a clone the game put back to
                    // life has lost it). Without it the adventure box takes the cat again.
                    if ((nf & 1) && !(nf & 0x32) && !clone_dead) {
                        const uint64_t fixed = nf | 2;
                        if (mem_write((uint8_t*)nc + kCatData_Flags, &fixed, sizeof(fixed))) { log_line("CATSYNC", "merge (%s): the new cat %016llx had lost its retired mark -- flags %llx -> %llx", why, (unsigned long long)target, (unsigned long long)nf, (unsigned long long)fixed); nf = fixed; }
                    }
                    log_line("CATSYNC", "merge (%s): the new cat %016llx carries flags %llx", why, (unsigned long long)target, (unsigned long long)nf);
                }
            }
            if (ok) log_line("CATSYNC", "merge (%s): session cat %016llx has no usable original -- kept as the NEW cat %016llx",
                             why, (unsigned long long)id, (unsigned long long)target);
            else log_line_lvl(LogLevel::Error, "CATSYNC", "!! merge (%s): session cat %016llx could not be kept as a new cat -- left as it is",
                              why, (unsigned long long)id);
        }
        free(image);
        if (!ok) continue;
        const uint64_t retired = 0;
        mem_write((uint8_t*)clone + kCatData_Flags, &retired, sizeof(retired));
        from_id[merged] = id; to_id[merged] = target;
        ++merged;
    }
    // The run's party list and its mirror still name the clones just retired, and the House's departure box starts from
    // them (2026-10-01 live: the retired clones came back as selectable cats, and were merged again as "new" cats).
    // Point both lists at the cats the clones became. The native settlement may already have emptied the party list, so
    // the mirror is the fallback source; the write itself does not need the current party to be non-empty.
    if (merged) {
        uint64_t ids[16] = {}; uint32_t n = 0;
        const char* source = "party";
        {
            uint32_t count = 0; const uint64_t* data = nullptr;
            if (mem_read(dir + kDir_CatIdCount, &count, 4) && count && count <= 4 && mem_read(dir + kDir_CatIdData, &data, sizeof(data)) &&
                data && mem_read(data, ids, count * sizeof(uint64_t))) n = count;
            if (!n && mem_read(dir + kDir_MirrorCount, &count, 4) && count && count <= 4 && mem_read(dir + kDir_MirrorData, &data, sizeof(data)) &&
                data && mem_read(data, ids, count * sizeof(uint64_t))) { n = count; source = "mirror"; }
        }
        bool changed = false;
        for (uint32_t i = 0; i < n; ++i)
            for (unsigned k = 0; k < merged; ++k) if (ids[i] == from_id[k]) { ids[i] = to_id[k]; changed = true; }
        if (!n) log_line("CATSYNC", "merge (%s): neither the run's party list nor its mirror holds anything to re-point", why);
        else if (!changed) log_line("CATSYNC", "merge (%s): the run's %s list (%u) names none of the merged clones", why, source, n);
        else {
            const bool wrote = roster_setup_replace_party(ids, n, "clones returned to their originals");
            log_line("CATSYNC", "merge (%s): the run's %s list re-pointed at the cats the clones became -- %s", why, source, wrote ? "written" : "NOT written");
        }
    }
    if (merged || as_new)
        log_line("CATSYNC", "merge (%s): summary -- %u clone(s) returned (%u by the recorded origin, %u by seed, %u of them dead), %u kept as new cats",
                 why, merged - as_new, by_record, by_seed, dead_n, as_new);
    settle_audit(registry, dir, why);
    return merged;
}

unsigned catsync_release_stuck_cats(const char* why) {
    ensure_state();
    if (!g.on || !g.director_slot) return 0;
    const uint8_t* dir = nullptr;
    const void* registry = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir ||
        !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry) return 0;
    uint64_t busy[64] = {};
    uint32_t nbusy = 0;
    {
        // The party list is read directly (run_cats also insists on the familiar list being sane, which it need not be
        // between runs); the familiars are added when they can be read. Without the party nothing is released.
        uint32_t count = 0; const uint64_t* ids = nullptr;
        if (!mem_read(dir + kDir_CatIdCount, &count, sizeof(count)) || count > 16 ||
            !mem_read(dir + kDir_CatIdData, &ids, sizeof(ids)) || (count && !ids) ||
            (count && !mem_read(ids, busy, count * sizeof(uint64_t)))) {
            log_line("CATSYNC", "release (%s): the run's party cannot be read -- nothing released", why);
            return 0;
        }
        nbusy = count;
        uint32_t fcap = 0, fcount = 0; const uint64_t* fam = nullptr;
        if (mem_read(dir + kDir_CatFamiliars, &fcap, 4) && mem_read(dir + kDir_CatFamiliars + 4, &fcount, 4) &&
            mem_read(dir + kDir_CatFamiliars + 8, &fam, 8) && fam && fcount <= fcap && fcount <= 64 - nbusy &&
            mem_read(fam, busy + nbusy, fcount * sizeof(uint64_t)))
            nbusy += fcount;
    }
    unsigned released = 0;
    log_line("CATSYNC", "release (%s): scanning for cats still out on adventure (%u in the run's own lists)", why, nbusy);
    registry_walk(registry, [&](uint64_t key, void* cat) {
        if (is_session_range(key)) return;
        for (uint32_t i = 0; i < nbusy; ++i) if (busy[i] == key) return;
        uint64_t flags = 0;
        if (!mem_read((uint8_t*)cat + kCatData_Flags, &flags, sizeof(flags)) || !(flags & kCatFlag_OnAdventure)) return;
        const uint64_t home = flags & ~kCatFlag_OnAdventure;
        if (!mem_write((uint8_t*)cat + kCatData_Flags, &home, sizeof(home))) return;
        if (++released <= 40)
            log_line("CATSYNC", "release (%s): cat %016llx was still flagged on adventure with no run in progress -- home again (flags %llx -> %llx)",
                     why, (unsigned long long)key, (unsigned long long)flags, (unsigned long long)home);
    });
    return released;
}

// The two saves can contain the same numeric cat ID.  Sending that ID across
// the setup barrier therefore aliases two different cats in the receiving
// registry (and, when it is selected on both sides, puts one identity in both
// party and familiars).  Give every selected slot a session-local identity
// before it crosses the wire.  The high range is outside IDs observed in the
// save and still fits the game's 32-bit run-list elements.
bool catsync_prepare_party_setup(SetupMsg& out, uint8_t owner_pos) {
    // Clones a previous run left as live cats at home (the old behaviour, still in saves made by it) go back to
    // their originals first: the targets below are fixed ids, and a live cat there would be overwritten.
    if (owner_pos == net_peer_pos()) { catsync_merge_session_cats("before setup"); catsync_release_stuck_cats("before setup"); }
    SetupMsg original{};
    if (!catsync_export_party_setup(original)) return false;
    if (owner_pos >= net_peer_count() || owner_pos >= kMaxPeers) {
        for (uint8_t i = 0; i < original.count; ++i) free(original.cats[i].data);
        return false;
    }

    RunCats rc{};
    if (!run_cats(rc) || !rc.registry) {
        for (uint8_t i = 0; i < original.count; ++i) free(original.cats[i].data);
        return false;
    }

    uint64_t session_ids[kSetupMaxCats] = {};
    for (uint8_t i = 0; i < original.count; ++i) {
        // 0x70/0x71 separates the host and client namespaces; the slot is
        // deliberately included so equal source cats remain distinct copies.
        const uint64_t id = 0x70000000ull + ((uint64_t)owner_pos << 24) + i + 1;
        void* existing = nullptr;
        if (!safe_by_id((void*)rc.registry, id, &existing)) {
            for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
            return false;
        }
        // A pre-existing target is valid only when it is the same session copy
        // from an idempotent retry.  The one other occupant is our OWN clone
        // from a previous run of this SAME session: a fresh process and a
        // checkpoint restore both rebuild the registry from the save, but an
        // in-session settlement (2026-09-28) does not, so the next chapter's
        // barrier finds the stale copy still registered.  Real save cat IDs
        // never use this range and peer imports live in the other 0x70/0x71
        // namespace, so replace the stale clone with the new selection's image
        // -- the same wholesale replacement the receiving peer's apply path
        // already performs for a re-delivered identity -- instead of retrying
        // "already owned by another cat" forever while the client's export
        // never leaves and the host's chapter click stays held.
        if (existing && original.cats[i].id != id) {
            if (!reset_cat(existing) ||
                !catsync_deserialize_into(existing, original.cats[i].data,
                                          original.cats[i].size) ||
                !mem_write((uint8_t*)existing + kCatData_SaveId, &id, sizeof(id)) ||
                !post_load_cat(existing)) {
                log_line_lvl(LogLevel::Error, "SETUP",
                             "!! reserved session cat ID %016llx could not be replaced with the new selection",
                             (unsigned long long)id);
                for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
                return false;
            }
            log_line("SETUP", "replaced previous run's session cat %016llx with the new selection (owner %u slot %u)",
                     (unsigned long long)id, (unsigned)owner_pos, (unsigned)i);
        }
        if (!existing) {
            void* source = nullptr;
            if (!safe_by_id((void*)rc.registry, original.cats[i].id, &source) || !source) {
                for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
                return false;
            }
            void* clone = construct_cat();
            if (!clone || !catsync_deserialize_into(clone, original.cats[i].data,
                                                    original.cats[i].size)) {
                for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
                return false;
            }
            // The serializer image carries the source identity.  The registry
            // key and CatData identity must agree after the read.
            if (!mem_write((uint8_t*)clone + kCatData_SaveId, &id, sizeof(id)) ||
                !register_cat((void*)rc.registry, clone, id)) {
                for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
                return false;
            }
            log_line("SETUP", "cloned selected cat %016llx -> session cat %016llx (owner %u slot %u)",
                     (unsigned long long)original.cats[i].id, (unsigned long long)id,
                     (unsigned)owner_pos, (unsigned)i);
        }
        session_ids[i] = id;
        adopted_clear(id);                                          // this slot is a new selection now
        if (owner_pos == net_peer_pos()) catsync_note_origin(id, original.cats[i].id);   // so the settlement need not guess
    }

    if (!roster_setup_replace_party(session_ids, original.count,
                                    "local setup: replace selected cats with session IDs")) {
        for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
        return false;
    }

    out = SetupMsg{};
    out.count = original.count;
    for (uint8_t i = 0; i < out.count; ++i) {
        void* cat = nullptr;
        if (!safe_by_id((void*)rc.registry, session_ids[i], &cat) || !cat) {
            for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
            for (uint8_t k = 0; k < out.count; ++k) free(out.cats[k].data);
            out = SetupMsg{};
            return false;
        }
        uint8_t* image = nullptr;
        const uint32_t size = serialize_cat(cat, &image);
        if (!size || !image) {
            for (uint8_t j = 0; j < original.count; ++j) free(original.cats[j].data);
            for (uint8_t k = 0; k < out.count; ++k) free(out.cats[k].data);
            out = SetupMsg{};
            return false;
        }
        out.cats[i].id = session_ids[i];
        out.cats[i].size = size;
        out.cats[i].hash = fnv1a(image, size);
        out.cats[i].data = image;
    }
    // The owner table is local state, but writing it on both peers makes the
    // first map swap deterministic even before the first battle snapshot.
    for (uint8_t i = 0; i < out.count; ++i)
        lockstep_note_owner(out.cats[i].id, owner_pos);
    for (uint8_t i = 0; i < original.count; ++i) free(original.cats[i].data);
    log_line("SETUP", "prepared %u selected cats as independent session IDs for owner %u",
             out.count, (unsigned)owner_pos);
    return true;
}

// Defined with the rest of the hold machinery, below the publish path; declared
// here because catsync_shutdown drains the hold and is written above it.
static void free_pending();

// ---------------------------------------------------------------------------

void catsync_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;

    // Only the four this module calls. It used to walk all of C_COUNT, which
    // meant drift in an address belonging to some OTHER module silently turned
    // CAT SYNC off -- the wrong feature, named in the wrong log line. Same
    // convention mgmp_invsync already states.
    static const int kNeed[] = { C_SerializeCatData, C_ByteStreamDtor,
                                 C_OfstreamCtor, C_CatDataById, C_CatStatusRangeDtor };
    for (int idx : kNeed) {
        if (!addr_of_call((Call)idx)) {
            log_line("CATSYNC", "!! %s did not resolve by signature "
                                "-- cat sync is OFF", kCalls[idx].name);
            return;
        }
    }

    g.serialize     = (fn_serialize_cat)addr_of_call(C_SerializeCatData);
    g.bs_dtor       = (fn_bs_dtor)      addr_of_call(C_ByteStreamDtor);
    g.of_ctor       = (fn_ofstream_ctor)addr_of_call(C_OfstreamCtor);
    g.by_id         = (fn_cat_by_id)    addr_of_call(C_CatDataById);
    g.status_range_dtor = (decltype(g.status_range_dtor))addr_of_call(C_CatStatusRangeDtor);
    g.max_health    = (fn_cat_max_health)addr_of_call(C_CatMaxHealth);
    g.director_slot = (const void**)    addr_of_data(D_MewDirectorPtr);
    if (!g.director_slot) {
        log_line("CATSYNC", "!! the MewDirector* global did not resolve -- cat sync is OFF");
        return;
    }
    g.allocate = (decltype(g.allocate))addr_of_call(C_CatAlloc);
    g.construct = (decltype(g.construct))addr_of_call(C_CatCtor);
    g.post_load = (decltype(g.post_load))addr_of_call(C_CatPostLoad);
    g.register_cat = (decltype(g.register_cat))addr_of_data(D_CatRegister);
    g.register_parents = (decltype(g.register_parents))addr_of_call(C_CatParents);
    g.append_id = (decltype(g.append_id))addr_of_call(C_CatIdAppend);
    g.id_counter = (uint64_t*)addr_of_data(D_CatIdCounter);
    g.import_ready = g.allocate && g.construct && g.post_load && g.register_cat &&
                     g.register_parents && g.append_id && g.id_counter;
    if (!g.import_ready)
        log_line_lvl(LogLevel::Error, "CATSYNC", "!! cat import targets unresolved -- new familiars cannot be applied");
    g.resolved = true;
}

bool catsync_read_familiars(EnterNodeMsg& node) {
    ensure_state();
    const uint8_t* dir = nullptr;
    const uint64_t* ids = nullptr;
    uint32_t capacity = 0;
    if (!g.on || !mem_read(g.director_slot, &dir, sizeof(dir)) || !dir ||
        !mem_read(dir + kDir_CatFamiliars, &capacity, 4) ||
        !mem_read(dir + kDir_CatFamiliars + 4, &node.familiar_count, 4) ||
        !mem_read(dir + kDir_CatFamiliars + 8, &ids, sizeof(ids)) ||
        node.familiar_count > capacity || node.familiar_count > 64) return false;
    if (node.familiar_count && (!ids || !mem_read(ids, node.familiar_ids,
                                                node.familiar_count * sizeof(uint64_t)))) return false;
    for (uint32_t i = 0; i < node.familiar_count; ++i) {
        if (!node.familiar_ids[i] || node.familiar_ids[i] == UINT64_MAX) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (node.familiar_ids[i] == node.familiar_ids[j]) return false;
    }
    // The party's own ids, in the party's order, travel with the familiars (proto 44). Not
    // fatal when unreadable: party_count 0 makes the receiver fall back to the batch order.
    node.party_count = 0;
    uint32_t party_count = 0;
    const uint64_t* party = nullptr;
    if (mem_read(dir + kDir_CatIdCount, &party_count, 4) && party_count && party_count <= 16 &&
        mem_read(dir + kDir_CatIdData, &party, sizeof(party)) && party) {
        uint64_t ids16[16] = {};
        bool ok = mem_read(party, ids16, party_count * sizeof(uint64_t));
        for (uint32_t i = 0; ok && i < party_count; ++i) {
            if (!ids16[i] || ids16[i] == UINT64_MAX) ok = false;
            for (uint32_t j = 0; ok && j < i; ++j) if (ids16[i] == ids16[j]) ok = false;
        }
        if (ok) {
            memcpy(node.party_ids, ids16, party_count * sizeof(uint64_t));
            node.party_count = party_count;
        }
    }
    return true;
}

// WHAT IS ACTUALLY IN THE TWO LISTS (2026-09-23, and it is the instrument the last two rounds
// were missing). The client's node messages kept arriving with FOUR cats -- and all four were
// the CLIENT's own, as were the four familiar ids in the same message -- while the host kept
// reporting that it had published its own four. Both cannot be true, and the question that
// decides it is "which cat ids does the host see in the run's party list, and which in the
// familiar list". No amount of reading the loader answers that on a live run whose party
// arrived from a save, so this prints both lists, by id, at the moment the host is about to
// build a node -- the moment its publish is supposed to carry everything the client needs.
void catsync_dump_lists(const char* why) {
    ensure_state();
    if (!g.on) return;
    const uint8_t* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) {
        log_line_lvl(LogLevel::Warn, "CATSYNC", "!! LISTS (%s): the director does not resolve", why);
        return;
    }

    char party[360] = {};
    uint32_t party_count = 0;
    int off = 0;
    {
        const uint64_t* ids = nullptr;
        uint32_t cap = 0;
        if (mem_read(dir + kDir_CatIdCount, &party_count, 4) && party_count <= 64 &&
            mem_read(dir + kDir_CatIdData, &ids, sizeof(ids)) && ids) {
            for (uint32_t i = 0; i < party_count && off < (int)sizeof(party) - 24; ++i) {
                uint64_t v = 0;
                if (!mem_read(ids + i, &v, 8)) break;
                off += _snprintf_s(party + off, sizeof(party) - off, _TRUNCATE, " %016llX",
                                   (unsigned long long)v);
            }
        } else {
            party_count = 0;
        }
    }

    char fam[360] = {};
    uint32_t fam_count = 0;
    off = 0;
    {
        uint32_t cap = 0;
        const uint64_t* ids = nullptr;
        if (mem_read(dir + kDir_CatFamiliars, &cap, 4) &&
            mem_read(dir + kDir_CatFamiliars + 4, &fam_count, 4) &&
            mem_read(dir + kDir_CatFamiliars + 8, &ids, sizeof(ids)) && ids &&
            fam_count <= cap && fam_count <= 64) {
            for (uint32_t i = 0; i < fam_count && off < (int)sizeof(fam) - 24; ++i) {
                uint64_t v = 0;
                if (!mem_read(ids + i, &v, 8)) break;
                off += _snprintf_s(fam + off, sizeof(fam) - off, _TRUNCATE, " %016llX",
                                   (unsigned long long)v);
            }
        } else {
            fam_count = 0;
        }
    }

    log_line_lvl(LogLevel::Warn, "CATSYNC",
                 "!! LISTS (%s): run_cats %u [%s ] | familiars %u [%s ] | this peer is %s",
                 why, party_count, party, fam_count, fam,
                 g.is_client ? "the client" : "the host");
    catsync_dump_vectors(why);
}

// THE THREE VECTORS AS RAW FIELDS (2026-09-29). LISTS above prints ids; the promotion bug of
// the same day was a MIRROR that disagreed with the party, and the vector that decided it was
// invisible in every log. This prints {cap, count, data pointer, ids} of the party, the mirror
// and the familiars exactly as read, so a later shape problem is one line away from its cause.
void catsync_dump_vectors(const char* why) {
    ensure_state();
    if (!g.on) return;
    const uint8_t* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return;
    struct V { const char* name; uintptr_t cap; } vs[] = {
        {"party", kDir_CatIdCap}, {"mirror", kDir_MirrorCap}, {"familiars", kDir_CatFamiliars}};
    char line[900] = {};
    int off = 0;
    for (const V& v : vs) {
        uint32_t cap = 0, count = 0;
        const uint64_t* data = nullptr;
        const bool ok = mem_read(dir + v.cap, &cap, 4) && mem_read(dir + v.cap + 4, &count, 4) &&
                        mem_read(dir + v.cap + 8, &data, 8);
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "%s%s ", off ? " | " : "", v.name);
        if (!ok) { off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "UNREADABLE"); continue; }
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "cap %u count %u @%p [", cap,
                           count, (const void*)data);
        for (uint32_t i = 0; i < count && i < 16 && data; ++i) {
            uint64_t id = 0;
            if (!mem_read(data + i, &id, 8)) { off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " ??"); break; }
            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " %llx", (unsigned long long)id);
        }
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "%s ]", count > 16 ? " ..." : "");
    }
    log_line("CATSYNC", "VECTORS (%s): %s", why, line);
}

bool catsync_apply_familiars(const EnterNodeMsg& node) {
    ensure_state();
    // "CANNOT ACT" IS NOT "FAILED" -- THE SAME RULE AS THE DISABLED MODULES
    // (2026-09-22). This returned false when the import targets had not resolved,
    // which made one missing capability FATAL for the whole session: mgmp_follow
    // reads a false here as an incomplete node snapshot, latches its fault flag and
    // stops following for the rest of the run. Measured 2026-09-22: the client
    // stopped on the host's first event node and never entered the battle after it.
    //
    // Nothing to do is success, and refusing to do something is a loud log rather
    // than a stopped run -- if the two familiar sets really do differ, the battle
    // hash reports it, which is the layer that is supposed to decide this.
    if (!g.on || !g.is_client) return true;
    if (node.familiar_count > 64) {
        log_line_lvl(LogLevel::Error, "CATSYNC", "!! familiar count %u exceeds snapshot capacity", node.familiar_count);
        return false;
    }
    EnterNodeMsg before{};
    if (!catsync_read_familiars(before)) return true;
    if (before.familiar_count == node.familiar_count &&
        memcmp(before.familiar_ids, node.familiar_ids, node.familiar_count * sizeof(uint64_t)) == 0) return true;
    const uint8_t* dir = nullptr;
    void* registry = nullptr;
    const uint64_t* party = nullptr;
    uint32_t party_count = 0;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir ||
        !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry ||
        !mem_read(dir + kDir_CatIdCount, &party_count, 4) || party_count > 64 ||
        !mem_read(dir + kDir_CatIdData, &party, sizeof(party)) || !party) return false;
    for (uint32_t i = 0; i < node.familiar_count; ++i) {
        const uint64_t id = node.familiar_ids[i];
        if (!id || id == UINT64_MAX) return false;
        for (uint32_t j = 0; j < i; ++j) if (node.familiar_ids[j] == id) return false;
        for (uint32_t j = 0; j < party_count; ++j) {
            uint64_t main_id = 0;
            if (!mem_read(&party[j], &main_id, sizeof(main_id)) || main_id == id) return false;
        }
        void* cat = nullptr;
        if (!safe_by_id(registry, id, &cat) || !cat) return false;
    }
    log_line_lvl(LogLevel::Warn, "CATSYNC", "!! cat familiar membership: %u -> %u (node %016llx)",
                 before.familiar_count, node.familiar_count, (unsigned long long)node.seed0);
    for (uint32_t i = 0; i < before.familiar_count; ++i)
        log_line_lvl(LogLevel::Warn, "CATSYNC", "  before[%u]=%016llx", i, (unsigned long long)before.familiar_ids[i]);
    for (uint32_t i = 0; i < node.familiar_count; ++i)
        log_line_lvl(LogLevel::Warn, "CATSYNC", "  after[%u]=%016llx", i, (unsigned long long)node.familiar_ids[i]);
    if (!g.import_ready ||
        !replace_familiars((void*)(dir + kDir_CatFamiliars), node.familiar_ids, node.familiar_count)) {
        log_line_lvl(LogLevel::Error, "CATSYNC", "!! familiar sets differ at node %u and this"
                          " peer cannot write its list (import targets unresolved, or the"
                          " write refused) -- LOGGED, NOT FATAL: a stopped run is worse than"
                          " a divergence the battle hash is going to report anyway",
                     (unsigned)node.seed0);
        return true;
    }
    EnterNodeMsg after{};
    const bool ok = catsync_read_familiars(after) && after.familiar_count == node.familiar_count &&
                    memcmp(after.familiar_ids, node.familiar_ids, node.familiar_count * sizeof(uint64_t)) == 0;
    if (!ok)
        log_line_lvl(LogLevel::Error, "CATSYNC", "!! the familiar list did not read back as the"
                          " host's after the write -- LOGGED, NOT FATAL for the same reason");
    return true;
}

void catsync_init() {
    ensure_state();
    if (!g.on || g.announced) return;
    g.announced = true;
    log_line_lvl(LogLevel::Trace, "CATSYNC", "armed -- %s",
             g.is_client ? "the host's cats are applied here, and this peer's own "
                           "edits are published back to it"
                         : "this peer's cats are published to the client, and the "
                           "client's own edits are applied here");
}

void catsync_shutdown() {
    const uint32_t stranded = g.pend_count;
    free_pending();
    if (!g.announced) return;
    // `stranded` is the drop this module is allowed to make, so it says so:
    // a cat held for a map tick that never came is a cat this peer never got.
    // It is also the ONLY thing in this line that means something went wrong,
    // which is why it and not the line decides the severity.
    log_line_lvl(stranded ? LogLevel::Warn : LogLevel::Trace, "CATSYNC",
             "done: %u pushed, %u applied, %u unchanged, "
             "%u held (%u superseded), %u still held at exit",
             g.pushed, g.applied, g.skipped, g.deferred, g.coalesced, stranded);
    log_line_lvl(g.heal_stuck ? LogLevel::Warn : LogLevel::Trace, "CATSYNC",
             "digest: %u sent, %u received, %u difference(s) seen, %u resend(s) as owner, "
             "%u healed, %u stuck past the quick retries, %u cat image(s) dumped",
             g.digests_sent, g.digests_seen, g.mismatches, g.heals_sent, g.heals_ok,
             g.heal_stuck, g.dumps);
}

// Drop the "already sent this" cache, so the next publish sends every cat
// whatever its bytes are. For a peer that RECONNECTS: the dedupe is per-run,
// not per-peer, so without this the next node's publish tells a freshly
// arrived peer about only the cats that happened to change since the last
// node -- and it has none of the others.
void catsync_forget() {
    ensure_state();
    if (!g.on) return;
    g.settling = false;
    g.base_count = 0;
    memset(g.base_id,   0, sizeof(g.base_id));
    memset(g.base_hash, 0, sizeof(g.base_hash));
    // A forgotten session may be a NEW RUN, and the import is a once-per-run act:
    // forgetting the baseline and then refusing to import again would leave a peer that
    // rejoined without the other player's cats and with nothing in the log to say why.
    // Re-importing is idempotent -- the cats already exist and are simply re-applied.
    g.import_tried = false;
    // The digest state belongs to the run as well: a stale heal entry would keep
    // re-sending a cat of the previous run, and a stored peer digest describes it.
    g.heal_count = 0;
    g.digest_sig = 0;
    g.digest_ticks = 0;
    memset(g.peer_fresh, 0, sizeof(g.peer_fresh));
    memset(g.peer_members, 0, sizeof(g.peer_members));
}

static bool publish_impl(uint8_t target, const char* why, bool quiet_if_nothing, bool force);

bool catsync_publish(const char* why, bool quiet_if_nothing, bool force) {
    return publish_impl(kNoPeer, why, quiet_if_nothing, force);
}

bool catsync_publish_to(uint8_t peer, const char* why, bool force) {
    return publish_impl(peer, why, /*quiet_if_nothing=*/false, force);
}

static bool publish_impl(uint8_t target, const char* why, bool quiet_if_nothing, bool force) {
    ensure_state();
    if (!g.on || g.settling || !net_active()) return false;
#if defined(MGMP_WITH_SETUP)
    if (!setup_runtime_ready()) return false;
#endif
    // NOT WHILE CATS ARE HELD. A held cat is one the peer sent and this peer has
    // not applied yet, so this peer's copy of it is KNOWN to be behind -- and
    // publishing it would undo the peer's change. The host drains its hold at the
    // node entry immediately before this call, and the client's map tick skips
    // while anything is held, so this is a backstop rather than the usual path.
    //
    // AND THE REFUSAL REACHES THE CALLER. It has to: the catch-up sends this
    // push followed by an ENTERNODE that only works if the cats went out first
    // -- see the caller in mgmp_session.cpp -- and the old void-ish shape let a
    // refused push be followed by that node anyway, which left the joining peer
    // holding a batch that would never be complete.
    if (g.pend_count) { ++g.held_pub; return false; }

    RunCats rc{};
    if (!run_cats(rc)) {
        log_line("CATSYNC", "!! could not read the run's cat list -- nothing "
                            "published (%s)", why);
        return false;
    }

    // A CLIENT'S FIRST PUBLISH SEEDS THE BASELINE FROM THE RUN IT HOLDS, and it has
    // to: measured on the first run of this change (2026-09-22), the client loaded
    // the host's save, equipped something on one of its own cats, and every map tick
    // reported `2 not published (no baseline yet)` -- because the host had not
    // entered a node yet and so had never sent a cat. The equip never left.
    //
    // What made that possible is that the rule above is right in general and wrong
    // once. It exists to stop this peer from pushing its copy of a run the host owns
    // back at it -- but a client's run CAME from the host (mgmp_savefile writes it to
    // mgmp_coop.sav and redirects the load), so the state at load time is not an
    // unknown: it IS what the two agreed on. Seeding from it once says exactly that,
    // and the distinction the rule protects survives intact: anything the host
    // changed after that flush is not in this peer's copy, so those bytes still
    // equal the baseline and are still not published.
    if (g.is_client && g.base_count == 0) seed_client_baseline(rc);

    uint32_t sent = 0, same = 0, failed = 0, nobase = 0, not_mine = 0;

    // THE ORDER, NOT ONLY THE SET.
    //
    // Observed 2026-09-21: both peers entered the same node (same seed, printed
    // by both) and the host pushed 4 cats which the client applied -- and the
    // node-entry roster hashes still disagreed, and the two battle rosters came
    // out as PERMUTATIONS of each other. That matters more than it looks: the
    // control split is derived by POSITION over the roster (`split_owns(range,
    // seen++)` as the loop walks the human cats), so a permutation moves every
    // human cat to the other peer, each peer greys the bar for the cats it thinks
    // the peer owns, and the battle is unplayable from both sides.
    //
    // Four ids in enumeration order is the cheapest way to separate "the same
    // cats, different order" from "different cats" -- and those two need fixes in
    // completely different places. Printed on every publish, because the publish
    // happens once per node and this is the line that would have caught it.
    char order[192] = {};
    int  ooff = 0;

    for (uint32_t i = 0; i < rc.count; ++i) {
        uint64_t id = 0;
        if (!mem_read(&rc.ids[i], &id, sizeof(id)) || !id) continue;

        // AUTHORITY, BEFORE ANY SERIALIZING: A CAT HAS ONE EDITOR (2026-09-22).
        //
        // This is the rule the bidirectional design rests on, made executable. For
        // a cat in the human roster the owner is known -- lockstep copies the
        // control split out of the battle so it can still be answered on the map,
        // see lockstep_cat_is_mine -- and ONLY THE OWNER MAY PUBLISH: the client
        // sends its own cats, the host sends its own, and neither sends the
        // other's. A cat with no entry (an AI cat, a familiar) keeps the old
        // one-way behaviour, host-authoritative.
        //
        // Without this the two peers published competing versions of the same cat
        // and the run locked up. Measured 2026-09-22: a chest and a "?" event
        // changed cats on both sides, both peers wrote the same ids with different
        // bytes, and the next battle halted at turn 0 with `8 of 14 cat(s) differ`.
        bool mine = false;
        const bool own_known = cat_is_mine(id, mine);
        const bool my_cat    = own_known &&  mine;
        const bool peers_cat = own_known && !mine;
        // ONE EDITOR PER CAT -- AND THE HOST'S ARM OF THIS TEST WAS BACKWARDS
        // (2026-09-23, found by the two LISTS/inventory instruments after four halts).
        //
        // The rule is symmetric: each peer publishes ITS OWN cats and neither publishes the
        // other's. The client's arm did that; the host's arm tested `my_cat`, i.e. it SKIPPED
        // its own and sent the client's. Everything the last runs showed follows from that one
        // mistake and nothing else:
        //
        //   host log:  "-> 4 cat(s) changed and sent, ... 4 skipped (the other peer edits
        //               those)"  -- the 4 sent were the CLIENT's, the 4 skipped were the HOST's
        //   client log: "no baseline yet: the host has not sent this peer that cat" (forever)
        //   client node 6: 8 cats (the first node, before the control split is known, so
        //               `own_known` is false and NEITHER arm skips) then
        //   client node 15/10/4: 4 cats, all of them the client's own
        //
        // So the host's four cats were published by NOBODY, the client kept the copies it had
        // loaded at join, and every battle after the first halted with the client's copy of cat
        // 24d at 7/16 against the host's 5/20. The comment above already stated the rule; this
        // is the test that implements it.
        if (peers_cat) {
            ++not_mine;
            continue;
        }
        (void)my_cat;

        if (ooff < (int)sizeof(order) - 24)
            ooff += _snprintf_s(order + ooff, sizeof(order) - ooff, _TRUNCATE,
                                " %llx", (unsigned long long)id);

        void* cat = nullptr;
        if (!safe_by_id((void*)rc.registry, id, &cat) || !cat) { ++failed; continue; }

        uint8_t* bytes = nullptr;
        uint32_t n = serialize_cat(cat, &bytes);
        if (!n) { ++failed; continue; }

        CatDataMsg m{};
        m.id   = id;
        m.size = n;
        m.hash = fnv1a(bytes, n);
        m.data = bytes;

        // THE BASELINE RULE, asymmetric on purpose -- see the State comment. An
        // empty entry means these two have never agreed on this cat: the host must
        // send it, the client must not.
        uint64_t* base = baseline(id);
        if (!base) { ++failed; free(bytes); continue; }
        if (!*base && g.is_client) { ++nobase; free(bytes); continue; }
        if (*base == m.hash && !force) { ++same; free(bytes); continue; }

        const bool ok = (target == kNoPeer) ? net_send_catdata(m)
                                            : net_send_catdata_to(target, m);
        if (ok) {
            *base = m.hash;
            ++sent;
        } else {
            ++failed;
        }
        free(bytes);
    }

    g.pushed  += sent;
    g.skipped += same;
    // `to peer N` only when it went to one: the log is read next to the peer's
    // own "applied" line, and which peer a push was for is the whole difference
    // between the node-entry burst and the join catch-up.
    char dest[24] = {};
    if (target != kNoPeer)
        _snprintf_s(dest, sizeof(dest), _TRUNCATE, " to peer %u", (unsigned)target);
    // `not_mine` is NOT a reason to speak: the peer's cats are skipped on every tick, and counting
    // them printed one identical line per tick (2,289 in one 2026-09-29 host log).
    if (!quiet_if_nothing || sent || failed || nobase)
        log_line("CATSYNC", "-> %u cat(s) changed and sent%s, %u unchanged, %u not "
                            "published (no baseline yet: the host has not sent this "
                            "peer that cat), %u skipped (the other peer edits those)%s (%s)",
                 sent, dest, same, nobase, not_mine,
                 failed ? " -- SOME FAILED, see above" : "", why);
    // The order line belongs to the node entry -- once per node is a diagnostic, once
    // a second is noise nobody reads -- so it follows the same rule as the summary.
    if (rc.count && (!quiet_if_nothing || sent || failed))
        log_line("CATSYNC", "run cat order (%u):%s  -- compare this line against the "
                            "peer's; the control split walks the roster by POSITION, so a "
                            "different order here is a different split",
                 rc.count, order);
    return failed == 0 && nobase == 0;
}

// --- applying, and holding until it is safe to apply ------------------------

bool catsync_apply_snapshot(const CatDataMsg& m, const char* when, bool allow_create) {
    ensure_state();
    if (!g.on || g.settling || !m.data || !m.size || m.size > kMaxCatBytes) return false;
    if (fnv1a(m.data, m.size) != m.hash) {
        log_line("CATSYNC", "!! cat %016llx failed its hash -- not applied",
                 (unsigned long long)m.id);
        return false;
    }

    RunCats rc{};
    if (!run_cats(rc)) return false;

    void* cat = nullptr;
    if (!m.id || m.id == UINT64_MAX || !safe_by_id((void*)rc.registry, m.id, &cat)) return false;

    // A CAT THAT IS MINE IS NOT THE PEER'S TO SEND ME (2026-09-22). The mirror of
    // the authority rule in catsync_publish, and the fix for a race that lost real
    // upgrades: the peer marshals its snapshot, this peer edits one of its OWN cats
    // in the meantime, and the snapshot -- which is older than that edit -- used to
    // overwrite it. Measured 2026-09-22: a level-up bought in the shop was reverted
    // by exactly this, and the host never saw it at all.
    //
    // Skipped, NOT failed, and the baseline is left alone on purpose: my bytes still
    // differ from what the two last agreed on, so the next publish sends them to the
    // one peer allowed to keep them. See the authority rule.
    bool mine = false;
    if (cat_is_mine(m.id, mine) && mine) {
        log_line("CATSYNC", "kept my own cat %016llx -- the peer's copy left before my"
                            " change did, so adopting it would undo me (%s)",
                 (unsigned long long)m.id, when);
        return true;
    }

    const bool created = cat == nullptr;
    // THE ROLE GATE IS NOW A FEATURE GATE (2026-09-23). "Only a client may create" was
    // right while the only new cats were the host's arriving at a client. The 4+4 import
    // needs the mirror image: a cat the HOST has never seen, sent from the client's OWN
    // save. tune::kPartyImport is what says the session is deliberately in that shape;
    // with it off this is the old rule exactly, so a stray message still cannot
    // populate the host's registry.
    if (created && (!allow_create || !g.import_ready ||
                    (!g.is_client && !tune::kPartyImport))) {
        log_line_lvl(LogLevel::Error, "CATSYNC", "!! cat %016llx missing; import not authorized or unresolved",
                     (unsigned long long)m.id);
        return false;
    }
    uint64_t previous_hash = 0;
    if (!created) {
        uint8_t* current = nullptr;
        const uint32_t size = serialize_cat(cat, &current);
        if (!size) {
            log_line_lvl(LogLevel::Error, "CATSYNC", "!! cat %016llx could not be read before replacement",
                         (unsigned long long)m.id);
            return false;
        }
        previous_hash = fnv1a(current, size);
        const bool same = size == m.size && memcmp(current, m.data, size) == 0;
        if (!same) {
            char diff[256] = {};
            describe_diff(current, size, m.data, m.size, diff, sizeof(diff));
            log_line("CATSYNC", "cat %016llx before replacement (%s): %s",
                     (unsigned long long)m.id, when, diff);
        }
        free(current);
        // A receipt hash is not proof that the local cat still holds those bytes.
        if (same) {
            if (uint64_t* base = baseline(m.id)) *base = m.hash;
            return true;
        }
    }

    uint64_t* rng = rng_global_stream();
    uint64_t before[4] = {};
    if (!rng) return false;
    memcpy(before, rng, sizeof(before));
    if (created) {
        cat = construct_cat();
        memcpy(rng, before, sizeof(before));
        if (!cat || !mem_write((uint8_t*)cat + kCatData_SaveId, &m.id, sizeof(m.id))) return false;
    }

    // A session cat read in at SETUP over an object a previous run left behind is rebuilt first, so every
    // peer's copy starts from the same blank state a new one does (see reset_cat). Only at setup: nothing
    // is live then, and a mid-run update keeps its object as it is.
    const bool rebuild = !created && when && strcmp(when, "all-player setup exchange") == 0 &&
                         m.id >= 0x70000000ull && m.id < 0x80000000ull;
    if (rebuild) {
        if (!reset_cat(cat) || !mem_write((uint8_t*)cat + kCatData_SaveId, &m.id, sizeof(m.id))) return false;
    }
    bool ok = catsync_deserialize_into(cat, m.data, m.size);
    uint64_t actual_hash = 0;
    if (ok) {
        uint8_t* actual = nullptr;
        const uint32_t size = serialize_cat(cat, &actual);
        if (size) actual_hash = fnv1a(actual, size);
        ok = size == m.size && actual && memcmp(actual, m.data, size) == 0;
        char diff[256] = "unreadable";
        if (!ok && actual) describe_diff(actual, size, m.data, m.size, diff, sizeof(diff));
        free(actual);
        if (!ok)
            log_line_lvl(LogLevel::Error, "CATSYNC",
                         "!! cat %016llx readback mismatch: wanted %016llx/%u bytes, got %016llx/%u -- NOT acknowledged; %s",
                         (unsigned long long)m.id, (unsigned long long)m.hash, m.size,
                         (unsigned long long)actual_hash, size, diff);
    }
    if (ok && created) ok = register_cat((void*)rc.registry, cat, m.id);
    else if (ok && rebuild) ok = post_load_cat(cat);
    memcpy(rng, before, sizeof(before));
    log_line_lvl(ok ? LogLevel::Warn : LogLevel::Error, "CATSYNC",
                 "!! cat %016llx replacement (%s): %016llx -> %016llx, %s; RNG preserved",
                 (unsigned long long)m.id, when, (unsigned long long)previous_hash,
                 (unsigned long long)actual_hash, ok ? "full image verified" : "INCOMPLETE; copy untrustworthy");
    if (ok) {
        ++g.applied;
        if (uint64_t* base = baseline(m.id)) *base = m.hash;
        log_line("CATSYNC", "<- applied cat %016llx (%u bytes, %s)",
                 (unsigned long long)m.id, m.size, when);
    }
    return ok;
}

static void free_pending() {
    for (uint32_t i = 0; i < g.pend_count; ++i) {
        free(g.pend_data[i]);
        g.pend_data[i] = nullptr;
        g.pend_size[i] = 0;
    }
    g.pend_count = 0;
}

static bool stash_pending(const CatDataMsg& m) {
    uint8_t* copy = (uint8_t*)malloc(m.size);
    if (!copy) return false;
    memcpy(copy, m.data, m.size);

    for (uint32_t i = 0; i < g.pend_count; ++i) {
        if (g.pend_id[i] != m.id) continue;
        free(g.pend_data[i]);
        g.pend_data[i] = copy;
        g.pend_size[i] = m.size;
        g.pend_hash[i] = m.hash;
        ++g.coalesced;
        return true;
    }
    if (g.pend_count >= State::kMaxCats) { free(copy); return false; }

    g.pend_id[g.pend_count]   = m.id;
    g.pend_hash[g.pend_count] = m.hash;
    g.pend_data[g.pend_count] = copy;
    g.pend_size[g.pend_count] = m.size;
    ++g.pend_count;
    return true;
}

// Deferral is only correct while something is guaranteed to drain the queue. The
// client's map-follow tick drains before it enters a node; the HOST now drains at
// its own node entry (see mgmp_follow), which is where the client's cats arrive.
// Declining instead would DROP them -- the sender dedupes against what it sent, so
// a declined cat is gone for the run -- which is the 2026-08-26 bug in mirror
// image. So the host holds too; the same rule and helper shape as mgmp_invsync.
static bool defer_applies() { return g.is_client ? config().net_follow : true; }

// ---------------------------------------------------------------------------
// THE 4+4 IMPORT: the other player's cats, out of THEIR own save (2026-09-23)
//
// WHY. The 4+4 shape is "each player's OWN cats, in one shared run". A peer that
// loads the host's save owns the host's cats, which is fine for 2+2 and cannot grow
// into 4+4. So a peer has to be handed the other half -- and it is handed it as the
// bytes the game itself produced, which is why this is short: the receiving half
// already exists (catsync_apply_snapshot's create path, RNG preserved, the same path
// familiars arrive through).
//
// THE FILE is written by tools/save_dump.py --export:
//     char magic[8] "MGMPCAT1"; u32 count; count x { u64 id; u32 size; u8 data[size] }
// where `data` is the DECOMPRESSED cat record. Nothing is re-encoded here, so there
// is no private format to keep in step with the game's.
//
// ONCE PER RUN, in front of the publish on the client's map tick -- so the cats that
// arrive here leave again on the same tick, marked to be sent. A HOST does not call
// that tick (mgmp_follow is the client's module), so a host-side import would need its
// own trigger; none is written, because nothing needs one yet.
//
// WHAT IT DOES NOT CLAIM: that the ids are free. The export carries the ids the cats
// have in THEIR save; two saves can use the same id for different cats, and this does
// not rename anything. It is the first thing to check if an imported cat looks wrong.

// A baseline value that no real hash can be. The publish rule is "send when the bytes
// differ from the baseline", so marking an imported cat with this makes the very next
// publish send it -- which is required, because an EMPTY baseline is deliberately
// skipped on a client (see the State comment) and the peer on the other side has not
// seen this cat at all.
static constexpr uint64_t kSendMe = 0xFFFFFFFFFFFFFFFFull;

static void mark_send(uint64_t id) {
    uint64_t* b = baseline(id);
    if (b) *b = kSendMe;
}

// ---------------------------------------------------------------------------
// THE DIGEST, AND HEALING FROM THE OWNER (2026-09-29, proto 43)
//
// WHY. Every cat sync this module does is push-on-change: a peer sends what it
// believes changed, measured against a baseline of what it believes the peers hold.
// Nothing ever checked that belief. A push that was refused ("kept my own cat"),
// lost with a dropped link, or undone by the game afterwards left the copies apart
// with every log line saying the sync was fine -- until the next battle's turn-0 hash
// halted the run. With 2-4 players and 1-4 cats each there are many more such copies.
//
// WHAT. Every peer lists its cats as (id, hash, size) about once a second on the map.
// Each peer compares every other peer's list with its own; for a cat that differs the
// OWNER decides (authority_of), and only the owner acts: it re-sends its copy. So a
// drifted cat is overwritten from the one peer allowed to edit it, rather than
// halting, and a peer that is not the owner never pushes its version of someone else's
// cat -- the one-editor rule, unchanged.
//
// BOUNDED. A difference is only answered when the same pair of hashes is seen in two
// consecutive digests (a push in flight is not a divergence), four quick resends, then
// one per ~16 digests with an Error line naming the cat. It is never a stop.

namespace {

constexpr uint32_t kDigestEvery = 10;   // publish cycles of 6 frames: about a second
constexpr uint32_t kQuickTries  = 4;
constexpr uint32_t kSlowEvery   = 16;

// Who decides what cat `id` looks like -- with the SAME precedence cat_is_mine uses,
// so the peer this names is always a peer whose publish is allowed to send it:
//   the control split says it is mine          -> me
//   the split says it is not mine              -> the noted owner, else the other
//                                                 peer of two, else "not me"
//   no split: the setup/import owner notes     -> that owner
//   nothing known                              -> the host
uint8_t authority_of(uint64_t id) {
    const uint8_t me = net_peer_pos(), n = net_peer_count();
    uint8_t owner = kNoPeer;
    const bool noted = lockstep_owner_pos(id, owner) && owner < n;
    bool mine = false;
    if (lockstep_cat_is_mine(id, mine)) {
        if (mine) return me;
        if (noted && owner != me) return owner;
        if (n == 2) return (uint8_t)(1 - me);
        return kNoPeer;
    }
    return noted ? owner : 0;
}

State::Heal* heal_find(uint8_t peer, uint64_t id, bool create) {
    for (uint32_t i = 0; i < g.heal_count; ++i)
        if (g.heal[i].peer == peer && g.heal[i].id == id) return &g.heal[i];
    if (!create || g.heal_count >= State::kMaxCats) return nullptr;
    State::Heal& h = g.heal[g.heal_count++];
    h = State::Heal{};
    h.peer = peer;
    h.id = id;
    return &h;
}

void heal_drop(State::Heal* h) {
    *h = g.heal[--g.heal_count];
}

bool build_digest(const RunCats& rc, CatDigestMsg& out) {
    out.count = 0;
    for (uint32_t i = 0; i < rc.count && out.count < kCatDigestMax; ++i) {
        const uint64_t id = rc.all_ids[i];
        void* cat = nullptr;
        if (!safe_by_id((void*)rc.registry, id, &cat) || !cat) continue;
        uint8_t* bytes = nullptr;
        const uint32_t n = serialize_cat(cat, &bytes);
        if (!n) continue;
        out.ids[out.count]    = id;
        out.hashes[out.count] = fnv1a(bytes, n);
        out.sizes[out.count]  = n;
        ++out.count;
        free(bytes);
    }
    return out.count != 0;
}

int digest_find(const CatDigestMsg& d, uint64_t id) {
    for (uint32_t i = 0; i < d.count; ++i) if (d.ids[i] == id) return (int)i;
    return -1;
}

// The membership half: which cats one side lists and the other does not. Not healed here
// -- membership belongs to the roster (the host's node snapshot is adopted at node entry)
// -- but said, once per change, because it is the first thing to read when a later battle
// counts a different number of human cats.
void compare_members(uint8_t from, const CatDigestMsg& mine, const CatDigestMsg& theirs) {
    char only_here[200] = {}, only_there[200] = {};
    int ho = 0, to = 0;
    uint64_t sig = 0;
    for (uint32_t i = 0; i < mine.count; ++i)
        if (digest_find(theirs, mine.ids[i]) < 0) {
            sig = sig * 31 + mine.ids[i];
            if (ho < (int)sizeof(only_here) - 20)
                ho += _snprintf_s(only_here + ho, sizeof(only_here) - ho, _TRUNCATE, " %llx",
                                  (unsigned long long)mine.ids[i]);
        }
    for (uint32_t i = 0; i < theirs.count; ++i)
        if (digest_find(mine, theirs.ids[i]) < 0) {
            sig = sig * 37 + theirs.ids[i] + 1;
            if (to < (int)sizeof(only_there) - 20)
                to += _snprintf_s(only_there + to, sizeof(only_there) - to, _TRUNCATE, " %llx",
                                  (unsigned long long)theirs.ids[i]);
        }
    if (sig == g.peer_members[from]) return;
    g.peer_members[from] = sig;
    if (!sig) {
        log_line("CATSYNC", "digest: peer %u lists the same %u cat(s) as this peer again",
                 (unsigned)from, (unsigned)mine.count);
        return;
    }
    log_line_lvl(LogLevel::Warn, "CATSYNC",
                 "!! digest: peer %u lists %u cat(s), this peer %u -- only here [%s ] only there [%s ]"
                 " (membership is the roster's; the node snapshot corrects it at the next node)",
                 (unsigned)from, (unsigned)theirs.count, (unsigned)mine.count, only_here, only_there);
    catsync_dump_vectors("digest membership differs");
}

// Returns true when this peer marked at least one cat to be re-sent to `from`.
bool compare_digest(uint8_t from, const RunCats& rc, const CatDigestMsg& mine) {
    const CatDigestMsg& theirs = g.peer_digest[from];
    const uint8_t me = net_peer_pos();
    compare_members(from, mine, theirs);

    bool resend = false, dumped_vectors = false;
    for (uint32_t i = 0; i < theirs.count; ++i) {
        const uint64_t id = theirs.ids[i];
        const int j = digest_find(mine, id);
        if (j < 0) continue;
        if (theirs.hashes[i] == mine.hashes[j] && theirs.sizes[i] == mine.sizes[j]) {
            if (State::Heal* h = heal_find(from, id, false)) {
                if (h->tries) {
                    ++g.heals_ok;
                    log_line_lvl(LogLevel::Warn, "CATSYNC",
                                 "digest: cat %016llx now agrees with peer %u (%016llx) after %u resend(s)"
                                 " from its owner", (unsigned long long)id, (unsigned)from,
                                 (unsigned long long)mine.hashes[j], h->tries);
                } else if (h->said) {
                    log_line("CATSYNC", "digest: cat %016llx agrees with peer %u again (%016llx)",
                             (unsigned long long)id, (unsigned)from, (unsigned long long)mine.hashes[j]);
                }
                heal_drop(h);
            }
            continue;
        }

        State::Heal* h = heal_find(from, id, true);
        if (!h) continue;
        if (h->theirs == theirs.hashes[i] && h->mine == mine.hashes[j]) {
            ++h->seen;
        } else {
            h->theirs = theirs.hashes[i];
            h->mine   = mine.hashes[j];
            h->seen   = 1;
            h->said   = false;
        }
        const uint8_t auth = authority_of(id);
        if (!h->said) {
            h->said = true;
            ++g.mismatches;
            char who[64] = {};
            if (auth == me)
                _snprintf_s(who, sizeof(who), _TRUNCATE, "THIS peer (%u) -- re-sends if it persists", (unsigned)me);
            else if (auth == from)
                _snprintf_s(who, sizeof(who), _TRUNCATE, "peer %u -- waiting for it to re-send", (unsigned)from);
            else if (auth == kNoPeer)
                _snprintf_s(who, sizeof(who), _TRUNCATE, "another peer");
            else
                _snprintf_s(who, sizeof(who), _TRUNCATE, "peer %u", (unsigned)auth);
            log_line_lvl(LogLevel::Warn, "CATSYNC",
                         "!! digest: cat %016llx differs from peer %u -- here %016llx/%u bytes, there"
                         " %016llx/%u bytes; the owner decides: %s",
                         (unsigned long long)id, (unsigned)from, (unsigned long long)mine.hashes[j],
                         mine.sizes[j], (unsigned long long)theirs.hashes[i], theirs.sizes[i], who);
            void* cat = nullptr;
            uint8_t* bytes = nullptr;
            uint32_t n = 0;
            if (safe_by_id((void*)rc.registry, id, &cat) && cat && (n = serialize_cat(cat, &bytes)) != 0) {
                dump_cat_image(id, bytes, n, "the digest shows this cat differing");
                free(bytes);
            }
            if (!dumped_vectors) { dumped_vectors = true; catsync_dump_vectors("digest difference"); }
        }
        if (auth != me || h->seen < 2) continue;
        if (h->tries >= kQuickTries && (h->seen % kSlowEvery) != 0) continue;

        mark_send(id);
        ++h->tries;
        ++g.heals_sent;
        resend = true;
        log_line_lvl(LogLevel::Warn, "CATSYNC",
                     "-> digest heal: re-sending cat %016llx to peer %u as its owner (attempt %u,"
                     " their copy %016llx, ours %016llx)", (unsigned long long)id, (unsigned)from,
                     h->tries, (unsigned long long)h->theirs, (unsigned long long)h->mine);
        if (h->tries == kQuickTries) {
            ++g.heal_stuck;
            log_line_lvl(LogLevel::Error, "CATSYNC",
                         "!! digest heal: cat %016llx still differs on peer %u after %u resends --"
                         " its copy does not take ours (look for 'kept my own cat' or 'readback"
                         " mismatch' in peer %u's log, and diff the mgmp_catdump_*.bin files)."
                         " Retrying every ~%u s; the run is not stopped",
                         (unsigned long long)id, (unsigned)from, kQuickTries, (unsigned)from, kSlowEvery);
        }
    }
    return resend;
}

void digest_cycle() {
    // Behind on something the peer sent: this peer's own copies are known to be stale,
    // so neither its list nor its comparison means anything yet.
    if (g.pend_count) return;
    RunCats rc{};
    if (!run_cats(rc)) return;
    CatDigestMsg mine{};
    if (!build_digest(rc, mine)) return;
    if (net_send_catdigest(mine)) ++g.digests_sent;

    const uint8_t me = net_peer_pos();
    for (uint8_t p = 0; p < State::kDigestPeers; ++p) {
        if (!g.peer_fresh[p] || p == me) continue;
        g.peer_fresh[p] = false;
        if (!compare_digest(p, rc, mine)) continue;
        // The host reaches every client directly. A client reaches only the host, which
        // relays CATDATA on to the others -- so a client owner heals by broadcasting.
        const bool ok = g.is_client
            ? catsync_publish("digest heal (broadcast, the host relays)", false, false)
            : catsync_publish_to(p, "digest heal", false);
        if (!ok)
            log_line_lvl(LogLevel::Warn, "CATSYNC",
                         "!! digest heal for peer %u did not go out complete -- retried at the"
                         " next digest", (unsigned)p);
    }
}

} // namespace

void catsync_on_digest(uint8_t from, const CatDigestMsg& m) {
    ensure_state();
    if (!g.on || from >= State::kDigestPeers || from == net_peer_pos()) return;
    if (m.count > kCatDigestMax) return;
    g.peer_digest[from] = m;
    g.peer_fresh[from]  = true;
    ++g.digests_seen;
}

// `mgmp_import.bin`, next to the mgmp.dll that is running. Both instances have their
// own copy of that directory, so each peer's import file is its own by construction --
// which is exactly the property a two-save test needs.
static bool default_import_path(char* out, size_t cap) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(const void*)&default_import_path, &self) || !self)
        return false;
    wchar_t wpath[MAX_PATH] = {};
    if (!GetModuleFileNameW(self, wpath, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(wpath, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    char dir[MAX_PATH * 2] = {};
    if (!WideCharToMultiByte(CP_ACP, 0, wpath, -1, dir, (int)sizeof(dir), nullptr, nullptr))
        return false;
    _snprintf_s(out, cap, _TRUNCATE, "%smgmp_import.bin", dir);
    return true;
}

static bool import_file(const char* path) {
    char resolved[MAX_PATH * 2] = {};
    if (!path || !*path) {
        if (!default_import_path(resolved, sizeof(resolved))) return false;
        path = resolved;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;   // no file: no noise

    char magic[8] = {};
    uint32_t count = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "MGMPCAT1", 8) != 0 ||
        fread(&count, 4, 1, f) != 1 || !count || count > 64) {
        fclose(f);
        log_line_lvl(LogLevel::Error, "CATSYNC",
                     "!! %s is not an MGMPCAT1 import file -- nothing imported", path);
        return false;
    }

    log_line_lvl(LogLevel::Warn, "CATSYNC",
                 "!! IMPORT: taking %u cat(s) from %s into THIS run (the 4+4 shape:"
                 " another player's own cats). The bytes go through the game's own"
                 " deserializer and the ids through the run's party list.", count, path);

    uint64_t ids[64] = {};
    uint32_t imported = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t id = 0;
        uint32_t size = 0;
        if (fread(&id, 8, 1, f) != 1 || fread(&size, 4, 1, f) != 1) break;
        if (!size || size > kMaxCatBytes) break;
        uint8_t* buf = (uint8_t*)malloc(size);
        if (!buf) break;
        if (fread(buf, 1, size, f) != size) { free(buf); break; }

        CatDataMsg m{};
        m.id   = id;
        m.size = size;
        m.hash = fnv1a(buf, size);
        m.data = buf;

        if (catsync_apply_snapshot(m, "the import file", /*allow_create=*/true)) {
            mark_send(id);              // AFTER the apply, which sets the baseline
            ids[imported++] = id;
        }
        free(buf);
    }
    fclose(f);

    if (!imported) {
        log_line_lvl(LogLevel::Error, "CATSYNC",
                     "!! IMPORT: none of the %u cat(s) in %s were taken", count, path);
        return false;
    }

    // REMEMBER WHAT WAS IMPORTED, so the run can be checked against it later. See
    // reassert_import for why the check has to exist.
    for (uint32_t i = 0; i < imported; ++i) g.import_id[i] = ids[i];
    g.import_n = imported;

    // WHERE THE IMPORTED CATS GO, and the switch is the whole point (T1, 2026-09-23).
    // The party is an exactly-four thing in the engine (six cats in it crash the map
    // layer with an access violation at exe+0x99A792, identical on both peers), so the
    // shape under test puts this peer's own cats in the party and the OTHER peer's cats
    // in the familiars list -- the engine's own slot for a cat that cannot join.
    // TELL LOCKSTEP WHOSE THESE ARE (2026-09-23, 4+4 step B). These ids came out of THIS
    // peer's own save, so their owner is this peer's own SESSION POSITION -- absolute,
    // not "mine": two peers that each noted their own cats as "mine" would produce
    // mirrored orders and hand each other their cats. See lockstep_note_owner.
    for (uint32_t i = 0; i < imported; ++i)
        lockstep_note_owner(ids[i], (uint8_t)net_peer_pos());

    const bool listed = tune::kImportToFamiliars
        ? roster_add_familiars(ids, imported, "the 4+4 import")
        : roster_add_ids(ids, imported, "the 4+4 import");
    log_line_lvl(listed ? LogLevel::Warn : LogLevel::Error, "CATSYNC",
                 "!! IMPORT: %u cat(s) in, %s -- %s", imported,
                 listed ? (tune::kImportToFamiliars ? "and filed as FAMILIARS"
                                                    : "and added to the run's party list")
                        : (tune::kImportToFamiliars
                               ? "but NOT filed as familiars (see ROSTER above)"
                               : "but NOT added to the run's party list (see ROSTER above)"),
                 listed ? (tune::kImportToFamiliars
                               ? "the party stays at four; the familiars carry them"
                               : "the next map publish offers them to the other peer")
                        : "the other peer will not hear about them");
    return listed;
}

// Put the imported ids BACK if the run no longer has them (2026-09-23).
//
// WHY THIS IS NEEDED, from the first B-group run rather than from theory: the peer
// broadcast its agreed list -- four ids, computed before it had ever seen the import --
// and that message arrived AFTER this peer had imported. The client's own apply path
// did what it is told to do and wrote four, the client's next publish then offered four
// (which replaced the peer's stored picks), and from that point neither side could
// produce the eight again: the peer merges "its list + the other peer's picks", and the
// picks were now the short list.
//
// The file is the record of what belongs in this run, so the run is compared against it.
// Re-adding is idempotent, and a run that already agrees logs nothing.
static void reassert_import() {
    RunCats rc{};
    if (!run_cats(rc)) return;

    uint64_t want[64] = {};
    uint32_t want_n = 0;
    for (uint32_t i = 0; i < g.import_n && want_n < 64; ++i) {
        bool have = false;
        for (uint32_t j = 0; j < rc.count && !have; ++j) have = (rc.all_ids[j] == g.import_id[i]);
        if (!have) want[want_n++] = g.import_id[i];
    }
    if (!want_n) return;

    // In T1 mode the imported cats are supposed to be absent from the party -- they are
    // familiars -- so "the import is not in the run any more" is the expected state, not
    // a lost import to put back. The check stays for the party mode it was written for.
    if (tune::kImportToFamiliars) return;

    log_line_lvl(LogLevel::Warn, "CATSYNC",
                 "!! IMPORT: %u imported cat(s) are NOT in the run any more -- the peer's"
                 " agreed list was computed before it saw them, so it wrote the old list"
                 " over the import. Putting them back and re-marking them to be sent.",
                 want_n);
    for (uint32_t i = 0; i < want_n; ++i) mark_send(want[i]);
    roster_add_ids(want, want_n, "the 4+4 import (put back)");
}

// THE CLIENT'S SIDE OF THE SAME PUSH, on the map, on a timer.
//
// The host's edits reach the client at the node boundary because the host
// publishes there -- and the client's own publish happens there too, but a node
// boundary on the CLIENT is a node the host has already entered, so anything sent
// then arrives too late to be in that battle. The case that needs covering is the
// one the design asks for: this player equips something on its own cat while the
// run is standing on the map, and the host must have it BEFORE it clicks the next
// node. Hence a tick, on the map, throttled.
//
// Only the client needs it. The host's own "entering a node" publish already
// covers the host's edits, because the host is the peer that decides when to enter.
//
// Throttled because deciding whether anything changed means serializing every cat,
// and this runs at frame rate. A second is far finer than a person edits and far
// coarser than a frame. Skipped while cats are HELD: this peer's copies are then
// known to be behind the peer's -- see the guard in catsync_publish.
void catsync_map_tick() {
    ensure_state();
    if (!g.on || g.settling) return;
#if defined(MGMP_WITH_SETUP)
    if (!setup_runtime_ready()) return;
#endif

    // THE IMPORT RUNS FIRST, ONCE PER RUN (2026-09-23). In front of the publish below
    // on purpose: what it takes in is marked to be sent, and this same tick is what
    // sends it. Guarded so it can never repeat within a run -- see catsync_forget for
    // the other half.
    if (tune::kPartyImport && !tune::kLocalSetup && !g.import_tried) {
        // ONLY A SUCCESS CONSUMES THE ONCE-PER-RUN GUARD (2026-09-23). Setting it
        // BEFORE the call was wrong and cost a control experiment: a session that
        // started with no import file marked the run as imported, so the file
        // appearing afterwards -- exactly the sequence a control test produces -- was
        // never read, and the run stayed at four cats while the log said nothing at
        // all. A failure is not an answer, so the question stays open. The cost is one
        // fopen per map tick when there is no file, and that tick is throttled to
        // ~10/s and silent on failure.
        if (import_file(nullptr)) g.import_tried = true;
    }

    // AND CHECK IT IS STILL TRUE, throttled like the publish below. Silent unless
    // something actually had to be put back; see reassert_import for the measurement
    // that made this necessary.
    if (tune::kPartyImport && g.import_n && ++g.reassert_ticks >= 30) {
        g.reassert_ticks = 0;
        reassert_import();
    }

    // AND THE HOST TICKS TOO (2026-09-23, the user's request): "check continuously on the map
    // whether the two peers' cats agree, and reconcile the moment they do not". This used to
    // return here on the host, on the argument that its node-entry publish already covers the
    // same ground. It does not. The node entry is the very moment a battle is built, so a cat
    // that changed on the host could only reach the client inside the node snapshot -- applied
    // in the same instant the battle is constructed, which is what left the client with a second
    // ability object for that cat and its passive played twice (the duplicated APPLY pair, and
    // the doubled scene behaviour the user saw). A tick on both peers puts every change on the
    // map instead, where catsync already drains: off the battle screen, and long before any node
    // is built. The publish is the check the user asked for, per cat: identical to the last
    // agreed bytes means nothing is sent, different means the peer is told at once.
    //
    // SIX FRAMES, NOT SIXTY (2026-09-22). A second was fine while this peer's edits
    // were the exception; now that each peer edits its OWN cats on the map, this tick
    // is what stands between an edit and the peer knowing about it, and the node
    // entry on the other side is what closes that window -- measured 2026-09-22, a
    // shop upgrade made inside it was lost. A run holds a handful of cats, so the
    // serialize is cheap; ~100 ms is far below anything a person does and far above
    // a frame.
    constexpr uint32_t kPublishFrames = 6;
    if (++g.map_ticks < kPublishFrames) return;
    g.map_ticks = 0;

    // AND TAKE THE PEER'S CATS HERE TOO, EVERY NODE-MINUS-ONE (2026-09-23, the user's
    // request, and the fix for a measured hole).
    //
    // Measured: the host published its own four cats the moment its level-up choice landed and
    // again, forced and full, at its node entry -- and the client's log contained NO apply line
    // for any of them. The reason is in catsync_on_message's hold rule: a cat arriving while
    // "a battle is in progress" is held, and lockstep_in_battle() stays TRUE after a fight (a
    // fact measured earlier tonight for the party swap), so everything the host sent while the
    // client was still on its level-up screen or walking the map sat in the pending list. The
    // client only drained it at its own node entry -- and by then the host had long since built
    // the battle out of its own (correct) copies while the client built out of stale ones:
    //
    //   roster composition (index:hp/maxhp): host ... 7:5/20 ...   client ... 7:7/16 ...
    //   !! HALT at turn 0: turn 0 hash mismatch (rng )
    //
    // The map tick is off the battle screen by definition and it already runs once a second, so
    // it is the one place a peer can catch up on the peer's edits BEFORE the node it is about to
    // enter is built -- which is exactly what "sync both peers' cats before every node" means in
    // this architecture. Applying before publishing also keeps the publish honest: a cat just
    // applied matches the baseline and is not echoed back.
    catsync_apply_pending("the map tick, before this peer's own publish");

    // Quiet when there is nothing to say: this runs once a second, and the node entry
    // is the caller that is expected to report even an empty publish.
    catsync_publish("the map tick", /*quiet_if_nothing=*/true);

    // AND THEN CHECK THAT IT ACTUALLY LANDED, about once a second: the digest. After
    // the publish on purpose, so what this peer lists is what it has just sent.
    if (++g.digest_ticks >= kDigestEvery) {
        g.digest_ticks = 0;
        digest_cycle();
    }
}

void catsync_apply_pending(const char* why) {
#if defined(MGMP_WITH_SETUP)
    if (!setup_runtime_ready()) return;
#endif
    ensure_state();
    if (!g.on || !g.pend_count) return;

    const uint32_t n = g.pend_count;
    for (uint32_t i = 0; i < n; ++i) {
        CatDataMsg m{};
        m.id   = g.pend_id[i];
        m.hash = g.pend_hash[i];
        m.size = g.pend_size[i];
        m.data = g.pend_data[i];
        catsync_apply_snapshot(m, why);
    }
    free_pending();
    g.held_in_battle = false;   // re-arms the line for the next battle
}

void catsync_on_message(const CatDataMsg& m) {
    if (g.settling) return;
#if defined(MGMP_WITH_SETUP)
    if (!setup_runtime_ready()) return; // setup uses its own validated snapshots
#endif
    ensure_state();
    if (!g.on) return;
    // BOTH ROLES APPLY NOW (2026-09-22). This used to refuse on the host -- "both
    // peers configured as host?" -- because only the client ever received a cat.
    // Now that each peer publishes its own edits, a cat arriving at the host is
    // ordinary, and it is held and applied exactly the way the client's is. The
    // misconfiguration that branch named is caught by the session handshake, which
    // will not pair two hosts.
    if (!m.data || !m.size) {
        // AND SAY SO (2026-09-23). This line used to be silent, which is why a run showed ZERO
        // "CATDATA arrived" receipts next to a client that was plainly receiving MSG_CATDATA
        // frames (34 in one run) -- every one of them arrived with id=0 and no payload, dropped
        // here without a word. Whether that is an empty frame or a misaligned one is the whole
        // question, so the fields are printed rather than assumed.
        log_line_lvl(LogLevel::Warn, "CATSYNC",
                     "!! empty cat frame dropped: id=%016llX size=%u hash=%08X data=%p",
                     (unsigned long long)m.id, m.size, m.hash, (const void*)m.data);
        return;
    }

    // SAY EVERY ARRIVAL (2026-09-23). Measured: the host published its own four cats with
    // "(a level-up choice landed on this peer's own cat)" and even applied the client's four,
    // while the client's log contained NO "applied cat" line for the host's four at all -- only
    // "kept my own cat ...", which is its correct no-revert rule for its OWN cats. Two readings,
    // and nothing in the log told them apart: the push never arrived, or it arrived and was
    // never applied. This line is the receipt; the apply/kept lines that follow it say what
    // happened next. One line per distinct cat id, so a busy map tick cannot flood it.
    {
        static uint64_t said[16] = {};
        static uint32_t said_n  = 0;
        bool fresh = true;
        for (uint32_t i = 0; i < said_n; ++i) if (said[i] == m.id) fresh = false;
        if (fresh && said_n < 16) {
            said[said_n++] = m.id;
            log_line_lvl(LogLevel::Warn, "CATSYNC",
                         "<- CATDATA cat %016llX arrived: %u byte(s), hash %08X -- held while a"
                         " battle is up, applied at the next node entry; if no apply or kept line"
                         " follows, this is where the divergence starts",
                         (unsigned long long)m.id, m.size, m.hash);
        }
    }

    // NOT WHILE A BATTLE THIS PEER IS ALREADY IN IS RUNNING -- but HELD, never
    // dropped. The difference cost a whole run on 2026-08-26 and is the reason
    // this branch is written twice over.
    //
    // Measured 2026-08-26: a client that dropped its socket mid-fight and
    // pressed `join` had four cats pushed at it by the host's reconnect
    // catch-up and applied them on the spot -- and a cat visibly gained a
    // shield with no action behind it. The catch-up is right to send them (a
    // peer whose process restarted has no run at all and needs every one), but
    // applying them into a live fight writes cat state that the battle's
    // Character objects derive from, outside the deterministic stream the
    // per-turn hash covers. It is the "is CatData written during a battle"
    // question this project wrote down as the first thing to check -- and the
    // answer turned out to be that WE write it.
    //
    // A peer that only lost its socket already has the right cats: it never
    // left the run. Declining is the same rule mgmp_savefile already applies
    // for the same reason -- see savefile_on_message refusing once `applied` is
    // set. A genuinely fresh joiner has no snapshot, so this does not fire for
    // the mid-fight join case that needs the data.
    //
    // What the original version got wrong is what happens NEXT. It returned,
    // and the host dedupes against the last hash it sent, so the push was gone
    // for the rest of the run. Worse, lockstep_in_battle() is "a roster has
    // been snapshotted and not yet replaced", which stays true on the map
    // screen BETWEEN battles -- so from the first fight onwards this peer
    // refused every ordinary per-node cat push. Measured 2026-08-26: 0 cats
    // applied over 22 turns and three map nodes, the two runs forked, and the
    // next battle opened with a turn-0 state-only mismatch.
    //
    // So: hold, and apply at the map-follow tick, exactly where the inventory
    // already lands. That is still before EnterNode, so it is in time for the
    // battle or shop the node opens, and it is off the battle screen, so it
    // never writes into a live fight.
    if (!lockstep_in_battle()) {
        g.declined_in_battle = false;   // re-arms the line for the next battle
    } else if (defer_applies()) {
        if (!stash_pending(m)) {
            log_line("CATSYNC", "!! could not hold cat %016llx for the map tick "
                                "(%u already held) -- applying it here instead, "
                                "into a live battle",
                     (unsigned long long)m.id, g.pend_count);
            catsync_apply_snapshot(m, "on arrival, deferral failed");
            return;
        }
        ++g.deferred;
        if (!g.held_in_battle) {
            g.held_in_battle = true;
            log_line("CATSYNC", "holding cat pushes while a battle is in progress "
                                "-- they will be applied on the map, just before "
                                "this peer enters the next node");
        }
        return;
    } else {
        // net_follow off: nothing would ever drain the queue, so the old
        // behaviour is all that is available. This peer is driving its own map.
        if (!g.declined_in_battle) {
            g.declined_in_battle = true;
            log_line("CATSYNC", "declining cat pushes while a battle is in progress "
                                "-- net_follow is off, so nothing would ever apply "
                                "a held one. This peer's cats will DRIFT.");
        }
        return;
    }

    catsync_apply_snapshot(m, "on arrival");
}

} // namespace mgmp
