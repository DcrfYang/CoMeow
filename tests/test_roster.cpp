#include "mgmp_roster.h"
#include "mgmp_addresses.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <utility>
#include <map>

namespace {
using namespace mgmp;
Config cfg;
uint8_t director[0x800]{};
void* director_ptr = director;
uint8_t button[600]{};
uint64_t party[4]{}, mirror[4]{}, familiars[4]{};
const uint64_t hosts[4] = {0x2da, 0x24d, 0x1fe, 0x2b3};
const uint64_t locals[4] = {0x28a, 0x2aa, 0x2d3, 0x2df};
bool active = true, in_battle = false, list_gone = true, battle_owners = true, import_owners = true;
uint8_t peer = 1;
uint8_t peers = 2;
std::map<uint64_t, uint8_t> owner_notes;
unsigned appends = 0;
bool fake_append = false, grow_vectors = false;
unsigned checks = 0, failures = 0, writes = 0, warnings = 0;
void* denied_write = nullptr;
const void* denied_read = nullptr;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
template<class T> void put(size_t off, T value) { std::memcpy(director + off, &value, sizeof(value)); }
bool same(const uint64_t* a, const uint64_t* b) { return std::memcmp(a, b, sizeof(party)) == 0; }
void initialize() {
    roster_party_swap_reset();
    active = true; peer = 1; in_battle = false; list_gone = true;
    battle_owners = import_owners = true; denied_write = nullptr; denied_read = nullptr;
    peers = 2; owner_notes.clear(); appends = 0; fake_append = false;
    std::memcpy(party, hosts, sizeof(party));
    std::memcpy(mirror, hosts, sizeof(mirror));
    std::memcpy(familiars, locals, sizeof(familiars));
    put(kDir_CatIdCap, uint32_t(4)); put(kDir_CatIdCount, uint32_t(4)); put(kDir_CatIdData, party + 0);
    put(kDir_MirrorCap, uint32_t(4)); put(kDir_MirrorCount, uint32_t(4)); put(kDir_MirrorData, mirror + 0);
    put(kDir_CatFamiliars, uint32_t(4)); put(kDir_CatFamiliars + 4, uint32_t(4));
    put(kDir_CatFamiliars + 8, familiars + 0);
}
void map_lists() {
    initialize();
    roster_catselect_redirect(button);
    CHECK(same(party, locals));
    CHECK(same(familiars, hosts));
    CHECK(same(mirror, locals));
    const unsigned before = writes;
    roster_catselect_redirect(button);
    CHECK(writes == before);
    party[1] = hosts[1];
    roster_catselect_redirect(button);
    CHECK(same(party, locals));
    CHECK(same(mirror, locals));
    roster_party_swap_release();
    CHECK(same(party, hosts));
    CHECK(same(familiars, locals));
    CHECK(same(mirror, hosts));
}
void node_lifecycle() {
    for (uint32_t type = 12; type <= 16; ++type) {
        initialize(); roster_party_swap_on_map();
        CHECK(roster_party_swap_release());
        CHECK(same(party, hosts));
        in_battle = true;
        roster_party_swap_after_node(type);
        CHECK(same(party, locals) && same(mirror, locals));
        list_gone = false; roster_party_swap_watch();
        list_gone = true; roster_party_swap_watch();
        CHECK(same(party, locals));
        party[2] = hosts[2];
        roster_party_swap_on_map();
        CHECK(same(party, locals));
    }
    for (uint32_t type = 5; type <= 8; ++type) {
        initialize(); roster_party_swap_on_map();
        CHECK(roster_party_swap_release());
        roster_party_swap_on_map();
        CHECK(same(party, hosts));
        roster_party_swap_after_node(type);
        roster_party_swap_on_map(); roster_catselect_redirect(button);
        CHECK(same(party, locals) && same(mirror, locals));
        roster_party_swap_watch();
        CHECK(same(party, locals));
        list_gone = false; roster_party_swap_watch();
        list_gone = true; roster_party_swap_watch();
        CHECK(same(party, locals));
        roster_set_turn_control(button);
        list_gone = false; roster_party_swap_watch();
        list_gone = true; roster_party_swap_watch();
        CHECK(same(party, locals) && same(mirror, locals));
        CHECK(same(familiars, hosts));
        const unsigned before = writes;
        roster_party_swap_watch(); roster_party_swap_on_map();
        CHECK(writes == before);
    }
    for (uint32_t type = 9; type <= 11; ++type) {
        initialize(); roster_party_swap_on_map();
        CHECK(roster_party_swap_release());
        roster_party_swap_after_node(type);
        list_gone = false; roster_party_swap_watch();
        list_gone = true; roster_party_swap_watch();
        roster_catselect_redirect(button);
        CHECK(same(party, hosts));
        roster_party_swap_on_map();
        CHECK(same(party, locals) && same(mirror, locals));
    }
}
void refused_writes_and_reset() {
    initialize(); roster_party_swap_on_map();
    denied_write = mirror;
    CHECK(!roster_party_swap_release());
    denied_write = nullptr;
    CHECK(roster_party_swap_release());
    CHECK(same(party, hosts) && same(familiars, locals) && same(mirror, hosts));
    roster_party_swap_after_node(12);
    // A mirror that merely disagrees with the party is what the native post-battle
    // promotion leaves behind (2026-09-29): it is repaired, not refused. Only a
    // mirror that cannot be read at all stops node entry.
    put(kDir_MirrorCount, uint32_t(17));
    const unsigned before = writes;
    CHECK(!roster_party_swap_release());
    CHECK(writes == before);
    put(kDir_MirrorCount, uint32_t(3));
    CHECK(roster_party_swap_release());
    CHECK(same(party, hosts) && same(familiars, locals) && same(mirror, hosts));
    roster_party_swap_reset();
    std::memcpy(party, hosts, sizeof(party));
    std::memcpy(mirror, hosts, sizeof(mirror));
    std::memcpy(familiars, locals, sizeof(familiars));
    std::swap(familiars[0], familiars[1]);
    roster_party_swap_on_map();
    CHECK(party[0] == locals[1] && party[1] == locals[0]);
    CHECK(roster_party_swap_release());
    CHECK(familiars[0] == locals[1] && familiars[1] == locals[0]);
}
void initial_capture() {
    initialize(); battle_owners = false;
    roster_party_swap_on_map();
    CHECK(same(party, locals) && same(mirror, locals));
    CHECK(roster_party_swap_release());
    CHECK(same(party, hosts));
    initialize(); denied_read = mirror;
    const unsigned before = writes;
    CHECK(!roster_party_swap_release());
    CHECK(writes == before);
    denied_read = nullptr;
    CHECK(roster_party_swap_release());
    party[1] = locals[1];
    roster_party_swap_after_node(12);
    CHECK(same(party, locals) && same(mirror, locals));
    CHECK(roster_party_swap_release());
    CHECK(same(party, hosts));
    initialize(); battle_owners = import_owners = false;
    // Real 12:26 failure: a host-loaded 4+4 save, no client import file,
    // and no first battle yet. Entry must not depend on that battle's owners.
    CHECK(roster_party_swap_release());
    CHECK(same(party, hosts));
    CHECK(owner_notes.size() == 8);
    roster_party_swap_after_node(5);
    roster_set_turn_control(button);
    list_gone = false; roster_party_swap_watch();
    list_gone = true; roster_party_swap_watch();
    CHECK(same(party, locals) && same(mirror, locals));
    CHECK(roster_party_swap_release());
    CHECK(same(party, hosts) && same(familiars, locals));
    initialize(); battle_owners = import_owners = false;
    roster_party_swap_on_map();
    CHECK(same(party, locals) && same(mirror, locals));
    CHECK(owner_notes[locals[0]] == 1 && owner_notes[hosts[0]] == 0);
}
void bootstrap_rejects_bad_identity() {
    for (unsigned bad = 0; bad < 9; ++bad) {
        initialize(); battle_owners = import_owners = false;
        if (bad == 0) put(kDir_MirrorCount, uint32_t(17));   // unreadable, not merely different
        if (bad == 1) familiars[0] = hosts[0];
        if (bad == 2) familiars[1] = familiars[0];
        if (bad == 3) familiars[0] = 0;
        if (bad == 4) peers = 3;
        if (bad == 5) owner_notes[locals[0]] = 0;
        if (bad == 6) roster_set_turn_control(button);
        if (bad == 7) owner_notes[hosts[0]] = 1;
        if (bad == 8) owner_notes[hosts[0]] = 2;
        const unsigned before = writes;
        const auto notes_before = owner_notes;
        CHECK(!roster_party_swap_release());
        CHECK(writes == before && owner_notes == notes_before);
    }
}
void familiar_import_membership() {
    initialize();
    CHECK(roster_add_familiars(locals, 4, "already in save"));
    CHECK(appends == 0 && same(familiars, locals));
    CHECK(roster_add_familiars(locals, 4, "retry"));
    CHECK(appends == 0);
    put(kDir_CatFamiliars + 4, uint32_t(2));
    CHECK(roster_add_familiars(locals, 4, "partly in save"));
    CHECK(appends == 2 && same(familiars, locals));
    initialize(); denied_read = familiars + 2;
    CHECK(!roster_add_familiars(locals, 4, "unreadable member"));
    CHECK(appends == 0);
    initialize(); put(kDir_CatFamiliars + 4, uint32_t(3)); fake_append = true;
    CHECK(!roster_add_familiars(locals, 4, "append did not store"));
    initialize(); put(kDir_CatFamiliars, uint32_t(3));
    CHECK(!roster_add_familiars(locals, 4, "invalid capacity"));
    initialize(); const uint64_t invalid[] = {0};
    CHECK(!roster_add_familiars(invalid, 1, "invalid id"));
}
void* __fastcall append_id(void* vector, const uint64_t* id) {
    ++appends;
    if (fake_append) return vector;
    auto* fields = static_cast<uint32_t*>(vector);
    if (fields[1] >= fields[0]) {
        if(!grow_vectors)return nullptr;
        uint64_t* old=nullptr;memcpy(&old,(uint8_t*)vector+8,8);
        fields[0]=fields[0]*3/2; if(fields[0]<2)fields[0]=2;
        old=(uint64_t*)realloc(old,fields[0]*8);memcpy((uint8_t*)vector+8,&old,8);
    }
    uint64_t* data = nullptr;
    std::memcpy(&data, static_cast<uint8_t*>(vector) + 8, sizeof(data));
    data[fields[1]++] = *id;
    // Native 48082 leaves the appended value in RAX, not a success pointer.
    return (void*)(uintptr_t)*id;
}
void variable_parties(){
 const size_t off[]={kDir_CatIdCap,kDir_CatFamiliars,kDir_MirrorCap};
 for(unsigned n=2;n<=4;++n)for(unsigned code=0;code<(1u<<(n*2));++code){
  unsigned sizes[4]{},x=code,total=0;uint64_t ids[16]{};
  for(unsigned p=0;p<n;++p){sizes[p]=1+x%4;x/=4;for(unsigned j=0;j<sizes[p];++j)ids[total++]=0x70000001ull+((uint64_t)p<<24)+j;}
  for(unsigned owner=0;owner<n;++owner){
   initialize();peer=(uint8_t)owner;peers=(uint8_t)n;grow_vectors=true;
   // Match native preparation: exact party/mirror capacity, empty null familiars.
   for(auto o:off){
    uint32_t initial=o==kDir_CatFamiliars?0:sizes[owner];
    auto* data=initial?(uint64_t*)calloc(initial,8):nullptr;
    for(unsigned i=0;i<initial;++i)data[i]=0x70000001ull+((uint64_t)owner<<24)+i;
    put(o,initial);put(o+4,initial);put(o+8,data);
   }
   CHECK(roster_setup_install_shared(ids,total,"variable party regression"));
   CHECK(owner_notes.size()==total);
   roster_party_swap_on_map();
   uint32_t pn=0,mn=0,fn=0;uint64_t *p=nullptr,*m=nullptr;
   memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&mn,director+kDir_MirrorCount,4);memcpy(&fn,director+kDir_CatFamiliars+4,4);
   memcpy(&p,director+kDir_CatIdData,8);memcpy(&m,director+kDir_MirrorData,8);
   CHECK(pn==sizes[owner]&&mn==pn&&fn==total-pn);
   CHECK(!memcmp(p,m,pn*8));for(unsigned j=0;j<pn;++j)CHECK(p[j]==0x70000001ull+((uint64_t)owner<<24)+j);
   CHECK(roster_party_swap_release());
   memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&fn,director+kDir_CatFamiliars+4,4);
   CHECK(pn==sizes[0]&&fn==total-pn);CHECK(!memcmp(p,ids,pn*8));
   // Real 2026-09-29 failure (Host3/Client2): after a battle the native promotion
   // fills the PARTY to four from the familiars and leaves the MIRROR alone. The
   // host must put the client's cat back, the client must still reach its local
   // view, and the next node entry must restore the owner-split shared layout.
   if(sizes[0]<4&&total>sizes[0]){
    roster_party_swap_after_node(5);
    uint32_t qn=0,gn=0;uint64_t *q=nullptr,*g=nullptr;
    memcpy(&qn,director+kDir_CatIdCount,4);memcpy(&gn,director+kDir_CatFamiliars+4,4);
    memcpy(&q,director+kDir_CatIdData,8);memcpy(&g,director+kDir_CatFamiliars+8,8);
    const unsigned k=(4-qn)<gn?(4-qn):gn;
    q=(uint64_t*)realloc(q,16*8);for(unsigned j=0;j<k;++j)q[qn+j]=g[j];
    memmove(g,g+k,(gn-k)*8);qn+=k;gn-=k;
    put(kDir_CatIdCap,uint32_t(16));put(kDir_CatIdCount,qn);put(kDir_CatIdData,q);
    put(kDir_CatFamiliars+4,gn);
    roster_party_swap_on_map();
    memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&mn,director+kDir_MirrorCount,4);
    memcpy(&p,director+kDir_CatIdData,8);memcpy(&m,director+kDir_MirrorData,8);
    CHECK(pn==sizes[owner]&&mn==pn&&!memcmp(p,m,pn*8));
    for(unsigned j=0;j<pn;++j)CHECK(p[j]==0x70000001ull+((uint64_t)owner<<24)+j);
    CHECK(roster_party_swap_release());
    memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&fn,director+kDir_CatFamiliars+4,4);
    memcpy(&p,director+kDir_CatIdData,8);
    CHECK(pn==sizes[0]&&fn==total-pn);CHECK(!memcmp(p,ids,pn*8));
    const unsigned before_idem=writes;
    if(owner==0){roster_party_swap_on_map();CHECK(writes==before_idem);}
   }
   // Real 2026-09-29 failure (Host3/Client2, event 'CatHole'): the host's cat 2 left
   // the run (leave_party_temporarily) on the host only. The client must take the
   // host's membership from the node snapshot, keep it through its local view and
   // the next release, and take the cat back when the host lists it again.
   if(sizes[0]>=2){
    uint64_t cats[4]{},fam[16]{};unsigned cn=0,fcount=0;
    for(unsigned j=0;j<sizes[0];++j)if(j!=1)cats[cn++]=ids[j];
    for(unsigned j=sizes[0];j<total;++j)fam[fcount++]=ids[j];
    const unsigned before_adopt=writes;
    CHECK(roster_adopt_host_shared(cats,cn,fam,fcount,"CatHole regression"));
    memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&mn,director+kDir_MirrorCount,4);memcpy(&fn,director+kDir_CatFamiliars+4,4);
    memcpy(&p,director+kDir_CatIdData,8);memcpy(&m,director+kDir_MirrorData,8);
    if(owner==0){
     CHECK(writes==before_adopt);CHECK(pn==sizes[0]);
    }else{
     CHECK(pn==cn&&mn==cn&&fn==fcount);CHECK(!memcmp(p,cats,cn*8)&&!memcmp(m,cats,cn*8));
     // A party cat owned by a client is not the host's roster: refused, nothing written.
     const unsigned before_bad=writes;uint64_t bad[1]={fam[0]};
     CHECK(roster_adopt_host_shared(bad,1,nullptr,0,"bad roster"));CHECK(writes==before_bad);
     roster_party_swap_after_node(12);roster_party_swap_on_map();
     memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&fn,director+kDir_CatFamiliars+4,4);memcpy(&p,director+kDir_CatIdData,8);
     CHECK(pn==sizes[owner]&&fn==total-1-sizes[owner]);
     for(unsigned j=0;j<pn;++j)CHECK(p[j]==0x70000001ull+((uint64_t)owner<<24)+j);
     CHECK(roster_party_swap_release());
     memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&fn,director+kDir_CatFamiliars+4,4);memcpy(&p,director+kDir_CatIdData,8);
     CHECK(pn==cn&&fn==fcount&&!memcmp(p,cats,cn*8));
     // The host lists the cat again after the skipped fight.
     CHECK(roster_adopt_host_shared(ids,sizes[0],fam,fcount,"CatHole return"));
     CHECK(roster_party_swap_release());
     memcpy(&pn,director+kDir_CatIdCount,4);memcpy(&fn,director+kDir_CatFamiliars+4,4);memcpy(&p,director+kDir_CatIdData,8);
     CHECK(pn==sizes[0]&&fn==fcount&&!memcmp(p,ids,pn*8));
    }
   }
   // A second noncombat return must restore this peer again with unequal lengths.
   roster_party_swap_after_node(12);roster_party_swap_on_map();
   memcpy(&pn,director+kDir_CatIdCount,4);CHECK(pn==sizes[owner]);
   for(auto o:off){uint64_t* data=nullptr;memcpy(&data,director+o+8,8);free(data);}
   roster_party_swap_reset();grow_vectors=false;
  }
 }
 initialize();
}
void no_session_and_host() {
    initialize(); active = false;
    const unsigned before = writes;
    roster_party_swap_on_map(); roster_catselect_redirect(button);
    roster_party_swap_after_node(12); roster_party_swap_watch();
    CHECK(writes == before);
    active = true; peer = 0;
    roster_party_swap_on_map(); roster_catselect_redirect(button);
    roster_party_swap_after_node(12); roster_party_swap_watch();
    CHECK(roster_party_swap_release());
    CHECK(writes == before);
}
}
namespace mgmp {
const Config& config() { return cfg; }
bool net_active() { return active; }
NetRole net_role() { return peer ? NetRole::Client : NetRole::Host; }
uint8_t net_peer_pos() { return peer; }
uint8_t net_peer_count() { return peers; }
bool net_send_party(const PartyMsg&) { return true; }
bool lockstep_in_battle() { return in_battle; }
bool lockstep_battle_list_gone() { return list_gone; }
bool lockstep_cat_is_mine(uint64_t id, bool& mine) {
    if (!battle_owners) return false;
    for (auto v : locals) if (v == id) { mine = peer == 1; return true; }
    for (auto v : hosts) if (v == id) { mine = peer == 0; return true; }
    return false;
}
bool catsync_cat_perished(uint64_t) { return false; }   // the roster asks whether a cat that left the lists is dead for good
bool catsync_cat_exists(uint64_t) { return false; }     // ... and whether an unknown id in them is a real cat
bool lockstep_owner_pos(uint64_t id, uint8_t& owner) {
    auto found = owner_notes.find(id);
    if (found != owner_notes.end()) { owner = found->second; return true; }
    if (!import_owners) return false;
    for (auto v : locals) if (v == id) { owner = 1; return true; }
    return false;
}
void lockstep_note_owner(uint64_t id, uint8_t owner) { owner_notes[id] = owner; }
uintptr_t addr_of_data(DataSym d) { return d == D_MewDirectorPtr ? (uintptr_t)&director_ptr : 0; }
uintptr_t addr_of_call(Call c) { return c == C_CatIdAppend ? (uintptr_t)&append_id : 0; }
void log_line(const char*, const char*, ...) {}
void log_line_lvl(LogLevel level, const char*, const char*, ...) {
    if (level == LogLevel::Warn || level == LogLevel::Error) ++warnings;
}
bool mem_read(const void* src, void* dst, size_t n) {
    if (src == denied_read) return false;
    if ((uintptr_t)src == (uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr)
        src = &director_ptr;
    __try { std::memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool mem_write(void* dst, const void* src, size_t n) {
    if (dst == denied_write) return false;
    __try { std::memcpy(dst, src, n); ++writes; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool mem_read_std_string(const void* str, char* out, size_t cap) {
    if (str != button + 504 || cap < 13) return false;
    strcpy_s(out, cap, "Map_Backpack"); return true;
}
}
int main() {
    map_lists(); node_lifecycle(); refused_writes_and_reset(); initial_capture();
    bootstrap_rejects_bad_identity(); familiar_import_membership(); no_session_and_host();
    variable_parties();
    std::printf("test_roster: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
