#include "mgmp_equipment_guard.h"
// Real setup state machine, independent peer states; every frame uses the wire codec.
#include "../src/session/mgmp_setup.cpp"
#include <deque>
#include <functional>
#include <vector>
#include <array>
#include <cstdio>
using namespace mgmp;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
namespace {
unsigned current=0,npeers=2,live=2,counts[4]{4,4,4,4},commits[4]{},imports[4]{};
uint8_t transport[4]={0,1,2,3};
State states[4];Config cfg{};uint64_t rngs[4][4]{};
bool online=true,send_ok=true,apply_ok=true,install_ok=true,capture_ok=true;
struct Frame{unsigned from,to;std::vector<uint8_t> bytes;};std::deque<Frame> frames;
void at(unsigned p,const std::function<void()>& f){current=p;strcpy_s(cfg.net_role,p?"client":"host");std::swap(g,states[p]);f();std::swap(g,states[p]);}
void drain(){unsigned budget=400;while(!frames.empty()){CHECK(budget--);auto f=frames.front();frames.pop_front();at(f.to,[&]{Reader r(f.bytes.data(),(uint32_t)f.bytes.size());auto t=r.u8v();
 if(t==MSG_SETUP){SetupMsg m{};CHECK(dec_setup(r,m));setup_on_message(m,(uint8_t)f.from);free_msg(m);}
 else if(t==MSG_CHAPTER){ChapterMsg m{};CHECK(dec_chapter(r,m));setup_on_chapter_message(m,(uint8_t)f.from);}
 else {ChapterSeedMsg m{};CHECK(t==MSG_CHAPTERSEED&&dec_chapterseed(r,m));setup_on_chapterseed_message(m,(uint8_t)f.from);}
 });}}
bool send(std::vector<uint8_t> b){if(!send_ok)return false;for(unsigned p=0;p<live;++p)if(p!=current)frames.push_back({transport[current],p,b});return true;}
void reset(unsigned n,unsigned code){
 for(unsigned p=0;p<4;++p)at(p,[]{setup_shutdown();});frames.clear();for(unsigned i=0;i<4;++i)transport[i]=(uint8_t)i;npeers=live=n;online=send_ok=apply_ok=install_ok=capture_ok=true;
 for(unsigned p=0;p<n;++p){counts[p]=1+(code%4);code/=4;commits[p]=imports[p]=0;for(unsigned i=0;i<4;++i)rngs[p][i]=100*p+i+1;at(p,[]{setup_init();});}
}
void tick(unsigned p,bool resume=false){at(p,[&]{if(resume)setup_on_map(0xabc);else setup_on_chapter_page();});}
void retry(bool resume=false){for(unsigned k=0;k<125;++k){for(unsigned p=0;p<npeers;++p)tick(p,resume);drain();}}
void native(void*,int act){CHECK(act==2);++commits[current];}
void control(unsigned p){at(p,[]{unsigned before=commits[current];int32_t d=0;int screen=1;setup_on_chapter_control(&screen,2,&d,native);if(commits[current]>before)CHECK(d==7);});}
void prepare(bool resume=false){tick(0,resume);for(unsigned p=1;p+1<npeers;++p){tick(p,resume);drain();}
 at(0,[]{CHECK(!setup_runtime_ready()&&!setup_host_can_enter());});tick(npeers-1,resume);drain();retry(resume);
 for(unsigned p=0;p<npeers;++p)at(p,[]{CHECK(setup_runtime_ready()&&setup_has_shared_roster());});
}
}
namespace mgmp {
const Config& config(){return cfg;}bool net_active(){return online;}
uint8_t net_peer_count(){return (uint8_t)live;}uint8_t net_peer_pos(){return (uint8_t)current;}
bool net_peer_ids(uint8_t* out,uint8_t cap){if(cap<live)return false;for(unsigned i=0;i<live;++i)out[i]=transport[i];return true;}
void log_line(const char*,const char*,...){}void log_line_lvl(LogLevel,const char*,const char*,...){}
bool catsync_prepare_party_setup(SetupMsg& m,uint8_t owner){if(!capture_ok)return false;m.count=(uint8_t)counts[owner];
 for(unsigned i=0;i<m.count;++i){auto* b=(uint8_t*)calloc(1,16);b[0]=(uint8_t)owner;m.cats[i]={0x70000001ull+((uint64_t)owner<<24)+i,16,owner*4+i+1,b};}return true;}
bool catsync_export_resume(SetupMsg& m,uint8_t owner){return catsync_prepare_party_setup(m,owner);}
void catsync_setup_sent(const SetupMsg&){}
bool catsync_apply_snapshot(const CatDataMsg& m,const char*,bool create){CHECK(create&&session_cat_owner(m.id)>=0);++imports[current];return apply_ok;}
bool roster_setup_install_shared(const uint64_t* ids,uint32_t n,const char*){unsigned seen[4]{};for(unsigned i=0;i<n;++i){int p=session_cat_owner(ids[i]);CHECK(p>=0&&p<(int)npeers);++seen[p];}
 for(unsigned p=0;p<npeers;++p)CHECK(seen[p]==counts[p]);return install_ok;}
void roster_setup_set_ready(bool){}
bool net_send_setup(const SetupMsg& m){std::vector<uint8_t>b(setup_frame_size(m));CHECK(enc_setup(b.data(),(uint32_t)b.size(),m)==b.size());return send(std::move(b));}
bool net_send_chapter(const ChapterMsg& m){std::vector<uint8_t>b(32);auto n=enc_chapter(b.data(),32,m);CHECK(n);b.resize(n);return send(std::move(b));}
bool net_send_chapterseed(const ChapterSeedMsg& m){std::vector<uint8_t>b(64);auto n=enc_chapterseed(b.data(),64,m);CHECK(n);b.resize(n);return send(std::move(b));}
uint64_t* rng_global_stream(){return rngs[current];}bool checkpoint_restart_run(){return true;}
}
namespace mgmp {
int say_client=0,say_wait=0,say_story=0;
void room_say_chapter_client(){++say_client;}
void room_say_chapter_wait(){++say_wait;}
void room_say_story_only_host(){++say_story;}
}
namespace {
int eq_calls=0; bool eq_legacy=false,eq_missing=false;int64_t eq_location=-1;
void __fastcall eq_refresh(void*){++eq_calls;}
void* __fastcall eq_lookup(void* p,uint64_t){return eq_missing?nullptr:p;}
int64_t __fastcall eq_where(void*,uint64_t){return eq_location;}
bool __fastcall eq_story(void*){return eq_legacy;}
// A fake CatData: five 0x60-byte equipment slots from +0x9B0, each a std::string name at +8 (inline form:
// 16 chars, size at +16, capacity at +24); an empty slot is all zero.
const char* g_legacy_names[]={"PutridLeech","Nuke","ThrobbingGristle",nullptr};
int g_legacy_asked=0;
bool __fastcall eq_by_name(void* slot){++g_legacy_asked;const char* n=(const char*)((uint8_t*)slot+8);
 for(int i=0;g_legacy_names[i];++i)if(!strcmp(n,g_legacy_names[i]))return true;return false;}
void put_item(uint8_t* cat,unsigned k,const char* name){
 uint8_t* slot=cat+kCat_Equipment+k*kEquip_Stride;memset(slot,0,kEquip_Stride);
 const uint64_t len=strlen(name),cap=15;strcpy_s((char*)slot+8,16,name);memcpy(slot+8+16,&len,8);memcpy(slot+8+24,&cap,8);}
void test_story_item_on_cat(){
 alignas(16) static uint8_t cat[0xC58];memset(cat,0,sizeof(cat));char item[48]="";
 CHECK(!cat_wears_story_item(cat,eq_by_name,item,sizeof(item))&&g_legacy_asked==0); // five empty slots: the game is never asked
 put_item(cat,0,"BasicSword");put_item(cat,2,"Sunglasses");
 CHECK(!cat_wears_story_item(cat,eq_by_name,item,sizeof(item))&&g_legacy_asked==2);   // ordinary gear, asked once per occupied slot
 put_item(cat,3,"PutridLeech");
 CHECK(cat_wears_story_item(cat,eq_by_name,item,sizeof(item))&&!strcmp(item,"PutridLeech")); // found in the fourth slot
 put_item(cat,3,"BoneNecklace");put_item(cat,4,"Nuke");
 CHECK(cat_wears_story_item(cat,eq_by_name,item,sizeof(item))&&!strcmp(item,"Nuke"));        // and in the last
 CHECK(cat_wears_story_item(cat,eq_by_name,nullptr,0));                                        // the name is optional
 CHECK(!cat_wears_story_item(nullptr,eq_by_name,item,sizeof(item))&&!cat_wears_story_item(cat,nullptr,item,sizeof(item)));
 memset(cat+kCat_Equipment+4*kEquip_Stride,0,kEquip_Stride);                                   // taken off again
 CHECK(!cat_wears_story_item(cat,eq_by_name,item,sizeof(item)));
 // a corrupt name length is treated as an empty slot, never passed on
 uint64_t huge=1ull<<40;memcpy(cat+kCat_Equipment+1*kEquip_Stride+8+16,&huge,8);int before=g_legacy_asked;
 CHECK(!cat_wears_story_item(cat,eq_by_name,item,sizeof(item)));(void)before;
 // a long name is cut at the buffer, not overrun
 put_item(cat,1,"PutridLeech");char small[6]="";
 CHECK(cat_wears_story_item(cat,eq_by_name,small,sizeof(small))&&strlen(small)<sizeof(small));
}
void test_equipment(){
 test_story_item_on_cat();
 unsigned char button[128]{};void* screen=button;uint64_t uid=123;
 memcpy(button+0x38,&screen,8);memcpy(button+0x58,&uid,8);
 auto gate=[&](bool prep){return equipment_gate(button,prep,eq_lookup,eq_where,eq_story,eq_refresh);};
 CHECK(gate(false)==EquipmentGate::Allow&&eq_calls==0);
 CHECK(gate(true)==EquipmentGate::Allow); // normal and blue sidequest: legacy=false
 eq_legacy=true;CHECK(gate(true)==EquipmentGate::MainStory);
 CHECK(gate(false)==EquipmentGate::Allow); // host, offline and shared run remain native
 eq_location=1234;CHECK(gate(true)==EquipmentGate::Allow); // unequip
 eq_location=-2;CHECK(gate(true)==EquipmentGate::Unreadable);
 eq_missing=true;CHECK(gate(true)==EquipmentGate::Unreadable);
 CHECK(equipment_gate(nullptr,true,nullptr,nullptr,nullptr,nullptr)==EquipmentGate::Unreadable);
 reset(4,0);
 at(0,[]{CHECK(!setup_client_preparing());});
 for(unsigned p=1;p<4;++p)at(p,[]{CHECK(setup_client_preparing());});
 prepare();for(unsigned p=0;p<4;++p)at(p,[]{CHECK(!setup_client_preparing());});
 // An early resume packet from a different checkpoint must not be applied later.
 reset(2,0);at(0,[]{SetupMsg m{};catsync_export_resume(m,1);m.kind=kSetupResumeExport;m.generation=20;m.checkpoint=0xbad;setup_on_message(m,1);free_msg(m);});
 tick(0,true);CHECK(!states[0].reply_sent&&imports[0]==0);
}
}
void test_hints(){
 // The chapter page: only the host chooses; the host must wait until every player is ready. Each refusal says so.
 reset(2,0);say_client=say_wait=0;
 at(0,[]{int32_t d=7;int screen=1;setup_on_chapter_control(&screen,2,&d,native);CHECK(!setup_on_select_act(2));});   // nobody ready yet
 CHECK(say_wait==1&&say_client==0);
 at(1,[]{CHECK(!setup_on_select_act(1));});                                                                           // a client pressed a chapter
 CHECK(say_client==1&&say_wait==1);
 prepare();say_wait=0;
 at(0,[]{CHECK(setup_on_select_act(2));});                                                                            // everybody ready: it goes through, silently
 CHECK(say_wait==0);
 at(0,[]{CHECK(!setup_on_select_act(2));});                                                                           // already committed: nothing to wait for
 CHECK(say_wait==0&&say_client==1);
}
int main(){
 test_equipment();test_hints();
 // Polling readiness before all players join must not freeze a two-player room.
 reset(4,0);live=2;at(0,[]{CHECK(!setup_runtime_ready());});live=4;prepare();
 // Transport peer IDs can have holes; ownership always uses session position.
 reset(3,0x24);transport[1]=2;transport[2]=3;prepare();

 for(unsigned n=2;n<=4;++n)for(unsigned code=0;code<(1u<<(2*n));++code){
  reset(n,code);prepare();
  for(unsigned p=1;p<n;++p)at(p,[]{CHECK(!setup_on_select_act(1)&&setup_client_controls_locked());});
  at(0,[]{int32_t d=7;int screen=1;setup_on_chapter_control(&screen,2,&d,native);CHECK(setup_on_select_act(2));CHECK(!setup_on_select_act(2));});drain();
  for(unsigned p=1;p<n;++p){control(p);control(p);CHECK(commits[p]==1);CHECK(!memcmp(rngs[0],rngs[p],32));}
 }
 // Resume and a second in-session preparation use the same actual participant mapping.
 reset(4,0x39);prepare(true);
 for(unsigned p=0;p<4;++p)at(p,[]{setup_reset_run();});prepare();
 // Reversed arrival order, immutable retry, failed imports and sends.
 reset(4,0xe4);tick(3);drain();tick(2);drain();tick(1);drain();CHECK(!states[0].reply_sent);
 send_ok=false;tick(0);drain();CHECK(!states[0].reply_sent);send_ok=true;apply_ok=false;retry();CHECK(!states[0].reply_sent);
 apply_ok=true;install_ok=false;retry();CHECK(!states[0].reply_sent);install_ok=true;retry();
 for(unsigned p=0;p<4;++p)at(p,[]{CHECK(setup_runtime_ready());});
 live=3;at(0,[]{CHECK(!setup_host_can_enter());});live=4;
 // A stale/foreign ACK must not unlock the last participant.
 reset(3,0);prepare();at(0,[]{g.ready[2]=false;ChapterMsg m{};m.generation=g.round+1;setup_on_chapter_message(m,2);CHECK(!setup_host_can_enter());m.generation=g.round;setup_on_chapter_message(m,3);CHECK(!setup_host_can_enter());setup_on_chapter_message(m,2);CHECK(setup_host_can_enter());});
 // Client waits for the seed even if a command is observed first.
 at(1,[]{g.have_seed=false;ChapterMsg m{};m.kind=kChapterSelect;m.generation=g.round;m.act=2;m.difficulty=7;setup_on_chapter_message(m,0);});control(1);CHECK(commits[1]==0);
 at(1,[]{ChapterSeedMsg m{};m.generation=g.round;memcpy(m.rng,rngs[0],32);setup_on_chapterseed_message(m,0);});control(1);CHECK(commits[1]==1);
 // Bad export, wrong owner, zero cats and too many per player are held.
 reset(2,0);tick(0);at(0,[]{SetupMsg m{};catsync_prepare_party_setup(m,0);m.generation=4;setup_on_message(m,1);CHECK(!g.exports[1].count);free_msg(m);m.generation=5;setup_on_message(m,1);CHECK(!g.exports[1].count);});
 // Old reply after run reset cannot satisfy a new request.
 reset(2,0);prepare();SetupMsg old{};at(0,[&]{CHECK(retain(old,g.reply));});
 for(unsigned p=0;p<2;++p)at(p,[]{setup_reset_run();});tick(1);at(1,[&]{setup_on_message(old,0);CHECK(!g.host_reply_seen);});free_msg(old);drain();tick(0);retry();
 for(unsigned p=0;p<4;++p)at(p,[]{setup_shutdown();});
 printf("test_setup: %u checks, 336 party combinations passed\n",checks);
}
