#include <chrono>
#include <thread>
#include "mgmp_catsync.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_bytestream.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_lockstep.h"
#include "mgmp_i18n.h"
namespace mgmp { int seat_toasts = 0; }
#include "mgmp_roster.h"
#include "mgmp_rng.h"
#include "mgmp_mem.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "mgmp_gameconsole.h"
#include <map>
#include <string>
#include <vector>

using namespace mgmp;
namespace {
Config cfg;
unsigned checks = 0, failures = 0, post_loads = 0, reads = 0, destroyed_statuses = 0;
bool short_read = false, omit_hp = false;
int fail_reads = 0; bool replace_party_for_real = false;
bool battle_up = false, known_owners = false;
bool setup_ready = true, resume_owners = false;
bool notes_ready = false, split_flipped = false;
uint8_t peer_position = 0, peer_count = 2;
bool net_active_flag = true;                         // the session exists (false: left / never joined)
int census_notices = 0; unsigned census_lost = 0, census_dup = 0, reserved_notices = 0; char census_lost_names[300] = {}, census_dup_names[300] = {};   // the room notices catsync raised
uint8_t member_ids[4] = {0, 1, 2, 3};                // net_peer_ids: who is in the session (peer ids), as many as peer_count says
int drop_calls = 0; uint32_t drop_mask = 0, absent_seats = 0; int drop_result = 0;   // roster_drop_positions, as the leave tick calls it (the seats gone: net_absent_seats; its result)
int prune_calls = 0, prune_keep = -1;                // roster_leave_prune, as the catsync alone-check calls it
uint64_t rng[4] = {1, 2, 3, 4}, counter = 10;
unsigned char director[0x800]{}, registry[0x200]{};
const void* director_ptr = director;
uint64_t party[4] = {1, 2, 3, 4};
struct Entry { uint64_t padding[3]{}; void* cat = nullptr; };
struct IdVector { uint32_t capacity, count; uint64_t* data; };
struct SavedStatus { char* name; uint32_t id; uint8_t padding[0xB8 - 12]; };
struct StatusVector { SavedStatus* begin; SavedStatus* end; SavedStatus* capacity; };
struct CatImage {
    uint64_t seed;
    int32_t hp;
    uint32_t count;
    uint32_t statuses[4];
    int32_t stat;
    uint32_t equipment;
    uint32_t items[5]; // Optional records: an absent item does not overwrite the destination.
    uint64_t flags;    // CatData+0xBF8, serialized with the cat
};
static_assert(sizeof(IdVector) == 16 && offsetof(Entry, cat) == 0x18);
static_assert(sizeof(SavedStatus) == kCatStatusEffectSize && sizeof(CatImage) == 72);
std::map<uint64_t, Entry> entries;
std::vector<void*> allocated;
std::vector<uint64_t> published;
std::map<uint64_t, CatImage> published_images;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
template<class T> T get(const void* p, size_t off) { T v; std::memcpy(&v, (const char*)p + off, sizeof(v)); return v; }
template<class T> void put(void* p, size_t off, T v) { std::memcpy((char*)p + off, &v, sizeof(v)); }
uint64_t hash(const void* p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < n; ++i) { h ^= ((const uint8_t*)p)[i]; h *= 0x100000001b3ULL; }
    return h;
}
StatusVector& statuses(void* cat) { return *(StatusVector*)((uint8_t*)cat + kCatData_StatusEffects); }
uint32_t status_count(void* cat) {
    const auto& v = statuses(cat);
    return v.begin ? (uint32_t)(v.end - v.begin) : 0;
}
void __fastcall destroy_status_range(void* first, void* last) {
    for (auto* p = (SavedStatus*)first; p != last; ++p) {
        std::free(p->name);
        p->name = nullptr;
        ++destroyed_statuses;
    }
}
void append_status(void* cat, uint32_t id) {
    auto& v = statuses(cat);
    const uint32_t count = status_count(cat);
    if (v.end == v.capacity) {
        v.begin = (SavedStatus*)std::realloc(v.begin, (count + 4) * sizeof(SavedStatus));
        v.end = v.begin + count;
        v.capacity = v.begin + count + 4;
    }
    *v.end = {};
    v.end->id = id;
    v.end->name = (char*)std::malloc(32);
    std::snprintf(v.end->name, 32, "status-%u", id);
    ++v.end;
}
void* __fastcall allocate(size_t n) { void* p = std::calloc(1, n); allocated.push_back(p); return p; }
void* __fastcall construct(void* p) { ++rng[0]; return p; }
void __fastcall post_load(void*) { ++post_loads; ++rng[1]; }
Entry list_head;
void sync_list() {                      // rebuild the circular node list the registry walk reads
    Entry* prev = &list_head;
    for (auto& kv : entries) {
        Entry* e = &kv.second;
        e->padding[2] = kv.first;
        prev->padding[0] = (uint64_t)e; e->padding[1] = (uint64_t)prev; prev = e;
    }
    prev->padding[0] = (uint64_t)&list_head; list_head.padding[1] = (uint64_t)prev;
    void* head = &list_head; std::memcpy(registry + 0xE8 + 8, &head, sizeof(head));
}
void* __fastcall by_id(void*, uint64_t id) {
    sync_list();
    auto it = entries.find(id);
    return it == entries.end() ? nullptr : it->second.cat;
}
void* __fastcall register_cat(void* table, void* result, const uint64_t* id) {
    CHECK(table == registry + 0xE8);
    auto inserted = entries.emplace(*id, Entry{});
    put(result, 0, &inserted.first->second);
    put(result, 8, uint8_t(inserted.second));
    return result;
}
// The game's family table (registry+0x38): a flat hash map id -> {parent a, parent b, kinship}: control bytes at +0, 32-byte slots at +8, capacity at
// +0x18. Modelled with every slot full, so the mod's walk reads exactly these entries.
std::map<uint64_t, std::pair<uint64_t, uint64_t>> pedigree;
std::vector<int8_t> ped_ctrl;
std::vector<uint64_t> ped_slots;
void sync_pedigree() {
    ped_ctrl.assign(pedigree.size() + 16, int8_t(-1));
    ped_slots.assign(pedigree.size() * 4 + 4, 0);
    size_t i = 0;
    for (auto& kv : pedigree) { ped_ctrl[i] = 0; ped_slots[i * 4] = kv.first; ped_slots[i * 4 + 1] = kv.second.first; ped_slots[i * 4 + 2] = kv.second.second; ++i; }
    const void* c = ped_ctrl.data(); const void* s = ped_slots.data(); const uint64_t cap = pedigree.size();
    std::memcpy(registry + 0x38, &c, 8); std::memcpy(registry + 0x40, &s, 8); std::memcpy(registry + 0x50, &cap, 8);
}
void __fastcall parents(void* table, uint64_t id, uint64_t a, uint64_t b) {
    CHECK(table == registry + 0x38);
    CHECK(entries.count(id) == 1);
    pedigree[id] = { a, b };
    sync_pedigree();
}
void __fastcall append(void* p, const uint64_t* id) {
    auto* v = (IdVector*)p;
    if (v->count == v->capacity) {
        v->capacity = v->capacity ? v->capacity * 2 : 2;
        v->data = (uint64_t*)std::realloc(v->data, v->capacity * sizeof(uint64_t));
    }
    v->data[v->count++] = *id;
}
void* __fastcall ofstream_ctor(void* p) { return p; }
void __fastcall bs_dtor(void* p) {
    if (get<uint32_t>(p, kBS_Mode) == 1) std::free(get<void*>(p, kBS_WriteBuf));
}
void __fastcall serialize(void* cat, void* bs, bool) {
    if (get<uint32_t>(bs, kBS_Mode) == 1) {
        CatImage image{};
        image.seed = get<uint64_t>(cat, 0);
        image.hp = get<int32_t>(cat, 0x7A8);
        image.stat = get<int32_t>(cat, 0x70C);
        image.equipment = get<uint32_t>(cat, 0x7D0);
        image.flags = get<uint64_t>(cat, kCatData_Flags);
        for (uint32_t i = 0; i < kCatItemCount; ++i) {
            const size_t off = kCatData_Items + i * kCatItemSize;
            if (get<uint64_t>(cat, off) != UINT64_MAX)
                image.items[i] = (uint32_t)get<uint64_t>(cat, off + 0x18);
        }
        image.count = status_count(cat);
        CHECK(image.count <= 4);
        for (uint32_t i = 0; i < image.count && i < 4; ++i) image.statuses[i] = statuses(cat).begin[i].id;
        void* data = std::malloc(sizeof(image));
        std::memcpy(data, &image, sizeof(image));
        put(bs, kBS_WriteBuf, data); put(bs, kBS_WriteLen, uint32_t(sizeof(image)));
    } else {
        ++reads;
        const uint32_t n = get<uint32_t>(bs, kBS_ReadLen);
        if (n == sizeof(CatImage)) {
            const auto& image = *get<const CatImage*>(bs, kBS_ReadBuf);
            put(cat, 0, image.seed);
            if (!omit_hp) put(cat, 0x7A8, image.hp);
            put(cat, 0x70C, image.stat);
            put(cat, 0x7D0, image.equipment);
            put(cat, kCatData_Flags, image.flags);
            for (uint32_t i = 0; i < kCatItemCount; ++i) {
                // RVA 0x22D470: absent record returns WITHOUT clearing the old item.
                if (!image.items[i]) continue;
                const size_t off = kCatData_Items + i * kCatItemSize;
                put(cat, off, ++counter);
                put(cat, off + 0x18, uint64_t(image.items[i]));
            }
            // RVA 0x22F301 appends each saved status without clearing the old vector.
            for (uint32_t i = 0; i < image.count && i < 4; ++i) append_status(cat, image.statuses[i]);
        }
        const bool fail_this = short_read || fail_reads > 0;
        if (fail_reads > 0) --fail_reads;
        put(bs, kBS_ReadPos, fail_this ? uint32_t(0) : n);
        ++rng[2];
    }
}
CatDataMsg message(uint64_t id, CatImage& image) {
    CatDataMsg m{}; m.id = id; m.size = sizeof(image);
    m.data = (uint8_t*)&image; m.hash = hash(&image, sizeof(image));
    return m;
}
void initialize() {
    std::strcpy(cfg.net_role, "client"); cfg.net_follow = true;
    std::memset(director, 0, sizeof(director));
    put(director, kDir_CatRegistry, registry + 0);
    put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_CatIdData, party + 0);
    for (auto id : party) {
        void* cat = allocate(0xC58); put(cat, kCatData_SaveId, id); put(cat, 0, id * 10);
        entries[id].cat = cat;
    }
    pedigree.clear(); sync_pedigree();
    catsync_set_base(0); catsync_forget(); catsync_init();
}
void imports_and_membership() {
    const uint64_t id = 0x112233440000031dULL;
    CatImage image{0xfedcba9876543210ULL, 16, 1, {41}, 5, 10};
    auto m = message(id, image);
    const size_t before_allocations = allocated.size();
    CHECK(!catsync_apply_snapshot(m, "test", false));
    CHECK(allocated.size() == before_allocations && !entries.count(id));
    uint64_t before_rng[4]; std::memcpy(before_rng, rng, sizeof(rng));
    CHECK(catsync_apply_snapshot(m, "test", true));
    CHECK(std::memcmp(before_rng, rng, sizeof(rng)) == 0);
    CHECK(counter == id && post_loads == 1);
    CHECK(entries.count(id) == 1);
    void* cat = entries[id].cat;
    CHECK(get<uint64_t>(cat, kCatData_SaveId) == id);
    CHECK(get<uint64_t>(cat, 0) == image.seed);
    CHECK(get<int32_t>(cat, 0x7A8) == 16);
    CHECK(allocated.size() == before_allocations + 1);
    CHECK(status_count(cat) == 1);

    EnterNodeMsg node{}; node.seed0 = 100; node.familiar_count = 1; node.familiar_ids[0] = id;
    CHECK(catsync_apply_familiars(node));
    EnterNodeMsg observed{}; CHECK(catsync_read_familiars(observed));
    CHECK(observed.party_count == 4 && observed.party_ids[0] == party[0] && observed.party_ids[3] == party[3]);
    CHECK(observed.familiar_count == 1 && observed.familiar_ids[0] == id);
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4);
    CHECK(std::memcmp(party, get<void*>(director, kDir_CatIdData), sizeof(party)) == 0);
    CHECK(catsync_apply_familiars(node));
    image.hp = 7; image.equipment = 20; m = message(id, image);
    CHECK(catsync_apply_snapshot(m, "changed image", true));
    CHECK(post_loads == 1 && allocated.size() == before_allocations + 1);
    CHECK(get<int32_t>(cat, 0x7A8) == 7);
    CHECK(get<uint32_t>(cat, 0x7D0) == 20);
    CHECK(status_count(cat) == 1 && statuses(cat).begin[0].id == 41);
    CHECK(std::memcmp(before_rng, rng, sizeof(rng)) == 0);

    const auto reads_before = reads;
    CHECK(catsync_apply_snapshot(m, "duplicate node batch", true));
    CHECK(reads == reads_before);
    put(cat, 0x7A8, int32_t(99));
    CHECK(catsync_apply_snapshot(m, "same hash but changed local data", true));
    CHECK(reads == reads_before + 1 && get<int32_t>(cat, 0x7A8) == 7);
    CHECK(status_count(cat) == 1);

    image.count = 0; image.statuses[0] = 0; m = message(id, image);
    CHECK(catsync_apply_snapshot(m, "empty saved statuses", true));
    CHECK(status_count(cat) == 0);
    CHECK(destroyed_statuses >= 3);

    image.hp = 12; m = message(id, image);
    omit_hp = true;
    CHECK(!catsync_apply_snapshot(m, "incomplete write", true));
    CHECK(get<int32_t>(cat, 0x7A8) == 7);
    omit_hp = false;
    CHECK(catsync_apply_snapshot(m, "retry after failed readback", true));
    CHECK(get<int32_t>(cat, 0x7A8) == 12);

    std::strcpy(cfg.net_role, "host");
    CHECK(catsync_publish("test", false, true));
    CHECK(published.size() == 5 && published.back() == id);
    std::strcpy(cfg.net_role, "client");
    node.familiar_count = 0;
    CHECK(catsync_apply_familiars(node));
    CHECK(catsync_read_familiars(observed) && observed.familiar_count == 0);
    CHECK(entries.count(id) == 1);
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4);
}
void wide_names() {
    alignas(8) unsigned char s[32]{};
    char out[64]{};
    const wchar_t short_name[] = L"Conan";
    std::memcpy(s, short_name, sizeof(short_name));
    put(s, 16, uint64_t(5)); put(s, 24, uint64_t(7));
    CHECK(mem_read_std_wstring_utf8(s, out, sizeof(out)) && !std::strcmp(out, "Conan"));
    const wchar_t long_name[] = L"Conan\u7684\u4e4c\u9e26";
    put(s, 0, long_name); put(s, 16, uint64_t(8)); put(s, 24, uint64_t(15));
    CHECK(mem_read_std_wstring_utf8(s, out, sizeof(out)) &&
          !std::strcmp(out, "Conan\xE7\x9A\x84\xE4\xB9\x8C\xE9\xB8\xA6"));
    CHECK(!mem_read_std_wstring_utf8(s, out, 4) && !out[0]);
    put(s, 24, uint64_t(7));
    CHECK(!mem_read_std_wstring_utf8(s, out, sizeof(out)) && !out[0]);
    const wchar_t invalid[] = {0xD800, 0};
    put(s, 0, invalid); put(s, 16, uint64_t(1)); put(s, 24, uint64_t(15));
    CHECK(!mem_read_std_wstring_utf8(s, out, sizeof(out)) && !out[0]);
    put(s, 16, uint64_t(0));
    CHECK(mem_read_std_wstring_utf8(s, out, sizeof(out)) && !out[0]);
}

void optional_item_replacement() {
    void* cat = entries[1].cat;
    CatImage image{10, 10, 0, {}, 1, 0, {11, 22, 33, 44, 55}};
    auto m = message(1, image);
    CHECK(catsync_apply_snapshot(m, "all item slots populated", true));
    // RaccoonForm's removal of MonkFist + MonkStyleChanger caused a persistent
    // readback mismatch. Exercise absent/present transitions in every slot,
    // including identical repeated snapshots and re-equipping afterwards.
    for (uint32_t slot = 0; slot < kCatItemCount; ++slot) {
        image.items[slot] = 0;
        m = message(1, image);
        uint64_t before_rng[4]; std::memcpy(before_rng, rng, sizeof(rng));
        CHECK(catsync_apply_snapshot(m, "remove optional item", true));
        CHECK(get<uint64_t>(cat, kCatData_Items + slot * kCatItemSize) == UINT64_MAX);
        CHECK(std::memcmp(before_rng, rng, sizeof(rng)) == 0);
        const unsigned before_reads = reads;
        CHECK(catsync_apply_snapshot(m, "repeat empty item", true));
        CHECK(reads == before_reads);
    }
    image.items[3] = 77; image.items[4] = 88;
    m = message(1, image);
    CHECK(catsync_apply_snapshot(m, "equip both after removal", true));
    image.items[3] = image.items[4] = 0;
    m = message(1, image);
    CHECK(catsync_apply_snapshot(m, "remove both class items together", true));
}

void entry_replacement() {
    void* entry = allocate(0xC58);
    CatImage image{55, 9, 1, {73}, 8, 4};
    const auto before = destroyed_statuses;
    CHECK(mgmp::catsync_deserialize_into(entry, (uint8_t*)&image, sizeof(image)));
    CHECK(status_count(entry) == 1);
    CHECK(mgmp::catsync_deserialize_into(entry, (uint8_t*)&image, sizeof(image)));
    CHECK(status_count(entry) == 1 && destroyed_statuses == before + 1);
    image.count = 0; image.statuses[0] = 0;
    CHECK(mgmp::catsync_deserialize_into(entry, (uint8_t*)&image, sizeof(image)));
    CHECK(status_count(entry) == 0 && destroyed_statuses == before + 2);
}
void rejected_inputs() {
    EnterNodeMsg node{}; node.familiar_count = 1;
    node.familiar_ids[0] = 999; CHECK(!catsync_apply_familiars(node));
    node.familiar_ids[0] = 1; CHECK(!catsync_apply_familiars(node));
    node.familiar_ids[0] = 0; CHECK(!catsync_apply_familiars(node));
    node.familiar_count = 65; CHECK(!catsync_apply_familiars(node));
    CatImage image{55, 5, 0, {}, 1, 0};
    auto m = message(998, image); m.hash ^= 1;
    CHECK(!catsync_apply_snapshot(m, "test", true)); CHECK(!entries.count(998));
    m.hash ^= 1;
    std::strcpy(cfg.net_role, "host");
    CHECK(!catsync_apply_snapshot(m, "unauthorized creation", false)); CHECK(!entries.count(998));
    std::strcpy(cfg.net_role, "client");
    m.id = 997; short_read = true;
    CHECK(!catsync_apply_snapshot(m, "short read", true)); CHECK(!entries.count(997));
    short_read = false;
}
void local_upgrade_after_peer_push() {
    // Client owns cat 2, host owns 1/3/4. Host's update is held while the
    // client's upgrade screen still reports a battle, then drained on the map.
    catsync_forget();
    known_owners = true;
    CHECK(catsync_publish("seed shared save baseline", true));
    published.clear(); published_images.clear();
    CatImage remote{10, 11, 0, {}, 21, 31};
    const auto m = message(1, remote);
    battle_up = true;
    catsync_on_message(m);
    void* own = entries[2].cat;
    put(own, 0x70C, int32_t(88));
    put(own, 0x7D0, uint32_t(99));
    append_status(own, 901);
    CHECK(!catsync_publish("upgrade while remote cats held", false));
    CHECK(published.empty());
    catsync_apply_pending("returned to map");
    CHECK(get<int32_t>(entries[1].cat, 0x70C) == 21);
    CHECK(get<int32_t>(own, 0x70C) == 88 && status_count(own) == 1);
    CHECK(catsync_publish("map after draining peer update", true));
    CHECK(published.size() == 1 && published[0] == 2);
    CHECK(published_images[2].stat == 88 && published_images[2].equipment == 99);
    CHECK(published_images[2].count == 1 && published_images[2].statuses[0] == 901);
    published.clear();
    CHECK(catsync_publish("unchanged map tick", true));
    CHECK(published.empty());
    battle_up = known_owners = false;
}
void settlement_owner_filter() {
    // Model the live 4+4 shared lists at the instant the home scene calls the
    // native finalizer. The host owns 70..., so its result is party=70... and
    // no familiars; the peer copies lose the owned bit before native traversal.
    catsync_forget();
    peer_position = 0;
    std::strcpy(cfg.net_role, "host");
    catsync_init();
    uint64_t fam[4] = {0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    uint64_t mir[4] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull};
    uint64_t ids[8] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull,
                       0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    for (unsigned i = 0; i < 8; ++i) {
        void* cat = entries[ids[i]].cat;
        if (!cat) { cat = allocate(0xC58); entries[ids[i]].cat = cat; }
        put(cat, kCatData_SaveId, ids[i]); put(cat, 0xBF8, uint64_t(1ull | (1ull << 19)));
    }
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdCap, uint32_t(4));
    put(director, kDir_CatIdData, (const uint64_t*)ids);
    put(director, kDir_MirrorCount, uint32_t(4)); put(director, kDir_MirrorCap, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(4));
    put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    resume_owners = true;
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    CHECK(std::memcmp(ids, get<void*>(director, kDir_CatIdData), sizeof(party)) == 0);
    for (unsigned i = 0; i < 4; ++i) {
        // Settlement filtering removes peer IDs from the native vectors.  It
        // deliberately leaves CatData flags untouched because their meaning
        // is broader than ownership and is not required for this boundary.
        CHECK(get<uint64_t>(entries[0x71000001ull+i].cat, 0xBF8) == (1ull | (1ull << 19)));
        CHECK(get<uint64_t>(entries[0x70000001ull+i].cat, 0xBF8) == (1ull | (1ull << 19)));
    }
    CHECK(!catsync_publish("after settlement", false, true));
    put(director, kDir_CatIdData, (const uint64_t*)party);
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    put(director, kDir_MirrorData, (const uint64_t*)party);
    catsync_forget(); resume_owners = false; peer_position = 0;
}

void settlement_without_battle_split() {
    // 2026-09-28 live regression: a session restored from the journal walked an
    // event node into the home node with no battle ever starting, so the battle
    // split's table was empty on BOTH peers and the finalize was held forever
    // with the game frozen at the door. The settlement's ownership source is
    // the NOTE table (roster install / journal import); the split table is only
    // a cross-check that may be absent.
    catsync_forget();
    peer_position = 0;
    std::strcpy(cfg.net_role, "host");
    catsync_init();
    uint64_t fam[4] = {0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    uint64_t mir[4] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull};
    uint64_t ids[8] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull,
                       0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    for (unsigned i = 0; i < 8; ++i) {
        void* cat = entries[ids[i]].cat;
        if (!cat) { cat = allocate(0xC58); entries[ids[i]].cat = cat; }
        put(cat, kCatData_SaveId, ids[i]);
    }
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdCap, uint32_t(4));
    put(director, kDir_CatIdData, (const uint64_t*)ids);
    put(director, kDir_MirrorCount, uint32_t(4)); put(director, kDir_MirrorCap, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(4));
    put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);

    // No battle: the split table is empty (is_mine fails for every 0x70/0x71
    // cat). Notes alone must settle the run -- this is the live fix.
    notes_ready = true;
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    CHECK(!catsync_publish("after settlement", false, true));
    catsync_forget();
    put(director, kDir_CatFamiliars + 4, uint32_t(4));

    // No notes either: the settlement must refuse rather than guess.
    notes_ready = false;
    CHECK(!catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 4);

    // Notes present but the battle split DISAGREES: a real desync still refuses.
    notes_ready = true; resume_owners = true; split_flipped = true;
    CHECK(!catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 4);

    put(director, kDir_CatIdData, (const uint64_t*)party);
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    put(director, kDir_MirrorData, (const uint64_t*)party);
    catsync_forget(); notes_ready = resume_owners = split_flipped = false; peer_position = 0;
}

void in_session_next_run_replaces_stale_session_cats() {
    // 2026-09-28: a run that settles IN THIS SESSION leaves this peer's own
    // previous-run session clones registered -- a fresh process and a
    // checkpoint restore both rebuild the registry from the save, but the
    // in-session next chapter does not (observed live: 151 "already owned by
    // another cat" retries, the client's export never left, and the host's
    // chapter click stayed held). The second chapter barrier must REPLACE the
    // stale copy with the new selection, exactly like the receiving peer's
    // apply path does for a re-delivered identity.
    entries.clear();                             // drop every prior test's registry
    initialize();                                // client role; party = save cats 1..4
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); // run 1: the clones are minted
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(first.cats[i].id == 0x71000001ull + i);
        CHECK(entries.count(0x71000001ull + i) == 1);
        CHECK(get<uint64_t>(entries[0x71000001ull + i].cat, 0) == party[i] * 10);
    }
    for (auto& c : first.cats) free(c.data);
    // The run settles. The peer re-preps from its own save; the registry is
    // NOT rebuilt, so the four stale 0x71... clones are still registered while
    // the party now points at four DIFFERENT save cats.
    uint64_t reselected[4] = {5, 6, 7, 8};
    for (auto id : reselected) {
        void* cat = allocate(0xC58); put(cat, kCatData_SaveId, id); put(cat, 0, id * 10);
        entries[id].cat = cat;
    }
    put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_CatIdData, (const uint64_t*)reselected);
    SetupMsg second{};
    CHECK(catsync_prepare_party_setup(second, 1)); // failed before the fix
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(second.cats[i].id == 0x71000001ull + i);
        CHECK(second.cats[i].size == sizeof(CatImage));
        // The stale clone now carries the NEW selection's image, and the
        // exported image is the re-serialized replacement.
        CHECK(get<uint64_t>(entries[0x71000001ull + i].cat, 0) == reselected[i] * 10);
        CHECK(get<uint64_t>(entries[0x71000001ull + i].cat, kCatData_SaveId) ==
              0x71000001ull + i);
        const auto& image = *reinterpret_cast<const CatImage*>(second.cats[i].data);
        CHECK(image.seed == reselected[i] * 10);
        CHECK(second.cats[i].hash == hash(second.cats[i].data, second.cats[i].size));
        free(second.cats[i].data);
    }
    // Restore the standard fixture for the tests that follow.
    put(director, kDir_CatIdData, (const uint64_t*)party);
    for (auto id : reselected) entries.erase(id);
    catsync_forget();
}

constexpr uint64_t kHome = 3, kAway = 0x80001;
void depart_all(unsigned n) { for (unsigned i = 0; i < n; ++i) put(entries[party[i]].cat, kCatData_Flags, uint64_t(kAway)); }
void settle_clone(uint64_t id, int32_t stat) {          // what the native settlement + the run did to a clone
    put(entries[id].cat, 0x70C, stat); put(entries[id].cat, kCatData_Flags, uint64_t(kHome));
}
uint64_t flags_of(uint64_t id) { return get<uint64_t>(entries[id].cat, kCatData_Flags); }
void free_msg_cats(SetupMsg& m) { for (auto& c : m.cats) { free(c.data); c.data = nullptr; } }

void clones_return_to_their_originals() {
    // F1 (2026-10-01): the game flags the ORIGINALS on adventure at departure and settles only the clones, so
    // after a run the progress sat on the clones and the originals stayed out forever. The merge writes each
    // settled clone back over the cat it was made from and retires it.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); depart_all(4);
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); free_msg_cats(first);
    for (unsigned i = 0; i < 4; ++i) CHECK(flags_of(0x71000001ull + i) == kAway);   // the clones left with the originals' flags
    CHECK(catsync_merge_session_cats("test") == 0);                                  // still out on the run: nothing to merge
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 70 + (int32_t)i);
    void* clone_obj[4]; void* orig_obj[4];
    for (unsigned i = 0; i < 4; ++i) { clone_obj[i] = entries[0x71000001ull + i].cat; orig_obj[i] = entries[party[i]].cat; }
    CHECK(catsync_merge_session_cats("test") == 4);
    for (unsigned i = 0; i < 4; ++i) {                                               // IDENTITIES SWAPPED (2026-10-03): the House holds the clone, so the clone object is now the original
        CHECK(entries[party[i]].cat == clone_obj[i] && entries[0x71000001ull + i].cat == orig_obj[i]);
        CHECK(get<uint64_t>(clone_obj[i], kCatData_SaveId) == party[i] && get<uint64_t>(orig_obj[i], kCatData_SaveId) == 0x71000001ull + i);
    }
    { const uint64_t* now = get<const uint64_t*>(director, kDir_CatIdData);       // the party list names the originals again
      CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4);
      for (unsigned i = 0; i < 4; ++i) CHECK(now[i] == party[i]); }
    for (unsigned i = 0; i < 4; ++i) {
        void* orig = entries[party[i]].cat;
        CHECK(get<int32_t>(orig, 0x70C) == 70 + (int32_t)i);                         // the run's progress is on the original
        CHECK(flags_of(party[i]) == kHome);                                          // at home again, no longer "on adventure"
        CHECK(get<uint64_t>(orig, kCatData_SaveId) == party[i]);                     // identity kept
        CHECK(get<uint64_t>(orig, 0) == party[i] * 10);                              // same cat: same seed
        CHECK(flags_of(0x71000001ull + i) == 0);                                     // the clone is retired
    }
    CHECK(catsync_merge_session_cats("again") == 0);                                 // idempotent
    entries.clear(); peer_position = 0;
}

// A RUN LEFT IN A ROOM THAT THE PROCESS DID NOT SEE END (2026-10-05): loaded alone, the other players' cats are recognised by having no original in this registry.
void alone_check_finds_this_players_position() {
    entries.clear(); peer_position = 1; peer_count = 2; net_active_flag = true; initialize(); depart_all(4);
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); free_msg_cats(first);                       // this player's clones: 71000001..4, with originals out on adventure
    static uint64_t own[4] = {0x71000001ull, 0x71000002ull, 0x71000003ull, 0x71000004ull};
    static uint64_t fam[2] = {0x70000001ull, 0x70000002ull};
    for (unsigned i = 0; i < 2; ++i) {                                                       // another player's clones: no original here
        void* cat = allocate(0xC58); put(cat, kCatData_SaveId, fam[i]); put(cat, 0, uint64_t(0x5000 + i)); put(cat, kCatData_Flags, uint64_t(kAway));
        entries[fam[i]].cat = cat;
    }
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)own);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(2)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    peer_count = 1; net_active_flag = true; prune_calls = 0; prune_keep = -1;
    catsync_leave_tick();
    CHECK(prune_calls == 0);                                                                  // a session is still up: it may get its player back
    net_active_flag = false; std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    catsync_leave_tick();
    CHECK(prune_calls == 1 && prune_keep == 1);                                               // position 1's cats have originals: they are this player's
    // nobody's cats have originals: which are this player's cannot be told, so nothing is removed
    entries.clear(); initialize();
    for (unsigned i = 0; i < 2; ++i) {
        void* cat = allocate(0xC58); put(cat, kCatData_SaveId, fam[i]); put(cat, 0, uint64_t(0x6000 + i)); put(cat, kCatData_Flags, uint64_t(kAway));
        entries[fam[i]].cat = cat;
        void* c2 = allocate(0xC58); put(c2, kCatData_SaveId, own[i]); put(c2, 0, uint64_t(0x7000 + i)); put(c2, kCatData_Flags, uint64_t(kAway));
        entries[own[i]].cat = c2;
    }
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_CatIdData, (const uint64_t*)own);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(2)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    prune_calls = 0; std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    catsync_leave_tick();
    CHECK(prune_calls == 0);
    entries.clear(); peer_position = 0; peer_count = 2; net_active_flag = true;
}

// A PLAYER DROPPED OUT OF A ROOM THAT GOES ON (2026-10-05): the two who stay take the leaver's cats out of the run; nobody else's.
// 0.1.119: read off the seat order (net_absent_seats), every update, until the lists are clean -- no memory of who was here when, no giving up.
void a_player_who_drops_out_loses_their_cats_for_everyone_else() {
    entries.clear(); peer_position = 2; peer_count = 3; net_active_flag = true; initialize();
    absent_seats = 0; drop_result = 0; drop_calls = 0; drop_mask = 0;
    catsync_leave_tick();                                   // the room is whole
    CHECK(drop_calls == 0);
    peer_count = 2; absent_seats = 1u << 1;                  // seat 1 left; this peer is seat 2, which is now ABOVE the count of the players present
    catsync_leave_tick();
    CHECK(drop_calls == 1 && drop_mask == (1u << 1));        // seat 1's cats, not seat 2's
    drop_calls = 0; drop_mask = 0;
    catsync_leave_tick();
    CHECK(drop_calls == 1 && drop_mask == (1u << 1));        // asked again at every update (cheap when the lists are clean)
    // a removal that fails is retried, never given up: far more than the 120 tries the old watch allowed
    drop_result = -1; drop_calls = 0;
    for (int i = 0; i < 500; ++i) catsync_leave_tick();
    CHECK(drop_calls == 500);
    drop_result = 0; drop_calls = 0;
    catsync_leave_tick();
    CHECK(drop_calls == 1);                                 // and it goes through once the lists can be written
    // a player who left BEFORE anybody looked (the room was never seen whole) is found just the same
    net_active_flag = false; catsync_leave_tick();           // the session is gone: the next one starts clean
    peer_position = 1;                                       // this peer is seat 1 now; seat 2 left
    net_active_flag = true; peer_count = 2; absent_seats = 1u << 2; drop_calls = 0; drop_mask = 0;
    catsync_leave_tick();
    CHECK(drop_calls == 1 && drop_mask == (1u << 2));
    // the HOST is the one gone
    absent_seats = 1u; drop_calls = 0; drop_mask = 0;
    catsync_leave_tick();
    CHECK(drop_calls == 1 && drop_mask == 1u);
    // a host left alone in a room that stays open
    peer_position = 0; peer_count = 2; absent_seats = 0; CHECK(catsync_my_seat() == 0);     // this peer is the host (seat 0), remembered while the room was whole
    peer_count = 1; absent_seats = (1u << 1) | (1u << 2); drop_calls = 0; drop_mask = 0;
    catsync_leave_tick();
    CHECK(drop_calls == 1 && drop_mask == ((1u << 1) | (1u << 2)));
    // 0.1.129: a seat order that counts THIS peer's own seat as gone -- its cats are kept, only the others' leave
    peer_position = 2; peer_count = 2; absent_seats = (1u << 1) | (1u << 2); drop_calls = 0; drop_mask = 0;
    catsync_leave_tick();
    CHECK(drop_calls == 1 && drop_mask == (1u << 1));
    absent_seats = 0;
    entries.clear(); peer_position = 0; peer_count = 2; net_active_flag = true;
}

void legacy_clones_are_merged_before_they_are_overwritten() {
    // The old behaviour left the clones as LIVE cats at home. Run 2 picks clones 3 and 4 only; the fixed slot ids
    // 71000001/2 are live cats that used to be overwritten (data lost). Now they are merged back first.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); depart_all(4);
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); free_msg_cats(first);
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 40 + (int32_t)i);   // legacy: live clones, originals still out
    uint64_t picked[2] = {0x71000003ull, 0x71000004ull};
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_CatIdData, (const uint64_t*)picked);
    for (auto id : picked) put(entries[id].cat, kCatData_Flags, uint64_t(kHome | 0x80000));   // departure flags them
    SetupMsg second{};
    CHECK(catsync_prepare_party_setup(second, 1)); free_msg_cats(second);
    for (unsigned i = 0; i < 2; ++i) {                                                   // 1 and 2 were NOT lost
        CHECK(get<int32_t>(entries[party[i]].cat, 0x70C) == 40 + (int32_t)i);
        CHECK(flags_of(party[i]) == kHome);
    }
    CHECK(get<uint64_t>(entries[0x71000001ull].cat, 0) == party[2] * 10);                // the slots now carry cats 3 and 4
    CHECK(get<uint64_t>(entries[0x71000002ull].cat, 0) == party[3] * 10);
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    entries.clear(); peer_position = 0; catsync_forget();
}

// The cat (if any) that is not one of the fixture's ids and carries `stat` -- what an adopted clone looks like.
bool adopted_with_stat(int32_t stat) {
    for (auto& kv : entries) {
        if (kv.first < 100 || (kv.first >= 0x70000000ull && kv.first < 0x80000000ull) || !kv.second.cat) continue;
        if (get<int32_t>(kv.second.cat, 0x70C) == stat && get<uint64_t>(kv.second.cat, kCatData_SaveId) == kv.first) return true;
    }
    return false;
}

void clones_without_an_original_and_the_id_counter() {
    // The game numbers new cats from a counter. Registering a session id used to raise it into the session range, so a
    // later recruit got an id the mod reads as a clone (and that fails the settlement's ownership check).
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); counter = 0x300;
    depart_all(4);
    SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 1)); free_msg_cats(m);
    CHECK(counter == 0x300);                                              // the clones did not drag it up
    // an older build had already done it: the counter sits inside the session range and is put back
    counter = 0x71000009ull;
    { void* c = allocate(0xC58); put(c, kCatData_SaveId, uint64_t(0x2f0)); put(c, 0, uint64_t(777)); entries[0x2f0].cat = c; }
    sync_list();
    catsync_merge_session_cats("t");
    CHECK(counter == 0x2f0);

    // a clone made from a clone of an older build (slot 8, still out on adventure): the origin is that cat, not a new one
    entries.clear(); initialize(); counter = 0x300; depart_all(4);
    { void* old = allocate(0xC58); put(old, kCatData_SaveId, uint64_t(0x71000008ull)); put(old, 0, party[0] * 10);
      put(old, kCatData_Flags, uint64_t(kAway)); entries[0x71000008ull].cat = old; }
    SetupMsg m2{}; CHECK(catsync_prepare_party_setup(m2, 1)); free_msg_cats(m2);
    entries[party[0]].cat = nullptr; entries.erase(party[0]);              // the true original is nowhere
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 50 + (int32_t)i);
    CHECK(catsync_merge_session_cats("t") == 4);
    CHECK(get<int32_t>(entries[0x71000008ull].cat, 0x70C) == 50 && flags_of(0x71000008ull) == kHome);   // revived with the run's progress
    CHECK(flags_of(0x71000001ull) == 0 && counter == 0x300 + 0);        // no new cat was needed for it
    // a leftover of an older build that is itself settled and has no original is simply the cat it is
    { void* lone = allocate(0xC58); put(lone, kCatData_SaveId, uint64_t(0x71000009ull)); put(lone, 0, uint64_t(31337)); put(lone, kCatData_Flags, uint64_t(kHome)); entries[0x71000009ull].cat = lone; }
    CHECK(catsync_merge_session_cats("t") == 0 && flags_of(0x71000009ull) == kHome);

    // 0.1.209 (ll's 八云紫): a leftover of an older build still out on adventure whose ordinary original is AT HOME is a stale copy -- retired at the settlement, never
    // before it (a merge outside the settlement leaves it), and never when the ordinary original is away or missing (then it may be the cat's only carrier).
    { void* home = allocate(0xC58); put(home, kCatData_SaveId, uint64_t(0x3d)); put(home, 0, uint64_t(0xae3b22b6)); put(home, kCatData_Flags, uint64_t(kHome)); entries[0x3d].cat = home;
      void* stale = allocate(0xC58); put(stale, kCatData_SaveId, uint64_t(0x71000011ull)); put(stale, 0, uint64_t(0xae3b22b6)); put(stale, kCatData_Flags, uint64_t(kHome | 0x80000)); entries[0x71000011ull].cat = stale;
      void* sole = allocate(0xC58); put(sole, kCatData_SaveId, uint64_t(0x71000012ull)); put(sole, 0, uint64_t(0x5eed)); put(sole, kCatData_Flags, uint64_t(kHome | 0x80000)); entries[0x71000012ull].cat = sole; }
    sync_list();
    catsync_merge_session_cats("not the settlement");
    CHECK(flags_of(0x71000011ull) == (kHome | 0x80000));                // outside the settlement: untouched
    catsync_merge_session_cats("settlement", true);
    CHECK(flags_of(0x71000011ull) == 0 && flags_of(0x3d) == kHome);   // the stale copy is retired, the cat at home stays as it is
    CHECK(flags_of(0x71000012ull) == (kHome | 0x80000));                // no ordinary cat with its seed: kept
    entries.clear(); peer_position = 0; catsync_forget();
}

void copies_at_home_do_not_make_the_original_ambiguous() {
    // 2026-10-01 live: every adopted clone is an ordinary cat with the SAME seed as the cat it came from, so after one
    // adoption the next merge saw two ordinary cats with that seed, called it ambiguous, and adopted again -- the
    // saves filled with copies. Only a cat that is OUT on adventure can be the original.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); counter = 0x300; depart_all(4);
    SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 1)); free_msg_cats(m);
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 60 + (int32_t)i);
    for (uint64_t id : {0x2f0ull, 0x2f1ull}) {                                        // two copies at home with cat 1's seed
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, party[0] * 10); put(c, kCatData_Flags, uint64_t(kHome)); entries[id].cat = c;
    }
    sync_list();
    CHECK(catsync_merge_session_cats("t") == 4);
    CHECK(get<int32_t>(entries[party[0]].cat, 0x70C) == 60 && flags_of(party[0]) == kHome);   // written to the real original
    CHECK(!adopted_with_stat(60));                                                            // and nothing new was made
    entries.clear(); peer_position = 0; catsync_forget();
}

// THE FALLBACK FILTER (0.1.79, "the other player's cat came home to my House"): when the ownership filter refuses and nobody else is connected, the run used to
// end unfiltered and the other player's copies became cats of this House. The fallback takes out only the other positions' cats -- by owner note, or by a clone /
// gained-cat slot of another position when there is no note -- and leaves this player's cats and real cats of older builds (high slots) alone.
void the_fallback_filter_takes_out_only_other_players_cats() {
    entries.clear(); peer_position = 0; peer_count = 1; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    uint64_t lst[4] = {0x70000001ull, 0x71000001ull, 0x70000002ull, 0x71000050ull};      // 0x71000050: a real cat of an older build
    uint64_t mir[4] = {0x70000001ull, 0x71000001ull, 0x70000002ull, 0x71000050ull};
    uint64_t fam[4] = {0x71000002ull, 0x70000019ull, 0x71000019ull, 0x355ull};          // a peer copy, the host's gained cat, a client-side gained slot, an ordinary cat
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdCap, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    put(director, kDir_MirrorCount, uint32_t(4)); put(director, kDir_MirrorCap, uint32_t(4)); put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(4)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    notes_ready = false;                                                                 // no owner notes: the ids alone decide
    CHECK(catsync_strip_peer_copies_for_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 3 && lst[0] == 0x70000001ull && lst[1] == 0x70000002ull && lst[2] == 0x71000050ull);
    CHECK(get<uint32_t>(director, kDir_MirrorCount) == 3 && mir[0] == 0x70000001ull && mir[1] == 0x70000002ull && mir[2] == 0x71000050ull);
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 2 && fam[0] == 0x70000019ull && fam[1] == 0x355ull);
    CHECK(catsync_strip_peer_copies_for_settlement(director));                           // a second pass finds nothing more
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 3 && get<uint32_t>(director, kDir_CatFamiliars + 4) == 2);
    catsync_forget();
    put(director, kDir_CatIdData, (const uint64_t*)party); put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)party); put(director, kDir_MirrorCount, uint32_t(0));
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    peer_count = 2;
}

// 0.1.212 (CHINAMI 11-08): the game emptied the host's familiar list at the end of a fight (an item-linked familiar's collar came off) and the clients' cats were in
// no list any more. At the next publish after a fight the living ones go back; a dead one, one of a player who left, or a loss after an event node stays out.
void the_host_puts_lost_client_cats_back_after_a_fight() {
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    for (uint64_t id : {0x71000001ull, 0x71000002ull, 0x72000001ull}) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, id * 7); put(c, kCatData_Flags, uint64_t(kHome)); entries[id].cat = c;
    }
    sync_list();
    static uint64_t lst[1] = {0x70000001ull}, fam[8] = {0x71000001ull, 0x71000002ull, 0x72000001ull};
    { void* h = allocate(0xC58); put(h, kCatData_SaveId, uint64_t(0x70000001ull)); put(h, kCatData_Flags, uint64_t(kHome)); entries[0x70000001ull].cat = h; sync_list(); }
    put(director, kDir_CatIdCount, uint32_t(1)); put(director, kDir_CatIdCap, uint32_t(1)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    put(director, kDir_CatFamiliars, uint32_t(8)); put(director, kDir_CatFamiliars + 4, uint32_t(3)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    CHECK(catsync_host_keep_familiars("t", false) == 0);                                  // the reference: three client cats
    put(director, kDir_CatFamiliars + 4, uint32_t(0));                                    // the fight emptied the list
    CHECK(catsync_host_keep_familiars("after an event", false) == 0);                     // not after a fight: left as it is
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    put(director, kDir_CatFamiliars + 4, uint32_t(3));
    CHECK(catsync_host_keep_familiars("t", false) == 0);                                  // the reference again
    put(director, kDir_CatFamiliars + 4, uint32_t(0));
    put(entries[0x71000002ull].cat, kCatData_Flags, uint64_t(0x13));                       // this one died in the fight
    absent_seats = 1u << 2;                                                               // and seat 2's player left
    CHECK(catsync_host_keep_familiars("after a fight", true) == 1);
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 1 && fam[0] == 0x71000001ull);
    absent_seats = 0;
    catsync_forget();
    put(director, kDir_CatIdData, (const uint64_t*)party); put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    entries.clear();
}

// 0.1.213 (proto 117): a client's report that the host's lists lost one of its living cats -- taken back only for the sender's own seat, a registered living cat,
// and not twice.
void the_host_takes_back_a_cat_its_owner_reports() {
    entries.clear(); peer_position = 0; peer_count = 3; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    for (uint64_t id : {0x71000001ull, 0x71000002ull, 0x72000001ull}) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, id * 3); put(c, kCatData_Flags, uint64_t(kHome)); entries[id].cat = c;
    }
    put(entries[0x71000002ull].cat, kCatData_Flags, uint64_t(0x13));                       // dead
    { void* h = allocate(0xC58); put(h, kCatData_SaveId, uint64_t(0x70000001ull)); put(h, kCatData_Flags, uint64_t(kHome)); entries[0x70000001ull].cat = h; }
    sync_list();
    static uint64_t lst[1] = {0x70000001ull}, fam[8] = {};
    put(director, kDir_CatIdCount, uint32_t(1)); put(director, kDir_CatIdCap, uint32_t(1)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    put(director, kDir_CatFamiliars, uint32_t(8)); put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    catsync_host_listfix(2, 0x71000001ull);                                               // seat 1 is peer 1, not peer 2: refused
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    catsync_host_listfix(1, 0x71000002ull);                                               // dead: refused
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    catsync_host_listfix(1, 0x71000001ull);                                               // its owner, alive: queued, not written under an open map
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    catsync_host_listfix(1, 0x71000001ull);                                               // reported twice: queued once
    catsync_host_keep_familiars("entering a node", false);                                // the next publish takes it back (after an event too)
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 1 && fam[0] == 0x71000001ull);
    catsync_host_listfix(1, 0x71000001ull);                                               // listed already: nothing queued
    catsync_host_listfix(2, 0x72000001ull);                                               // seat 2 is peer 2
    catsync_host_keep_familiars("entering a node", false);
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 2 && fam[1] == 0x72000001ull);
    catsync_forget();
    put(director, kDir_CatIdData, (const uint64_t*)party); put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    peer_count = 2; entries.clear();
}

// 0.1.131, THE CLASS COMPLETION CHECK: a client's party list is the room's shared layout (the host's cats) when the game checks the classes of a beaten boss; for the
// check it holds this player's own cats (party first, then the familiars), the departed list too, and both are put back exactly afterwards.
// 0.1.147: THE ROOM'S RULE for the solo-cat / same-class marks. The game counts them from the departed list; the check puts a list there that makes it come out as the rule.
Tx last_tr = Tx(-1); int marks_notices = 0; char marks_text[420] = {};
std::string captured; bool capture_on = false;          // the log lines of a test that asserts on them (0.1.149)
typedef std::pair<uint64_t, const char*> ClassedCat;
void set_class(uint64_t id, const char* cls) {
    void* cat = entries[id].cat;
    if (!cat) { cat = allocate(0xC58); put(cat, kCatData_SaveId, id); entries[id].cat = cat; }
    char* s = (char*)cat + 0xC10; std::memset(s, 0, 0x20);
    std::strcpy(s, cls); put<uint64_t>(cat, 0xC10 + 16, std::strlen(cls)); put<uint64_t>(cat, 0xC10 + 24, 15);        // an inline std::string
}
// runs the check as `seat` with `brought` registered (id, class) and remembered as the departure; returns what the lists held DURING the check
struct MarkRun { bool changed; uint32_t mirror_n; uint64_t mirror[2]; uint32_t party_n; uint64_t party0; int notice; };
static uint64_t mk_lst[4], mk_mir[4], mk_fam[4];
uint32_t setup_room(uint8_t seat, std::initializer_list<ClassedCat> brought, std::initializer_list<uint64_t> host_party, std::initializer_list<uint64_t> own_fam) {
    entries.clear(); peer_position = seat; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, seat ? "client" : "host"); catsync_init();
    uint64_t(&lst)[4] = mk_lst; uint64_t(&mir)[4] = mk_mir; uint64_t(&fam)[4] = mk_fam;
    std::memset(lst, 0, sizeof(lst)); std::memset(mir, 0, sizeof(mir)); std::memset(fam, 0, sizeof(fam));
    uint32_t pn = 0, fn = 0; for (auto id : host_party) lst[pn++] = id; for (auto id : own_fam) fam[fn++] = id;
    for (uint32_t i = 0; i < pn; ++i) mir[i] = lst[i];                                      // the shared layout: the departed list is the host's party
    put(director, kDir_CatIdCount, pn); put(director, kDir_CatIdCap, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    put(director, kDir_MirrorCount, pn); put(director, kDir_MirrorCap, uint32_t(4)); put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, fn); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    std::vector<uint64_t> ids; for (auto& c : brought) { set_class(c.first, c.second); ids.push_back(c.first); }
    catsync_note_departure(ids.data(), (uint32_t)ids.size());
    return pn;
}
MarkRun run_marks(uint8_t seat, std::initializer_list<ClassedCat> brought, std::initializer_list<uint64_t> host_party, std::initializer_list<uint64_t> own_fam) {
    const uint32_t pn = setup_room(seat, brought, host_party, own_fam);
    uint64_t(&lst)[4] = mk_lst; uint64_t(&mir)[4] = mk_mir;
    marks_notices = 0; catsync_announce_marks();                                         // what the chapter page told this player
    MarkRun r{}; r.notice = marks_notices ? (int)last_tr : -1;
    r.changed = catsync_completion_begin("beat_chapter_boss");
    r.mirror_n = get<uint32_t>(director, kDir_MirrorCount); r.mirror[0] = mir[0]; r.mirror[1] = mir[1];
    r.party_n = get<uint32_t>(director, kDir_CatIdCount); r.party0 = lst[0];
    catsync_completion_end();
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == pn && get<uint32_t>(director, kDir_MirrorCount) == pn);       // put back exactly
    for (uint32_t i = 0; i < pn; ++i) CHECK(lst[i] == host_party.begin()[i] && mir[i] == host_party.begin()[i]);
    catsync_departure_clear();
    return r;
}
void the_class_marks_follow_the_rooms_rule() {
    const uint64_t H1 = 0x70000001ull, H2 = 0x70000002ull, H3 = 0x70000003ull, H4 = 0x70000004ull, C1 = 0x71000001ull;
    // the host brought one cat, the client brought one, of ONE class: BOTH are solo (each gets the list of ONE cat -- its own)
    MarkRun r = run_marks(1, {{H1, "Hunter"}, {C1, "Hunter"}}, {H1}, {C1});
    CHECK(r.changed && r.mirror_n == 1 && r.mirror[0] == C1 && r.party_n == 1 && r.party0 == C1);
    CHECK(r.notice == (int)Tx::R_MARK_SOLO && std::strstr(marks_text, "Hunter"));                  // the chapter page said so, with the class
    r = run_marks(0, {{H1, "Hunter"}, {C1, "Hunter"}}, {H1}, {C1});
    CHECK(r.changed && r.mirror_n == 1 && r.mirror[0] == H1 && r.notice == (int)Tx::R_MARK_SOLO);
    // ... of two classes (0.1.148): neither mark, whatever the number of cats
    r = run_marks(1, {{H1, "Hunter"}, {C1, "Mage"}}, {H1}, {C1});
    CHECK(r.changed && r.mirror_n == 2 && r.mirror[0] != r.mirror[1] && r.party_n == 1 && r.party0 == C1);
    CHECK(r.notice == (int)Tx::R_MARK_NONE && std::strstr(marks_text, "Hunter/Mage"));             // the classes that make it "no mark"
    r = run_marks(0, {{H1, "Hunter"}, {C1, "Mage"}}, {H1}, {C1});
    CHECK(r.changed && r.mirror_n == 2 && r.mirror[0] != r.mirror[1]);
    // willkyu's room (X7SS): the host brought four, the client ONE. The list was the host's four, so the client got no solo mark; now nobody gets one (not everybody has one cat)
    r = run_marks(1, {{H1, "Fighter"}, {H2, "Mage"}, {H3, "Monk"}, {H4, "Fighter"}, {C1, "Colorless"}}, {H1, H2, H3, H4}, {C1});
    CHECK(r.changed && r.mirror_n == 2 && r.mirror[0] != r.mirror[1] && r.party_n == 1 && r.party0 == C1);
    // ... and the host's own check (its party is its own already) is changed all the same, its party left alone
    r = run_marks(0, {{H1, "Fighter"}, {H2, "Mage"}, {H3, "Monk"}, {H4, "Fighter"}, {C1, "Colorless"}}, {H1, H2, H3, H4}, {C1});
    CHECK(r.changed && r.mirror_n == 2 && r.mirror[0] != r.mirror[1] && r.party_n == 4 && r.party0 == H1);
    // everybody's cats are of one class, although a player brought four: the same-class mark for all (the same cat twice, so the game finds one class)
    r = run_marks(1, {{H1, "Fighter"}, {H2, "Fighter"}, {H3, "Fighter"}, {H4, "Fighter"}, {C1, "Fighter"}}, {H1, H2, H3, H4}, {C1});
    CHECK(r.changed && r.mirror_n == 2 && r.mirror[0] == r.mirror[1] && r.notice == (int)Tx::R_MARK_MONO);
    // one cat each, one class: solo, not same-class
    r = run_marks(1, {{H1, "Fighter"}, {C1, "Fighter"}}, {H1}, {C1});
    CHECK(r.changed && r.mirror_n == 1);
    // a player who has left does not count: seat 2 is not in this 2-seat room, so the host (one cat) with the client (one cat) is solo although seat 2 had brought two
    // (of another class, at that)
    r = run_marks(1, {{H1, "Hunter"}, {C1, "Hunter"}, {0x72000001ull, "Thief"}, {0x72000002ull, "Thief"}}, {H1}, {C1});
    CHECK(r.changed && r.mirror_n == 1 && r.mirror[0] == C1);
    // no record of who brought what (a run the mod did not set up, or a cat that cannot be read): the 0.1.131 behaviour -- the host's party is left alone
    entries.clear(); peer_position = 0; peer_count = 2; initialize(); catsync_departure_clear();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    static uint64_t lst[4] = {H1, H2, 0, 0};
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_CatIdCap, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    CHECK(!catsync_completion_begin("beat_chapter_boss"));
    catsync_completion_end();
    const uint64_t unreadable[2] = {H1, 0x71000009ull};                                  // the second is in no registry
    catsync_note_departure(unreadable, 2);
    CHECK(!catsync_completion_begin("beat_chapter_boss"));
    catsync_completion_end(); catsync_departure_clear();
}

// a fake GlobalProgressionData: records of 0x58 at +0x768/+0x770 (chapter string +0, class string +0x20, ints +0x40..+0x50)
alignas(16) static uint8_t g_prog[0x800]; alignas(16) static uint8_t g_recs[6 * 0x58];
void fake_str(uint8_t* at, const char* s) { std::memset(at, 0, 0x20); std::strcpy((char*)at, s); put<uint64_t>(at, 16, std::strlen(s)); put<uint64_t>(at, 24, 15); }
void fake_rec(uint32_t i, const char* chapter, const char* cls, int32_t d0, int32_t d1, int32_t clears, int32_t mono, int32_t solo) {
    uint8_t* r = g_recs + i * 0x58; std::memset(r, 0, 0x58); fake_str(r, chapter); fake_str(r + 0x20, cls);
    put<int32_t>(r, 0x40, d0); put<int32_t>(r, 0x44, d1); put<int32_t>(r, 0x48, clears); put<int32_t>(r, 0x4C, mono); put<int32_t>(r, 0x50, solo);
}
void fake_end(uint32_t n) { put<uint64_t>(g_prog, 0x768, (uint64_t)g_recs); put<uint64_t>(g_prog, 0x770, (uint64_t)(g_recs + n * 0x58)); }
bool logged(const char* s) { return captured.find(s) != std::string::npos; }
void the_marks_are_logged_and_compared() {
    const uint64_t H1 = 0x70000001ull, C1 = 0x71000001ull;
    capture_on = true;
    // 1. the rule says solo, the game gives it: the record is logged as changed, "SOLO-CAT MARK GIVEN", the check is in the ledger
    setup_room(1, {{H1, "Hunter"}, {C1, "Hunter"}}, {H1}, {C1});
    fake_rec(0, "alley", "Mage", 2, -1, 3, 0, 0); fake_rec(1, "alley", "Hunter", 1, -1, 1, 0, 0); fake_end(2);
    captured.clear(); catsync_marks_before(g_prog);
    CHECK(catsync_completion_begin("alley"));
    fake_rec(1, "alley", "Hunter", 1, -1, 2, 0, 1);                                    // the game's check: one more clear and a solo-cat clear
    catsync_completion_end(); catsync_marks_after(g_prog, "alley", false, true);
    CHECK(logged("rule: solo-cat mark"));
    CHECK(logged("party cats against the game's test"));
    CHECK(logged("record changed: chapter 'alley' class 'Hunter'") && logged("solo-cat clears 0 -> 1") && logged("SOLO-CAT MARK GIVEN"));
    CHECK(!logged("record changed: chapter 'alley' class 'Mage'"));                    // untouched records are not logged
    CHECK(logged("check done: chapter 'alley', hard path no, rule solo-cat | 0 new, 1 changed record(s) | clears +1, same-class +0, solo-cat +1"));
    CHECK(!logged("promised") && !logged("DIFFERS"));
    // 2. the rule says solo, the game does not give it: said loudly
    captured.clear(); catsync_marks_before(g_prog);
    CHECK(catsync_completion_begin("alley"));
    fake_rec(1, "alley", "Hunter", 1, -1, 3, 0, 1);                                    // one more clear, no new solo-cat clear
    catsync_completion_end(); catsync_marks_after(g_prog, "alley", true, true);
    CHECK(logged("the room's rule promised the solo-cat mark, the game gave no mark") && logged("DIFFERS FROM THE RULE") && logged("hard path yes"));
    // 3. a record that is new, and a check that counted nobody
    captured.clear(); catsync_marks_before(g_prog);
    CHECK(catsync_completion_begin("sewers"));
    fake_rec(2, "sewers", "Hunter", 0, -1, 1, 0, 1); fake_end(3);
    catsync_completion_end(); catsync_marks_after(g_prog, "sewers", false, true);
    CHECK(logged("record NEW: chapter 'sewers' class 'Hunter'"));
    captured.clear(); catsync_marks_before(g_prog);
    CHECK(catsync_completion_begin("lab"));
    catsync_completion_end(); catsync_marks_after(g_prog, "lab", false, true);
    CHECK(logged("no record's clear count rose") && logged("0 new, 0 changed"));
    // 4. the settlement: the ledger of the run and where the records of its classes stand
    captured.clear(); catsync_marks_run_summary("return home");
    CHECK(logged("run summary (return home): 4 chapter check(s) this run; this run's classes: Hunter"));
    CHECK(logged("#1 chapter 'alley': rule solo-cat, lists swapped | records compared | 0 new, 1 changed record(s); clears +1, same-class +0, solo-cat +1"));
    CHECK(logged("#3 chapter 'sewers'") && logged("#4 chapter 'lab'"));
    CHECK(logged("record now: chapter 'alley' class 'Hunter' | best difficulty 1/-1 | clears 3 | same-class clears 0 | solo-cat clears 1"));
    CHECK(!logged("class 'Mage'"));                                                    // only the classes of this run
    captured.clear(); catsync_marks_run_summary("again");
    CHECK(logged("0 chapter check(s) this run"));                                      // the ledger was emptied
    // 5. no record of who brought what: no notice, and the log says why; a rule that is not applied is not compared
    catsync_departure_clear(); marks_notices = 0; captured.clear();
    catsync_announce_marks();
    CHECK(marks_notices == 0 && logged("marks notice NOT given: no record of who brought what"));
    captured.clear(); catsync_marks_before(g_prog);
    CHECK(catsync_completion_begin("alley"));
    fake_rec(1, "alley", "Hunter", 1, -1, 4, 0, 1); catsync_completion_end(); catsync_marks_after(g_prog, "alley", false, true);
    CHECK(logged("rule NOT applied (no record of who brought what") && logged("rule not applied") && !logged("promised"));
    // 6. records that cannot be read (no progression object): the check still runs, the log says so
    captured.clear(); catsync_marks_before(nullptr); catsync_marks_after(nullptr, "alley", false, false);
    CHECK(logged("could not be read before the check") && logged("could not be compared"));
    catsync_marks_run_summary("end"); capture_on = false; captured.clear(); catsync_departure_clear();
}

// 0.1.150: THE GAME'S DEBUG CONSOLE: open only when it was seen, is enabled (+0x174) and is open (+0x198)
void the_game_console_state_is_read_from_the_object() {
    alignas(16) static uint8_t console[0x240];
    std::memset(console, 0, sizeof(console));
    gameconsole_note(nullptr); CHECK(!gameconsole_open());                          // never seen
    gameconsole_note(console); CHECK(!gameconsole_open());                          // seen, disabled, closed
    console[kConsole_Open] = 1; CHECK(!gameconsole_open());                         // a disabled console shows nothing, whatever the flag says
    console[kConsole_Enabled] = 1; CHECK(gameconsole_open());                       // enabled and open: Enter is the game's
    console[kConsole_Open] = 0; CHECK(!gameconsole_open());                         // closed again (the toggle, Esc, or Enter on an empty line)
    console[kConsole_Open] = 1; gameconsole_note((const void*)(uintptr_t)8); CHECK(!gameconsole_open());   // a pointer that cannot be read
    gameconsole_note(nullptr);
}

// 0.1.151: THE GAINED-CAT REVIEW. (1) the class check leaves a gained cat out of the party; (2) the settlement accepts a mirror that lacks only gained cats.
void gained_cats_in_the_class_check_and_the_settlement() {
    const uint64_t H1 = 0x70000001ull, H2 = 0x70000002ull, HG = 0x70000019ull, C1 = 0x71000001ull, CG = 0x71000019ull;
    // (1) the host's party holds its gained cat: the check gets the party without it (and the room's rule as before)
    setup_room(0, {{H1, "Hunter"}, {C1, "Hunter"}}, {H1, HG}, {C1});
    set_class(HG, "Mage");
    CHECK(catsync_completion_begin("alley"));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 1 && mk_lst[0] == H1);
    catsync_completion_end();
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 2 && mk_lst[0] == H1 && mk_lst[1] == HG);
    catsync_departure_clear();
    // ... a client's own gained cat (in its familiars) is not handed to the check either
    setup_room(1, {{H1, "Hunter"}, {C1, "Hunter"}}, {H1}, {C1, CG});
    set_class(CG, "Mage");
    CHECK(catsync_completion_begin("alley"));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 1 && mk_lst[0] == C1);
    catsync_completion_end(); catsync_departure_clear();
    // (2) the host's settlement: the party has its gained cat, the mirror (the game never adds it there) does not -- settled, the gained cat with the host's own
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    static uint64_t lst[4] = {H1, H2, HG, 0}, mir[4] = {H1, H2, 0, 0}, fam[4] = {C1, 0, 0, 0};
    lst[0] = H1; lst[1] = H2; lst[2] = HG; mir[0] = H1; mir[1] = H2; fam[0] = C1;
    for (uint64_t id : {H1, H2, HG, C1}) { void* cat = allocate(0xC58); put(cat, kCatData_SaveId, id); entries[id].cat = cat; }
    put(director, kDir_CatIdCount, uint32_t(3)); put(director, kDir_CatIdCap, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    put(director, kDir_MirrorCount, uint32_t(2)); put(director, kDir_MirrorCap, uint32_t(4)); put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(1)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    notes_ready = true;
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 3 && lst[2] == HG && get<uint32_t>(director, kDir_MirrorCount) == 3 && mir[2] == HG);   // the mirror is this player's own too now
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);                                                 // the client's cat leaves
    catsync_forget();
    // 0.1.152: room 2026-10-28 (the nuclear ending, a white screen): the host brought THREE cats; after the final battle the game promoted the client's cat into
    // its party (party 4, mirror 3) and no map tick came before the settlement. Settled: the host's own three home, the client's cat out of both lists.
    lst[0] = H1; lst[1] = H2; lst[2] = HG; lst[3] = C1; mir[0] = H1; mir[1] = H2; mir[2] = HG; fam[0] = 0x71000002ull;
    { void* cat = allocate(0xC58); put(cat, kCatData_SaveId, uint64_t(0x71000002ull)); entries[0x71000002ull].cat = cat; }
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_MirrorCount, uint32_t(3)); put(director, kDir_CatFamiliars + 4, uint32_t(1));
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 3 && lst[0] == H1 && lst[2] == HG && get<uint32_t>(director, kDir_MirrorCount) == 3 && get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    catsync_forget();
    // ... a death in the final battle: the dead cat left the party, the mirror still names it (party 2, mirror 3)
    lst[0] = H1; lst[1] = H2; mir[0] = H1; mir[1] = H2; mir[2] = HG; fam[0] = C1;
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_MirrorCount, uint32_t(3)); put(director, kDir_CatFamiliars + 4, uint32_t(1));
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 2 && get<uint32_t>(director, kDir_MirrorCount) == 2 && mir[0] == H1 && mir[1] == H2);
    catsync_forget();
    // ... every cat of this player died and the others won: the lists are emptied of the others' cats, the door is not held
    // (the game promoted the client's cats into the emptied party; the mirror still names the host's dead)
    lst[0] = C1; lst[1] = 0x71000002ull; mir[0] = H1; mir[1] = H2; mir[2] = HG;
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_MirrorCount, uint32_t(3)); put(director, kDir_CatFamiliars + 4, uint32_t(0));
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 0 && get<uint32_t>(director, kDir_MirrorCount) == 0 && get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    catsync_forget();
    put(director, kDir_CatIdData, (const uint64_t*)party); put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)party); put(director, kDir_MirrorCount, uint32_t(0));
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    notes_ready = false; std::strcpy(cfg.net_role, "host"); peer_position = 0; peer_count = 2;
}

void the_class_completion_check_counts_own_cats() {
    entries.clear(); peer_position = 1; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "client"); catsync_init();
    uint64_t lst[4] = {0x70000001ull, 0x70000002ull, 0x70000003ull, 0x70000004ull};          // the shared layout: the host's four
    uint64_t mir[7] = {0x70000001ull, 0x70000002ull, 0x70000003ull, 0x70000004ull, 0x71000001ull, 0x71000002ull, 0x71000003ull};
    uint64_t fam[3] = {0x71000001ull, 0x71000002ull, 0x71000003ull};                        // this player's own three
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdCap, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    put(director, kDir_MirrorCount, uint32_t(7)); put(director, kDir_MirrorCap, uint32_t(7)); put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(3)); put(director, kDir_CatFamiliars + 4, uint32_t(3)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    CHECK(catsync_completion_begin("beat_chapter_boss"));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 3 && lst[0] == 0x71000001ull && lst[1] == 0x71000002ull && lst[2] == 0x71000003ull);
    CHECK(get<uint32_t>(director, kDir_MirrorCount) == 3 && mir[0] == 0x71000001ull && mir[2] == 0x71000003ull);
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 3);                               // the familiars are not touched
    catsync_completion_end();
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4 && lst[0] == 0x70000001ull && lst[3] == 0x70000004ull);
    CHECK(get<uint32_t>(director, kDir_MirrorCount) == 7 && mir[0] == 0x70000001ull && mir[6] == 0x71000003ull);
    // the host: its party is its own already -- nothing changes
    peer_position = 0;
    CHECK(!catsync_completion_begin("beat_chapter_boss"));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4 && lst[0] == 0x70000001ull);
    catsync_completion_end();
    // a run of the plain game (no session cats): nothing changes
    uint64_t plain[2] = {0x355ull, 0x356ull};
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_CatIdData, (const uint64_t*)plain);
    CHECK(!catsync_completion_begin("beat_chapter_boss") && get<uint32_t>(director, kDir_CatIdCount) == 2 && plain[0] == 0x355ull);
    catsync_forget();
    put(director, kDir_CatIdData, (const uint64_t*)party); put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)party); put(director, kDir_MirrorCount, uint32_t(0));
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    std::strcpy(cfg.net_role, "host"); peer_position = 0; peer_count = 2;
}

void settlement_survives_the_other_player_leaving() {
    // 2026-10-03: the other player crashed mid-run. The survivor's settlement then (a) was held for ever (the filter refused a clone of a peer that is no longer counted), or (b) skipped the filter
    // altogether because the session was down, and settled the departed player's clones and never merged its own. The filter now works with whoever is left, a real cat of an older build on a
    // high session id (slot 5 and up) is no reason to refuse, and a client keeps its own position after the roster is gone.
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    uint64_t ids[8] = {0x70000001ull,0x70000002ull,0x70000003ull,0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull,0};
    uint64_t mir[3] = {0x70000001ull,0x70000002ull,0x70000003ull};
    uint64_t fam[4] = {0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    uint64_t lst[3] = {0x70000001ull,0x70000002ull,0x70000003ull};
    for (unsigned i = 0; i < 7; ++i) { void* cat = allocate(0xC58); put(cat, kCatData_SaveId, ids[i]); entries[ids[i]].cat = cat; }
    put(director, kDir_CatIdCount, uint32_t(3)); put(director, kDir_CatIdCap, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)lst);
    put(director, kDir_MirrorCount, uint32_t(3)); put(director, kDir_MirrorCap, uint32_t(4)); put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(4)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    notes_ready = true;
    CHECK(catsync_run_is_shared());                                                          // the clones in the lists are known ones
    peer_count = 1;                                                                          // the client is gone
    CHECK(catsync_prepare_settlement(director));                                             // refused before: pos 1 >= the peer count of 1
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0 && get<uint32_t>(director, kDir_CatIdCount) == 3);
    catsync_forget();

    // a real cat of an older build on slot 5 of the session range, in the host's party, does not hold the door
    peer_count = 2;
    uint64_t lst2[3] = {0x70000001ull,0x70000002ull,0x71000005ull};
    uint64_t mir2[3] = {0x70000001ull,0x70000002ull,0x71000005ull};
    { void* cat = allocate(0xC58); put(cat, kCatData_SaveId, uint64_t(0x71000005ull)); entries[0x71000005ull].cat = cat; }
    put(director, kDir_CatIdCount, uint32_t(3)); put(director, kDir_CatIdData, (const uint64_t*)lst2);
    put(director, kDir_MirrorCount, uint32_t(3)); put(director, kDir_MirrorData, (const uint64_t*)mir2);
    put(director, kDir_CatFamiliars + 4, uint32_t(4)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 3 && get<const uint64_t*>(director, kDir_CatIdData)[2] == 0x71000005ull);
    catsync_forget();
    put(director, kDir_CatIdData, (const uint64_t*)party); put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)party); put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    notes_ready = false;

    // the CLIENT keeps its position after the roster is gone: its clones (0x71...) still go home
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); depart_all(4);
    replace_party_for_real = true;
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); free_msg_cats(first);
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 90 + (int32_t)i);
    peer_count = 1; peer_position = 0;                                                       // the live roster is gone: the position now reads 0
    CHECK(catsync_merge_session_cats("after the host left") == 4);
    for (unsigned i = 0; i < 4; ++i) CHECK(get<int32_t>(entries[party[i]].cat, 0x70C) == 90 + (int32_t)i && flags_of(party[i]) == kHome);
    replace_party_for_real = false;
    entries.clear(); peer_position = 0; peer_count = 2; catsync_forget();
}

void a_settlement_retires_the_other_players_copies() {
    // 2026-10-03: the host's save held 71000001-3 and the client's 70000001-4, flagged out on adventure. After a settlement the other player's copies are retired (flags 0) unless the run's lists still hold them.
    entries.clear(); peer_position = 0; peer_count = 2; initialize(); std::strcpy(cfg.net_role, "host"); catsync_init();
    for (uint64_t id : {0x71000001ull, 0x71000002ull, 0x71000003ull}) { void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, kCatData_Flags, uint64_t(kAway)); entries[id].cat = c; }
    { void* c = allocate(0xC58); put(c, kCatData_SaveId, uint64_t(0x70000001ull)); put(c, kCatData_Flags, uint64_t(kHome)); entries[0x70000001ull].cat = c; }   // this peer's own: not touched
    static uint64_t in_run[1] = {0x71000003ull};
    put(director, kDir_CatIdCount, uint32_t(1)); put(director, kDir_CatIdData, (const uint64_t*)in_run);
    CHECK(catsync_retire_peer_copies("t") == 2);
    CHECK(flags_of(0x71000001ull) == 0 && flags_of(0x71000002ull) == 0);
    CHECK(flags_of(0x71000003ull) == kAway && flags_of(0x70000001ull) == kHome);
    CHECK(catsync_retire_peer_copies("again") == 0);
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    entries.clear(); peer_position = 0; catsync_forget();
}

// The halt after a resume (2026-10-03): an identical session cat was skipped at setup, so the owner's loader-built object and the other peer's rebuilt copy derived different stats. At setup it is rebuilt anyway.
void identical_session_cats_are_rebuilt_at_setup() {
    const uint64_t id = 0x71000004ULL;
    CatImage image{0x1122334455667788ULL, 16, 1, {41}, 5, 10};
    auto m = message(id, image);
    CHECK(catsync_apply_snapshot(m, "all-player setup exchange", true));          // created
    const unsigned loads_after_create = post_loads;
    uint64_t before_rng[4]; std::memcpy(before_rng, rng, sizeof(rng));
    CHECK(catsync_apply_snapshot(m, "all-player setup exchange", true));          // identical bytes, same object
    CHECK(post_loads == loads_after_create + 1);                                  // rebuilt: reset + read + post-load
    CHECK(std::memcmp(before_rng, rng, sizeof(rng)) == 0);
    CHECK(entries.count(id) == 1 && get<int32_t>(entries[id].cat, 0x7A8) == 16);
    CHECK(get<uint64_t>(entries[id].cat, kCatData_SaveId) == id);
    CHECK(catsync_apply_snapshot(m, "the map tick", true));                       // a mid-run identical update still does nothing
    CHECK(post_loads == loads_after_create + 1);
}

void real_cats_on_reserved_ids_are_moved() {
    // 2026-10-03 live: the HOST's own three cats were 71000004/5/6 (older builds adopted settled clones and let the id counter run on from the session range). 71000004 is the CLIENT's fourth
    // clone slot, so the host's panel dropped it as somebody else's, and the client's real fourth clone would have been imported over it. A real cat on a reserved id is given an ordinary id
    // (identities swapped: the object keeps its address, a retired copy takes the reserved id) and the run's party list follows. A live clone (out on adventure), a retired slot and a
    // recorded clone are left alone.
    entries.clear(); peer_position = 0; peer_count = 2; initialize(); counter = 0x300;
    void* real = allocate(0xC58); put(real, kCatData_SaveId, uint64_t(0x71000004ull)); put(real, 0, uint64_t(4242)); put(real, 0x70C, int32_t(77));
    put(real, kCatData_Flags, uint64_t(kAway)); entries[0x71000004ull].cat = real;               // chosen for the run: the game has flagged it "out on adventure" already
    void* live = allocate(0xC58); put(live, kCatData_SaveId, uint64_t(0x70000003ull)); put(live, 0, uint64_t(31)); put(live, kCatData_Flags, uint64_t(kAway)); entries[0x70000003ull].cat = live;
    catsync_note_origin(0x70000003ull, party[2]);                                              // a clone this peer made: it has a recorded origin
    void* spent = allocate(0xC58); put(spent, kCatData_SaveId, uint64_t(0x70000002ull)); put(spent, 0, uint64_t(32)); put(spent, kCatData_Flags, uint64_t(0)); entries[0x70000002ull].cat = spent;
    static uint64_t chosen[3] = {party[0], party[1], 0x71000004ull};
    put(director, kDir_CatIdCount, uint32_t(3)); put(director, kDir_CatIdData, (const uint64_t*)chosen);
    sync_list();

    CHECK(catsync_relocate_squatters("t") == 1);
    CHECK(entries.count(0x301) == 1 && entries[0x301].cat == real);                          // the real object, under the next ordinary id
    CHECK(get<uint64_t>(real, kCatData_SaveId) == 0x301 && get<int32_t>(real, 0x70C) == 77 && get<uint64_t>(real, kCatData_Flags) == kAway);
    void* copy = entries[0x71000004ull].cat;                                                  // the reserved id now holds a retired copy
    CHECK(copy && copy != real && get<uint64_t>(copy, kCatData_SaveId) == 0x71000004ull && get<uint64_t>(copy, kCatData_Flags) == 0 && get<uint64_t>(copy, 0) == 4242);
    CHECK(entries[0x70000003ull].cat == live && flags_of(0x70000003ull) == kAway);           // a live clone is not touched
    CHECK(entries[0x70000002ull].cat == spent);                                              // nor a retired slot
    uint32_t n = 0; const uint64_t* data = nullptr;
    n = get<uint32_t>(director, kDir_CatIdCount); data = get<const uint64_t*>(director, kDir_CatIdData);
    CHECK(n == 3 && data[0] == party[0] && data[1] == party[1] && data[2] == 0x301);        // the party list follows the cat
    CHECK(catsync_relocate_squatters("again") == 0);                                         // idempotent: the reserved id is retired now
    // a cat on a reserved id that the roster install NOTED (the other peer's clone, sent to this one) is not a squatter, whatever its flags say
    void* theirs = allocate(0xC58); put(theirs, kCatData_SaveId, uint64_t(0x71000003ull)); put(theirs, 0, uint64_t(55)); put(theirs, kCatData_Flags, uint64_t(kHome)); entries[0x71000003ull].cat = theirs;
    // a past session's saved copy: flagged out on adventure, in nobody's party -- nobody can pick it, so it is left alone (and made no duplicate)
    void* junk = allocate(0xC58); put(junk, kCatData_SaveId, uint64_t(0x71000002ull)); put(junk, 0, uint64_t(66)); put(junk, kCatData_Flags, uint64_t(kAway)); entries[0x71000002ull].cat = junk;
    sync_list();
    notes_ready = true;
    CHECK(catsync_relocate_squatters("noted") == 0 && entries[0x71000003ull].cat == theirs);
    notes_ready = false;
    CHECK(catsync_relocate_squatters("unnoted") == 1 && entries[0x71000003ull].cat != theirs);
    CHECK(entries[0x71000002ull].cat == junk && get<uint64_t>(junk, kCatData_Flags) == kAway && counter == 0x302);   // the saved copy stayed; only two ids were handed out
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    catsync_origin_clear();
    entries.clear(); peer_position = 0; catsync_forget();
}

// The House object the House-save hook is handed: ((house+0x18)->+8)->+0x20 -> +0x4480 = a vector {count at +0xC, entity pointers at +0x10}; id at +0x80.
struct FakeHouse {
    uint8_t house[0x20]{}, a[0x10]{}, b[0x28]{}, c[0x4488]{}, vec[0x18]{};
    std::vector<std::vector<uint8_t>> ents;
    std::vector<uint8_t*> ptrs;
    explicit FakeHouse(std::initializer_list<uint64_t> ids) {
        for (uint64_t id : ids) { ents.emplace_back(0x88, 0); std::memcpy(ents.back().data() + 0x80, &id, 8); }
        for (auto& e : ents) ptrs.push_back(e.data());
        void* p = a; std::memcpy(house + 0x18, &p, 8); p = b; std::memcpy(a + 8, &p, 8); p = c; std::memcpy(b + 0x20, &p, 8); p = vec; std::memcpy(c + 0x4480, &p, 8);
        const uint32_t n = (uint32_t)ptrs.size(); std::memcpy(vec + 0xC, &n, 4); p = ptrs.data(); std::memcpy(vec + 0x10, &p, 8);
    }
    uint64_t id(size_t i) const { uint64_t v; std::memcpy(&v, ents[i].data() + 0x80, 8); return v; }
};

// THE HOST'S SAVE OF 2026-10-11 (LANYI, 20:16), rebuilt: no cat has an ordinary id (an older build dragged the counter to 710000d6), four REAL cats sit on
// the reserved ids 70000001..4, and the family table still names long-dead ancestors 1..3. That save lost the four cats to the setup and came home with a
// duplicate. Now: the squatters move to ids the save never used (not to 1), keep their family, the run's clones on the same reserved ids come home, the
// House save gives every entity its own cat, and the census finds everything -- and finds it when a cat IS lost.
void host_save_with_squatters_and_a_session_range_counter() {
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    for (uint64_t id : party) entries.erase(id);
    static uint64_t box[2] = {0x710000cbull, 0x710000c5ull};
    auto make = [](uint64_t id, uint64_t seed, uint64_t flags, int32_t stat) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, seed); put(c, kCatData_Flags, flags); put(c, 0x70C, stat); entries[id].cat = c; return c;
    };
    void* susu = make(box[0], 0x3e92abba3ffabe87ull, kHome, 11);
    void* chirya = make(box[1], 0x72b8877f10ae3c44ull, kHome, 12);
    void* sq[4];
    for (unsigned i = 0; i < 4; ++i) sq[i] = make(0x70000001ull + i, 0x5000 + i, kHome, 40 + (int32_t)i);
    pedigree.clear();
    pedigree[1] = {UINT64_MAX, UINT64_MAX}; pedigree[2] = {UINT64_MAX, UINT64_MAX}; pedigree[3] = {1, 2};
    pedigree[0x70000002ull] = {1, 2}; pedigree[box[1]] = {0x70000002ull, 3}; pedigree[box[0]] = {UINT64_MAX, UINT64_MAX};
    sync_pedigree();
    counter = 0x710000d6ull;
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_CatIdData, (const uint64_t*)box);
    put(director, kDir_MirrorCount, uint32_t(2)); put(director, kDir_MirrorCap, uint32_t(4)); put(director, kDir_MirrorData, (const uint64_t*)box);
    sync_list();

    catsync_census_take(box, 2);                                                         // the departure: six cats alive at home
    put(susu, kCatData_Flags, uint64_t(kAway)); put(chirya, kCatData_Flags, uint64_t(kAway));   // the game flags the box's cats

    // 1. the squatters move, to 4..7: above every id the save knows (the ancestors 1..3 are only in the family table)
    CHECK(catsync_relocate_squatters("t") == 4);
    for (unsigned i = 0; i < 4; ++i) {
        const uint64_t nid = 4 + i;
        CHECK(entries.count(nid) == 1 && entries[nid].cat == sq[i]);
        CHECK(get<uint64_t>(sq[i], kCatData_SaveId) == nid && get<int32_t>(sq[i], 0x70C) == 40 + (int32_t)i && flags_of(nid) == kHome);
        CHECK(flags_of(0x70000001ull + i) == 0 && entries[0x70000001ull + i].cat != sq[i]);
    }
    CHECK(!entries.count(1) && !entries.count(2) && !entries.count(3));
    CHECK(pedigree[5].first == 1 && pedigree[5].second == 2);                          // the moved cat kept its parents
    CHECK(pedigree[box[1]].first == 5 && pedigree[box[1]].second == 3);                // its child names the new id
    CHECK(counter == 7);                                                                // the game's next cat gets 8: never used, ordinary

    // 2. the run: the clones go on 70000001/2 (retired copies there now), the moved cats are not touched
    SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 0)); free_msg_cats(m);
    CHECK(reserved_notices == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(entries[4 + i].cat == sq[i] && get<int32_t>(sq[i], 0x70C) == 40 + (int32_t)i);
    CHECK(get<uint64_t>(entries[0x70000001ull].cat, 0) == 0x3e92abba3ffabe87ull && get<uint64_t>(entries[0x70000002ull].cat, 0) == 0x72b8877f10ae3c44ull);

    // 3. home: both clones merge into their originals
    settle_clone(0x70000001ull, 71); settle_clone(0x70000002ull, 72);
    CHECK(catsync_merge_session_cats("t") == 2);
    CHECK(get<int32_t>(entries[box[0]].cat, 0x70C) == 71 && get<int32_t>(entries[box[1]].cat, 0x70C) == 72);
    CHECK(flags_of(box[0]) == kHome && flags_of(box[1]) == kHome);

    // 4. the House save: the squatters' entities (from the save, still 70000001..4) come first, the clones' (added at the settlement) after
    FakeHouse h({0x70000001ull, 0x70000002ull, 0x70000003ull, 0x70000004ull, 0x70000001ull, 0x70000002ull});
    catsync_house_save(h.house);
    CHECK(h.id(0) == 4 && h.id(1) == 5 && h.id(2) == 6 && h.id(3) == 7);               // each moved cat keeps its place
    CHECK(h.id(4) == box[0] && h.id(5) == box[1]);                                     // each returned cat is its original -- once
    FakeHouse later({0x70000001ull});                                                  // every note was used once: nothing pairs with a later entity
    catsync_house_save(later.house);
    CHECK(later.id(0) == 0x70000001ull);

    // 5. the census: everything accounted for, no notice
    census_notices = 0;
    catsync_census_check("t");
    CHECK(census_notices == 0);
    // ... and a lost cat IS reported
    catsync_census_take(box, 2);
    entries.erase(6); sync_list();
    catsync_census_check("t");
    CHECK(census_notices == 1 && census_lost == 1 && census_dup == 0);
    printf("census notice names: lost [%s] dup [%s]\n", census_lost_names, census_dup_names);
    CHECK(census_lost_names[0] && strcmp(census_lost_names, "-") != 0 && strcmp(census_dup_names, "-") == 0);   // the notice names the lost cat
    // ... and a duplicate
    catsync_census_take(box, 2);
    make(0x900, get<uint64_t>(entries[4].cat, 0), kHome, 1); sync_list();
    catsync_census_check("t");
    CHECK(census_notices == 2 && census_lost == 0 && census_dup == 1);
    printf("census notice names: lost [%s] dup [%s]\n", census_lost_names, census_dup_names);
    CHECK(strcmp(census_lost_names, "-") == 0 && census_dup_names[0] && strcmp(census_dup_names, "-") != 0);    // ... and the duplicated one
    // ... and two House entities saved with one id
    catsync_census_take(box, 2);
    FakeHouse twice({0x710000cbull, 0x710000cbull});
    catsync_house_save(twice.house);
    catsync_census_check("t");
    CHECK(census_notices == 3 && census_dup >= 1);
    printf("census notice names: lost [%s] dup [%s]\n", census_lost_names, census_dup_names);
    CHECK(census_dup_names[0] && strcmp(census_dup_names, "-") != 0);                                               // ... and the cat two House entities carry

    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    put(director, kDir_MirrorCount, uint32_t(0)); put(director, kDir_MirrorData, (const uint64_t*)nullptr);
    catsync_origin_clear();
    entries.clear(); peer_position = 0; catsync_forget();
}

// THE HOST'S RUN OF 2026-10-23, rebuilt: a cat that PERISHED long ago (flags 40021) sat on the reserved id 70000002. The setup moved it away and noted a House
// rename for it -- a House entity it never had -- and at the settlement that note took the entity of this run's clone 70000002, kept as a new cat: the House
// showed the dead cat, the new cat was in no entity. Now: no note for a dead squatter, and the House check puts an entity on a dead cat right when a cat
// that came home is missing.
void a_dead_squatter_does_not_take_a_new_cats_house_entity() {
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    for (uint64_t id : party) entries.erase(id);
    static uint64_t box[2] = {0x300ull, 0x301ull};
    auto make = [](uint64_t id, uint64_t seed, uint64_t flags, int32_t stat) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, seed); put(c, kCatData_Flags, flags); put(c, 0x70C, stat); entries[id].cat = c; return c;
    };
    void* a = make(box[0], 0x1111, kHome, 11);
    make(box[1], 0x2222, kHome, 12);
    void* dead = make(0x70000002ull, 0x3333, 0x40021ull, 5);
    pedigree.clear(); sync_pedigree();
    counter = 0x301ull;
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_CatIdData, (const uint64_t*)box);
    put(director, kDir_MirrorCount, uint32_t(2)); put(director, kDir_MirrorCap, uint32_t(4)); put(director, kDir_MirrorData, (const uint64_t*)box);
    sync_list();
    put(a, kCatData_Flags, uint64_t(kAway));

    CHECK(catsync_relocate_squatters("t") == 1);                                         // the dead cat leaves the reserved id
    uint64_t dead_id = 0;
    for (auto& e : entries) if (e.second.cat == dead) dead_id = e.first;
    CHECK(dead_id && dead_id != 0x70000002ull && (flags_of(dead_id) & 0x40000));

    SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 0)); free_msg_cats(m);
    entries.erase(box[1]); sync_list();                                                  // the second cat's original is gone: its clone is kept as a new cat
    settle_clone(0x70000001ull, 71); settle_clone(0x70000002ull, 72);
    CHECK(catsync_merge_session_cats("t") == 2);
    uint64_t new_id = 0;
    for (auto& e : entries) if (e.first != box[0] && e.first != dead_id && !(e.first >= 0x70000000ull) && get<int32_t>(e.second.cat, 0x70C) == 72) new_id = e.first;
    CHECK(new_id != 0);

    // the House: the clones' entities, plus entities the save already had on cats that are not alive (a dead cat, an older build's retired clone).
    // The clone's own entity is written as the new cat -- no note for the dead squatter takes it -- and the save's own odd entities are left alone,
    // without a notice (0.1.78 judged them too and told the players 14 cats were lost).
    census_notices = 0;
    void* old_clone = allocate(0xC58); put(old_clone, kCatData_SaveId, uint64_t(0x71000005ull)); put(old_clone, kCatData_Flags, uint64_t(0)); entries[0x71000005ull].cat = old_clone;
    FakeHouse h({dead_id, 0x70000001ull, 0x70000002ull, 0x71000005ull});
    catsync_house_save(h.house);
    CHECK(h.id(0) == dead_id);                                                           // the save's own entity: untouched
    CHECK(h.id(1) == box[0]);
    CHECK(h.id(2) == new_id);                                                            // the cat that came home, not the dead one
    CHECK(h.id(3) == 0x71000005ull);
    CHECK(census_notices == 0);

    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    put(director, kDir_MirrorCount, uint32_t(0)); put(director, kDir_MirrorData, (const uint64_t*)nullptr);
    catsync_origin_clear();
    entries.clear(); peer_position = 0; catsync_forget();
}

// NOT EVERY LIVING CAT HAS A HOUSE ENTITY (0.1.86, DESKTOP-RNOL7FR 2026-10-24: two cats, flags 1003, sat on 70000001/2 with no entity; moved, they were
// reported as two lost cats). A squatter the House names is renamed in it; one the House does not name is not expected there -- no notice.
// Dcrf 48PG 11-11 (0.1.223): a "before setup" note 71000003 -> 446 (no House up) and this run's settlement note 71000003 -> 720002a6; the House holds ONE entity
// 71000003 and the Warrior 446 its own. The stale note must not win: 446 stays once, the Druid gets the entity.
void a_stale_note_does_not_take_a_new_runs_entity() {
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "client"); catsync_init();
    catsync_drop_swap_notes("t");
    auto make = [](uint64_t id, uint64_t seed, uint64_t flags) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, seed); put(c, kCatData_Flags, flags); entries[id].cat = c; return c;
    };
    make(0x446ull, 0x4460, kHome); make(0x720002a6ull, 0x2a60, kHome); sync_list();
    catsync_add_swap_note(0x71000003ull, 0x446ull, false);           // the stale one, from the "before setup" merge
    catsync_add_swap_note(0x71000003ull, 0x720002a6ull, true);       // this run's
    FakeHouse h({0x446ull, 0x71000003ull});
    catsync_house_save(h.house);
    CHECK(h.id(0) == 0x446ull && h.id(1) == 0x720002a6ull);
    // the repair of a House already written that way: 446 twice, 720002a6 due and named by none
    catsync_drop_swap_notes("t");
    catsync_add_swap_note(0x71000009ull, 0x720002a6ull, true);       // makes 720002a6 due; no entity carries 71000009
    FakeHouse twice({0x446ull, 0x446ull});
    catsync_house_save(twice.house);
    CHECK((twice.id(0) == 0x446ull && twice.id(1) == 0x720002a6ull) || (twice.id(1) == 0x446ull && twice.id(0) == 0x720002a6ull));
    catsync_drop_swap_notes("t");
}

void a_squatter_without_a_house_entity_is_not_lost() {
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    for (uint64_t id : party) entries.erase(id);
    static uint64_t box[1] = {0x300ull};
    auto make = [](uint64_t id, uint64_t seed, uint64_t flags, int32_t stat) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, seed); put(c, kCatData_Flags, flags); put(c, 0x70C, stat); entries[id].cat = c; return c;
    };
    make(box[0], 0x1111, kHome, 11);
    void* away_from_house = make(0x72000001ull, 0x4444, 0x1003ull, 21);
    void* in_house = make(0x72000002ull, 0x5555, 0x3ull, 22);
    pedigree.clear(); sync_pedigree();
    counter = 0x300ull;
    put(director, kDir_CatIdCount, uint32_t(1)); put(director, kDir_CatIdData, (const uint64_t*)box);
    put(director, kDir_MirrorCount, uint32_t(1)); put(director, kDir_MirrorCap, uint32_t(4)); put(director, kDir_MirrorData, (const uint64_t*)box);
    sync_list();
    FakeHouse h({box[0], 0x72000002ull});
    catsync_house_save(h.house);                                                         // the House is seen: it names 72000002, not 72000001
    census_notices = 0;
    CHECK(catsync_relocate_squatters("t") == 2);
    uint64_t id21 = 0, id22 = 0;
    for (auto& e : entries) { if (e.second.cat == away_from_house) id21 = e.first; if (e.second.cat == in_house) id22 = e.first; }
    CHECK(id21 && id21 < 0x70000000ull && id22 && id22 < 0x70000000ull);
    catsync_house_save(h.house);
    CHECK(h.id(0) == box[0]);
    CHECK(h.id(1) == id22);                                                              // the House's squatter: renamed
    CHECK(census_notices == 0);                                                          // the other one was never in the House: not lost

    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    put(director, kDir_MirrorCount, uint32_t(0)); put(director, kDir_MirrorData, (const uint64_t*)nullptr);
    catsync_origin_clear();
    entries.clear(); peer_position = 0; catsync_forget();
}

// A REAL CAT THAT CANNOT BE MOVED IS NEVER OVERWRITTEN: the family table is unreadable (no id may be invented), so the squatter stays, the setup stops,
// the player is told, and the cat is exactly as it was.
void a_real_cat_that_cannot_move_is_not_overwritten() {
    entries.clear(); peer_position = 0; peer_count = 2; initialize();
    std::strcpy(cfg.net_role, "host"); catsync_init();
    for (uint64_t id : party) entries.erase(id);
    static uint64_t box[1] = {0x710000cbull};
    void* own = allocate(0xC58); put(own, kCatData_SaveId, box[0]); put(own, 0, uint64_t(0xabc)); put(own, kCatData_Flags, uint64_t(kAway)); entries[box[0]].cat = own;
    void* sq = allocate(0xC58); put(sq, kCatData_SaveId, uint64_t(0x70000001ull)); put(sq, 0, uint64_t(0xdef)); put(sq, kCatData_Flags, uint64_t(kHome)); put(sq, 0x70C, int32_t(99));
    entries[0x70000001ull].cat = sq;
    counter = 0x710000d6ull;
    const void* bad = nullptr; const uint64_t cap = 8; std::memcpy(registry + 0x38, &bad, 8); std::memcpy(registry + 0x50, &cap, 8);   // the family table cannot be read
    put(director, kDir_CatIdCount, uint32_t(1)); put(director, kDir_CatIdData, (const uint64_t*)box);
    sync_list();
    reserved_notices = 0;
    SetupMsg m{}; CHECK(!catsync_prepare_party_setup(m, 0)); free_msg_cats(m);
    CHECK(reserved_notices == 1);
    CHECK(entries[0x70000001ull].cat == sq && get<int32_t>(sq, 0x70C) == 99 && get<uint64_t>(sq, 0) == 0xdef && flags_of(0x70000001ull) == kHome);
    CHECK(counter == 0x710000d6ull);
    pedigree.clear(); sync_pedigree();
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    entries.clear(); peer_position = 0; catsync_forget();
}

void recorded_origin_beats_the_seed_search() {
    // 2026-10-02: setup records which original each clone came from; the merge trusts the record first and keeps the seed search as the fallback.
    auto fresh = [] {
        entries.clear(); peer_position = 1; peer_count = 2; initialize(); counter = 0x300; depart_all(4);
        SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 1)); free_msg_cats(m);
        for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 70 + (int32_t)i);
    };
    // the setup recorded all four
    fresh();
    CHECK(catsync_origin_of(0x71000001ull) == party[0] && catsync_origin_of(0x71000004ull) == party[3]);
    CloneOriginNote saved[8]{}; const uint32_t ns = catsync_origin_export(saved, 8);
    CHECK(ns == 4);
    // a twin with the clone's seed that is ALSO out on adventure makes the seed search ambiguous; the record is not
    { void* twin = allocate(0xC58); put(twin, kCatData_SaveId, uint64_t(99)); put(twin, 0, party[1] * 10); put(twin, kCatData_Flags, uint64_t(kAway)); entries[99].cat = twin; }
    CHECK(catsync_merge_session_cats("t") == 4);
    CHECK(get<int32_t>(entries[party[1]].cat, 0x70C) == 71 && flags_of(party[1]) == kHome);          // by the record, not a new cat
    CHECK(!adopted_with_stat(71) && get<int32_t>(entries[99].cat, 0x70C) != 71);                     // and the twin was left alone
    CHECK(catsync_origin_of(0x71000002ull) == 0);                                                    // forgotten once returned
    // after a restart (a resumed run) the record comes back from the journal and still works, even if the original lost its flag
    fresh(); catsync_origin_clear(); catsync_origin_import(saved, ns);
    put(entries[party[0]].cat, kCatData_Flags, uint64_t(kHome));
    CHECK(catsync_merge_session_cats("t") == 4 && get<int32_t>(entries[party[0]].cat, 0x70C) == 70 && !adopted_with_stat(70));
    // a record that no longer fits (the cat at that id is another cat now) is ignored: the seed search takes over
    fresh(); catsync_origin_clear(); catsync_note_origin(0x71000001ull, 12345);
    CHECK(catsync_merge_session_cats("t") == 4 && get<int32_t>(entries[party[0]].cat, 0x70C) == 70);   // found by seed
    // the death of a clone that is still flagged out (it left the run's lists): the original comes home dead
    fresh();
    put(entries[0x71000003ull].cat, 0x7AC, uint8_t(1)); put(entries[0x71000003ull].cat, kCatData_Flags, uint64_t(kAway | 0x40000));
    static uint64_t alive[3] = {0x71000001ull, 0x71000002ull, 0x71000004ull};      // the game took the dead cat out of the run's lists
    put(director, kDir_CatIdCount, uint32_t(3)); put(director, kDir_CatIdData, (const uint64_t*)alive);
    CHECK(catsync_merge_session_cats("t") == 4);
    CHECK((flags_of(party[2]) & 0x20) && (flags_of(party[2]) & 0x40000) && !(flags_of(party[2]) & kCatFlag_OnAdventure));   // 0x20 added, departure bit gone
    entries.clear(); peer_position = 0; catsync_forget();
}

void stuck_originals_are_released() {
    // Cats the old behaviour left "on adventure" for good come home when no run of this player is in progress;
    // the run's own party (just departed) and the session range are never touched.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); depart_all(4);
    for (uint64_t id : {50ull, 51ull, 52ull}) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, id * 10);
        put(c, kCatData_Flags, uint64_t(id == 52 ? kHome : kAway)); entries[id].cat = c;
    }
    { void* sc = allocate(0xC58); put(sc, kCatData_SaveId, uint64_t(0x70000001ull)); put(sc, 0, uint64_t(777)); put(sc, kCatData_Flags, uint64_t(kAway)); entries[0x70000001ull].cat = sc; }
    sync_list();
    CHECK(catsync_release_stuck_cats("t") == 2);
    CHECK(flags_of(50) == 1 && flags_of(51) == 1 && flags_of(52) == kHome);          // 50 and 51 are home again
    for (unsigned i = 0; i < 4; ++i) CHECK(flags_of(party[i]) == kAway);             // the run's own party stays out
    CHECK(flags_of(0x70000001ull) == kAway);                                         // a session cat is not this function's business
    CHECK(catsync_release_stuck_cats("again") == 0);
    entries.clear(); peer_position = 0; catsync_forget();
}

void merge_refusals_and_rollback() {
    auto fresh = [] {
        entries.clear(); peer_position = 1; peer_count = 2; initialize(); counter = 0x300; depart_all(4);
        SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 1)); free_msg_cats(m);
        for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 90 + (int32_t)i);
    };
    // the original is not on adventure: it may have been played on its own -- never write over it. The clone is kept
    // as a NEW cat of its own instead (its slot is about to be re-used, so leaving it would lose it).
    fresh(); catsync_origin_clear(); put(entries[party[0]].cat, kCatData_Flags, uint64_t(kHome));      // (no record: the seed search alone)
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x71000001ull) == 0 && get<int32_t>(entries[party[0]].cat, 0x70C) != 90);
    CHECK(adopted_with_stat(90) && flags_of(party[0]) == kHome);
    // the game put the flags of the retired clone back (live: the departure box starts from the party/mirror lists):
    // the next merge must not keep the same clone as yet another new cat
    { size_t before = entries.size(); put(entries[0x71000001ull].cat, kCatData_Flags, uint64_t(kHome));
      CHECK(catsync_merge_session_cats("again") == 0 && flags_of(0x71000001ull) == 0 && entries.size() == before); }
    // ... but a twin: the cat at home IS the clone's image (only the flags differ -- the game put a retired clone back to life next to its original): the clone is
    // only retired again, no second identical cat is made (2026-10-03: "the returned cats are not retired and can go out again")
    fresh(); catsync_origin_clear(); put(entries[party[0]].cat, 0x70C, int32_t(90)); put(entries[party[0]].cat, kCatData_Flags, uint64_t(kHome));
    { size_t before = entries.size();
      CHECK(catsync_merge_session_cats("t") == 3 && flags_of(0x71000001ull) == 0 && entries.size() == before && flags_of(party[0]) == kHome); }
    // two ordinary cats with the clone's seed: ambiguous, so no match -- the clone becomes a new cat
    fresh(); catsync_origin_clear(); { void* twin = allocate(0xC58); put(twin, kCatData_SaveId, uint64_t(99)); put(twin, 0, party[1] * 10); put(twin, kCatData_Flags, uint64_t(kAway)); entries[99].cat = twin; }
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x71000002ull) == 0 && adopted_with_stat(91));
    // no original at all: kept as a new cat, with every bit of the progress it carries
    fresh(); entries[party[2]].cat = nullptr; entries.erase(party[2]);
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x71000003ull) == 0 && adopted_with_stat(92));
    // 0.1.129: NOT FROM THIS SAVE -- a cat the run settled, with no recorded origin and no cat here with its seed (another player's clone taken for this one's): retired, never kept as a new cat
    { static uint64_t run[4] = { 0x71000001ull, 0x71000002ull, 0x71000003ull, 0x71000004ull };
      fresh(); catsync_origin_clear(); entries[party[2]].cat = nullptr; entries.erase(party[2]);
      put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)run);
      const int toasts = seat_toasts; const size_t before = entries.size();
      catsync_merge_session_cats("t", true);
      CHECK(flags_of(0x71000003ull) == 0 && !adopted_with_stat(92) && entries.size() == before && seat_toasts > toasts);
      put(director, kDir_CatIdCount, uint32_t(0)); }
    // 0.1.129: THE SEAT THE CATS COME FROM WINS. This peer's clones (seat 1) have their originals here; the room now says seat 2 (a seat order gone wrong): the merge still brings seat 1's clones home
    { fresh(); const int toasts = seat_toasts;
      peer_position = 2; peer_count = 3;
      CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x71000001ull) == 0 && get<int32_t>(entries[party[0]].cat, 0x70C) == 90 && seat_toasts > toasts);
      CHECK(catsync_my_seat() == 1);
      SetupMsg m{}; catsync_prepare_party_setup(m, 2); free_msg_cats(m);              // a new run's setup ends the correction
      CHECK(catsync_my_seat() == 2);
      catsync_origin_clear(); peer_position = 1; peer_count = 2; }
    // a clone still out on a run
    fresh(); put(entries[0x71000004ull].cat, kCatData_Flags, uint64_t(kAway));
    CHECK(catsync_merge_session_cats("t") == 3 && flags_of(0x71000004ull) == kAway);
    // another player's clones are never touched
    fresh(); { void* other = allocate(0xC58); put(other, kCatData_SaveId, uint64_t(0x70000001ull)); put(other, 0, party[0] * 10); put(other, kCatData_Flags, uint64_t(kHome)); entries[0x70000001ull].cat = other; }
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x70000001ull) == kHome);
    // a failed read: the identity swap is refused and the image copy takes over (or, when that fails too, the original is put back as it was and the clone left alone)
    fresh(); const int32_t before = get<int32_t>(entries[party[0]].cat, 0x70C); fail_reads = 1;
    { const unsigned n = catsync_merge_session_cats("t");
      CHECK(n == 3 || n == 4);
      if (n == 4) CHECK(get<int32_t>(entries[party[0]].cat, 0x70C) == 90 && !(flags_of(party[0]) & kCatFlag_OnAdventure));
      else CHECK(get<int32_t>(entries[party[0]].cat, 0x70C) == before && flags_of(party[0]) == kAway && flags_of(0x71000001ull) == kHome); }
    fail_reads = 0;
    entries.clear(); peer_position = 0; catsync_forget();
}

void variable_exports_and_settlement() {
 peer_count=4;entries.clear();initialize();
 // Each local count, each owner, cloning over previous-run registry entries.
 for(unsigned p=0;p<4;++p)for(unsigned n=1;n<=4;++n){
  put(director,kDir_CatIdCount,(uint32_t)n);SetupMsg m{};
  CHECK(catsync_prepare_party_setup(m,(uint8_t)p));CHECK(m.count==n);
  for(unsigned j=0;j<n;++j){CHECK(m.cats[j].id==0x70000001ull+((uint64_t)p<<24)+j);free(m.cats[j].data);}
 }
 // Invalid partial export must release every already serialized cat.
 uint64_t badids[]={1,1};put(director,kDir_CatIdData,badids+0);put(director,kDir_CatIdCount,uint32_t(2));
 SetupMsg bad{};CHECK(!catsync_export_party_setup(bad)&&bad.count==0);for(auto& c:bad.cats)CHECK(!c.data);
 put(director,kDir_CatIdCount,uint32_t(0));CHECK(!catsync_export_party_setup(bad));
 uint64_t rng_before[4];memcpy(rng_before,rng,32);
 for(unsigned n=2;n<=4;++n)for(unsigned code=0;code<(1u<<(2*n));++code){
  unsigned sizes[4]{},x=code;for(unsigned p=0;p<n;++p){sizes[p]=1+x%4;x/=4;}
  peer_count=(uint8_t)n;
  for(unsigned owner=0;owner<n;++owner)for(unsigned local=0;local<2;++local){
   catsync_forget();setup_ready=notes_ready=resume_owners=true;peer_position=(uint8_t)owner;
   uint64_t pa[4]{},mi[4]{},fa[16]{};uint32_t pn=0,fn=0;
   unsigned displayed=local?owner:0;
   for(unsigned p=0;p<n;++p)for(unsigned j=0;j<sizes[p];++j){uint64_t id=0x70000001ull+((uint64_t)p<<24)+j;if(p==displayed)pa[pn++]=id;else fa[fn++]=id;}
   memcpy(mi,pa,32);
   put(director,kDir_CatIdCap,uint32_t(4));put(director,kDir_CatIdCount,pn);put(director,kDir_CatIdData,pa+0);
   put(director,kDir_MirrorCap,uint32_t(4));put(director,kDir_MirrorCount,pn);put(director,kDir_MirrorData,mi+0);
   put(director,kDir_CatFamiliars,uint32_t(16));put(director,kDir_CatFamiliars+4,fn);put(director,kDir_CatFamiliars+8,fa+0);
   SetupMsg m{};CHECK(catsync_export_resume(m,(uint8_t)owner));CHECK(m.count==sizes[owner]);
   for(unsigned j=0;j<m.count;++j){CHECK(m.cats[j].id==0x70000001ull+((uint64_t)owner<<24)+j);free(m.cats[j].data);}
   if(code==0 && local==0){
    // Before any battle there are owner notes, but no battle split yet.
    resume_owners=false;published.clear();
    CHECK(catsync_publish("first map owner-only publish",false,true));
    CHECK(published.size()==sizes[owner]);for(auto id:published)CHECK(session_cat_owner(id)==(int)owner);
    CatImage forged{123456,5};auto own_message=message(0x70000001ull+((uint64_t)owner<<24),forged);
    const unsigned before_reads=reads;
    CHECK(catsync_apply_snapshot(own_message,"keep own pre-battle cat"));CHECK(reads==before_reads);
    resume_owners=true;
   }
   CHECK(catsync_prepare_settlement(director));
   CHECK(get<uint32_t>(director,kDir_CatIdCount)==sizes[owner]);
   CHECK(get<uint32_t>(director,kDir_MirrorCount)==sizes[owner]);
   CHECK(get<uint32_t>(director,kDir_CatFamiliars+4)==0);
   for(unsigned j=0;j<sizes[owner];++j)CHECK(pa[j]==0x70000001ull+((uint64_t)owner<<24)+j&&mi[j]==pa[j]);
   CHECK(!catsync_prepare_settlement(director)); // cannot settle twice
  }
 }
 CHECK(!memcmp(rng_before,rng,32));
 peer_count=2;peer_position=0;notes_ready=resume_owners=false;catsync_forget();
 put(director,kDir_CatIdData,party+0);put(director,kDir_CatIdCount,uint32_t(4));
 put(director,kDir_MirrorData,party+0);put(director,kDir_MirrorCount,uint32_t(4));
 put(director,kDir_CatFamiliars+4,uint32_t(0));put(director,kDir_CatFamiliars+8,(uint64_t*)nullptr);
}

void resume_owned_snapshots() {
    catsync_forget();
    peer_position = 1;
    auto* vec = (IdVector*)(director + kDir_CatFamiliars);
    const IdVector saved_vec = *vec;
    uint64_t old_party[4]; memcpy(old_party, party, sizeof(party));
    uint64_t companions[4] = {};
    for (int i = 0; i < 8; ++i) {
        uint64_t id = 0x70000001ull + (uint64_t(i / 4) << 24) + (i % 4);
        void* cat = allocate(0xC58); entries[id].cat = cat;
        put(cat, kCatData_SaveId, id); put(cat, 0x7A8, int32_t(10 + i));
        // Deliberately use CLIENT party, HOST familiars, just like saved slot2.
        if (i < 4) companions[i] = id; else party[i - 4] = id;
    }
    *vec = {4, 4, companions};
    SetupMsg own{}, host{};
    uint64_t old_rng[4]; memcpy(old_rng, rng, sizeof(rng));
    CHECK(catsync_export_resume(own, 1));
    CHECK(catsync_export_resume(host, 0));
    CHECK(memcmp(old_rng, rng, sizeof(rng)) == 0);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(own.cats[i].id == 0x71000001ull + i);
        CHECK(host.cats[i].id == 0x70000001ull + i);
        CatImage c{}; memcpy(&c, own.cats[i].data, sizeof(c));
        CHECK(c.hp == 14 + i);
    }
    CHECK(party[0] == 0x71000001ull && companions[0] == 0x70000001ull);
    SetupMsg bad{};
    companions[0] = party[0]; CHECK(!catsync_export_resume(bad, 1) && bad.count == 0);
    companions[0] = 1; CHECK(!catsync_export_resume(bad, 1) && bad.count == 0);
    companions[0] = 0x70000001ull;
    vec->count = 3; CHECK(catsync_export_resume(bad, 1));
    for(auto& c:bad.cats) free(c.data); bad=SetupMsg{}; vec->count = 4;
    CHECK(!catsync_export_resume(bad, 2));

    published.clear(); setup_ready = false;
    CHECK(!catsync_publish("before resume handshake", false, true));
    for (int i = 0; i < 8; ++i) catsync_map_tick();
    CHECK(published.empty());
    CatImage incoming{99, 44}; auto remote = message(0x70000001ull, incoming);
    unsigned old_reads = reads;
    catsync_on_message(remote); catsync_apply_pending("before handshake");
    CHECK(reads == old_reads);
    // SETUP explicitly applies the peer snapshot, while normal CATDATA is gated.
    CHECK(catsync_apply_snapshot(remote, "resume setup"));
    CHECK(get<int32_t>(entries[remote.id].cat, 0x7A8) == 44);
    catsync_setup_sent(own);
    setup_ready = resume_owners = true;
    put(entries[own.cats[0].id].cat, 0x70C, int32_t(77));
    CHECK(catsync_publish("edit after resume", false));
    CHECK(published.size() == 1 && published[0] == own.cats[0].id);
    CHECK(published_images[own.cats[0].id].stat == 77);
    for (auto& c : own.cats) free(c.data);
    for (auto& c : host.cats) free(c.data);
    *vec = saved_vec; memcpy(party, old_party, sizeof(party));
    resume_owners = false;
    catsync_forget();
}

// 2026-09-29: the per-cat digest heals a drifted copy from its OWNER, is bounded, and
// never lets a peer push a cat it does not own. Ownership in this harness: with
// known_owners, cat 2 "is mine" on whichever peer asks; with nothing known, the host.
std::vector<CatDigestMsg> digests;
std::vector<uint8_t> published_to;
bool digest_cycle_ran() {
    const size_t n = digests.size();
    for (int i = 0; i < 200 && digests.size() == n; ++i) catsync_map_tick();
    return digests.size() > n;
}
CatDigestMsg altered(const CatDigestMsg& d, uint64_t id, uint64_t hash) {
    CatDigestMsg out = d;
    for (uint32_t i = 0; i < out.count; ++i) if (out.ids[i] == id) out.hashes[i] = hash;
    return out;
}
void digest_heals_from_owner() {
    const uint64_t ids[4] = {1, 2, 3, 4};
    std::memcpy(party, ids, sizeof(party));
    initialize();
    known_owners = true; notes_ready = false; battle_up = false; setup_ready = true;
    peer_position = 1; peer_count = 2;
    CHECK(catsync_publish("seed shared save baseline", true));
    published.clear(); published_to.clear(); digests.clear();

    // A digest goes out about once a second, listing every cat with its image hash.
    CHECK(digest_cycle_ran());
    const CatDigestMsg mine = digests.back();
    CHECK(mine.count == 4);
    for (uint32_t i = 0; i < mine.count; ++i) CHECK(mine.ids[i] == ids[i] && mine.sizes[i] == sizeof(CatImage));
    CHECK(published.empty());

    // The host holds a different copy of cat 2, which is THIS client's: one digest is
    // not enough (a push may be in flight), the second identical one triggers a resend.
    const CatDigestMsg host_stale = altered(mine, 2, 0x1234);
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.empty());
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.size() == 1 && published[0] == 2);
    // A client reaches only the host, so it heals by broadcast (the host relays).
    CHECK(published_to.empty());
    CHECK(published_images[2].seed == 20);

    // Bounded: four quick resends, then one per 16 digests, never a stop.
    published.clear();
    for (int k = 0; k < 18; ++k) { catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran()); }
    CHECK(published.size() == 4);   // attempts 2, 3, 4 and the one at the 16th digest

    // Agreement drops the entry; a later difference starts from one again.
    published.clear();
    catsync_on_digest(0, mine); CHECK(digest_cycle_ran());
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.empty());
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.size() == 1);

    // Cat 1 is the host's: this client only reports the difference, however long it lasts.
    published.clear();
    const CatDigestMsg other = altered(mine, 1, 0x5678);
    for (int k = 0; k < 6; ++k) { catsync_on_digest(0, other); CHECK(digest_cycle_ran()); }
    CHECK(published.empty());

    // Membership differences are reported, never healed here.
    CatDigestMsg fewer = mine; fewer.count = 3;
    catsync_on_digest(0, fewer); CHECK(digest_cycle_ran());
    catsync_on_digest(0, fewer); CHECK(digest_cycle_ran());
    CHECK(published.empty());

    // Its own digest echoed back and a digest from an impossible position are ignored.
    catsync_on_digest(1, host_stale); catsync_on_digest(200, host_stale);
    CHECK(digest_cycle_ran()); CHECK(published.empty());

    // HOST, three players, owners unknown: the host is the authority for every cat and
    // answers the drifted peer DIRECTLY, not everyone.
    std::strcpy(cfg.net_role, "host");
    catsync_forget();
    known_owners = false; peer_position = 0; peer_count = 3;
    CHECK(catsync_publish("host baseline", true));
    published.clear(); published_to.clear(); digests.clear();
    CHECK(digest_cycle_ran());
    const CatDigestMsg host_mine = digests.back();
    const CatDigestMsg client2 = altered(host_mine, 3, 0x9ABC);
    catsync_on_digest(2, client2); CHECK(digest_cycle_ran());
    catsync_on_digest(2, client2); CHECK(digest_cycle_ran());
    CHECK(published.size() == 1 && published[0] == 3);
    CHECK(published_to.size() == 1 && published_to[0] == 2);

    // CLIENT 1 of three, owners unknown: the host decides, so client 1 never sends.
    std::strcpy(cfg.net_role, "client");
    catsync_forget();
    peer_position = 1;
    CHECK(catsync_publish("client baseline", true));
    published.clear(); published_to.clear();
    for (int k = 0; k < 4; ++k) { catsync_on_digest(2, client2); CHECK(digest_cycle_ran()); }
    CHECK(published.empty());

    // Held cats mean this peer is behind: no digest is sent and nothing is compared.
    battle_up = true;
    CatImage incoming{77, 5};
    catsync_on_message(message(4, incoming));
    battle_up = false;
    CHECK(digest_cycle_ran());   // the drain happens on the map tick, before the digest
    CHECK(get<int32_t>(entries[4].cat, 0x7A8) == 5);

    peer_position = 0; peer_count = 2;
    catsync_forget();
}

}
namespace mgmp {
const Config& config() { return cfg; }
bool net_active() { return net_active_flag; }
bool setup_runtime_ready() { return setup_ready; }
uint8_t net_peer_pos() { return peer_position; }
uint8_t net_peer_count() { return peer_count; }
uint8_t net_peer_rank() { return peer_position; }
uint8_t net_seat_span() { return peer_count; }
bool lockstep_in_battle() { return battle_up; }
bool lockstep_cat_is_mine(uint64_t id, bool& mine) {
    if (resume_owners && session_cat_owner(id)>=0) {
        mine = session_cat_owner(id)==peer_position;
        if (split_flipped) mine = !mine;   // a split that contradicts the notes
        return true;
    }
    if (!known_owners || id < 1 || id > 4) return false;
    mine = id == 2; return true;
}
// The settlement's ownership source: the session note table, filled by the
// roster install / journal import WITHOUT any battle.
void room_say_cat_census(unsigned lost, const char* lost_names, unsigned dup, const char* dup_names) { ++census_notices; census_lost = lost; census_dup = dup; strncpy_s(census_lost_names, lost_names ? lost_names : "", _TRUNCATE); strncpy_s(census_dup_names, dup_names ? dup_names : "", _TRUNCATE); }
void room_say_reserved_real() { ++reserved_notices; }
void room_toast(const char*) { ++seat_toasts; }
const char* tr(Tx k) { last_tr = k; return "[%s]"; }
const char* gametext_class_name(uint8_t) { return nullptr; }
void room_say_marks(const char* t) { ++marks_notices; strncpy_s(marks_text, t ? t : "", _TRUNCATE); }
bool lockstep_owner_pos(uint64_t id, uint8_t& owner_pos) {
    if (!notes_ready && !resume_owners) return false;
    const int p=session_cat_owner(id);if(p>=0){owner_pos=(uint8_t)p;return true;}
    return false;
}
void lockstep_note_owner(uint64_t, uint8_t) {}
bool lockstep_live_health(uint64_t, int32_t&, int32_t&) { return false; }
bool roster_add_ids(const uint64_t*, uint32_t, const char*) { return true; }
bool roster_setup_replace_party(const uint64_t* ids, uint32_t count, const char*) {
    if (!replace_party_for_real) return true;                       // the real one writes the run's party vector
    static uint64_t written[4]; for (uint32_t i = 0; i < count && i < 4; ++i) written[i] = ids[i];
    put(director, kDir_CatIdCount, count); put(director, kDir_CatIdData, (const uint64_t*)written);
    return true;
}
void roster_setup_set_ready(bool) {}
bool roster_add_familiars(const uint64_t*, uint32_t, const char*) { return true; }
bool indepevt_in_own_event() { return false; }   // 0.1.146: a cat that joins is the host's unless this player's own random event brought it in
int roster_leave_prune(uint8_t keep, const char*) { ++prune_calls; prune_keep = keep; return 0; }
int roster_drop_positions(uint32_t mask, const char*) { ++drop_calls; drop_mask |= mask; return drop_result; }
uint32_t net_absent_seats() { return absent_seats; }
bool net_seat_peer(uint8_t pos, uint8_t& id) { if (pos >= peer_count) return false; id = member_ids[pos]; return true; }
bool net_peer_ids(uint8_t* out, uint8_t cap) { if (cap < peer_count) return false; for (unsigned i = 0; i < peer_count; ++i) out[i] = member_ids[i]; return true; }
void roster_party_swap_reset() {}
uint64_t* rng_global_stream() { return rng; }
void log_stage(const char*, ...) {}
void log_stage_quiet(const char*, ...) {}
static void capture_line(const char* tag, const char* fmt, va_list a) { char b[2400]; vsnprintf(b, sizeof(b), fmt, a); captured += tag; captured += ": "; captured += b; captured += "\n"; }
void log_line(const char* tag, const char* fmt, ...) { va_list a; va_start(a, fmt); if (capture_on) { va_list c; va_copy(c, a); capture_line(tag, fmt, c); va_end(c); } if (std::getenv("MGMP_TEST_LOG")) { std::printf("[%s] ", tag); std::vprintf(fmt, a); std::putchar(10); } va_end(a); }
void log_line_lvl(LogLevel, const char* tag, const char* fmt, ...) { va_list a; va_start(a, fmt); if (capture_on) { va_list c; va_copy(c, a); capture_line(tag, fmt, c); va_end(c); } if (std::getenv("MGMP_TEST_LOG")) { std::printf("[%s] ", tag); std::vprintf(fmt, a); std::putchar(10); } va_end(a); }
bool net_send_catdata(const CatDataMsg& m) {
    published.push_back(m.id);
    if (m.size == sizeof(CatImage)) std::memcpy(&published_images[m.id], m.data, sizeof(CatImage));
    return true;
}
bool net_send_catdata_to(uint8_t peer, const CatDataMsg& m) {
    published.push_back(m.id); published_to.push_back(peer);
    if (m.size == sizeof(CatImage)) std::memcpy(&published_images[m.id], m.data, sizeof(CatImage));
    return true;
}
bool net_send_catdigest(const CatDigestMsg& m) { digests.push_back(m); return true; }
bool net_send_chapter(const ChapterMsg&) { return true; }   // 0.1.213: the list-fix report (catsync_apply_familiars)
uintptr_t addr_of_call(Call c) {
    switch (c) {
    case C_SerializeCatData: return (uintptr_t)&serialize;
    case C_ByteStreamDtor: return (uintptr_t)&bs_dtor;
    case C_OfstreamCtor: return (uintptr_t)&ofstream_ctor;
    case C_CatDataById: return (uintptr_t)&by_id;
    case C_CatAlloc: return (uintptr_t)&allocate;
    case C_CatCtor: return (uintptr_t)&construct;
    case C_CatPostLoad: return (uintptr_t)&post_load;
    case C_CatParents: return (uintptr_t)&parents;
    case C_CatIdAppend: return (uintptr_t)&append;
    case C_CatStatusRangeDtor: return (uintptr_t)&destroy_status_range;
    default: return 0;
    }
}
uintptr_t addr_of_data(DataSym d) {
    if (d == D_MewDirectorPtr) return (uintptr_t)&director_ptr;
    if (d == D_CatIdCounter) return (uintptr_t)&counter;
    if (d == D_CatRegister) return (uintptr_t)&register_cat;
    return 0;
}
}
int main() {
    // a clone's id is 0x70000000 + (position << 24) + a slot 1..4; a cat the game made into the reserved range (a newborn kitten, 0x71000013) is no clone and keeps the game's kitten answer
    CHECK(catsync_is_clone_id(0x70000001ull) && catsync_is_clone_id(0x70000004ull) && catsync_is_clone_id(0x71000002ull) && catsync_is_clone_id(0x73000004ull));
    CHECK(!catsync_is_clone_id(0x71000013ull) && !catsync_is_clone_id(0x70000000ull) && !catsync_is_clone_id(0x70000005ull) && !catsync_is_clone_id(0x6FFFFFFFull) && !catsync_is_clone_id(0x80000001ull) && !catsync_is_clone_id(0x309ull));
    wide_names(); initialize(); imports_and_membership(); entry_replacement(); rejected_inputs();
    local_upgrade_after_peer_push(); optional_item_replacement(); resume_owned_snapshots(); settlement_owner_filter();
    settlement_without_battle_split();
    in_session_next_run_replaces_stale_session_cats(); replace_party_for_real = true; clones_return_to_their_originals(); alone_check_finds_this_players_position(); a_player_who_drops_out_loses_their_cats_for_everyone_else();
    legacy_clones_are_merged_before_they_are_overwritten(); merge_refusals_and_rollback(); clones_without_an_original_and_the_id_counter(); copies_at_home_do_not_make_the_original_ambiguous(); recorded_origin_beats_the_seed_search(); real_cats_on_reserved_ids_are_moved(); host_save_with_squatters_and_a_session_range_counter(); a_dead_squatter_does_not_take_a_new_cats_house_entity(); a_squatter_without_a_house_entity_is_not_lost(); a_stale_note_does_not_take_a_new_runs_entity(); a_real_cat_that_cannot_move_is_not_overwritten(); identical_session_cats_are_rebuilt_at_setup(); settlement_survives_the_other_player_leaving(); the_fallback_filter_takes_out_only_other_players_cats(); the_host_puts_lost_client_cats_back_after_a_fight(); the_host_takes_back_a_cat_its_owner_reports(); the_class_completion_check_counts_own_cats(); the_game_console_state_is_read_from_the_object(); the_class_marks_follow_the_rooms_rule(); the_marks_are_logged_and_compared(); gained_cats_in_the_class_check_and_the_settlement(); a_settlement_retires_the_other_players_copies(); stuck_originals_are_released(); replace_party_for_real = false; variable_exports_and_settlement();
    digest_heals_from_owner(); catsync_shutdown();
    auto* ids = (IdVector*)(director + kDir_CatFamiliars);
    std::free(ids->data);
    for (void* p : allocated) {
        auto& v = statuses(p);
        destroy_status_range(v.begin, v.end);
        std::free(v.begin);
        std::free(p);
    }
    std::printf("test_catsync: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
