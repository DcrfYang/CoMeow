// test_wire_sizes.cpp -- every message, filled to the limits of its struct, must fit the buffer its sender encodes into (0.1.182).
//
// A message that does not fit is not sent at all: enc_* returns 0, net_send_* returns false, and most callers do not look. MSG_CATS was the one found -- 8 cats with
// 13-character Chinese names took 522 bytes of a 512-byte buffer. The buffers are the kSend* table of mgmp_proto.h, which mgmp_net.cpp uses too.
#include "mgmp_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mgmp;

static int g_fail = 0;
static std::vector<uint8_t> g_big(1 << 20);

// A string field at its longest: every byte but the NUL (3-byte UTF-8 characters take the same bytes as 'x').
template <size_t N> static void fill(char (&s)[N]) { memset(s, 'x', N - 1); s[N - 1] = 0; }

template <class M, class Enc>
static void fits(const char* what, uint32_t cap, const M& m, Enc enc) {
    const uint32_t need = enc(g_big.data(), (uint32_t)g_big.size(), m);
    if (!need) { printf("  FAIL  %-14s the filled message was refused by its encoder (the test fill is wrong)\n", what); ++g_fail; return; }
    std::vector<uint8_t> p(cap);
    const uint32_t n = enc(p.data(), cap, m);
    const bool ok = n == need;
    printf("  %s  %-14s %5u / %5u bytes\n", ok ? "ok  " : "FAIL", what, (unsigned)need, (unsigned)cap);
    if (!ok) ++g_fail;
}

int main() {
    printf("test_wire_sizes: every message at its limits fits its sender's buffer\n");
    { Hello m; fill(m.name); fits("hello", kSendSmall, m, enc_hello); }
    { Welcome m; m.cat_count = 32; fits("welcome", kSendSmall, m, enc_welcome); }
    { ActionMsg m; m.battle_id = 1; fill(m.gon); fits("action", kSendSmall, m, enc_action); }
    { HashMsg m; m.battle_id = 1; fits("hash", kSendSmall, m, enc_hash); }
    { HaltMsg m; fill(m.reason); fits("halt", kSendSmall, m, enc_halt); }
    { ControlMsg m; m.battle_id = 1; m.count = 32; fits("control", kSendSmall, m, enc_control); }
    { CursorMsg m; m.battle_id = 1; fits("cursor", kSendSmall, m, enc_cursor); }
    { AimMsg m; m.battle_id = 1; fill(m.gon); fits("aim", kSendSmall, m, enc_aim); }
    { ChoiceMsg m; fill(m.name); fill(m.event_name); fits("choice", kSendSmall, m, enc_choice); }
    { DebugHitMsg m; m.battle_id = 1; fits("debughit", kSendSmall, m, enc_debughit); }
    { PartyMsg m; m.count = kMaxPartyIds; fits("party", kSendSmall, m, enc_party); }
    { ChapterMsg m; m.generation = 1; fits("chapter", kSendSmall, m, enc_chapter); }
    { ChapterSeedMsg m; m.generation = 1; fits("chapterseed", kSendSmall, m, enc_chapterseed); }
    { NodeHashMsg m; fill(m.event); fits("nodehash", kSendSmall, m, enc_nodehash); }
    { HostLeftMsg m; fill(m.scene); fits("hostleft", kSendSmall, m, enc_hostleft); }
    { PageMsg m; fits("page", kSendSmall, m, enc_page); }
    { NodeVoteMsg m; m.seed0 = 1; fits("nodevote", kSendSmall, m, enc_nodevote); }
    { VoiceMsg m; m.samples = (uint16_t)(kVoiceMaxBytes * 2 < kVoiceMaxSamples ? kVoiceMaxBytes * 2 : kVoiceMaxSamples); m.len = kVoiceMaxBytes; fits("voice", kSendSmall, m, enc_voice); }
    { CatsMsg m; m.count = kBriefCats; for (auto& c : m.cats) fill(c.name); fits("cats", kSendCats, m, enc_cats); }
    { SaveWaitMsg m; fits("savewait", kSendSmall, m, enc_savewait); }
    { AbandonMsg m; m.kind = kAbandonPropose; fits("abandon", kSendSmall, m, enc_abandon); }
    { RoomCtlMsg m; m.kind = kRoomName; fill(m.name); fits("roomctl", kSendSmall, m, enc_roomctl); }
    { MapSeedsMsg m; m.epoch = 1; m.total = kMapSeedsMaxNodes; m.count = kMapSeedsChunk; fits("mapseeds", kSendSmall, m, enc_mapseeds); }
    { EnterNodeMsg m; m.familiar_count = 64; m.party_count = 16;
      for (uint32_t i = 0; i < 64; ++i) m.familiar_ids[i] = 100 + i; for (uint32_t i = 0; i < 16; ++i) m.party_ids[i] = 1000 + i;
      fits("enter_node", kSendEnterNode, m, enc_enter_node); }
    { ChatMsg m; fill(m.name); fill(m.text); fits("chat", kSendChat, m, enc_chat); }
    { DrawMsg m; m.count = kDrawChunk; fits("draw", kSendDraw, m, enc_draw); }
    { EquipMsg m; m.battle = 1; fill(m.item); fill(m.str_aux); m.has_state = 1; fits("equip", kSendEquip, m, enc_equip); }
    { TreasureMsg m; m.kind = kTreasureState; m.node = 1; m.n = kTreasureMaxSlots; fits("treasure", kSendTreasure, m, enc_treasure); }
    { HouseCoopMsg m; fill(m.boss); fits("housecoop", kSendHouseCoop, m, enc_housecoop); }
    { RnglMsg m; m.battle = 1; m.detail = 1; m.sites = kRnglSites; fits("rngl", kSendSmall, m, enc_rngl); }
    { UqdMsg m; m.battle = 1; fits("uqd", kSendUqd, m, enc_uqd); }
    { PingMsg m; m.n_rtt = kMaxPeers; fits("ping", kSendPing, m, enc_ping); }
    { SettingMsg m; m.id = kSettingCombatSpeed; m.value = 1.0f; fits("setting", kSendSetting, m, enc_setting); }
    { RollMsg m; m.battle = 1; m.site = 1; fits("roll", kSendRoll, m, enc_roll); }
    { BoardReqMsg m; m.battle = 1; fits("boardreq", kSendBoardReq, m, enc_boardreq); }
    { PeersMsg m; m.count = kMaxPeers; fits("peers", kSendPeers, m, enc_peers); }
    { DeepMsg m; m.battle = 1; m.detail = 1; m.n = kDeepUnits; fits("deep", kSendDeep, m, enc_deep); }
    { PropsMsg m; m.epoch = 1; m.total = kPropsMax; m.count = kPropsChunk; fits("props", kSendProps, m, enc_props); }
    { BoardMsg m; m.battle = 1; m.cats = kBoardMax; m.total = kBoardMax; m.count = kBoardChunk;
      for (uint32_t i = 0; i < kBoardChunk; ++i) { m.units[i].index = (uint8_t)i; fill(m.units[i].def); }
      fits("board", kSendBoard, m, enc_board); }
    { BoardSumMsg m; m.battle = 1; m.cats = kBoardMax; fits("boardsum", 16 + 4 + 4 + 32 + 4 * kBoardMax, m, enc_boardsum); }
    { TurnQueueMsg m; m.battle = 1; m.n = kTurnQueueMax; fits("turnqueue", 16 + 2 * kTurnQueueMax, m, enc_turnqueue); }
    { UstateReqMsg m; m.n = kUstateReqMax; fits("ustatereq", 32 + kUstateReqMax, m, enc_ustatereq); }
    {   // 0.1.195: the fullest ground map -- every cell a tile with elements, every name at its longest
        static TilesMsg m; m.battle = 1; m.w = 32; m.h = 32; m.nnames = kTileNamesMax;
        for (uint32_t i = 0; i < kTileNamesMax; ++i) { memset(m.names[i], 'x', kTileNameLen - 1); m.names[i][kTileNameLen - 1] = 0; }
        for (uint32_t c = 0; c < kTilesMaxCells; ++c) { m.cell[c] = (uint8_t)(c % kTileNamesMax); m.m0[c] = 0xFFFFFFFF; m.m1[c] = 1; }
        fits("tiles", kSendTiles, m, enc_tiles);
        static uint8_t buf[kSendTiles]; static TilesMsg back;
        const uint32_t n = enc_tiles(buf, sizeof(buf), m);
        const bool same = n && dec_tiles(buf, n, back) && back.w == 32 && back.nnames == kTileNamesMax && back.cell[1023] == m.cell[1023] && back.m0[5] == m.m0[5] && strcmp(back.names[3], m.names[3]) == 0;
        printf("  %s  tiles round trip\n", same ? "ok  " : "FAIL"); if (!same) ++g_fail;
    }
    {   // 0.1.213: the fullest per-action check -- every row a living unit -- and its round trip
        static ActCheckMsg m; m.battle = 1; m.turn = 7; m.action = (3u << 8) | 2; m.n = (uint8_t)kActCheckMax;
        for (uint32_t i = 0; i < kActCheckMax; ++i) { m.row[i].flags = kActRowOk; m.row[i].hp = 100000 + (int32_t)i; m.row[i].shield = -1; m.row[i].tx = -5000; m.row[i].ty = (int16_t)i; }
        m.row[5].flags = kActRowOk | kActRowDead;
        fits("actcheck", kSendActCheck, m, enc_actcheck);
        static uint8_t buf[kSendActCheck]; static ActCheckMsg back;
        const uint32_t n = enc_actcheck(buf, sizeof(buf), m);
        bool same = n && dec_actcheck(buf, n, back) && back.n == m.n && back.action == m.action;
        for (uint32_t i = 0; same && i < kActCheckMax; ++i) same = act_rows_same(back.row[i], m.row[i]);
        printf("  %s  actcheck round trip\n", same ? "ok  " : "FAIL"); if (!same) ++g_fail;
    }
    { CatDigestMsg m; m.count = kCatDigestMax; for (uint32_t i = 0; i < kCatDigestMax; ++i) m.ids[i] = 1 + i; fits("catdigest", 2 + kCatDigestMax * 20, m, enc_catdigest); }
    { CheckpointMsg m; m.count = kMaxPeers; for (auto& c : m.candidates) c.run = 1;
      fits("checkpoint", 8192, m, [](uint8_t* p, uint32_t cap, const CheckpointMsg& x) { return enc_checkpoint(p, cap, x); }); }
    { std::string reason(255, 'x');
      fits("refuse", kSendSmall, reason, [](uint8_t* p, uint32_t cap, const std::string& r) { return enc_refuse(p, cap, r.c_str()); }); }
    { UnlocksMsg* m = new UnlocksMsg;
      m->epoch = 1; m->n_events = 64; m->n_spawn = (uint8_t)kSpawnMax; fill(m->level_name); m->n_level_name = (uint8_t)(sizeof(m->level_name) - 1);
      for (auto& q : m->n_pending) q = (uint8_t)kPendingMax;
      m->n_weather = (uint8_t)kWeatherNames; for (auto& w : m->weather) memset(w, 'x', kWeatherLen - 1);
      m->n_classes[0] = m->n_classes[1] = (uint8_t)kClassMax; for (auto& side : m->classes) for (auto& c : side) memset(c, 'x', kClassLen - 1);
      fits("unlocks", kSendUnlocks, *m, enc_unlocks);
      delete m; }
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
