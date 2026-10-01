// Exercise the production opener and target redirect, including allocator reuse.
#include "../src/session/mgmp_choice.cpp"
#include <cstdio>

namespace {
int checks=0, failures=0, writes=0;
bool write_ok=true;
unsigned char director[0x1800]{}, pending[0xD00]{}, own_a[0xD00]{}, own_b[0xD00]{};
const void* director_ptr=director;
uint64_t party_ids[]={0x24D}, familiar_ids[]={0x28A,0x2AA};
uint64_t rendered=0;
template<class T> void put(void* p,size_t at,T v) { memcpy((char*)p+at,&v,sizeof(v)); }
template<class T> T get(const void* p,size_t at) { T v; memcpy(&v,(const char*)p+at,sizeof(v)); return v; }
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
void* __fastcall by_id(void*, uint64_t id) { return id==0x28A ? own_a : id==0x2AA ? own_b : pending; }
void* __fastcall render(void* screen,void* cat,void*) {
    // Verified native constructor effects at 37A350 and 37A357.
    put(screen,0xA0,cat);
    put(cat,0xC30,get<uint32_t>(cat,0xC30)+1);
    rendered=get<uint64_t>(cat,0);
    return screen;
}
}
namespace mgmp {
void log_line(const char*,const char*,...) {}
void log_line_lvl(LogLevel,const char*,const char*,...) {}
uint8_t net_peer_pos() { return 1; }
bool lockstep_owner_pos(uint64_t id,uint8_t& pos) { pos=id==0x24D?0:1; return id==0x24D||id==0x28A||id==0x2AA; }
uintptr_t addr_of_data(DataSym d) { return d==D_MewDirectorPtr?(uintptr_t)&director_ptr:0; }
uintptr_t addr_of_call(Call c) { return c==C_CatDataById?(uintptr_t)&by_id:0; }
uint32_t serialize_cat(void* cat,uint8_t** out) { *out=(uint8_t*)malloc(8); memcpy(*out,cat,8); return 8; }
bool catsync_deserialize_into(void* cat,const uint8_t* bytes,uint32_t len) { ++writes; if(!write_ok)return false; memcpy(cat,bytes,len); return true; }
void follow_reset_run() {}
uint64_t follow_here_seed() { return g.here_seed; }
NetRole net_role() { return NetRole::Client; }
uint8_t net_self() { return 1; }
bool net_send_choice(const ChoiceMsg&) { return true; }
uintptr_t addr_of(Target) { return 0; }
bool lockstep_cat_owner(uint64_t id,uint64_t,uint8_t& pos) { return lockstep_owner_pos(id,pos); }
void lockstep_reset_cat_owners() {}
bool lockstep_cat_is_mine(uint64_t id,bool& mine) { uint8_t pos=0; bool seen=lockstep_owner_pos(id,pos); mine=pos==1; return seen; }
bool catsync_publish(const char*,bool,bool) { return true; }
void listprobe_watch_screen(const void*) {}
}
extern "C" MH_STATUS WINAPI MH_CreateHook(LPVOID,LPVOID,LPVOID*) { return MH_ERROR_NOT_INITIALIZED; }
extern "C" MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }
int main() {
    using namespace mgmp;
    put(pending,kCatData_SaveId,uint64_t(0x24D)); put(pending,0,uint64_t(0xDDDD));
    put(pending,0xC30,uint32_t(5));
    put(own_a,kCatData_SaveId,uint64_t(0x28A)); put(own_a,0,uint64_t(0xAAAA));
    put(own_b,kCatData_SaveId,uint64_t(0x2AA)); put(own_b,0,uint64_t(0xBBBB));
    put(director,kDir_CatRegistry,(void*)director);
    put(director,kDir_CatIdCount,uint32_t(1)); put(director,kDir_CatIdData,party_ids);
    put(director,kDir_CatFamiliars+4,uint32_t(2)); put(director,kDir_CatFamiliars+8,familiar_ids);
    o_lvl_opener=render;
    unsigned char screen[0xD00]{};
    g.on=true; reset_level_caches(true); choice_on_node_entered(101);
    CHECK(h_lvl_opener(screen,pending,nullptr)==screen);
    CHECK(get<void*>(screen,0xA0)==own_a && rendered==0xAAAA);
    CHECK(get<uint32_t>(own_a,0xC30)==1);
    CHECK(get<uint32_t>(pending,0xC30)==5 && get<uint64_t>(pending,0)==0xDDDD);
    CHECK(get<uint64_t>(pending,kCatData_SaveId)==0x24D && writes==0);
    // A new constructor at reused addresses is a new award, even in one node.
    h_lvl_opener(screen,pending,nullptr);
    CHECK(get<void*>(screen,0xA0)==own_b && rendered==0xBBBB);
    CHECK(get<uint32_t>(own_b,0xC30)==1);
    choice_on_node_entered(202); h_lvl_opener(screen,pending,nullptr);
    CHECK(get<void*>(screen,0xA0)==own_a && get<uint32_t>(own_a,0xC30)==2);
    // Candy already targets a local cat; let native increment it exactly once.
    h_lvl_opener(screen,own_b,nullptr);
    CHECK(get<void*>(screen,0xA0)==own_b && get<uint32_t>(own_b,0xC30)==2);
    CHECK(get<uint32_t>(pending,0xC30)==5 && writes==0);
    // Choice replication off: constructor behaves as the original game.
    g.on=false; h_lvl_opener(screen,pending,nullptr);
    CHECK(get<void*>(screen,0xA0)==pending && get<uint32_t>(pending,0xC30)==6);
    printf("level opener: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
