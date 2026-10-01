#include "mgmp_checkpoint.h"
#include "mgmp_checkpoint_io.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_savefile.h"
#include "mgmp_lockstep.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace mgmp {
namespace {
using checkpoint_io::Bytes;
// Since disk version 2 the journal also carries the session's owner-note
// table (see lockstep_owner_note_export): a restore re-imports it so the home
// settlement's ownership filter knows whose cat is whose even when the
// resumed run walks straight to the home node without a battle. Version 1
// entries have no section and import nothing; the roster exchange still
// notes the cats, so old runs resume exactly as they did before.
constexpr uint32_t kMaxJournalOwners = 64;
struct Entry { CheckpointMsg certificate{}; Bytes database;
               LockstepOwnerNote owners[kMaxJournalOwners]{}; uint32_t owner_count=0; };
struct State {
    bool on=false, host=false, failed=false, configured=false, selected=false;
    bool released=false, loadIssued=false, dirty=false, arrived=false, preparing=false;
    bool awaitingMap=false, inSession=false;
    bool capture=false, committed=false, advancing=false, finished=false;
    // One node's checkpoint SKIPPED (not failed): see checkpoint_soft_fault.
    bool skip=false; uint64_t skipSeed=0;
    uint8_t slot=255, pos=255, mode=0;
    uint64_t identity=0, run=0, seq=0, map=0, selectionNonce=0;
    std::wstring root, key, path;
    CheckpointMsg config{}, tx{}, selections[kMaxPeers]{}, offers[kMaxPeers]{};
    CheckpointMsg arrivals[kMaxPeers]{};
    CheckpointMsg nextSelections[kMaxPeers]{};
    bool haveNextSelection[kMaxPeers]{};
    bool haveSelection[kMaxPeers]{}, haveOffer[kMaxPeers]{}, loaded[kMaxPeers]{};
    bool haveArrival[kMaxPeers]{}, ack[kMaxPeers]{}, committedAck[kMaxPeers]{};
    Entry pending{};
    char status[192]="waiting for all players to select saves";
    // The host-picked handshake (checkpoint_set_manual). The host fills `list`; a client only ever
    // learns waitPhase/waitMask from the host's SAVEWAIT.
    bool choosing=false, invalid=false, dismissed=false;
    CheckpointRef list[kCheckpointCandidates]{}; unsigned listN=0; bool listPrep=false;
    uint8_t waitPhase=0, waitMask=0;
    uint8_t sentPhase=255, sentMask=255; ULONGLONG sentAt=0; unsigned invalidSends=0;
};
State g;
bool g_manual=false;
// When the round was last started over here. The host repeats "invalid" a few times, a second apart; a
// repeat still in flight when this peer has already started over must not latch the panel over its new
// pick (that is what left a client unable to pick again: selected, not failed, and "invalid" on screen).
ULONGLONG g_abort_at=0;
constexpr unsigned kDiskCandidates=2;   // the journal keeps its version-39 wire shape; see enc_checkpoint
const wchar_t* const kQueue[kCheckpointCandidates]={L".0",L".1",L".2",L".3"};
uint64_t compatibility_build=0, compatibility_gpak=0;
// Protocol 41 adds selection epochs. Save/journal layout and certificate inputs
// remain generation 39 so existing certified runs can still be resumed. Bump
// this separately when a save/recovery semantic change makes them incompatible.
constexpr uint32_t kCheckpointCompatibility = 39;
// kDiskVersion 2 appends an owner-note section after the database (see
// encode_entry). Version 1 files stay readable -- a missing section is simply
// an empty table. The compatibility generation is NOT bumped: certificates
// and save semantics are unchanged.
constexpr uint32_t kDiskMagic=0x384b5043, kDiskVersion=1, kDiskVersionOwners=2;

void status(const char* text) {
    if(strcmp(g.status,text)!=0) {
        strncpy_s(g.status,text,_TRUNCATE);
        log_line("CHECKPOINT","%s",text);
    }
}
void fail(const char* why,bool broadcast=true) {
    if(g.failed) return;
    g.failed=true; status(why);
    if(broadcast && g.on) {
        CheckpointMsg m{}; m.kind=kCheckpointReject; m.run=g.run; m.identity=g.config.identity;
        net_send_checkpoint(m);
    }
}
uint64_t now() {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
}
bool all(const bool* flags) {
    if(g.config.count<2) return false;
    for(unsigned i=0;i<g.config.count;++i) if(!flags[i]) return false;
    return true;
}
int position(uint8_t peer) {
    for(unsigned i=0;i<g.config.count;++i) if(g.config.peers[i]==peer) return int(i);
    return -1;
}
bool membership() {
    uint8_t ids[kMaxPeers]{};
    return net_active() && net_peer_count()==g.config.count && net_peer_ids(ids,kMaxPeers) &&
        !memcmp(ids,g.config.peers,g.config.count);
}
bool same_config(const CheckpointMsg& m) {
    if(m.count!=g.config.count) return false;
    for(unsigned i=0;i<m.count;++i)
        if(m.identities[i]!=g.config.identities[i] || m.slots[i]!=g.config.slots[i]) return false;
    return true;
}
uint64_t certificate_hash(const CheckpointMsg& m) {
    uint8_t bytes[256]; Writer w(bytes,sizeof(bytes));
    w.u32v(kCheckpointCompatibility); w.u64v(compatibility_build); w.u64v(compatibility_gpak); w.u64v(m.run); w.u64v(m.seq); w.u64v(m.stamp); w.u64v(m.map); w.u8v(m.count);
    for(unsigned i=0;i<m.count && i<kMaxPeers;++i) {
        w.u8v(m.slots[i]); w.u64v(m.identities[i]); w.u64v(m.hashes[i]);
    }
    return w.ok?savefile_hash(bytes,w.len):0;
}
std::wstring file(const wchar_t* suffix) { return g.root+L"\\"+g.key+suffix; }
bool disk_exists(const std::wstring& path) { return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES; }
Bytes encode_entry(const Entry& e) {
    uint8_t meta[512]; uint32_t n=enc_checkpoint(meta,sizeof(meta),e.certificate,kDiskCandidates);
    if(!n || e.database.size()>kMaxSaveBytes) return {};
    Bytes out(36+n+e.database.size()+4+(size_t)e.owner_count*9+8); Writer w(out.data(),(uint32_t)out.size());
    w.u32v(kDiskMagic); w.u32v(kDiskVersionOwners); w.u32v(kCheckpointCompatibility);
    w.u64v(compatibility_build); w.u64v(compatibility_gpak); w.u32v(n);
    w.raw(meta,n); w.u32v((uint32_t)e.database.size());
    if(!e.database.empty()) w.raw(e.database.data(),(uint32_t)e.database.size());
    // The owner-note section: [u32 count][count x (u64 save_id, u8 owner_pos)].
    w.u32v(e.owner_count);
    for(uint32_t i=0;i<e.owner_count;++i) {
        w.u64v(e.owners[i].save_id); w.u8v(e.owners[i].owner_pos);
    }
    w.u64v(savefile_hash(out.data(),w.len));
    if(!w.ok) return {}; out.resize(w.len); return out;
}
bool read_entry(const std::wstring& path,Entry& out,bool confirmed) {
    Bytes bytes; if(!checkpoint_io::read(path,bytes) || bytes.size()<44) return false;
    uint64_t hash=0; memcpy(&hash,bytes.data()+bytes.size()-8,8);
    if(hash!=savefile_hash(bytes.data(),(uint32_t)bytes.size()-8)) return false;
    Reader r(bytes.data(),(uint32_t)bytes.size()-8);
    uint32_t magic=r.u32v(); uint32_t version=r.u32v();
    if(magic!=kDiskMagic || (version!=kDiskVersion && version!=kDiskVersionOwners) ||
       r.u32v()!=kCheckpointCompatibility) return false;
    if(r.u64v()!=compatibility_build || r.u64v()!=compatibility_gpak) return false;
    uint32_t n=r.u32v(); if(!r.ok || n>512 || n>r.len-r.pos) return false;
    Reader meta(r.buf+r.pos,n); if(meta.u8v()!=MSG_CHECKPOINT) return false;
    CheckpointMsg m{}; if(!dec_checkpoint(meta,m,kDiskCandidates) || !same_config(m)) return false;
    r.pos+=n; uint32_t size=r.u32v();
    if(!r.ok || size>kMaxSaveBytes) return false;
    uint32_t tail=r.len-r.pos;   // database + (version 2 only) the owner section
    if(version==kDiskVersion) { if(size!=tail) return false; }
    else if(tail<4 || size>tail-4) return false;
    if(confirmed) {
        if(!size || m.kind!=kCheckpointCommit || !m.run || !m.seq || !m.map ||
           m.token!=certificate_hash(m) || g.pos>=m.count ||
           m.hashes[g.pos]!=savefile_hash(r.buf+r.pos,size)) return false;
        for(unsigned i=0;i<m.count;++i) if(!m.hashes[i]) return false;
    }
    out.certificate=m; out.database.assign(r.buf+r.pos,r.buf+r.pos+size);
    out.owner_count=0;
    if(version==kDiskVersionOwners) {
        Reader o(r.buf+r.pos+size,tail-size);
        uint32_t count=o.u32v();
        if(!o.ok || count>kMaxJournalOwners || count*9>o.len-o.pos) return false;
        for(uint32_t i=0;i<count;++i) {
            uint64_t id=o.u64v(); uint8_t pos=o.u8v();
            if(!o.ok || !id) return false;
            out.owners[i].save_id=id; out.owners[i].owner_pos=pos;
        }
        if(o.pos!=o.len) return false;   // exact consumption, no trailing bytes
        out.owner_count=count;
    }
    return true;
}
// Is this an intact entry file written under a DIFFERENT build identity (another mod ruleset, another game build,
// another resources.gpak, another file format)? Such a file is not corrupt -- nothing of its content can be
// trusted by THIS build, which is exactly why read_entry refuses it -- but treating it as corruption wedged the
// room on "recovery refused: corrupt pairing state" for as long as the file sat there (2026-10-01: the first run
// after the ruleset joined the build identity, every slot pair played earlier was stuck).
bool entry_is_foreign(const std::wstring& path) {
    Bytes bytes; if(!checkpoint_io::read(path,bytes) || bytes.size()<44) return false;
    uint64_t hash=0; memcpy(&hash,bytes.data()+bytes.size()-8,8);
    if(hash!=savefile_hash(bytes.data(),(uint32_t)bytes.size()-8)) return false;   // torn/corrupt: not this case
    Reader r(bytes.data(),(uint32_t)bytes.size()-8);
    const uint32_t magic=r.u32v(); const uint32_t version=r.u32v(); const uint32_t compat=r.u32v();
    const uint64_t build=r.u64v(); const uint64_t gpak=r.u64v();
    if(!r.ok || magic!=kDiskMagic || (version!=kDiskVersion && version!=kDiskVersionOwners)) return false;
    return compat!=kCheckpointCompatibility || build!=compatibility_build || gpak!=compatibility_gpak;
}
bool marker(uint8_t mode) {
    Entry e{}; e.certificate=g.config; e.certificate.kind=kCheckpointOffer;
    e.certificate.mode=mode; e.certificate.run=g.run;
    return checkpoint_io::atomic_write(file(L".state"),encode_entry(e));
}
bool erase(const wchar_t* suffix) {
    auto path=file(suffix); return DeleteFileW(path.c_str()) || GetLastError()==ERROR_FILE_NOT_FOUND;
}
bool cleanup() {
    // Exact pairing only. No wildcard deletion of other runs or other local instances.
    bool ok=true; for(auto suffix:kQueue) ok=erase(suffix)&&ok;
    return erase(L".pending")&&ok;
}
CheckpointRef reference(const Entry& e) {
    auto& m=e.certificate; return {m.run,m.seq,m.stamp,m.token};
}
bool equal(const CheckpointRef& a,const CheckpointRef& b) {
    return a.run && a.seq && a.run==b.run && a.seq==b.seq && a.stamp==b.stamp && a.certificate==b.certificate;
}
bool find(const CheckpointRef& ref,Entry& out) {
    for(auto suffix:kQueue)
        if(read_entry(file(suffix),out,true) && equal(reference(out),ref)) return true;
    return false;
}
bool promote(const CheckpointMsg& c) {
    Entry newest{};
    if(read_entry(file(L".0"),newest,true) && equal(reference(newest),{c.run,c.seq,c.stamp,c.token})) return true;
    if(g.pending.database.empty() || c.hashes[g.pos]!=savefile_hash(g.pending.database.data(),(uint32_t)g.pending.database.size())) return false;
    // Each entry embeds its own certificate and checksum. A torn file cannot invalidate an older
    // entry. Repeat COMMIT never rotates twice (the equal() test above).
    //
    // THE QUEUE IS FOUR DEEP (2026-09-30; it was two): the new entry first, then the valid entries
    // already there, newest to oldest, of THE SAME RUN -- an entry of another run is not part of this
    // run's history and must not be carried along. Written oldest first, so a crash part-way leaves
    // every file either the old one or the new one, each self-validating, and at worst a duplicate
    // that the offer's common-save test collapses.
    g.pending.certificate=c;
    std::vector<Entry> chain; chain.push_back(g.pending);
    for(auto suffix:kQueue) {
        Entry e{};
        if(!read_entry(file(suffix),e,true) || e.certificate.run!=c.run || equal(reference(e),reference(g.pending))) continue;
        bool dup=false; for(const auto& k:chain) if(equal(reference(k),reference(e))) dup=true;
        if(!dup && chain.size()<kCheckpointCandidates) chain.push_back(std::move(e));
    }
    for(size_t k=chain.size();k-->0;)
        if(!checkpoint_io::atomic_write(file(kQueue[k]),encode_entry(chain[k]))) return false;
    return erase(L".pending");
}
// A restore puts the chosen entry at the head and keeps only what is OLDER than it in the same run:
// an entry newer than the restore point belongs to a timeline that has just been abandoned, and left
// in the queue it would be offered as "the latest" next time.
bool restore_queue(const Entry& chosen) {
    std::vector<Entry> older;
    for(auto suffix:kQueue) {
        Entry o{};
        if(!read_entry(file(suffix),o,true) || o.certificate.run!=chosen.certificate.run ||
           o.certificate.seq>=chosen.certificate.seq) continue;
        bool dup=false; for(const auto& k:older) if(equal(reference(k),reference(o))) dup=true;
        if(!dup) older.push_back(std::move(o));
    }
    std::sort(older.begin(),older.end(),[](const Entry& a,const Entry& b){ return a.certificate.seq>b.certificate.seq; });
    std::vector<Entry> chain; chain.push_back(chosen);
    for(auto& o:older) if(chain.size()<kCheckpointCandidates) chain.push_back(std::move(o));
    for(size_t k=chain.size();k-->0;)
        if(!checkpoint_io::atomic_write(file(kQueue[k]),encode_entry(chain[k]))) return false;
    // Best effort: a leftover that cannot be deleted (a directory squatting on the name, a sharing
    // violation) must not fail a restore whose own entry is safely at the head.
    for(size_t k=chain.size();k<kCheckpointCandidates;++k) erase(kQueue[k]);
    return true;
}
bool has_candidate(const CheckpointMsg& m) { for(const auto& c:m.candidates) if(c.run) return true; return false; }
bool has_ref(const CheckpointMsg& m,const CheckpointRef& ref) { for(const auto& c:m.candidates) if(equal(ref,c)) return true; return false; }
void choose();
void prepare();
void apply_select(const CheckpointMsg& m);
void apply_commit(const CheckpointMsg& m);
void send_offer() {
    CheckpointMsg offer=g.config; offer.kind=kCheckpointOffer; offer.mode=g.mode;
    Entry state{}; bool state_marker=false;
    if(disk_exists(file(L".state")) && !read_entry(file(L".state"),state,false) && entry_is_foreign(file(L".state"))) {
        // Written by another build/ruleset: it cannot be read here and neither can the snapshots beside it
        // (they carry the same identity), so there is nothing this build could recover. Move it aside -- not
        // delete -- and pair as if it were not there.
        const std::wstring from=file(L".state");
        const std::wstring to=from+L".foreign-"+std::to_wstring(GetTickCount64());
        const bool moved=MoveFileExW(from.c_str(),to.c_str(),0)!=0;
        log_line("CHECKPOINT","pairing state from another build or mod ruleset set aside%s -- it cannot be used by this build (identity now %016llx)",
                 moved?"":" FAILED",(unsigned long long)compatibility_build);
        if(!moved && !erase(L".state")) { fail("cannot set aside a pairing state from another build"); return; }
    }
    if(disk_exists(file(L".state"))) {
        if(!read_entry(file(L".state"),state,false)) { fail("recovery refused: corrupt pairing state"); return; }
        offer.run=state.certificate.run;
        offer.mode=state.certificate.mode;
        // Warehouse status alone must never silently release an unfinished journal.
        if(offer.mode==0) offer.mode=1;
        state_marker=true;
    }
    for(unsigned i=0;i<kCheckpointCandidates;++i) {
        Entry e{};
        if(read_entry(file(kQueue[i]),e,true)) offer.candidates[i]=reference(e);
    }
    // A mode-1 state is a debt: "this run still owes a recovery." The debt is
    // only redeemable through a snapshot the run CONFIRMED (.0/.1, written by
    // apply_select's restore or a COMMIT promote) or STAGED (.pending). A run
    // interrupted before its first boundary -- the fresh run refused at "map
    // state differs between participants" on 2026-09-28 16:55, then killed --
    // leaves the marker behind owing nothing it can pay: every later session
    // failed choose() with "no common confirmed save", and no save swap could
    // clear it, because the journal is a sidecar that never travels with the
    // .sav (17:35/17:38, slot0 restored over slot1/slot2). Discard the orphan
    // openly -- logged, never silently -- and offer whatever the selected
    // save itself says. A peer that DID confirm snapshots for this run still
    // reports them, and choose() still refuses that mix as no-common; the
    // discard only fires when both sides hold nothing.
    if(state_marker && offer.mode==1 && !has_candidate(offer) &&
       !disk_exists(file(L".pending"))) {
        log_line("CHECKPOINT","orphaned recovery state discarded: run %016llx never confirmed a snapshot", offer.run);
        if(!erase(L".state")) { fail("cannot discard an orphaned recovery state"); return; }
        offer.run=0; offer.mode=g.mode;
    }
    // Native settlement can commit its database immediately before a crash
    // prevents our finalizer hook from writing the tombstone. A warehouse save
    // must never be rolled back into that completed adventure.
    if(g.mode==0 && offer.mode==1 && has_candidate(offer)) {
        g.run=offer.run;
        if(!marker(3) || !cleanup()) { fail("cannot retire an already-settled recovery run"); return; }
        offer.mode=3; for(auto& c:offer.candidates) c={};
    }
    if(g.host) { g.offers[0]=offer; g.haveOffer[0]=true; choose(); }
    else if(!net_send_checkpoint(offer)) fail("recovery offer send failed");
}
bool configure(const CheckpointMsg& m) {
    if(g.configured) return same_config(m) && m.identity==g.config.identity;
    if(!g.selected || m.count<2 || m.count>kMaxPeers || m.peers[0]!=kHostPeer) return false;
    g.pos=255;
    for(unsigned i=0;i<m.count;++i) {
        if(!m.identities[i] || m.slots[i]>31) return false;
        for(unsigned j=0;j<i;++j) if(m.identities[i]==m.identities[j] || m.peers[i]<=m.peers[j]) return false;
        if(m.peers[i]==net_self()) g.pos=(uint8_t)i;
    }
    if(g.pos>=m.count || m.identities[g.pos]!=g.identity || m.slots[g.pos]!=g.slot ||
       !m.identity || m.hashes[g.pos]!=g.selectionNonce || m.mode!=(g.inSession?1:0)) return false;
    g.config=m; g.configured=true;
    // Human-readable zero-based slot key; participant identity digest isolates
    // unrelated players using the same slot numbers.
    uint8_t identity_bytes[40]{}; Writer w(identity_bytes,sizeof(identity_bytes));
    w.u8v(m.count); std::wstring key;
    for(unsigned i=0;i<kMaxPeers;++i) {
        if(i) key+=L"-";
        key+=i<m.count?L"slot"+std::to_wstring(m.slots[i]):L"na";
        if(i<m.count) w.u64v(m.identities[i]);
    }
    wchar_t digest[32]; swprintf_s(digest,L"-%016llx",savefile_hash(identity_bytes,w.len));
    g.key=key+digest;
    if(!membership()) return false;
    send_offer(); return !g.failed;
}
void configure_host() {
    if(!g.host || !g.selected || g.configured || g.failed) return;
    uint8_t ids[kMaxPeers]{}; uint8_t n=net_peer_count();
    if(n<2 || !net_peer_ids(ids,kMaxPeers)) return;
    CheckpointMsg m{}; m.kind=kCheckpointConfig; m.count=n; m.identity=checkpoint_io::nonce();
    m.mode=g.inSession?1:0;
    for(unsigned i=0;i<n;++i) {
        // Selection table is indexed by transport ID, not position. This
        // implementation refuses IDs outside the bounded protocol table.
        if(ids[i]>=kMaxPeers || !g.haveSelection[ids[i]]) return;
        if(g.selections[ids[i]].seq!=(g.inSession?1:0)) {
            fail("save selection and in-session chapter restart differ; return both players to save selection"); return;
        }
        m.peers[i]=ids[i]; m.slots[i]=g.selections[ids[i]].slot;
        m.identities[i]=g.selections[ids[i]].identity;
        m.hashes[i]=g.selections[ids[i]].token; // selection nonce, not a DB hash yet
    }
    if(!net_send_checkpoint(m) || !configure(m)) fail("recovery refused: invalid participants or slot configuration");
}
// Every confirmed save that ALL participants hold, newest first (same run: higher seq; else later stamp).
unsigned common_saves(CheckpointRef* out) {
    unsigned n=0;
    for(const auto& candidate:g.offers[0].candidates) {
        if(!candidate.run) continue;
        bool dup=false; for(unsigned k=0;k<n;++k) if(equal(out[k],candidate)) dup=true;
        if(dup) continue;
        bool common=true;
        for(unsigned i=0;i<g.config.count;++i) {
            auto& o=g.offers[i];
            if(o.mode==3 || (o.run && o.run!=candidate.run) || !has_ref(o,candidate)) common=false;
        }
        if(common && n<kCheckpointCandidates) out[n++]=candidate;
    }
    for(unsigned i=1;i<n;++i)
        for(unsigned j=i;j>0;--j) {
            const auto& a=out[j]; const auto& b=out[j-1];
            const bool newer=a.run==b.run ? a.seq>b.seq : a.stamp>b.stamp;
            if(!newer) break;
            std::swap(out[j],out[j-1]);
        }
    return n;
}
void send_wait(uint8_t phase);
void choose() {
    if(!g.host || !all(g.haveOffer) || g.failed || g.tx.kind==kCheckpointSelect) return;
    bool fresh=true;
    for(unsigned i=0;i<g.config.count;++i) {
        const auto& m=g.offers[i];
        if(m.mode==2) { fail("recovery refused: a participant chose play alone"); return; }
        if(m.mode==1 || has_candidate(m)) fresh=false;
    }
    CheckpointRef common[kCheckpointCandidates]{}; const unsigned nc=common_saves(common);
    if(g_manual && !g.inSession) {
        // The host decides, from a list. Preparation (a fresh start) is offered when nobody's save is on
        // the map -- mode 1 also covers a journal that still owes a recovery, which a warehouse save must
        // never silently walk away from.
        if(g.choosing || g.invalid) return;
        bool running=false;
        for(unsigned i=0;i<g.config.count;++i) if(g.offers[i].mode==1) running=true;
        g.listN=nc; for(unsigned i=0;i<nc;++i) g.list[i]=common[i];
        g.listPrep=!running;
        if(!nc && !g.listPrep) {
            g.invalid=true;
            fail("recovery refused: no common confirmed save (or this run already settled)");
            send_wait(kSaveWaitInvalid); return;
        }
        g.choosing=true;
        status("all saves selected; the host chooses the handshake save");
        send_wait(kSaveWaitChoosing); return;
    }
    const CheckpointRef best=nc?common[0]:CheckpointRef{};
    if(!fresh && !best.run) { fail("recovery refused: no common confirmed save (or this run already settled)"); return; }
    CheckpointMsg m=g.config; m.kind=kCheckpointSelect; m.mode=fresh?0:1;
    m.run=fresh?checkpoint_io::nonce():best.run; m.seq=best.seq; m.stamp=best.stamp; m.token=best.certificate;
    if(!m.run) { fail("cannot allocate recovery run identity"); return; }
    g.tx=m;
    if(!net_send_checkpoint(m)) { fail("recovery selection send failed"); return; }
    apply_select(m);
}
uint8_t selected_mask() {
    uint8_t mask=0;
    for(unsigned id=0;id<kMaxPeers;++id) if(id==0 ? g.selected : g.haveSelection[id]) mask|=(uint8_t)(1u<<id);
    return mask;
}
void send_wait(uint8_t phase) {
    if(!g.host || !g_manual || g.inSession) return;
    SaveWaitMsg m; m.phase=phase; m.selected=selected_mask();
    net_send_savewait(m);
    g.sentPhase=phase; g.sentMask=m.selected; g.sentAt=GetTickCount64();
}
void release() {
    if(!all(g.loaded) || g.released || g.failed) return;
    CheckpointMsg m=g.config; m.kind=kCheckpointRelease; m.run=g.run; m.seq=g.seq;
    if(!net_send_checkpoint(m)) { fail("recovery release send failed"); return; }
    g.released=true; g.dirty=true; status("all saves validated; loading local save");
}
void send_loaded() {
    CheckpointMsg ack{}; ack.kind=kCheckpointLoaded; ack.run=g.run; ack.seq=g.seq; ack.identity=g.config.identity;
    if(!net_send_checkpoint(ack)) fail("restored-save acknowledgement failed");
}
bool publish_chapter_map() {
    // A fresh session loads each peer's own save, and a departure-ready save
    // carries the map the game generated when its owner chose the party. Two
    // peers that prepared on their own generated two DIFFERENT maps, and the
    // first node boundary then refused to certify either ("checkpoint refused:
    // map state differs between participants", 2026-09-28 10:37) -- after which
    // every node click was held forever. The host's save is the run's authority:
    // before anyone loads, its serialized files.chapter_map row replaces the
    // row in every other participant's save. Cat data is not touched -- the
    // setup exchange owns the parties. A clean save (no row) sends size=0.
    int have=0; Bytes row;
    if(!checkpoint_io::read_file_row(g.path,"chapter_map",have,row)) { fail("fresh session refused: the host's chapter_map row is unreadable"); return false; }
    ChapterMapMsg m{}; m.selection=g.config.identity;
    if(have) { m.size=(uint32_t)row.size(); m.hash=savefile_hash(row.data(),m.size); m.data=row.data(); }
    uint8_t ids[kMaxPeers]{}; uint8_t n=net_peer_count();
    if(!net_peer_ids(ids,kMaxPeers)) { fail("fresh session refused: the peer table is unreadable"); return false; }
    for(unsigned i=0;i<n && i<kMaxPeers;++i) {
        if(ids[i]==net_self()) continue;
        if(!net_send_chaptermap_to(ids[i],m)) { fail("fresh chapter map send failed"); return false; }
    }
    log_line("CHECKPOINT", have?"-> fresh chapter map row published (%u bytes) before load"
                               :"-> fresh session: no chapter_map row to publish (clean saves)", m.size);
    return true;
}
void apply_select(const CheckpointMsg& m) {
    if(g.released || g.failed || !same_config(m) || !m.run || g.loadIssued) return;
    if(m.mode==1) {
        Entry e{};
        if(!find({m.run,m.seq,m.stamp,m.token},e) || !checkpoint_io::restore(g.path,e.database) ||
           !restore_queue(e)) {
            fail("recovery refused: selected snapshot unavailable, corrupt or local save is open"); return;
        }
        // Repopulate the ownership table from the journal BEFORE the game loads
        // the save: a resumed run can walk straight into the home node, and the
        // settlement's ownership filter must not wait for a battle's split.
        // Duplicates from the roster exchange note the same values silently.
        if(e.owner_count) {
            lockstep_owner_note_import(e.owners,e.owner_count);
            log_line("CHECKPOINT","restored %u owner note(s) from the journal",e.owner_count);
        }
    } else if(g.mode!=0 || m.seq) { fail("recovery refused: unfinished local save cannot start a fresh session"); return; }
    log_line("CHECKPOINT", "%s run=%016llx seq=%llu key=%ls", m.mode?"RESTORE":"NEW", m.run, m.seq, g.key.c_str());
    g.run=m.run; g.seq=m.seq;
    if(!marker(1)) { fail("cannot persist active recovery run"); return; }
    g.tx=m;
    if(g.host) {
        if(m.mode==0 && !g.inSession && !publish_chapter_map()) return;
        g.loaded[0]=true; release();
    } else if(m.mode==0 && !g.inSession) {
        // Fresh: the host's chapter_map row must arrive and be written into our
        // save BEFORE the Loaded ack, or the release would let both peers load
        // two different generated maps. checkpoint_on_chaptermap sends the ack.
        g.awaitingMap=true;
    } else send_loaded();
}
bool transaction(const CheckpointMsg& m) { return m.run==g.tx.run && m.seq==g.tx.seq && m.map==g.tx.map && m.stamp==g.tx.stamp; }
void prepare() {
    if(!g.host || g.preparing || !all(g.haveArrival) || g.failed) return;
    uint64_t map=g.arrivals[0].map;
    for(unsigned i=0;i<g.config.count;++i) if(g.arrivals[i].map!=map || !map) {
        fail("checkpoint refused: map state differs between participants"); return;
    }
    CheckpointMsg m=g.config; m.kind=kCheckpointPrepare; m.run=g.run; m.seq=g.seq+1; m.map=map; m.stamp=now();
    memset(m.hashes,0,sizeof(m.hashes)); // configuration carried selection nonces
    g.tx=m; g.preparing=true; g.capture=true;
    if(!net_send_checkpoint(m)) fail("checkpoint prepare send failed");
    else status("checkpoint: saving each player's local state");
}
void commit_if_ready() {
    if(!g.host || !all(g.ack) || g.committed || g.failed) return;
    CheckpointMsg m=g.tx; m.kind=kCheckpointCommit; m.token=certificate_hash(m);
    // The certificate proves every frozen participant durably staged its OWN
    // database at this boundary. Never reduce this set when a link disappears.
    if(!membership()) { fail("checkpoint held: session membership changed"); return; }
    if(!net_send_checkpoint(m)) { fail("checkpoint commit send failed"); return; }
    apply_commit(m);
}
void advance() {
    if(!g.host || !all(g.committedAck) || g.advancing || g.failed) return;
    CheckpointMsg m=g.tx; m.kind=kCheckpointAdvance;
    if(!net_send_checkpoint(m)) { fail("checkpoint completion send failed"); return; }
    g.advancing=true; g.seq=m.seq; g.dirty=false; g.capture=false;
    status("checkpoint confirmed by all players; next node unlocked");
}
void apply_commit(const CheckpointMsg& m) {
    if(!g.preparing || !transaction(m) || !same_config(m) || m.token!=certificate_hash(m)) return;
    for(unsigned i=0;i<m.count;++i) if(!m.hashes[i]) { fail("invalid checkpoint certificate"); return; }
    if(!promote(m)) { fail("checkpoint disk commit failed; previous confirmed save retained"); return; }
    log_line("CHECKPOINT", "COMMIT run=%016llx seq=%llu certificate=%016llx members=%u", m.run, m.seq, m.token, m.count);
    g.committed=true; g.tx=m;
    if(g.host) { g.committedAck[0]=true; advance(); }
    else { CheckpointMsg ack=m; ack.kind=kCheckpointCommitted;
        if(!net_send_checkpoint(ack)) fail("checkpoint commit acknowledgement failed"); }
}
} // namespace

void checkpoint_on_chaptermap(uint8_t from,const ChapterMapMsg& m) {
    // The fresh-session map pre-sync arrives between the select and the
    // release, while this peer's save is still the pre-load one on disk.
    if(!g.on || g.failed || g.host || !g.awaitingMap || from!=kHostPeer || m.selection!=g.config.identity) return;
    if(m.size) {
        if(m.size>kMaxSaveBytes || !m.data || savefile_hash(m.data,m.size)!=m.hash) { fail("fresh chapter map refused: transfer corrupt"); return; }
        Bytes row(m.data,m.data+m.size);
        if(!checkpoint_io::write_file_row(g.path,"chapter_map",row)) { fail("fresh chapter map write failed -- local save must be closed and hold a files table"); return; }
        log_line("CHECKPOINT","<- fresh chapter map row applied (%u bytes) before load",m.size);
    } else log_line("CHECKPOINT","<- fresh chapter map: the host save has no row (clean saves); nothing to sync");
    g.awaitingMap=false;
    send_loaded();
}

void checkpoint_init(uint64_t build_hash,uint64_t gpak_hash) {
    compatibility_build=build_hash; compatibility_gpak=gpak_hash;
    g=State{}; g.on=net_active(); g.host=net_role()==NetRole::Host;
    if(!g.on) return;
    wchar_t dir[MAX_PATH]{};
    if(!checkpoint_io::identity(g.identity) || !savefile_save_dir(dir,MAX_PATH)) { fail("cannot read persistent player fingerprint"); return; }
    std::wstring root=std::wstring(dir)+L"\\mgmp_handshake";
    if(!CreateDirectoryW(root.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) { fail("cannot create checkpoint directory"); return; }
    // Same-machine host/client tests must never rotate or clear each other's queue.
    g.root=root+L"\\"+std::to_wstring(g.identity);
    if(!CreateDirectoryW(g.root.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) fail("cannot create player checkpoint directory");
}
void checkpoint_shutdown() {
    g.on=false; g.pending.database.clear();
    // Keep run/key until local solo settlement; durable invalidation outlives sockets.
}
bool checkpoint_active() { return g.on; }
// Volatile negotiation only: never remove certified snapshots or tombstones.
// A client may pick its save before the host returns to the menu; keep that
// next-round request, but never reuse the old round's selection table.
static void reset_selection() {
    State old=std::move(g);
    g=State{};
    g.on=old.on; g.host=old.host; g.identity=old.identity; g.root=std::move(old.root);
    if(g.host) for(unsigned i=1;i<kMaxPeers;++i) {
        if(old.haveNextSelection[i]) {
            g.selections[i]=old.nextSelections[i]; g.haveSelection[i]=true;
        } else if(!old.configured && old.haveSelection[i]) {
            g.selections[i]=old.selections[i]; g.haveSelection[i]=true;
        }
    }
}
static bool select_impl(uint8_t slot,const wchar_t* path,bool inSession) {
    if(!g.on) return true;
    if(!path || slot>31 || !g.identity || g.root.empty()) return false;
    // path may alias g.path. Copy before clearing a completed/failed round.
    const std::wstring selected_path(path);
    if(g.finished || g.released || g.loadIssued || g.inSession || g.failed) reset_selection();
    g.inSession=inSession;
    if(g.selected) {
        status("save selection pending; finish both selections or reconnect the room to change slots");
        return false;
    }
    bool running=false;
    if(!checkpoint_io::in_run(selected_path,running)) { fail("save selection refused: unreadable or invalid SQLite save"); return false; }
    if(!net_active() || net_peer_count()<2) {
        if(!running && !inSession) {
            // No adventure can be advanced by opening an already settled House.
            // Caller tears down the leftover session before any new co-op run.
            g.on=false;
            status("no connected peer; opening local warehouse save");
            return true;
        }
        status("unfinished save held; reconnect all players and select saves again");
        return false;
    }
    if(running) {
        // "In a run" means ON THE MAP, not merely mid-departure. The game sets
        // on_adventure (and writes chapter_map) during departure preparation --
        // cat, gear and chapter screens all count -- and the 09:50 chapter-page
        // saves therefore gated as unfinished. With the journal tombstoned by
        // settlement, choose() minted a fresh session and apply_select refused
        // it: recovery was impossible from EITHER side (2026-09-28, 10:04 logs).
        // The durable separator is the start node: preparation keeps it in the
        // serialized chapter_map, departure consumes it. A save that cannot be
        // answered (query failure) keeps the old unfinished reading.
        bool on_map=false;
        if(!checkpoint_io::on_map(selected_path,on_map)) on_map=true;
        running=on_map;
    }
    g.slot=slot; g.path=selected_path; g.mode=running?1:0; g.selected=true;
    g.selectionNonce=checkpoint_io::nonce();
    if(!g.selectionNonce) { fail("cannot allocate save-selection identity"); return false; }
    CheckpointMsg m{}; m.kind=kCheckpointSlot; m.slot=slot; m.identity=g.identity; m.mode=g.mode;
    m.token=g.selectionNonce;
    m.seq=inSession?1:0;
    if(g.host) { g.selections[0]=m; g.haveSelection[0]=true; configure_host(); }
    else if(!net_send_checkpoint(m)) fail("save selection message failed");
    if(!g.failed && !g.released) status("waiting for all players' save validation");
    return false;
}
bool checkpoint_select(uint8_t slot,const wchar_t* path) { return select_impl(slot,path,false); }
int checkpoint_autoselect() {
    if(!g.on || g.failed || g.finished || g.inSession || !g.released || g.loadIssued) return -1;
    if(!membership()) { status("save load waiting: reconnect the original participants"); return -1; }
    g.loadIssued=true; return g.slot;
}
void checkpoint_on_node(uint64_t seed,uint32_t) {
    if(!g.on || !g.released) return;
    g.dirty=true; g.arrived=g.preparing=g.capture=g.committed=g.advancing=false;
    g.map=0; g.pending=Entry{};
    memset(g.haveArrival,0,sizeof(g.haveArrival)); memset(g.ack,0,sizeof(g.ack)); memset(g.committedAck,0,sizeof(g.committedAck));
    // A skip belongs to ONE node. NODEHASH can report the mismatch before this call (the client
    // compares as soon as the snapshot lands), so the seed is what ties the two together; any
    // other node starts with the checkpoint armed again.
    g.skip=(seed && seed==g.skipSeed);
    status(g.skip ? "node in progress; this node's checkpoint is skipped (roster disagreement), previous confirmed saves retained"
                  : "node in progress; previous confirmed saves retained");
}
bool checkpoint_needs_map() { return g.on && g.released && !g.failed && g.dirty && !g.skip; }
void checkpoint_on_map(uint64_t map_hash) {
    if(!checkpoint_needs_map() || !map_hash) return;
    if(lockstep_halted()) { fail("checkpoint refused: battle lockstep halted"); return; }
    if(!membership()) { status("checkpoint waiting: a participant disconnected"); return; }
    if(!g.arrived) {
        g.map=map_hash; g.arrived=true;
        CheckpointMsg m{}; m.kind=kCheckpointArrive; m.run=g.run; m.seq=g.seq+1; m.map=map_hash; m.identity=g.config.identity;
        if(g.host) { g.arrivals[0]=m; g.haveArrival[0]=true; prepare(); }
        else if(!net_send_checkpoint(m)) fail("checkpoint arrival send failed");
    }
    if(!g.capture || g.failed) return;
    if(map_hash!=g.tx.map) { fail("checkpoint refused: map changed during preparation"); return; }
    uint8_t* data=nullptr; uint32_t size=0; uint64_t hash=0;
    if(!savefile_read_checkpoint(&data,&size,&hash)) { fail("checkpoint capture failed; next node held"); return; }
    g.pending.certificate=g.tx; g.pending.certificate.hashes[g.pos]=hash;
    g.pending.database.assign(data,data+size); free(data);
    // The staged entry snapshots the ownership table too: the journal is what
    // a future restore imports it back from (2026-09-28).
    g.pending.owner_count=lockstep_owner_note_export(g.pending.owners,kMaxJournalOwners);
    if(!checkpoint_io::atomic_write(file(L".pending"),encode_entry(g.pending))) { fail("checkpoint staging failed; previous saves retained"); return; }
    log_line("CHECKPOINT", "STAGED run=%016llx seq=%llu map=%016llx local-hash=%016llx bytes=%u", g.run, g.tx.seq, g.tx.map, hash, size);
    g.capture=false;
    if(g.host) { g.tx.hashes[0]=hash; g.ack[0]=true; commit_if_ready(); }
    else { CheckpointMsg m=g.tx; m.kind=kCheckpointAck; m.hashes[g.pos]=hash;
        if(!net_send_checkpoint(m)) fail("checkpoint staging acknowledgement failed"); }
}
bool checkpoint_can_enter() {
    return !g.on || (!g.failed && !g.finished && !lockstep_halted() && g.released && (!g.dirty || g.skip) && membership());
}
void checkpoint_on_message(uint8_t from,const CheckpointMsg& m) {
    if(!g.on || from==net_self()) return;
    if(m.kind==kCheckpointSlot) {
        if(!g.host || from==0 || from>=kMaxPeers || m.slot>31 || !m.identity || !m.token) return;
        if(g.configured || g.failed) {
            if(g.haveSelection[from] && g.selections[from].token==m.token) return;
            g.nextSelections[from]=m; g.haveNextSelection[from]=true;
            log_line("CHECKPOINT", "peer %u selected a save for the next handshake; waiting for local selection", from);
            return;
        }
        if(g.haveSelection[from]) {
            if(g.selections[from].identity!=m.identity || g.selections[from].slot!=m.slot) fail("peer changed selected slot during handshake");
            return;
        }
        g.selections[from]=m; g.haveSelection[from]=true; configure_host(); return;
    }
    if(g.failed) return;
    if(m.kind==kCheckpointConfig) {
        if(g.host || from!=kHostPeer) return;
        // Reject an old configuration before it can consume the new selection.
        bool ours=false;
        for(unsigned i=0;i<m.count && i<kMaxPeers;++i)
            if(m.peers[i]==net_self() && m.hashes[i]==g.selectionNonce && g.selectionNonce) ours=true;
        if(!ours) return;
        if(!configure(m)) fail("invalid recovery participant fingerprint/slot configuration"); return;
    }
    if(!g.configured) return;
    int pos=position(from); if(pos<0) return;
    if(m.identity!=g.config.identity) return; // stale messages from a previous selection
    if(m.kind==kCheckpointReject) { fail("peer refused recovery/checkpoint; see peer log",false); return; }
    if(m.kind==kCheckpointFinished) {
        if(m.run==g.run) { status("peer finished settlement; this run cannot be recovered again"); }
        return; // only LOCAL native finalization may clear our queue
    }
    if(!membership()) { fail("checkpoint membership changed; reconnect from save selection"); return; }
    if(g.host) {
        switch(m.kind) {
        case kCheckpointOffer:
            if(!same_config(m) || g.released || g.haveOffer[pos]) return;
            g.offers[pos]=m; g.haveOffer[pos]=true; choose(); break;
        case kCheckpointLoaded:
            if(m.run!=g.run || m.seq!=g.seq || !g.run) return;
            g.loaded[pos]=true; release(); break;
        case kCheckpointArrive:
            if(!g.released || m.run!=g.run || m.seq!=g.seq+1 || !m.map) return;
            g.arrivals[pos]=m; g.haveArrival[pos]=true; prepare(); break;
        case kCheckpointAck:
            if(!g.preparing || !transaction(m) || !m.hashes[pos]) return;
            if(g.ack[pos] && g.tx.hashes[pos]!=m.hashes[pos]) { fail("peer changed staged checkpoint hash"); return; }
            g.tx.hashes[pos]=m.hashes[pos]; g.ack[pos]=true; commit_if_ready(); break;
        case kCheckpointCommitted:
            if(!g.committed || !transaction(m) || m.token!=g.tx.token) return;
            g.committedAck[pos]=true; advance(); break;
        default: break;
        }
    } else if(from==kHostPeer) {
        switch(m.kind) {
        case kCheckpointSelect: apply_select(m); break;
        case kCheckpointRelease:
            if(m.run!=g.run || m.seq!=g.seq || !g.run) return;
            g.released=true; g.dirty=true; status("all saves validated; loading local save"); break;
        case kCheckpointPrepare:
            if(!g.released || !g.arrived || g.preparing || m.run!=g.run || m.seq!=g.seq+1 || m.map!=g.map || !same_config(m)) return;
            g.tx=m; g.preparing=true; g.capture=true; break;
        case kCheckpointCommit: apply_commit(m); break;
        case kCheckpointAdvance:
            if(!g.committed || !transaction(m) || m.token!=g.tx.token) return;
            g.seq=m.seq; g.dirty=false; status("checkpoint confirmed by all players; next node unlocked"); break;
        default: break;
        }
    }
}
bool checkpoint_on_alone() {
    if(!g.configured || !g.run) return true;
    // Tombstone FIRST. A crash between this write and deletion still vetoes resume.
    if(!marker(2)) { fail("play alone held: cannot persist recovery invalidation"); return false; }
    cleanup(); g.failed=true; status("play alone: this recovery run is permanently invalid"); return true;
}
void checkpoint_clear(bool broadcast) {
    if(!g.configured || !g.run || g.finished) return;
    // Called after native EndRunFinalize returns, including solo finalization.
    // This is scoped to the run+participants+slots, never the whole save directory.
    if(!marker(3)) { fail("settlement finished but recovery tombstone write failed"); return; }
    if(!cleanup()) { fail("settlement complete; recovery invalidated but file cleanup failed"); return; }
    g.finished=true; g.dirty=false; g.pending=Entry{};
    g.loadIssued=true; // never replay the chapter handshake at a later save menu
    if(broadcast && g.on) { CheckpointMsg m{}; m.kind=kCheckpointFinished; m.run=g.run; m.identity=g.config.identity; net_send_checkpoint(m); }
    status("settlement complete; this run's recovery snapshots cleared");
}
bool checkpoint_restart_run() {
    if(!g.on) return true;                     // solo: nothing to re-arm
    if(!g.finished || g.failed) return false;  // only a settled run re-arms in-session
    if(!net_active() || net_peer_count()<2) {
        status("next chapter held: reconnect all participants"); return false;
    }
    const uint8_t slot=g.slot; const std::wstring path=g.path;
    if(slot>31 || path.empty()) return false;
    // The wipe checkpoint_init performs, minus re-reading the fingerprint and
    // the save directory -- neither changes for this process -- so the re-arm
    // also works where the directory cannot be re-resolved. Slot/path survive
    // the wipe in locals because the handshake re-selects them; g.key is
    // rebuilt deterministically inside configure() before any file path is
    // used again.
    reset_selection();
    // checkpoint_select revalidates the settled database, both offers read the
    // mode-3 tombstone (settled: no candidates), and choose() mints a fresh
    // nonce run -- so the next chapter stages under a new identity the settled
    // tombstone cannot veto, and apply_select's marker(1) turns the tombstone
    // back into an active journal for the new run.
    select_impl(slot,path.c_str(),true); // no native reload or SQLite map replacement
    return !g.failed && g.selected;
}
void checkpoint_fault(const char* reason) { if(g.on) fail(reason); }
// A DISAGREEMENT THAT IS NOT A DESYNC MUST NOT LOCK THE RUN (2026-09-29). A cat-roster mismatch
// between two peers whose RNG agrees is what the cat digest and the node snapshot are there to
// repair; fail() latches for the whole session, and the host then sat on "holding host node 7
// (checkpoint barrier)" with nothing left to try. This skips only THIS node's checkpoint: no
// arrival, no staging, no commit, the barrier opens, and the previous confirmed save stays the
// recovery point. The next node arms the checkpoint again.
void checkpoint_soft_fault(uint64_t node_seed,const char* reason) {
    if(!g.on || g.failed || g.finished || !g.released) return;
    g.skip=true; g.skipSeed=node_seed;
    g.capture=g.arrived=g.preparing=g.advancing=false; g.pending=Entry{};
    status(reason);
}
const char* checkpoint_status() { return g.status; }

// --- the save-selection stage ----------------------------------------------------------------
void checkpoint_set_manual(bool on) { g_manual=on; }

void checkpoint_on_savewait(uint8_t from,const SaveWaitMsg& m) {
    if(!g.on || g.host || from!=kHostPeer || g.dismissed) return;
    if(m.phase==kSaveWaitInvalid) {
        // Only a round this peer is IN can be invalid, and never one it started over a moment ago.
        if(!g.selected || g.released || GetTickCount64()-g_abort_at<3000) return;
        g.invalid=true;
    }
    g.waitPhase=m.phase; g.waitMask=m.selected;
}

bool checkpoint_round_open() {
    return g.on && !g.inSession && g.selected && !g.failed && !g.invalid && !g.released && !g.loadIssued &&
           g.tx.kind!=kCheckpointSelect;
}

bool checkpoint_abort_round() {
    if(!g.on || g.inSession) return false;
    if(g.released || g.loadIssued || g.tx.kind==kCheckpointSelect) return false;
    const bool had=g.selected || g.failed || g.invalid;
    State old=std::move(g);
    g=State{};
    g.on=old.on; g.host=old.host; g.identity=old.identity; g.root=std::move(old.root);
    g_abort_at=GetTickCount64();
    if(had) log_line("CHECKPOINT","save selection round started over -- every player picks again");
    status("waiting for all players to select saves");
    return true;
}

bool checkpoint_sync_view(SaveSyncView& v) {
    v=SaveSyncView{};
    if(!g.on || !g_manual || g.inSession || g.dismissed) return false;
    v.host=g.host;
    if(g.invalid) { v.phase=kSyncInvalid; return true; }
    if(!g.selected || g.released || g.loadIssued) return false;
    if(g.host) {
        v.selected=selected_mask();
        if(g.choosing) {
            v.phase=kSyncHostChoosing; v.prep=g.listPrep; v.n=g.listN;
            for(unsigned i=0;i<g.listN && i<4;++i) v.entry[i]={g.list[i].run,g.list[i].seq,g.list[i].stamp};
        } else v.phase=kSyncWaiting;
        return true;
    }
    v.selected=g.waitMask|(uint8_t)(1u<<net_self());
    v.phase=g.waitPhase==kSaveWaitChoosing ? kSyncGuestWaiting : kSyncWaiting;
    return true;
}

bool checkpoint_host_pick(int index) {
    if(!g.on || !g.host || !g_manual || !g.choosing || g.failed || g.invalid) return false;
    CheckpointMsg m=g.config; m.kind=kCheckpointSelect;
    if(index<0) {
        if(!g.listPrep) return false;
        m.mode=0; m.run=checkpoint_io::nonce();
    } else {
        if((unsigned)index>=g.listN) return false;
        const CheckpointRef& c=g.list[index];
        m.mode=1; m.run=c.run; m.seq=c.seq; m.stamp=c.stamp; m.token=c.certificate;
    }
    if(!m.run) { fail("cannot allocate recovery run identity"); return false; }
    g.choosing=false; g.tx=m;
    send_wait(kSaveWaitLoading);
    if(!net_send_checkpoint(m)) { fail("recovery selection send failed"); return false; }
    log_line("CHECKPOINT","host chose %s",index<0?"the preparation stage":"a handshake save");
    apply_select(m);
    return true;
}

void checkpoint_sync_dismiss() { g.dismissed=true; }

void checkpoint_tick() {
    if(!g.on || !g.host || !g_manual || g.inSession) return;
    const uint8_t phase=g.invalid ? kSaveWaitInvalid : g.choosing ? kSaveWaitChoosing
                      : (g.released||g.loadIssued||g.tx.kind==kCheckpointSelect) ? kSaveWaitLoading : kSaveWaitCollecting;
    const uint8_t mask=selected_mask();
    if(phase==kSaveWaitCollecting && !mask) return;
    // "Invalid" is announced a few times and then left alone: it is sticky on the receiving side, and a
    // host that kept repeating it would put the panel back over the next round.
    if(phase==kSaveWaitInvalid && g.invalidSends>=5) return;
    const ULONGLONG t=GetTickCount64();
    if(phase==g.sentPhase && mask==g.sentMask && t-g.sentAt<1000) return;
    if(phase==kSaveWaitInvalid) ++g.invalidSends;
    send_wait(phase);
}
} // namespace mgmp
