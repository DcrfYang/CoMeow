#include "mgmp_setup.h"
#include "mgmp_catsync.h"
#include "mgmp_checkpoint.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_rng.h"
#include "mgmp_roster.h"
#include "mgmp_room.h"
#include "mgmp_tuning.h"
#include "mgmp_unlocks.h"
#include "mgmp_mem.h"
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {
// A gear-screen lock taken over for a player without chapter 2 (setup_on_equip_done): the game's closure waits here until the room lets it go.
struct Hold{bool on=false;void* director=nullptr;void(*go)(void*)=nullptr;};
struct State {
    Hold hold;
    bool on=false,is_client=false,client_lock=false,chapter_seen=false,resume=false,resume_error=false;
    bool sent=false,reply_sent=false,host_reply_seen=false,ready_sent=false,committed=false,have_command=false,have_seed=false;
    uint8_t members=0,peers[kMaxPeers]{};
    bool ready[kMaxPeers]{};
    uint32_t mapflags[kMaxPeers]{};  // by session position, from each client's READY (host only): the map flags that player has
    uint64_t checkpoint=0;
    uint32_t generation=0,round=0,retry_ticks=0;
    bool said_blocked=false;
    int32_t difficulty[3]{};bool have_difficulty[3]{};
    SetupMsg exports[kMaxPeers]{},outgoing{},pending{},reply{};
    ChapterMsg command{};ChapterSeedMsg seed{};
} g;
uint32_t next_generation=0;
// The seed of the chapter being generated, armed at the commit (host) / when the command is applied (client) and used once by setup_on_generate_map.
// Outside State: the commit restarts the checkpoint round, which may reset the barrier before the map is generated.
uint64_t map_seed[4]={};bool map_seed_armed=false;
void arm_map_seed(const uint64_t* s){memcpy(map_seed,s,sizeof(map_seed));map_seed_armed=true;}
void free_msg(SetupMsg& m){for(auto& c:m.cats)free(c.data);m=SetupMsg{};}
bool retain(SetupMsg& dst,const SetupMsg& src){
    if(&dst==&src)return true;
    SetupMsg copy=src;for(auto& c:copy.cats)c.data=nullptr;
    for(unsigned i=0;i<src.count;++i){copy.cats[i].data=(uint8_t*)malloc(src.cats[i].size);
        if(!copy.cats[i].data){free_msg(copy);return false;}memcpy(copy.cats[i].data,src.cats[i].data,src.cats[i].size);}
    free_msg(dst);dst=copy;return true;
}
void clear(){for(auto& m:g.exports)free_msg(m);free_msg(g.outgoing);free_msg(g.pending);free_msg(g.reply);g=State{};}
void refresh(){const auto& c=config();g.is_client=_stricmp(c.net_role,"client")==0;
    g.on=tune::kLocalSetup && (g.is_client || _stricmp(c.net_role,"host")==0) && net_active();}
bool freeze(){
    uint8_t n=net_peer_count(),ids[kMaxPeers]{};
    if(n<2||n>kMaxPeers||!net_peer_ids(ids,kMaxPeers))return false;
    if(g.members && (n!=g.members||memcmp(ids,g.peers,n))) {
        // Runtime-ready is polled while the lobby is still filling. Freeze
        // membership only after an actual export, not that first idle poll.
        if(g.sent||g.outgoing.count||g.reply_sent||g.host_reply_seen)return false;
        for(const auto& m:g.exports)if(m.count)return false;
    }
    g.members=n;memcpy(g.peers,ids,n);return true;
}
int pos(uint8_t peer){for(unsigned i=0;i<g.members;++i)if(g.peers[i]==peer)return int(i);return -1;}
bool all_ready(){if(!freeze() || !g.reply_sent)return false;for(unsigned i=1;i<g.members;++i)if(!g.ready[i])return false;return true;}
bool valid(const SetupMsg& m,int owner=-1){
    if(!m.count||m.count>(owner<0?kSetupMaxCats:kPartyMaxCats)||!m.generation)return false;
    unsigned counts[kMaxPeers]{};
    for(unsigned i=0;i<m.count;++i){const auto& c=m.cats[i];int p=session_cat_owner(c.id);
        if(p<0||p>=g.members||(owner>=0&&p!=owner)||++counts[p]>4||!c.data||!c.size||c.size>kMaxCatBytes)return false;
        for(unsigned j=0;j<i;++j)if(m.cats[j].id==c.id)return false;
    }
    if(owner<0)for(unsigned i=0;i<g.members;++i)if(!counts[i])return false;
    return true;
}
bool install(const SetupMsg& m){uint64_t ids[kSetupMaxCats]{};for(unsigned i=0;i<m.count;++i)ids[i]=m.cats[i].id;
    return roster_setup_install_shared(ids,m.count,"all-player setup barrier");}
bool apply(const SetupMsg& m,bool skip_own){
    for(unsigned i=0;i<m.count;++i){const auto& c=m.cats[i];if(skip_own&&session_cat_owner(c.id)==net_peer_pos())continue;
        CatDataMsg x{};x.id=c.id;x.size=c.size;x.hash=c.hash;x.data=c.data;
        if(!catsync_apply_snapshot(x,"all-player setup exchange",true))return false;}
    return true;
}
void progress(){
    if(!freeze()||!(g.chapter_seen||g.resume))return;
    if(g.retry_ticks){--g.retry_ticks;return;}g.retry_ticks=120;
    if(g.is_client){
        if(!g.sent){
            if(!g.outgoing.count){
                if(!(g.resume?catsync_export_resume(g.outgoing,net_peer_pos()):catsync_prepare_party_setup(g.outgoing,net_peer_pos()))){
                    free_msg(g.outgoing);log_line_lvl(LogLevel::Error,"SETUP","cannot capture this player's selected 1..4 cats");return;}
                g.outgoing.kind=g.resume?kSetupResumeExport:kSetupClientExport;g.outgoing.generation=g.generation;
                g.outgoing.checkpoint=g.checkpoint;
            }
            if(!net_send_setup(g.outgoing))return;
            catsync_setup_sent(g.outgoing);g.sent=true;
            log_line("SETUP","exported %u cats for player %u, request %u",g.outgoing.count,net_peer_pos(),g.generation);
        }else if(!g.host_reply_seen)net_send_setup(g.outgoing);
        if(g.pending.count&&!g.host_reply_seen){
            if(!apply(g.pending,true)||!install(g.pending))return;
            g.round=g.pending.generation;g.host_reply_seen=true;roster_setup_set_ready(true);free_msg(g.pending);
        }
        if(g.host_reply_seen&&!g.ready_sent){ChapterMsg m{};m.generation=g.round;m.mapflags=unlocks_mapflags();g.ready_sent=net_send_chapter(m);}
        return;
    }
    if(g.reply_sent){if(!all_ready())net_send_setup(g.reply);return;}
    for(unsigned i=1;i<g.members;++i){
        const auto& m=g.exports[i];
        if(!m.count)return;
        // Exports may arrive before this peer reaches its chapter/map screen.
        // Revalidate against the now-known run mode before importing anything.
        // The map fingerprint is NOT compared (2026-10-02): each peer restored its own local checkpoint, so the two maps can differ in
        // flags and still be the same run -- the map sync that follows makes them agree. Comparing them dropped the client's export for good
        // and the restored run could never leave its first map ("holding host node 8 (checkpoint barrier)").
        if(m.kind!=(g.resume?kSetupResumeExport:kSetupClientExport)){
            free_msg(g.exports[i]);return;
        }
    }
    if(!g.exports[0].count){
        if(!(g.resume?catsync_export_resume(g.exports[0],0):catsync_prepare_party_setup(g.exports[0],0))){free_msg(g.exports[0]);return;}
        g.exports[0].generation=g.generation;
    }
    SetupMsg reply{};reply.kind=g.resume?kSetupResumeReply:kSetupHostReply;reply.generation=g.generation;
    reply.checkpoint=g.checkpoint;reply.members=g.members;memcpy(reply.peers,g.peers,g.members);
    for(unsigned p=0;p<g.members;++p){
        reply.requests[p]=g.exports[p].generation;
        for(unsigned i=0;i<g.exports[p].count;++i)reply.cats[reply.count++]=g.exports[p].cats[i];
    }
    if(!valid(reply)||!apply(reply,true)||!install(reply)||!retain(g.reply,reply)||!net_send_setup(g.reply))return;
    catsync_setup_sent(g.reply);g.round=g.generation;g.reply_sent=true;roster_setup_set_ready(true);
    log_line("SETUP","shared roster sent: %u players, %u cats; waiting for every READY",g.members,reply.count);
}
} // namespace
void setup_init(){clear();refresh();g.generation=++next_generation;g.client_lock=g.on&&g.is_client;roster_setup_set_ready(!g.on);}
void setup_shutdown(){map_seed_armed=false;clear();roster_setup_set_ready(true);}
void setup_reset_run(){setup_init();}
void setup_on_chapter_page(){refresh();if(!g.on||g.resume)return;g.chapter_seen=true;progress();}
void setup_on_generate_map(){
    if(!map_seed_armed)return;
    map_seed_armed=false;
    uint64_t* s=rng_global_stream();
    if(!s)return;
    log_line("SETUP","the chapter map is being generated -- the stream is set to the host's committed seed %016llx (it was %016llx)",(unsigned long long)map_seed[0],(unsigned long long)s[0]);
    memcpy(s,map_seed,sizeof(map_seed));
}
void setup_on_map(uint64_t checkpoint){
    if(checkpoint){unlocks_override_clear();map_seed_armed=false;}   // a readable live map: its generation is over
    refresh();if(!g.on||g.chapter_seen||!checkpoint||!freeze())return;
    if(!g.resume){SetupMsg probe{};if(!catsync_export_resume(probe,net_peer_pos())){free_msg(probe);
        if(!g.resume_error)log_line_lvl(LogLevel::Error,"SETUP","resume held: invalid saved player roster");g.resume_error=true;return;}
        free_msg(probe);g.resume=true;g.checkpoint=checkpoint;}
    progress();
}
bool setup_runtime_ready(){refresh();return !g.on || (freeze()&&(g.is_client?(g.host_reply_seen&&g.ready_sent):all_ready()));}
bool setup_client_preparing(){refresh();return g.on&&g.is_client&&!g.resume&&!g.committed&&!g.host_reply_seen;}
bool setup_has_shared_roster(){refresh();return g.on&&(g.is_client?g.host_reply_seen:g.reply_sent);}
bool setup_host_can_enter(){refresh();return !g.on||g.is_client||((g.chapter_seen||g.resume)&&all_ready());}
bool setup_client_controls_locked(){refresh();return g.client_lock||(g.on&&(g.is_client||g.committed));}
// 0: every player has chapter `act`; 2: somebody lacks chapter 2 (so chapters 2 and 3 are both closed); 3: everybody has chapter 2 but somebody lacks 3.
int chapter_locked_for(int act){
    if(act<2)return 0;
    bool lack2=false,lack3=false;
    for(unsigned i=0;i<g.members;++i){
        const uint32_t m=i==0?unlocks_mapflags():g.mapflags[i];
        if(!unlocks_allow(m,2))lack2=true;
        if(!unlocks_allow(m,3))lack3=true;}
    if(lack2)return 2;
    return act==3&&lack3?3:0;
}
bool setup_on_select_act(int act){
    refresh();if(g.client_lock||(g.on&&g.is_client)){room_say_chapter_client();return false;}if(!g.on)return true;
    if(g.resume)return false;setup_on_chapter_page();
    if(!all_ready()||g.committed||act<1||act>3||!g.have_difficulty[act-1]){
        if(!g.said_blocked)log_line("SETUP","chapter held: waiting for every player's preparation/READY");g.said_blocked=true;
        if(!g.committed)room_say_chapter_wait();
        return false;}
    // The host chooses for everybody: it may only choose a chapter every player has unlocked.
    if(const int why=chapter_locked_for(act)){
        if(why==2)room_say_chapter_locked_2();else room_say_chapter_locked_3();
        log_line("SETUP","chapter %d refused: a player has not unlocked chapter %d",act,why);return false;}
    ChapterSeedMsg seed{};seed.generation=g.round;
    auto* rng=rng_global_stream();if(!rng)return false;memcpy(seed.rng,rng,sizeof(seed.rng));
    if(!net_send_chapterseed(seed))return false; // seed must precede the command
    arm_map_seed(seed.rng);
    ChapterMsg m{};m.kind=kChapterSelect;m.generation=g.round;m.act=(uint8_t)act;m.difficulty=g.difficulty[act-1];
    m.mapflags=unlocks_mapflags();for(unsigned i=1;i<g.members;++i)m.mapflags&=g.mapflags[i];   // the map is built from what EVERY player has
    if(!net_send_chapter(m))return false;
    unlocks_override_set(m.mapflags);
    g.committed=true;g.said_blocked=false;
#if defined(MGMP_WITH_CHECKPOINT)
    checkpoint_restart_run();
#endif
    log_line("SETUP","chapter %d committed for all %u players",act,g.members);return true;
}
bool setup_on_equip_done(void* director,void(*go)(void*)){
    refresh();
    if(!g.on||g.resume||g.committed||g.hold.on||!director||!go)return false;
    if(checkpoint_run_restored())return false;   // a resumed run never comes through the gear screen
    g.hold.on=true;g.hold.director=director;g.hold.go=go;
    g.have_difficulty[0]=true;g.difficulty[0]=0;  // chapter 1 has no difficulty to choose
    log_line("SETUP","no chapter page for this player (chapter 2 is locked): the gear screen's lock is the %s",
             g.is_client?"READY":"chapter-1 choice");
    setup_on_chapter_page();   // chapter_seen, and a client's export
    if(g.is_client)room_say_no_chapter_ready();else room_say_no_chapter_wait();
    return true;
}
void setup_tick(){
    if(!g.hold.on)return;
    refresh();
    void* director=g.hold.director;auto go=g.hold.go;
    if(!g.on||g.resume||!net_active()){   // the room is gone: the player pressed lock, let the game do what it would have done
        log_line("SETUP","the session ended while the chapter-1 start was held -- starting it on its own");
        g.hold=Hold{};go(director);return;}
    setup_on_chapter_page();   // progress(): the export / the shared roster, retried on its own cadence as the chapter page's buttons do
    if(g.is_client){
        if(!g.have_command||g.committed||!g.have_seed||!freeze()||!rng_global_stream())return;
        if(g.command.act!=1)log_line_lvl(LogLevel::Warn,"SETUP","the host chose chapter %d, which this player has not unlocked -- starting chapter 1 anyway",(int)g.command.act);
        memcpy(rng_global_stream(),g.seed.rng,sizeof(g.seed.rng));g.have_seed=false;g.have_command=false;g.committed=true;
#if defined(MGMP_WITH_CHECKPOINT)
        checkpoint_restart_run();
#endif
        unlocks_override_set(g.command.mapflags);
        arm_map_seed(g.seed.rng);
        log_line("SETUP","the host's command arrived -- starting chapter 1");
        g.hold=Hold{};go(director);return;
    }
    if(setup_on_select_act(1)){g.hold=Hold{};go(director);}   // the host: the chapter-1 choice, once every player is READY
}
bool setup_no_chapter_pending(){return g.hold.on;}
void setup_on_chapter_control(void* screen,int act,int32_t* difficulty,void(*select_act)(void*,int)){
    refresh();if(!g.on||!screen||!difficulty||!select_act||act<1||act>3)return;
    int32_t current=0;if(!mem_read(difficulty,&current,4)||current<0||current>1000)return;
    g.have_difficulty[act-1]=true;g.difficulty[act-1]=current;
    if(!g.is_client||!g.have_command||g.committed||g.command.act!=act||!g.have_seed||!freeze()||!rng_global_stream())return;
    if(!mem_write(difficulty,&g.command.difficulty,4))return;
    unlocks_override_set(g.command.mapflags);
    arm_map_seed(g.seed.rng);
    memcpy(rng_global_stream(),g.seed.rng,sizeof(g.seed.rng));g.have_seed=false;g.have_command=false;g.committed=true;
#if defined(MGMP_WITH_CHECKPOINT)
    checkpoint_restart_run();
#endif
    select_act(screen,act);
}
void setup_on_chapter_message(const ChapterMsg& m,uint8_t from){
    refresh();if(!g.on||!valid_chapter(m)||!freeze()||m.generation!=g.round)return;
    const int p=pos(from);
    if(!g.is_client&&m.kind==kChapterReady&&g.reply_sent&&p>0){g.ready[p]=true;g.mapflags[p]=m.mapflags;return;}
    if(g.is_client&&from==kHostPeer&&m.kind==kChapterSelect&&g.host_reply_seen&&g.ready_sent&&!g.resume&&!g.committed){g.command=m;g.have_command=true;}
}
void setup_on_chapterseed_message(const ChapterSeedMsg& m,uint8_t from){
    refresh();if(g.on&&g.is_client&&from==kHostPeer&&g.host_reply_seen&&!g.committed&&m.generation==g.round){g.seed=m;g.have_seed=true;}
}
void setup_on_message(const SetupMsg& m,uint8_t from){
    refresh();if(!g.on||!freeze())return;
    const bool resumed=m.kind==kSetupResumeExport||m.kind==kSetupResumeReply;
    if((g.resume&&!resumed)||(g.chapter_seen&&resumed)||(!resumed&&m.checkpoint)||
       (resumed&&!m.checkpoint))return;   // a resume message names a readable map; WHICH map is not compared (see progress)
    const int p=pos(from);
    if(!g.is_client&&(m.kind==kSetupClientExport||m.kind==kSetupResumeExport)&&p>0&&valid(m,p)){
        if(g.reply_sent){if(m.generation==g.reply.requests[p])net_send_setup(g.reply);return;}
        if(!retain(g.exports[p],m))return;
        g.retry_ticks=0;progress();return;
    }
    if(g.is_client&&(m.kind==kSetupHostReply||m.kind==kSetupResumeReply)&&from==kHostPeer&&g.sent&&
       m.members==g.members&&!memcmp(m.peers,g.peers,g.members)&&m.requests[net_peer_pos()]==g.generation&&valid(m)){
        // The host must echo this exact local selection, not old registry leftovers.
        unsigned own=0;for(unsigned i=0;i<m.count;++i)if(session_cat_owner(m.cats[i].id)==net_peer_pos()){
            bool found=false;for(unsigned j=0;j<g.outgoing.count;++j)if(m.cats[i].id==g.outgoing.cats[j].id&&m.cats[i].hash==g.outgoing.cats[j].hash)found=true;
            if(!found)return;++own;}
        if(own!=g.outgoing.count)return;
        if(g.host_reply_seen){if(m.generation==g.round){g.ready_sent=false;g.retry_ticks=0;progress();}return;}
        if(!retain(g.pending,m))return;g.retry_ticks=0;progress();
    }
}
} // namespace mgmp
