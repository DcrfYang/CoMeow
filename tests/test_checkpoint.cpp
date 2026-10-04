// Exercise the real recovery state machine with independent process images,
// disk directories and SQLite databases. Only transport/game callbacks are fake.
#include "../src/session/mgmp_checkpoint.cpp"
#include <deque>
#include <functional>
#include <stdexcept>
#include <cstdarg>

using namespace mgmp;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
namespace {
unsigned current=0, participants=2, live=2;
State states[kMaxPeers];
struct Packet { unsigned from,to; CheckpointMsg msg; };
std::deque<Packet> packets;
struct MapPacket { unsigned from,to; ChapterMapMsg msg; };
std::deque<MapPacket> map_packets;
struct WaitPacket { unsigned to; SaveWaitMsg msg; };
std::deque<WaitPacket> wait_packets;
std::wstring testroot;
bool capture_ok=true, connected=true;
void at(unsigned who,const std::function<void()>& fn) {
    current=who; g=states[who]; fn(); states[who]=g;
}
void drain() {
    unsigned budget=300;
    while(!packets.empty() || !map_packets.empty() || !wait_packets.empty()) {
        CHECK(budget--);
        if(!wait_packets.empty()) {
            auto p=wait_packets.front(); wait_packets.pop_front();
            at(p.to,[&]{ checkpoint_on_savewait(0,p.msg); });
        } else if(!packets.empty()) {
            auto p=packets.front(); packets.pop_front();
            at(p.to,[&]{ checkpoint_on_message((uint8_t)p.from,p.msg); });
        } else {
            auto p=map_packets.front(); map_packets.pop_front();
            at(p.to,[&]{ checkpoint_on_chaptermap((uint8_t)p.from,p.msg); });
            free(p.msg.data);   // the round-trip decoder allocated it
        }
    }
}
void sql_exec(const std::wstring& file,const char* command) {
    HMODULE mod=LoadLibraryW(L"winsqlite3.dll"); CHECK(mod);
    auto open=(int(*)(const char*,void**,int,const char*))GetProcAddress(mod,"sqlite3_open_v2");
    auto close=(int(*)(void*))GetProcAddress(mod,"sqlite3_close");
    auto exec=(int(*)(void*,const char*,void*,void*,char**))GetProcAddress(mod,"sqlite3_exec");
    std::string path(file.begin(),file.end()); void* db=nullptr;
    CHECK(open(path.c_str(),&db,6,nullptr)==0); CHECK(exec(db,command,nullptr,nullptr,nullptr)==0); CHECK(close(db)==0);
    FreeLibrary(mod);
}
void reset(const wchar_t* name,unsigned count=2) {
    participants=live=count; packets.clear(); map_packets.clear(); capture_ok=true; connected=true;
    wchar_t cwd[MAX_PATH]{}; GetCurrentDirectoryW(MAX_PATH,cwd);
    testroot=std::wstring(cwd)+L"\\_buildcheck\\checkpoint-tests-"+std::to_wstring(checkpoint_io::nonce());
    CHECK(CreateDirectoryW(testroot.c_str(),nullptr));
    for(unsigned i=0;i<count;++i) {
        states[i]=State{}; auto& s=states[i]; s.on=true; s.host=i==0; s.identity=100+i;
        s.root=testroot+L"\\peer"+std::to_wstring(i); CHECK(CreateDirectoryW(s.root.c_str(),nullptr));
        s.path=s.root+L"\\game.sav";
        sql_exec(s.path,"CREATE TABLE properties(key TEXT PRIMARY KEY,data ANY); INSERT INTO properties VALUES('on_adventure',0);");
    }
    wprintf(L"CASE %ls\n",name);
}
void selections() {
    // Client chooses first; host must still select before any original loads.
    for(unsigned i=participants;i-->0;) at(i,[&]{ CHECK(!checkpoint_select((uint8_t)(i+1),g.path.c_str())); });
    drain();
}
void start() {
    selections();
    for(unsigned i=0;i<participants;++i) at(i,[&]{
        CHECK(g.released && !g.failed); CHECK(checkpoint_autoselect()==int(i+1)); CHECK(checkpoint_autoselect()==-1);
        std::string cmd="UPDATE properties SET data=1 WHERE key='on_adventure'; INSERT INTO properties VALUES('coins',"+std::to_string(100+i)+");";
        sql_exec(g.path,cmd.c_str());
    });
}
void boundary(uint64_t map) {
    for(unsigned i=0;i<participants;++i) at(i,[&]{checkpoint_on_map(map);});
    drain();
    for(unsigned i=0;i<participants;++i) at(i,[&]{checkpoint_on_map(map);});
    drain();
}
void next(uint64_t map,uint32_t type=0) {
    for(unsigned i=0;i<participants;++i) at(i,[&]{checkpoint_on_node(123,1,type);});
    boundary(map);
}
void restart() {
    packets.clear(); map_packets.clear(); live=participants;
    for(unsigned i=0;i<participants;++i) {
        State old=states[i]; states[i]=State{};
        states[i].on=true; states[i].host=i==0; states[i].identity=old.identity;
        states[i].root=old.root; states[i].path=old.path;
    }
    selections();
}
void certified(unsigned seq) {
    for(unsigned i=0;i<participants;++i) at(i,[&]{
        CHECK(!g.failed); CHECK(g.seq==seq); CHECK(checkpoint_can_enter());
        Entry e{}; CHECK(read_entry(file(L".0"),e,true)); CHECK(e.certificate.seq==seq);
        CHECK(e.certificate.hashes[i]==savefile_hash(e.database.data(),(uint32_t)e.database.size()));
        // Every staged entry carries the ownership snapshot (export stub: 2 notes).
        CHECK(e.owner_count==2 && e.owners[0].save_id==0x70000001ull && e.owners[1].owner_pos==1);
    });
}
}
namespace mgmp {
unsigned imported_notes = 0;   // set by the import stub, asserted in main
unsigned imported_origins = 0; // the clone -> original record the journal now carries (disk version 3)
bool net_active(){return connected;}
bool net_send_savewait(const SaveWaitMsg& m) {
    // Through the real codec, host -> every client.
    uint8_t bytes[16]; auto n=enc_savewait(bytes,sizeof(bytes),m); CHECK(n);
    Reader r(bytes,n); CHECK(r.u8v()==MSG_SAVEWAIT); SaveWaitMsg d{}; CHECK(dec_savewait(r,d));
    CHECK(current==0);
    for(unsigned i=1;i<live;++i) wait_packets.push_back({i,d});
    return true;
}
bool lockstep_halted(){return false;}
void lockstep_share_log(const char*) {}
uint32_t lockstep_owner_note_export(LockstepOwnerNote* out, uint32_t max) {
    if (max < 2) return 0;
    out[0] = {0x70000001ull, 0}; out[1] = {0x71000001ull, 1};
    return 2;
}
void lockstep_owner_note_import(const LockstepOwnerNote*, uint32_t count) { imported_notes += count; }
uint32_t catsync_origin_export(CloneOriginNote* out, uint32_t max) {
    if (max < 2) return 0;
    out[0] = {0x70000001ull, 0x2daull}; out[1] = {0x71000001ull, 0x2fcull};
    return 2;
}
void catsync_origin_import(const CloneOriginNote* in, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) if (in[i].clone && in[i].original) ++imported_origins;
}
NetRole net_role(){return current==0?NetRole::Host:NetRole::Client;}
uint8_t net_self(){return (uint8_t)current;}
uint8_t net_peer_count(){return (uint8_t)live;}
bool net_peer_ids(uint8_t* out,uint8_t cap) { if(cap<live)return false; for(unsigned i=0;i<live;++i)out[i]=(uint8_t)i;return true; }
bool net_send_checkpoint(const CheckpointMsg& m) {
    // Round-trip EVERY simulated network message through the real wire codec.
    static uint8_t bytes[8192]; auto n=enc_checkpoint(bytes,sizeof(bytes),m); CHECK(n);
    Reader r(bytes,n); CHECK(r.u8v()==MSG_CHECKPOINT); CheckpointMsg decoded{}; CHECK(dec_checkpoint(r,decoded));
    if(current==0) { for(unsigned i=1;i<live;++i)packets.push_back({current,i,decoded}); }
    else packets.push_back({current,0,decoded});
    return live>1;
}
bool net_send_chaptermap_to(uint8_t peer,const ChapterMapMsg& m) {
    // Same round-trip discipline: the frame must survive the real encoder and
    // decoder, and the decoder's malloc copy is what travels the fake wire.
    uint32_t need=chaptermap_frame_size(m); CHECK(need && need<=4096);
    uint8_t* bytes=(uint8_t*)malloc(need); CHECK(bytes);
    auto n=enc_chaptermap(bytes,need,m); CHECK(n);
    Reader r(bytes,n); CHECK(r.u8v()==MSG_CHAPTERMAP);
    ChapterMapMsg decoded{}; CHECK(dec_chaptermap(r,decoded));
    free(bytes);
    map_packets.push_back({current,peer,decoded});
    return live>1;
}
bool savefile_save_dir(wchar_t*,size_t){return false;} // tests provide isolated roots
bool savefile_read_checkpoint(uint8_t** data,uint32_t* size,uint64_t* hash) {
    if(!capture_ok)return false;
    Bytes bytes; if(!checkpoint_io::snapshot(g.path,bytes))return false;
    *size=(uint32_t)bytes.size(); *hash=savefile_hash(bytes.data(),*size); *data=(uint8_t*)malloc(*size); memcpy(*data,bytes.data(),*size); return true;
}
void log_line(const char*,const char* fmt,...) {
    va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a); putchar('\n');
}
}
// The player identity lives in mgmp-peer-id.bin beside the binary. A fresh folder (the mod updated by unpacking a new zip) used to mean a NEW identity and every recovery record of the run in
// progress orphaned (2026-10-03: "save combination invalid" after both players re-downloaded). With exactly one non-empty earlier identity folder it is taken over instead.
static void test_identity_adoption() {
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr,exe,MAX_PATH);
    std::wstring id_file=std::wstring(exe); id_file=id_file.substr(0,id_file.find_last_of(L"\\/"))+L"\\mgmp-peer-id.bin";
    const std::wstring kept=id_file+L".test-keep";
    const bool had=GetFileAttributesW(id_file.c_str())!=INVALID_FILE_ATTRIBUTES;
    if(had) CHECK(MoveFileExW(id_file.c_str(),kept.c_str(),MOVEFILE_REPLACE_EXISTING));
    std::wstring root=std::wstring(exe); root=root.substr(0,root.find_last_of(L"\\/"))+L"\\idtest-"+std::to_wstring(GetTickCount64());
    CHECK(CreateDirectoryW(root.c_str(),nullptr));
    auto mkdir_with=[&](const wchar_t* name,bool file){ std::wstring d=root+L"\\"+name; CHECK(CreateDirectoryW(d.c_str(),nullptr)); if(file) { HANDLE h=CreateFileW((d+L"\\x.state").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr); CHECK(h!=INVALID_HANDLE_VALUE); CloseHandle(h); } };
    mkdir_with(L"1234567890123",true); mkdir_with(L"777",false); mkdir_with(L"notanumber",true);
    uint64_t id=0; bool adopted=false;
    CHECK(checkpoint_io::identity(id,root.c_str(),&adopted));
    CHECK(adopted && id==1234567890123ull);                      // the empty one and the non-number are ignored
    uint64_t again=0; bool a2=true;
    CHECK(checkpoint_io::identity(again,root.c_str(),&a2) && !a2 && again==id);   // now it is kept in the file
    DeleteFileW(id_file.c_str());
    mkdir_with(L"42",true);                                      // two candidates: ambiguous -> a fresh random id
    uint64_t fresh=0; bool a3=true;
    CHECK(checkpoint_io::identity(fresh,root.c_str(),&a3) && !a3 && fresh && fresh!=id && fresh!=42);
    DeleteFileW(id_file.c_str());
    if(had) MoveFileExW(kept.c_str(),id_file.c_str(),MOVEFILE_REPLACE_EXISTING);
    printf("identity adoption: ok\n");
}

int main() {
    test_identity_adoption();
    reset(L"four participants / independent DBs / latest common restore",4); start(); boundary(1000); certified(1);
    CHECK(states[0].tx.hashes[0]!=states[0].tx.hashes[1]);
    next(1001); certified(2); next(1002); certified(3);
    for(unsigned i=0;i<participants;++i) at(i,[]{Entry e{};CHECK(read_entry(file(L".1"),e,true));CHECK(e.certificate.seq==2);});
    restart(); for(unsigned i=0;i<participants;++i)CHECK(states[i].released && states[i].seq==3);
    // The restore path imported the journal's owner notes on every peer.
    CHECK(imported_notes>=2*participants);
    CHECK(imported_origins>=2*participants);   // the journal brought the clone -> original record back too
    // Journal format: a version-1 entry (no owner section) is still readable and
    // reports an empty table; a version-2 entry round-trips its notes.
    at(0,[]{
        Entry e{}; CHECK(read_entry(file(L".0"),e,true)); CHECK(e.owner_count==2);
        CHECK(e.origin_count==2 && e.origins[0].clone==0x70000001ull && e.origins[0].original==0x2daull && e.origins[1].original==0x2fcull);   // disk version 3
        Bytes v3=encode_entry(e);      // now version 4 (flags) -- the strips below drop the flags section as well
        // a version-3 file (no flags section) is still read, with no flags
        {
            Bytes v3o(v3.begin(),v3.end()-8-4);
            v3o[4]=3; v3o[5]=0; v3o[6]=0; v3o[7]=0;
            uint64_t h3=savefile_hash(v3o.data(),(uint32_t)v3o.size());
            for(int i=0;i<8;++i) v3o.push_back((uint8_t)(h3>>(i*8)));
            CHECK(checkpoint_io::atomic_write(file(L".pending"),v3o));
            Entry r3{}; CHECK(read_entry(file(L".pending"),r3,true));
            CHECK(r3.origin_count==2 && r3.flags==0);
        }
        // a version-2 file (owner notes, no origin record) is still read, with an empty origin table
        {
            const uint32_t strip3=4+4+e.origin_count*16;
            Bytes v2(v3.begin(),v3.end()-8-strip3);
            v2[4]=2; v2[5]=0; v2[6]=0; v2[7]=0;
            uint64_t h2=savefile_hash(v2.data(),(uint32_t)v2.size());
            for(int i=0;i<8;++i) v2.push_back((uint8_t)(h2>>(i*8)));
            CHECK(checkpoint_io::atomic_write(file(L".pending"),v2));
            Entry r2{}; CHECK(read_entry(file(L".pending"),r2,true));
            CHECK(r2.owner_count==2 && r2.origin_count==0 && r2.database.size()==e.database.size());
        }
        const uint32_t strip=4+e.owner_count*9+4+e.origin_count*16+4;
        Bytes v1(v3.begin(),v3.end()-8-strip);
        v1[4]=1; v1[5]=0; v1[6]=0; v1[7]=0;                 // disk version 1
        uint64_t h=savefile_hash(v1.data(),(uint32_t)v1.size());
        for(int i=0;i<8;++i) v1.push_back((uint8_t)(h>>(i*8)));
        CHECK(checkpoint_io::atomic_write(file(L".pending"),v1));
        Entry r{}; CHECK(read_entry(file(L".pending"),r,true));
        CHECK(r.owner_count==0 && r.database.size()==e.database.size());
        CHECK(erase(L".pending"));
    });

    reset(L"host committed, client crashed before COMMIT"); start(); boundary(2000); certified(1);
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_on_node(1,1); checkpoint_on_map(2001);}); drain();
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_on_map(2001);});
    while(!packets.empty() && packets.front().msg.kind!=kCheckpointCommit) {
        auto p=packets.front();packets.pop_front();at(p.to,[&]{checkpoint_on_message((uint8_t)p.from,p.msg);});
    }
    CHECK(!packets.empty()); CHECK(states[0].committed); CHECK(!states[1].committed);
    restart(); CHECK(states[0].released && states[1].released); CHECK(states[0].seq==1 && states[1].seq==1);
    // Rollback aligns q0 with the COMMON snapshot; another partial commit must
    // not rotate an unshared newer snapshot over the only common fallback.
    boundary(2000); certified(2);

    reset(L"corrupt newest entry falls back, unrelated pairing survives settlement"); start(); boundary(3000); next(3001);
    at(1,[]{ CHECK(checkpoint_io::atomic_write(file(L".0"),Bytes{1,2,3})); });
    restart(); CHECK(states[0].released && states[1].released && states[0].seq==1);
    at(0,[]{ CHECK(checkpoint_io::atomic_write(g.root+L"\\unrelated.0",Bytes{4})); checkpoint_clear(); CHECK(!disk_exists(file(L".0"))); CHECK(disk_exists(g.root+L"\\unrelated.0")); });
    drain(); at(1,[]{CHECK(disk_exists(file(L".0")));}); // remote finish cannot erase local pending settlement
    restart(); CHECK(states[0].failed && states[1].failed);

    reset(L"play alone invalidation survives process restart"); start(); boundary(4000);
    at(1,[]{CHECK(checkpoint_on_alone());}); restart(); CHECK(states[0].failed && states[1].failed);

    reset(L"missing confirmed files refuse an existing run"); start(); boundary(5000);
    at(1,[]{CHECK(erase(L".0"));}); restart(); CHECK(states[0].failed && states[1].failed);

    reset(L"frozen membership cannot shrink quorum",3); start(); boundary(6000);
    for(unsigned i=0;i<3;++i)at(i,[]{checkpoint_on_node(1,1);checkpoint_on_map(6001);});drain();
    live=2; at(0,[]{checkpoint_on_map(6001);CHECK(!checkpoint_can_enter());Entry e{};CHECK(read_entry(file(L".0"),e,true));CHECK(e.certificate.seq==1);});

    reset(L"a roster-only mismatch skips ONE node's checkpoint and never latches"); start(); boundary(7500); certified(1);
    // The client's NODEHASH reports before its checkpoint_on_node, the host's after it.
    at(1,[]{checkpoint_soft_fault(123,"roster differs");});
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_on_node(123,1);});
    at(0,[]{checkpoint_soft_fault(123,"roster differs");});
    for(unsigned i=0;i<2;++i)at(i,[]{CHECK(!g.failed);CHECK(checkpoint_can_enter());CHECK(!checkpoint_needs_map());});
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_on_map(7501);}); drain();
    for(unsigned i=0;i<2;++i)at(i,[]{CHECK(g.seq==1);});   // nothing staged or committed for the skipped node
    // The next node arms the checkpoint again and certifies normally.
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_on_node(124,1);CHECK(!g.skip);});
    boundary(7502); certified(2);

    // Different map fingerprints no longer stop the checkpoint (2026-10-02): the host's map names the
    // transaction and every peer stages its own database under it.
    reset(L"different map states are still confirmed"); start();
    for(int round=0;round<2;++round){
        for(unsigned i=0;i<2;++i)at(i,[i]{checkpoint_on_map(7000+i);});
        drain();
    }
    certified(1);
    // ...and a peer whose own map changes between arrival and capture (the map sync adopting the host's) is not refused either: the transaction is named by the host's map.
    reset(L"a map that changes during preparation is not an error"); start();
    for(unsigned i=0;i<2;++i)at(i,[i]{checkpoint_on_map(7100+i);});
    drain();
    at(1,[]{checkpoint_on_map(7199);});at(0,[]{checkpoint_on_map(7100);});drain(); CHECK(!states[1].failed && !states[0].failed);
    certified(1);

    reset(L"SQLite snapshot failure preserves confirmed queue"); start();boundary(8000); capture_ok=false;next(8001);
    at(0,[]{Entry e{};CHECK(read_entry(file(L".0"),e,true));CHECK(e.certificate.seq==1);CHECK(!checkpoint_can_enter());});

    reset(L"all settle clears exactly the current run and permits a new warehouse run");start();boundary(9000);
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_clear();sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'");});
    restart(); CHECK(states[0].released && states[1].released); CHECK(states[0].seq==0);

    reset(L"in-session next chapter re-arms a fresh journal after settlement");start();boundary(9500);
    const uint64_t settled_run=states[0].run;
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_clear();sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'");});
    drain();
    for(unsigned i=0;i<2;++i)at(i,[]{
        CHECK(g.finished); CHECK(!checkpoint_can_enter()); // finished blocks node entry
        CHECK(checkpoint_restart_run());                   // the chapter commit re-arms
    });
    drain();
    for(unsigned i=0;i<2;++i)at(i,[&]{
        CHECK(g.released && !g.failed && !g.finished && g.selected);
        CHECK(g.run && g.run!=settled_run);                // a FRESH nonce run identity
        Entry e{}; CHECK(read_entry(file(L".state"),e,false));
        CHECK(e.certificate.run==g.run && e.certificate.mode==1); // active journal again
    });
    boundary(9501); certified(1);                          // the new run checkpoints at seq 1
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_clear();});
    drain();
    for(unsigned i=0;i<2;++i)at(i,[]{CHECK(g.finished);}); // and settles cleanly a second time

    reset(L"departure preparation (trollengine counter zero) starts a fresh session");start();boundary(9600);
    for(unsigned i=0;i<2;++i)at(i,[]{
        checkpoint_clear();
        // The 09:50 shape, measured on both chapter-page saves: on_adventure is
        // already set, but the party never departed -- the adventure's event
        // engine has not run, so trollengine_state's counter is still zero.
        sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure';"
                        "CREATE TABLE files(key TEXT PRIMARY KEY,data BLOB);"
                        "INSERT INTO files VALUES('trollengine_state',x'03000000000000000000000000000000');");
    });
    restart(); CHECK(states[0].released && states[1].released && !states[0].failed && !states[1].failed);
    CHECK(states[0].seq==0 && states[0].mode==0 && states[0].run);

    reset(L"fresh departure-ready saves sync the host's chapter_map row before load");start();boundary(9750);
    // Two peers that prepared separately carry two different generated maps
    // (the 09:50 saves). Host writes the start node in, client wrote a battle
    // node -- as serialized rows they cannot both be the run's map.
    at(0,[&]{ sql_exec(g.path,"CREATE TABLE files(key TEXT PRIMARY KEY,data BLOB);"
                              "INSERT INTO files VALUES('chapter_map',x'05000000000000007374617274');"); });
    at(1,[&]{ sql_exec(g.path,"CREATE TABLE files(key TEXT PRIMARY KEY,data BLOB);"
                              "INSERT INTO files VALUES('chapter_map',x'0600000000000000626174746c65');"); });
    for(unsigned i=0;i<2;++i) at(i,[]{
        checkpoint_clear();
        sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure';"
                        "INSERT INTO files VALUES('trollengine_state',x'03000000000000000000000000000000');");
    });
    restart(); CHECK(states[0].released && states[1].released && !states[0].failed && !states[1].failed);
    // The client's row must now BE the host's row: written before load, and
    // read back byte for byte through the same SQLite path the mod uses.
    at(1,[&]{
        int have=0; Bytes row; CHECK(checkpoint_io::read_file_row(g.path,"chapter_map",have,row));
        CHECK(have && row.size()==13 && row[8]=='s' && row[12]=='t');
    });
    at(0,[&]{
        int have=0; Bytes row; CHECK(checkpoint_io::read_file_row(g.path,"chapter_map",have,row));
        CHECK(have && row.size()==13 && row[8]=='s' && row[12]=='t');   // host row untouched
    });
    // And with one map on both peers, the first map boundary must certify.
    next(9800); certified(1);

    reset(L"on-map save (trollengine counter nonzero) still refuses a fresh session");start();boundary(9700);
    for(unsigned i=0;i<2;++i)at(i,[]{
        checkpoint_clear();
        // on_adventure set AND the event engine has run: this peer stands on
        // the map. Recovery was the only legal path and the settled journal
        // has none, so the 10:04 refusal must survive for this shape.
        sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure';"
                        "CREATE TABLE files(key TEXT PRIMARY KEY,data BLOB);"
                        "INSERT INTO files VALUES('trollengine_state',x'03000000000000000000000001000000');");
    });
    restart(); CHECK(states[0].failed && states[1].failed);

    // ---- 2026-10-03: the same shape through the save-selection panel. The host's save stood on the map after a run the other player never entered, its journal only a settled tombstone: the host was
    // offered the preparation stage, apply_select refused it, and nobody was told. Now both peers get the "invalid" panel, and it says the saves are on the map with NO RECORD (kHoldersNoRecord). ----
    {
        reset(L"handshake: a save on the map whose journal is only a settled tombstone is invalid, and the panel says there is no record");start();boundary(9705);
        at(0,[]{
            checkpoint_clear();
            sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure';"
                            "CREATE TABLE files(key TEXT PRIMARY KEY,data BLOB);"
                            "INSERT INTO files VALUES('trollengine_state',x'03000000000000000000000001000000');");
        });
        at(1,[]{ checkpoint_clear(); sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'"); });   // the other player is back in the warehouse
        drain();
        struct Guard { Guard(){g_manual=true;} ~Guard(){g_manual=false;} } manual;      // the selection panel from here on
        restart();
        at(0,[]{ checkpoint_tick(); }); drain();
        for(unsigned i=0;i<2;++i) at(i,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncInvalid); CHECK(v.selected & 1); CHECK(v.selected & kHoldersNoRecord); CHECK(!(v.selected & 2)); });
        CHECK(!states[0].released && !states[1].released);
        at(0,[]{ CHECK(!checkpoint_host_pick(-1)); });
    }

    reset(L"on-map save before its first node (counter zero, adventure_started 1) refuses a fresh session");start();boundary(9710);
    for(unsigned i=0;i<2;++i)at(i,[]{
        checkpoint_clear();
        // The 2026-10-01 shape (host steamcampaign02.sav): the party has departed and stands on the map,
        // but no node has resolved yet, so the event count is still zero. adventure_started says it.
        sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure';"
                        "INSERT INTO properties VALUES('adventure_started',1);"
                        "CREATE TABLE files(key TEXT PRIMARY KEY,data BLOB);"
                        "INSERT INTO files VALUES('trollengine_state',x'03000000000000000000000000000000');");
    });
    for(unsigned i=0;i<2;++i)at(i,[]{ bool on=false; CHECK(checkpoint_io::on_map(g.path,on)); CHECK(on); });
    restart(); CHECK(states[0].failed && states[1].failed);

    reset(L"run killed before its first boundary -- orphan state discarded, saves swapped");start();
    // The 16:55 shape: a fresh run refused at "map state differs between
    // participants" and was killed before any boundary confirmed. The marker
    // stays behind owing nothing -- no .0/.1/.pending -- and refused every
    // later session with "no common confirmed save", no matter which .sav was
    // restored over the slots (17:35/17:38), because the journal is a
    // sidecar. Both peers discard the orphan openly and start fresh.
    const uint64_t dead_run=states[0].run;
    for(unsigned i=0;i<2;++i)at(i,[]{
        CHECK(disk_exists(file(L".state")));
        CHECK(!disk_exists(file(L".0")) && !disk_exists(file(L".1")) && !disk_exists(file(L".pending")));
    });
    for(unsigned i=0;i<2;++i)at(i,[&]{sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'");});
    restart(); CHECK(states[0].released && states[1].released && !states[0].failed && !states[1].failed);
    CHECK(states[0].seq==0 && states[0].run && states[0].run!=dead_run);
    at(0,[]{ Entry e{}; CHECK(read_entry(file(L".state"),e,false)); CHECK(e.certificate.run==g.run); });
    next(9650); certified(1);

    reset(L"pairing state written by another build or mod ruleset is set aside, not a wedge");start();
    // 2026-10-01: the first run after the ruleset joined the build identity found every earlier slot pair's .state
    // "corrupt" (it was only foreign) and sat on the waiting screen forever. Nothing this build could recover is in
    // it, so it is moved aside and the pair starts fresh. The mid-run case (confirmed snapshots owed) is above/below.
    for(unsigned i=0;i<2;++i)at(i,[]{
        CHECK(disk_exists(file(L".state")));
        CHECK(!disk_exists(file(L".0")) && !disk_exists(file(L".1")) && !disk_exists(file(L".pending")));
        sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'");
    });
    compatibility_build=77; restart();
    CHECK(states[0].released && states[1].released && !states[0].failed && !states[1].failed);
    for(unsigned i=0;i<2;++i)at(i,[]{
        Entry e{}; CHECK(read_entry(file(L".state"),e,false));   // the new run's own marker, readable under the new identity
    });
    compatibility_build=0;

    reset(L"crash after native settlement before journal cleanup");start();boundary(10000);
    at(0,[]{sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'");});
    restart();CHECK(states[0].failed && states[1].failed);

    reset(L"newest confirmation corrupted on both peers and build mismatch");start();boundary(11000);next(11001);
    for(unsigned i=0;i<2;++i)at(i,[]{Bytes bytes;CHECK(checkpoint_io::read(file(L".0"),bytes));bytes.back()^=1;CHECK(checkpoint_io::atomic_write(file(L".0"),bytes));});
    restart();CHECK(states[0].released && states[0].seq==1 && states[1].seq==1);
    compatibility_build=77;restart();CHECK(states[0].failed && states[1].failed);compatibility_build=0;

    reset(L"no premature rotation while another participant has not ACKed",3);start();boundary(12000);
    for(unsigned i=0;i<3;++i)at(i,[]{checkpoint_on_node(1,1);checkpoint_on_map(12001);});drain();
    for(unsigned i=0;i<2;++i)at(i,[]{checkpoint_on_map(12001);});drain();
    at(0,[]{Entry e{};CHECK(read_entry(file(L".0"),e,true));CHECK(e.certificate.seq==1);CHECK(!checkpoint_can_enter());});
    // Repeated ACK from one peer is not the missing peer's acknowledgement.
    at(0,[]{CheckpointMsg m=g.tx;m.kind=kCheckpointAck;m.hashes[1]=g.tx.hashes[1];checkpoint_on_message(1,m);CHECK(!g.committed);});
    at(2,[]{checkpoint_on_map(12001);});drain();certified(2);

    reset(L"filesystem failure during promotion keeps the confirmed previous snapshot");start();boundary(13000);
    at(0,[]{CHECK(CreateDirectoryW(file(L".1").c_str(),nullptr));});next(13001);
    at(0,[]{Entry e{};CHECK(read_entry(file(L".0"),e,true));CHECK(e.certificate.seq==1);CHECK(g.failed);});
    restart();CHECK(states[0].released && states[1].released && states[0].seq==1);

    reset(L"wrong fingerprint / slot never finds another pairing's saved run");start();boundary(14000);
    states[1].identity=999;restart();CHECK(states[0].failed && states[1].failed);

    reset(L"SQLite WAL snapshot includes committed changes and restore refuses sidecars");
    at(0,[]{
        HMODULE mod=LoadLibraryW(L"winsqlite3.dll");CHECK(mod);
        auto open=(int(*)(const char*,void**,int,const char*))GetProcAddress(mod,"sqlite3_open_v2");
        auto exec=(int(*)(void*,const char*,void*,void*,char**))GetProcAddress(mod,"sqlite3_exec");
        auto close=(int(*)(void*))GetProcAddress(mod,"sqlite3_close");
        std::string path(g.path.begin(),g.path.end());void* db=nullptr;
        CHECK(open(path.c_str(),&db,6,nullptr)==0);
        CHECK(exec(db,"PRAGMA journal_mode=WAL;PRAGMA wal_autocheckpoint=0;UPDATE properties SET data=1 WHERE key='on_adventure';",nullptr,nullptr,nullptr)==0);
        CHECK(disk_exists(g.path+L"-wal"));
        Bytes snapshot;CHECK(checkpoint_io::snapshot(g.path,snapshot));
        auto standalone=g.root+L"\\snapshot.sav";CHECK(checkpoint_io::atomic_write(standalone,snapshot));
        bool running=false;CHECK(checkpoint_io::in_run(standalone,running) && running);
        CHECK(!checkpoint_io::restore(g.path,snapshot)); // never write under an open WAL DB
        CHECK(close(db)==0);FreeLibrary(mod);
        CHECK(checkpoint_io::restore(g.path,snapshot));
        CHECK(disk_exists(g.path+L".before-mgmp-recovery"));
        CHECK(!checkpoint_io::restore(g.path,Bytes{1,2,3}));
    });


    for(unsigned first=0;first<2;++first) {
        reset(L"two settlements, same process, save re-selection in either order");
        start(); boundary(15000);
        for(unsigned run=0;run<2;++run) {
            for(unsigned i=0;i<2;++i) at(i,[]{
                checkpoint_clear(); sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'");
                CHECK(checkpoint_autoselect()==-1);
            });
            drain();
            if(run==0) {
                at(first,[]{CHECK(checkpoint_restart_run());}); drain();
                at(1-first,[]{CHECK(checkpoint_restart_run());}); drain();
                for(unsigned i=0;i<2;++i) at(i,[]{CHECK(!g.failed && g.released);CHECK(checkpoint_autoselect()==-1);});
                boundary(15001); certified(1);
            }
        }
        CheckpointMsg old_config=states[0].config, old_reject=old_config;
        old_reject.kind=kCheckpointReject;
        auto old_epoch=old_config.identity;
        auto old_run=states[0].run;
        at(first,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));}); drain();
        CHECK(!states[first].released);
        at(1-first,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));}); drain();
        CHECK(states[0].config.identity!=old_epoch && states[0].run!=old_run);
        at(1,[&]{checkpoint_on_message(0,old_config);checkpoint_on_message(0,old_reject);CHECK(!g.failed);});
        at(0,[&]{checkpoint_on_message(1,old_reject);CHECK(!g.failed);});
        for(unsigned i=0;i<2;++i) at(i,[&]{CHECK(g.released);CHECK(checkpoint_autoselect()==int(i+1));CHECK(checkpoint_autoselect()==-1);});
    }

    for(unsigned first=0;first<2;++first) {
        reset(L"unfinished run reselects same-process last common checkpoint"); start(); boundary(16000);
        const auto old_run=states[0].run;
        CheckpointMsg stale=states[0].config; stale.kind=kCheckpointRelease;stale.run=old_run;stale.seq=1;
        at(first,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));});drain();
        at(1-first,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));});drain();
        for(unsigned i=0;i<2;++i) at(i,[&]{CHECK(g.released && !g.failed && g.run==old_run && g.seq==1);});
        at(1,[&]{g.released=false;checkpoint_on_message(0,stale);CHECK(!g.released);g.released=true;});
        live=1; at(0,[]{CHECK(checkpoint_autoselect()==-1 && !g.failed);});
        live=2; at(0,[]{CHECK(checkpoint_autoselect()==1);});
    }


    reset(L"disconnect before native auto-load permits explicit re-selection"); selections();
    const auto abandoned_epoch=states[0].config.identity;
    live=1;at(0,[]{CHECK(checkpoint_autoselect()==-1 && !g.failed);});live=2;
    at(1,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));});drain();
    at(0,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));});drain();
    CHECK(states[0].released && states[1].released && states[0].config.identity!=abandoned_epoch);

    reset(L"old chapter map cannot write the new save before its own transfer");
    at(1,[]{CHECK(!checkpoint_select(2,g.path.c_str()));});
    at(0,[]{CHECK(!checkpoint_select(1,g.path.c_str()));});
    while(!packets.empty()) {auto p=packets.front();packets.pop_front();at(p.to,[&]{checkpoint_on_message((uint8_t)p.from,p.msg);});}
    CHECK(states[1].awaitingMap && !map_packets.empty());
    at(1,[]{ChapterMapMsg old{};old.selection=g.config.identity+1;checkpoint_on_chaptermap(0,old);CHECK(g.awaitingMap && !g.failed);});
    drain(); CHECK(states[0].released && states[1].released);

    reset(L"without peers a settled warehouse AND a run in progress both open alone (single player)");
    connected=false; live=1;
    at(0,[]{CHECK(checkpoint_select(1,g.path.c_str()));CHECK(!checkpoint_active());});
    at(1,[]{sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure'");CHECK(checkpoint_select(2,g.path.c_str()));CHECK(!checkpoint_active() && !g.failed && !g.selected);});
    connected=true; live=2;

    reset(L"invalid local SQLite is retryable by explicit save selection");
    at(0,[]{auto bad=g.root+L"\\bad.sav";CHECK(checkpoint_io::atomic_write(bad,Bytes{1,2}));CHECK(!checkpoint_select(1,bad.c_str()));CHECK(g.failed);});
    at(1,[]{CHECK(!checkpoint_select(2,g.path.c_str()));});drain();
    at(0,[]{CHECK(!checkpoint_select(1,g.path.c_str()));});drain();
    CHECK(states[0].released && states[1].released && !states[1].failed);

    reset(L"failed boundary may be retried without erasing confirmed saves");start();boundary(17000);
    capture_ok=false;next(17001);capture_ok=true;
    at(1,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));});drain();
    at(0,[]{CHECK(!checkpoint_select(g.slot,g.path.c_str()));});drain();
    CHECK(states[0].released && states[1].released && states[0].seq==1);

    // ---- the queue keeps the whole run (2026-10-04; it was four deep) ----
    reset(L"the confirmed-save queue keeps every save of the run, newest first");start();boundary(20000);
    for(unsigned k=1;k<=11;++k) next(20000+k, k==7 ? 8u : 0u);       // seq 8 is confirmed right after a boss node (type 8)
    for(unsigned i=0;i<2;++i) at(i,[]{
        CHECK(queue_indices().size()==12);
        for(unsigned s=0;s<12;++s) {
            Entry e{}; CHECK(read_entry(qfile(s),e,true)); CHECK(e.certificate.seq==12-s);
            CHECK(((e.flags&kCheckpointAfterBoss)!=0)==(e.certificate.seq==8));
        }
    });
    {   // every one of them is offered, with its flag, through the real codec
        struct Guard { Guard(){g_manual=true;} ~Guard(){g_manual=false;} } manual;
        restart();
        at(0,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncHostChoosing);
                 CHECK(v.n==12); CHECK(v.entry[0].seq==12 && v.entry[11].seq==1);
                 for(unsigned s=0;s<12;++s) CHECK(((v.entry[s].flags&kCheckpointAfterBoss)!=0)==(v.entry[s].seq==8)); });
        at(0,[]{ CHECK(checkpoint_host_pick(4)); }); drain();          // seq 8, the after-boss one
        for(unsigned i=0;i<2;++i) at(i,[]{
            Entry e{}; CHECK(read_entry(qfile(0),e,true)); CHECK(e.certificate.seq==8 && (e.flags&kCheckpointAfterBoss));
            CHECK(queue_indices().size()==12);                          // 8 at the head; 12..9 and 7..1 are KEPT (2026-10-04)
        });
    }
    {   // a gap in the numbering (a file lost) is closed by the next commit, nothing is overwritten
        reset(L"a gap in the queue is closed by the next commit");start();boundary(20500);
        for(unsigned k=1;k<=4;++k) next(20500+k);                         // .0..4 = seq 5..1
        for(unsigned i=0;i<2;++i) at(i,[]{ CHECK(erase_index(0) && erase_index(1)); });   // seq 5 and 4 gone: .2 .3 .4 = 3 2 1
        next(20510);                                                      // seq 6
        for(unsigned i=0;i<2;++i) at(i,[]{
            CHECK(queue_indices().size()==4);
            const uint64_t want[4]={6,3,2,1};
            for(unsigned s=0;s<4;++s) { Entry e{}; CHECK(read_entry(qfile(s),e,true)); CHECK(e.certificate.seq==want[s]); }
        });
    }

    // ---- restoring an older entry keeps the ones newer than it ----
    {
        reset(L"handshake: host chooses an older save; newer entries stay in the queue");start();boundary(21000);
        for(unsigned k=1;k<=3;++k) next(21000+k);            // seq 1..4 confirmed, by the automatic path
        struct Guard { Guard(){g_manual=true;} ~Guard(){g_manual=false;} } manual;
        restart();
        // Nothing loaded yet: the host is waiting on its list.
        CHECK(!states[0].released && !states[1].released && states[0].choosing);
        at(0,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncHostChoosing && v.host);
                 CHECK(v.n==4 && !v.prep && v.selected==3);
                 CHECK(v.entry[0].seq==4 && v.entry[3].seq==1); });
        at(0,[]{ checkpoint_tick(); }); drain();
        at(1,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncGuestWaiting && !v.host); });
        at(1,[]{ CHECK(!checkpoint_host_pick(0)); });                    // only the host may
        at(0,[]{ CHECK(!checkpoint_host_pick(-1)); CHECK(!checkpoint_host_pick(9)); });   // no preparation stage mid-run
        at(0,[]{ CHECK(checkpoint_host_pick(2)); }); drain();            // seq 2
        CHECK(states[0].released && states[1].released && states[0].seq==2 && states[1].seq==2);
        for(unsigned i=0;i<2;++i) at(i,[]{
            Entry e{}; CHECK(read_entry(file(L".0"),e,true)); CHECK(e.certificate.seq==2);
            CHECK(read_entry(file(L".1"),e,true));      // the others follow in creation order, newest first
            CHECK(disk_exists(file(L".2")) && disk_exists(file(L".3")) && queue_indices().size()==4);   // the two newer ones are kept
        });
        // the next commit after the restore has the number 3, like the kept one: both exist, and the new one is listed first
        next(21100);
        for(unsigned i=0;i<2;++i) at(i,[]{
            CHECK(queue_indices().size()==5);
            Entry e{}; CHECK(read_entry(qfile(0),e,true)); CHECK(e.certificate.seq==3);
            unsigned threes=0;
            for(unsigned k:queue_indices()) { Entry x{}; CHECK(read_entry(qfile(k),x,true)); if(x.certificate.seq==3) ++threes; }
            CHECK(threes==2);
        });
    }

    // ---- preparation stage, and who sees what while people are still picking ----
    {
        struct Guard { Guard(){g_manual=true;} ~Guard(){g_manual=false;} } manual;
        reset(L"handshake: a fresh pair chooses the preparation stage");
        at(1,[]{ CHECK(!checkpoint_select(2,g.path.c_str())); });drain();
        at(1,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncWaiting && v.selected==2); });
        at(0,[]{ SaveSyncView v; CHECK(!checkpoint_sync_view(v)); });    // the host has not picked: nothing to show
        at(0,[]{ CHECK(!checkpoint_select(1,g.path.c_str())); });drain();
        at(0,[]{ checkpoint_tick(); }); drain();
        at(0,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncHostChoosing && v.prep && v.n==0); });
        at(1,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncGuestWaiting && v.selected==3); });
        CHECK(!states[0].released);
        at(0,[]{ CHECK(checkpoint_host_pick(-1)); }); drain();
        CHECK(states[0].released && states[1].released && states[0].seq==0 && !states[0].failed);
    }

    // ---- nothing common and somebody mid-run: the combination is refused, for everyone ----
    {
        struct Guard { Guard(){g_manual=true;} ~Guard(){g_manual=false;} } manual;
        reset(L"handshake: a save on the map with no handshake save is invalid");
        at(1,[]{ sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure'"); });
        selections();
        at(0,[]{ checkpoint_tick(); }); drain();
        for(unsigned i=0;i<2;++i) at(i,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncInvalid); });
        CHECK(!states[0].released && !states[1].released);
        at(0,[]{ CHECK(!checkpoint_host_pick(-1)); });
        at(1,[]{ checkpoint_sync_dismiss(); SaveSyncView v; CHECK(!checkpoint_sync_view(v)); });
        // Back in the warehouse, the same players can select again.
        at(1,[]{ sql_exec(g.path,"UPDATE properties SET data=0 WHERE key='on_adventure'"); });
        at(1,[]{ CHECK(!checkpoint_select(2,g.path.c_str())); });drain();
        at(0,[]{ CHECK(!checkpoint_select(1,g.path.c_str())); });drain();
        at(0,[]{ CHECK(g.choosing && g.listPrep); });
    }

    // ---- the lock-up of 2026-09-30: a late "invalid" must not stick to a peer that already started over ----
    {
        struct Guard { Guard(){g_manual=true;} ~Guard(){g_manual=false;} } manual;
        reset(L"handshake: starting over after invalid, with the host's repeats still arriving");
        at(1,[]{ sql_exec(g.path,"UPDATE properties SET data=1 WHERE key='on_adventure'"); });
        selections();
        const std::wstring p0=states[0].path, p1=states[1].path;   // a started-over state forgets its path
        at(0,[]{ checkpoint_tick(); }); drain();
        at(1,[]{ SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncInvalid); });
        // Everybody starts over (the panel's OK, or "re-pick"): both states are clean and nobody holds a pick.
        for(unsigned i=0;i<2;++i) at(i,[]{ CHECK(checkpoint_abort_round()); CHECK(!g.selected && !g.failed && !g.invalid && !checkpoint_round_open()); });
        // The client picks again (a warehouse save now) BEFORE a repeat of the old "invalid" lands.
        sql_exec(p1,"UPDATE properties SET data=0 WHERE key='on_adventure'");
        at(1,[&]{ CHECK(!checkpoint_select(2,p1.c_str())); });
        at(1,[]{ SaveWaitMsg w; w.phase=kSaveWaitInvalid; w.selected=3; checkpoint_on_savewait(0,w);
                 SaveSyncView v; CHECK(checkpoint_sync_view(v)); CHECK(v.phase==kSyncWaiting); CHECK(!g.invalid); });
        drain();
        at(0,[&]{ CHECK(!checkpoint_select(1,p0.c_str())); }); drain();
        at(0,[]{ CHECK(g.choosing && g.listPrep); });
        // ...and "re-pick" while the host is choosing drops every pick, the host's included.
        at(0,[]{ CHECK(checkpoint_abort_round()); CHECK(!g.choosing && !g.haveSelection[1]); });
        at(1,[]{ CHECK(checkpoint_abort_round()); });
        at(1,[&]{ CHECK(!checkpoint_select(2,p1.c_str())); }); drain();
        at(0,[&]{ CHECK(!checkpoint_select(1,p0.c_str())); }); drain();
        at(0,[]{ CHECK(g.choosing); CHECK(checkpoint_host_pick(-1)); }); drain();
        CHECK(states[0].released && states[1].released);
        // Once loading, a round can no longer be started over.
        at(0,[]{ CHECK(!checkpoint_abort_round()); });
    }

    // A player is known by TWO ids (proto 73): reinstalling the mod changes the fingerprint beside the DLL, and the records of the run in progress must still be found through the other id.
    {
        auto layout=[&](bool reinstall,bool keep_alt) {
            for(unsigned i=0;i<participants;++i) {
                State old=states[i]; states[i]=State{}; auto& s=states[i];
                s.on=true; s.host=i==0; s.path=old.path;
                s.hroot=testroot;
                if(!reinstall) { s.identity=old.identity; s.identity2=old.identity2; }
                else { s.identity=500+i; s.identity2=keep_alt?old.identity:0; }
                s.root=testroot+L"\\"+std::to_wstring(s.identity); CreateDirectoryW(s.root.c_str(),nullptr);
            }
        };
        reset(L"reinstall keeps the run: the old id is the other id",2);
        layout(false,false);                       // the first install: primary ids 100/101, no alternates
        start(); boundary(1000); certified(1); next(1001); certified(2);
        const std::wstring first_key=states[0].key;
        // The reinstall: new primary ids (500/501), the old ones kept as the alternates
        layout(true,true); selections();
        for(unsigned i=0;i<participants;++i) CHECK(states[i].released && !states[i].failed && states[i].seq==2);
        CHECK(states[0].key==first_key);           // the same record, found under the other combination
        CHECK(states[0].root==testroot+L"\\100");  // ...in the folder of the old id
        // And with nothing to connect the new ids to the old ones the run is NOT found (saves on the map, no record)
        reset(L"a reinstall with no alternate id cannot find the run",2);
        layout(false,false); start(); boundary(1000); certified(1);
        layout(true,false); selections();
        for(unsigned i=0;i<participants;++i) CHECK(!states[i].released);
        // The Steam id out of the save directory path
        CHECK(checkpoint_io::steam_id_from_dir(L"C:\\Users\\a\\AppData\\Roaming\\Glaiel Games\\Mewgenics\\76561198862908557\\saves")==76561198862908557ull);
        CHECK(checkpoint_io::steam_id_from_dir(L"C:\\Users\\a\\saves")==0);
        CHECK(checkpoint_io::steam_id_from_dir(L"C:\\x\\12345678901234567\\saves")==0);      // 17 digits but not a SteamID64
        CHECK(checkpoint_io::steam_id_from_dir(nullptr)==0);
        printf("two ids: ok\n");
    }

    // Wire decoder rejects all truncations and trailing data, no heap ownership.
    CheckpointMsg m{}; uint8_t wire[512]{}; auto n=enc_checkpoint(wire,sizeof(wire),m);
    for(unsigned i=0;i<n;++i){ Reader r(wire,i);r.u8v();CheckpointMsg c{};CHECK(!dec_checkpoint(r,c)); }
    Reader extra(wire,n+1);extra.u8v();CHECK(!dec_checkpoint(extra,m));
    printf("checkpoint: %u checks passed\n",checks);
}
