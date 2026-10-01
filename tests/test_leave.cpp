// Real scene reader and departure lifecycle, with only game/transport endpoints stubbed.
#include "../src/session/mgmp_leave.cpp"
#include <string>
#include <new>
#include <cstdlib>
using namespace mgmp;
static unsigned checks=0, sent=0, disconnected=0;
static bool loaded=true;
static Config cfg{};
#define CHECK(x) do {++checks;if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
namespace mgmp {
const Config& config(){return cfg;}
bool net_active(){return true;}
bool net_send_hostleft(const HostLeftMsg&){++sent;return true;}
bool lockstep_in_battle(){return false;}
bool lockstep_cat_is_mine(uint64_t,bool& mine){mine=true;return true;}
bool savefile_adventure_is_loaded(){return loaded;}
bool checkpoint_on_alone(){return true;}
void setup_on_chapter_page(){}
void follow_shutdown(){}
void session_request_disconnect(){++disconnected;}
uintptr_t addr_of_data(DataSym){return 0;}
uintptr_t addr_of_call(Call){return 0;}
void log_line(const char*,const char*,...){}
void log_line_lvl(LogLevel,const char*,const char*,...){}
}
struct SceneFixture {
 alignas(8) unsigned char scene[1280]{}, director[32]{}, owner[64]{};
 const void* entry=scene; const void* ownerPtr=owner;
 SceneFixture(){
   new(scene+kScene_Name) std::string("Map");
   const void* begin=&entry; const void* end=&entry+1; const void* dir=director;
   memcpy(director+kDirector_ScenesBegin,&begin,8);memcpy(director+kDirector_ScenesEnd,&end,8);
   memcpy(owner+kDir_SceneDirector,&dir,8);
   g.director_slot=&ownerPtr;
 }
 ~SceneFixture(){reinterpret_cast<std::string*>(scene+kScene_Name)->~basic_string();}
 void name(const char* s){*reinterpret_cast<std::string*>(scene+kScene_Name)=s;}
};
void poll(){for(unsigned i=0;i<kPollFrames;++i)leave_pump();}
void reset(bool client){g=State{};strcpy_s(cfg.net_role,client?"client":"host");sent=disconnected=0;loaded=true;leave_init();}
int main(){
 reset(false); {
   SceneFixture s;poll();CHECK(g.was_in_run && g.saw_map);
   leave_on_settlement();CHECK(g.settled && !g.was_in_run && !g.saw_map);
   poll();CHECK(!g.saw_map); // stale Map during native teardown must not re-arm
   s.name("House");poll();poll();CHECK(sent==0 && disconnected==0);
   s.name("MainMenu");poll();poll();CHECK(sent==0);
   leave_run_reset();s.name("Map");poll();CHECK(g.was_in_run && !g.settled);
   s.name("MainMenu");poll();poll();CHECK(sent==1); // actual departure still works
 }
 reset(true); {
   SceneFixture s;poll();HostLeftMsg m{};strcpy_s(m.scene,"MainMenu");
   leave_on_message(m);CHECK(g.pending);
   leave_on_settlement();CHECK(!g.pending && !g.solo);
   s.name("House");poll();CHECK(disconnected==0);
   leave_on_message(m);CHECK(!g.pending); // late previous departure ignored
   leave_run_reset();s.name("Map");poll();leave_on_message(m);CHECK(g.pending);
   s.name("House");poll();CHECK(disconnected==1 && !g.pending); // manual abandon uses full teardown
 }
 reset(false); {
   SceneFixture s;poll();g.presses=5;g.said_gave_up=true;
   leave_run_reset();CHECK(!g.was_in_run && !g.saw_map && !g.announced);
   leave_on_settlement();CHECK(g.presses==0 && !g.said_gave_up);
 }
 printf("leave: %u checks passed\n",checks);
}
