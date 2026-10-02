// test_proto.cpp -- covers the wire codec.
//
// The transport it belongs to can only be exercised with two game instances, so
// the encoding is tested here instead. Same reasoning as test_timedelay.cpp:
// the parts that are pure logic get tested where they are cheap to test, and
// the in-game run is then only asked to prove the parts that need a game.
//
//     cl /nologo /EHsc /std:c++17 /I..\src\net test_proto.cpp && test_proto.exe
#include "mgmp_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_fail; }
    else       printf("  ok    %s\n", what);
}

int main() {
    uint8_t buf[512];

    printf("-- ActionMsg round-trips every field --\n");
    {
        ActionMsg a{};
        a.turn = 7; a.actor = 3; a.type = 2;
        a.battle_id = 0x211f6ea51e365ae0ULL;   // a real node seed0, from a log
        a.slot_kind = 4; a.slot_index = 1;
        a.b30 = 1; a.b31 = 0;
        a.tx = 5; a.ty = -3; a.dx = -1; a.dy = 0;
        strcpy_s(a.gon, "wp_BearTraps");

        uint32_t n = enc_action(buf, sizeof(buf), a);
        check(n > 0, "encodes");

        Reader r(buf, n);
        check(r.u8v() == MSG_ACTION, "message type is ACTION");
        ActionMsg b{};
        check(dec_action(r, b), "decodes");
        check(b.battle_id == 0x211f6ea51e365ae0ULL,
              "battle_id, which keys every per-battle message -- all 64 bits of it, "
              "since it is a node seed and not a small counter");
        check(b.turn == 7 && b.actor == 3 && b.type == 2, "turn/actor/type");
        check(b.slot_kind == 4 && b.slot_index == 1, "slot identity");
        check(b.b30 == 1 && b.b31 == 0, "the two tail bytes");
        check(b.tx == 5 && b.ty == -3, "target survives a negative");
        check(b.dx == -1 && b.dy == 0, "direction survives a negative");
        check(strcmp(b.gon, "wp_BearTraps") == 0, "GON name, the second identity");
    }

    printf("\n-- types 6 and 7 are refused at the edge --\n");
    {
        // They are generated locally on both peers. One arriving over the wire
        // means the sender is broken, and applying it would double-fire a
        // reaction on this peer -- so the decoder rejects rather than trusts.
        for (uint8_t bad : { (uint8_t)1, (uint8_t)6, (uint8_t)7, (uint8_t)99 }) {
            ActionMsg a{}; a.type = bad; a.turn = 1;
            uint32_t n = enc_action(buf, sizeof(buf), a);
            Reader r(buf, n); r.u8v();
            ActionMsg b{};
            char what[64];
            sprintf_s(what, "type %u rejected", (unsigned)bad);
            check(!dec_action(r, b), what);
        }
        for (uint8_t good : { (uint8_t)2, (uint8_t)3 }) {
            ActionMsg a{}; a.type = good; a.turn = 1;
            uint32_t n = enc_action(buf, sizeof(buf), a);
            Reader r(buf, n); r.u8v();
            ActionMsg b{};
            char what[64];
            sprintf_s(what, "type %u accepted", (unsigned)good);
            check(dec_action(r, b), what);
        }
    }

    printf("\n-- Hello and the data-identity fields --\n");
    {
        Hello h{};
        h.gpak_hash = 0xDEADBEEFCAFEF00DULL;
        h.build_hash = 0x0123456789ABCDEFULL;
        h.rules = 1;
        strcpy_s(h.name, "host");
        uint32_t n = enc_hello(buf, sizeof(buf), h);
        Reader r(buf, n); r.u8v();
        Hello o{};
        check(dec_hello(r, o), "decodes");
        check(o.proto == kProtoVersion, "protocol version");
        check(o.gpak_hash == h.gpak_hash, "gpak hash survives the high bit");
        check(o.build_hash == h.build_hash, "build hash");
        check(strcmp(o.name, "host") == 0, "name");
        check(o.rules == 1, "the host's rule bits ride in the HELLO (proto 51)");
        check(kProtoVersion >= 51, "the rule bits require protocol 51 or later");
    }

    printf("\n-- Welcome carries all four xoshiro words --\n");
    {
        Welcome w{};
        w.rng_state[0] = 0x967e2d6d328620b1ULL;   // the state four launches
        w.rng_state[1] = 0x1111111111111111ULL;   // all entered a battle at
        w.rng_state[2] = 0x2222222222222222ULL;
        w.rng_state[3] = 0xFFFFFFFFFFFFFFFFULL;
        w.cat_count = 2; w.cats[0] = 0; w.cats[1] = 2;
        uint32_t n = enc_welcome(buf, sizeof(buf), w);
        Reader r(buf, n); r.u8v();
        Welcome o{};
        check(dec_welcome(r, o), "decodes");
        check(o.rng_state[0] == w.rng_state[0] && o.rng_state[3] == w.rng_state[3],
              "the simulation stream round-trips");
        check(o.cat_count == 2 && o.cats[0] == 0 && o.cats[1] == 2, "control list");
    }

    printf("\n-- HashMsg --\n");
    {
        HashMsg h{}; h.battle_id = 6; h.turn = 12; h.rng_hash = 0xABCDEF; h.state_hash = 0;
        h.queue_depth = 3; h.queue_sig = 0x99;
        uint32_t n = enc_hash(buf, sizeof(buf), h);
        Reader r(buf, n); r.u8v();
        HashMsg o{};
        check(dec_hash(r, o), "decodes");
        check(o.battle_id == 6, "battle id");
        check(o.turn == 12 && o.rng_hash == 0xABCDEF, "turn and rng hash");
        check(o.queue_depth == 3 && o.queue_sig == 0x99, "queue depth and signature");
    }

    printf("\n-- HashMsg carries the state hash --\n");
    {
        // state_hash was 0 on the wire for the whole of phase 4, so it is the
        // field most likely to be dropped by a codec change and least likely
        // to be noticed: two peers that both send 0 agree forever.
        HashMsg h{}; h.turn = 4; h.rng_hash = 1; h.state_hash = 0x8877665544332211ULL;
        uint32_t n = enc_hash(buf, sizeof(buf), h);
        Reader r(buf, n); r.u8v();
        HashMsg o{};
        check(dec_hash(r, o), "decodes");
        check(o.state_hash == 0x8877665544332211ULL, "state hash survives the high bit");
    }

    printf("\n-- ControlMsg round-trips the split --\n");
    {
        ControlMsg c{};
        c.battle_id = 3;
        c.humans = 4; c.count = 2; c.cats[0] = 19; c.cats[1] = 20;
        c.fp[0] = 0x03b4ff86; c.fp[1] = 0xba5139f8;   // real ones: a taken run
        uint32_t n = enc_control(buf, sizeof(buf), c);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_CONTROL, "message type is CONTROL");
        ControlMsg o{};
        check(dec_control(r, o), "decodes");
        check(o.battle_id == 3, "battle id");
        check(o.humans == 4, "human count");
        check(o.count == 2 && o.cats[0] == 19 && o.cats[1] == 20, "claimed indices");
        // The fingerprints, and this check exists because its absence was a real
        // bug: the field was added to the struct and forgotten in the codec, so
        // the sender filled it, the wire carried zeros, and the receiver reported
        // a healthy split as "the two runs have already diverged". The compiler
        // had nothing to say -- a struct field nobody serializes is legal.
        check(o.fp[0] == 0x03b4ff86 && o.fp[1] == 0xba5139f8,
              "and WHAT those cats are -- the ability fingerprints");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        // An empty claim is legal: a pure observer claims nothing, and the
        // peer must be able to tell that apart from a malformed frame.
        ControlMsg c{}; c.humans = 2; c.count = 0;
        uint32_t n = enc_control(buf, sizeof(buf), c);
        Reader r(buf, n); r.u8v();
        ControlMsg o{}; o.count = 7;
        check(dec_control(r, o) && o.count == 0, "an empty claim decodes as empty");
    }
    {
        ControlMsg c{}; c.humans = 1; c.count = 1; c.cats[0] = 5;
        uint32_t n = enc_control(buf, sizeof(buf), c);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            ControlMsg o{};
            dec_control(r, o);          // must not crash
        }
        check(true, "truncated CONTROL frames parse without faulting");
        Reader r(buf, n - 1); r.u8v();
        ControlMsg o{};
        check(!dec_control(r, o), "one byte short is rejected");
    }

    printf("\n-- AimMsg carries a SLOT, never an ability pointer --\n");
    {
        AimMsg a{};
        a.battle_id = 0xABCDEF0123456789ull;
        a.cat = 13; a.active = 1; a.slot_kind = 4; a.slot_index = 2;
        a.tx = 6; a.ty = -3; a.dx = -1; a.dy = 0;
        strcpy(a.gon, "FireballSpell");
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_AIM, "message type is AIM");
        AimMsg o{};
        check(dec_aim(r, o), "decodes");
        check(o.battle_id == a.battle_id, "battle id survives all 64 bits");
        check(o.cat == 13 && o.active == 1, "cat and active");
        check(o.slot_kind == 4 && o.slot_index == 2, "the slot, which is the identity");
        check(o.tx == 6 && o.ty == -3, "target, including a negative coordinate");
        check(o.dx == -1 && o.dy == 0, "direction");
        check(strcmp(o.gon, "FireballSpell") == 0, "the GON name, the second identity");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        // The tile reaches a game function that indexes the tactics grid, the
        // same hazard CURSOR's does, so it is refused at the decoder.
        AimMsg a{}; a.active = 1; a.tx = 999999;
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        Reader r(buf, n); r.u8v();
        AimMsg o{};
        check(!dec_aim(r, o), "an out-of-range target is rejected");
    }
    {
        // A direction is one step, never a vector across the board. Anything
        // else means the fields moved.
        AimMsg a{}; a.active = 1; a.dx = 50;
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        Reader r(buf, n); r.u8v();
        AimMsg o{};
        check(!dec_aim(r, o), "an implausible direction is rejected");
    }
    {
        AimMsg a{}; a.cat = 1; a.active = 1; a.tx = 2; a.ty = 3;
        strcpy(a.gon, "DefaultMove");
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            AimMsg o{};
            dec_aim(r, o);              // must not fault at any truncation
        }
        check(true, "truncated AIM frames parse without faulting");
    }

    printf("\n-- CursorMsg is a tile, not a pixel --\n");
    {
        CursorMsg c{};
        c.battle_id = 7; c.x = 11; c.y = 4; c.on_board = 1; c.owns_turn = 1;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_CURSOR, "message type is CURSOR");
        CursorMsg o{};
        check(dec_cursor(r, o), "decodes");
        check(o.battle_id == 7, "battle id");
        check(o.x == 11 && o.y == 4, "tile coordinates");
        check(o.on_board == 1 && o.owns_turn == 1, "both flags");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        // Off the board is the common case -- reading a tooltip, moving to the
        // end-turn button -- and must survive whatever stale tile is still in
        // the coordinate fields, including a negative one.
        CursorMsg c{}; c.x = -1; c.y = -1; c.on_board = 0;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        Reader r(buf, n); r.u8v();
        CursorMsg o{}; o.on_board = 1;
        check(dec_cursor(r, o), "an off-board cursor decodes");
        check(o.on_board == 0, "and stays off-board");
    }
    {
        // The tile reaches an array subscript inside the game, so a nonsense
        // one is refused at the decoder rather than at the draw call.
        CursorMsg c{}; c.x = 999999; c.y = 0; c.on_board = 1;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        Reader r(buf, n); r.u8v();
        CursorMsg o{};
        check(!dec_cursor(r, o), "an out-of-range tile is rejected");
    }
    {
        CursorMsg c{}; c.battle_id = 1; c.x = 2; c.y = 3; c.on_board = 1;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            CursorMsg o{};
            dec_cursor(r, o);           // must not crash
        }
        check(true, "truncated CURSOR frames parse without faulting");
        Reader r(buf, n - 1); r.u8v();
        CursorMsg o{};
        check(!dec_cursor(r, o), "one byte short is rejected");
    }

    printf("\n-- EnterNodeMsg carries the node seed --\n");
    {
        // seed0 is the field that matters: EnterNode copies MapNode+0x118
        // into TLS+0x178, so a mismatch here IS the battle starting from a
        // different RNG state. It must survive the high bit intact.
        EnterNodeMsg m{};
        m.index = 12; m.node_count = 40; m.type = 5;
        m.seed0 = 0xF7E6D5C4B3A29180ULL;
        // The three appended words, with a zero among them on purpose: a field
        // that is present but EMPTY has to survive too, because "the host could
        // not read the full 32 bytes" is a state the adoption path keys on.
        m.seed[1] = 0x1122334455667788ULL;
        m.seed[2] = 0;
        m.seed[3] = 0xDEADBEEFCAFEF00DULL;
        uint32_t n = enc_enter_node(buf, sizeof(buf), m);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_ENTERNODE, "message type is ENTERNODE");
        EnterNodeMsg o{};
        check(dec_enter_node(r, o), "decodes");
        check(o.index == 12 && o.node_count == 40, "index and map size");
        check(o.type == 5, "node type (5 = battle)");
        check(o.seed0 == 0xF7E6D5C4B3A29180ULL, "seed survives the high bit");
        // seed[0] is not on the wire -- seed0 is -- and the decoder makes the two
        // agree, so every reader can use whichever it has to hand.
        check(o.seed[0] == o.seed0, "word 0 mirrors seed0 after decoding");
        check(o.seed[1] == 0x1122334455667788ULL && o.seed[2] == 0 &&
              o.seed[3] == 0xDEADBEEFCAFEF00DULL,
              "the appended three words round-trip, a zero included");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        EnterNodeMsg m{}; m.index = 1; m.seed0 = 1;
        uint32_t n = enc_enter_node(buf, sizeof(buf), m);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            EnterNodeMsg o{};
            dec_enter_node(r, o);
        }
        check(true, "truncated ENTERNODE frames parse without faulting");
        Reader r(buf, n - 1); r.u8v();
        EnterNodeMsg o{};
        check(!dec_enter_node(r, o), "one byte short is rejected");
    }

    printf("\n-- Node cat familiar membership --\n");
    {
        EnterNodeMsg m{}; m.seed0 = 1; m.familiar_count = 2;
        m.familiar_ids[0] = 0x112233440000031dULL;
        m.familiar_ids[1] = 0x998877660000031eULL;
        uint8_t frame[1024]{};
        const uint32_t n = enc_enter_node(frame, sizeof(frame), m);
        check(n != 0, "familiar list encodes");
        Reader r(frame, n); r.u8v(); EnterNodeMsg o{};
        check(dec_enter_node(r, o) && o.familiar_count == 2 &&
              o.familiar_ids[0] == m.familiar_ids[0] && o.familiar_ids[1] == m.familiar_ids[1],
              "familiar IDs retain all 64 bits and order");
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader short_frame(frame, cut); short_frame.u8v(); EnterNodeMsg truncated{};
            check(!dec_enter_node(short_frame, truncated), "every truncated familiar frame fails");
        }
        m.familiar_ids[1] = m.familiar_ids[0];
        check(!enc_enter_node(frame, sizeof(frame), m), "duplicate familiar IDs rejected");
        m.familiar_ids[1] = 0;
        check(!enc_enter_node(frame, sizeof(frame), m), "zero familiar ID rejected");
        m.familiar_ids[1] = UINT64_MAX;
        check(!enc_enter_node(frame, sizeof(frame), m), "sentinel familiar ID rejected");
        m.familiar_count = 65;
        check(!enc_enter_node(frame, sizeof(frame), m), "oversized familiar list rejected");
        m.familiar_count = 64;
        for (uint32_t i = 0; i < 64; ++i) m.familiar_ids[i] = i + 1;
        check(enc_enter_node(frame, sizeof(frame), m) > 512, "maximum list fits enlarged transport buffer");
        m.familiar_count = 2;
        const uint32_t valid_size = enc_enter_node(frame, sizeof(frame), m);
        uint32_t invalid_count = 65;
        std::memcpy(frame + 45, &invalid_count, 4);
        Reader too_many(frame, valid_size); too_many.u8v();
        check(!dec_enter_node(too_many, o), "decoder rejects excessive familiar count");
        invalid_count = 2; std::memcpy(frame + 45, &invalid_count, 4);
        std::memcpy(frame + 57, frame + 49, 8);
        Reader duplicate(frame, valid_size); duplicate.u8v();
        check(!dec_enter_node(duplicate, o), "decoder rejects duplicate familiar IDs");
        uint64_t invalid_id = 0; std::memcpy(frame + 49, &invalid_id, 8);
        Reader zero_id(frame, valid_size); zero_id.u8v();
        check(!dec_enter_node(zero_id, o), "decoder rejects zero familiar ID");
    }

    printf("\n-- ChoiceMsg: the message that replaces RUNSTATE --\n");
    {
        // The whole meta layer rests on this being right: both peers compute
        // the same event effects from the same node seed, so the ONLY thing
        // that crosses the wire is which option a person picked. Get the index
        // wrong and the client applies a different outcome silently.
        ChoiceMsg m{};
        m.kind = kChoiceLevelUp; m.index = 2; m.count = 4; m.aux = 7;
        m.node_seed = 0x99AF76329BA60F33ull;
        m.cat_id = 0xFEDCBA9876543210ULL;
        m.cat_seed = 0x89ABCDEF01234567ULL;
        m.level_step = UINT32_MAX;
        strcpy_s(m.name, "FurySwipes");
        uint32_t n = enc_choice(buf, sizeof(buf), m);
        check(n == 44 + strlen(m.name), "encodes the event-page identity suffix");
        Reader r(buf, n);
        check(r.u8v() == MSG_CHOICE, "message type is CHOICE");
        ChoiceMsg o{};
        check(dec_choice(r, o), "decodes");
        check(o.kind == kChoiceLevelUp, "kind");
        check(o.index == 2 && o.count == 4, "index and option count");
        check(o.aux == 7, "the level-up option type rides in aux");
        check(strcmp(o.name, "FurySwipes") == 0, "the name cross-check survives");
        // Proto 19. Without this the choice has no idea which node it is about,
        // and a held one surfaces on a later node's screen -- measured live on
        // 2026-08-25, where node 6's pick landed on node 3 and corrupted a run.
        check(o.node_seed == 0x99AF76329BA60F33ull, "the node seed survives");
        check(o.cat_id == m.cat_id, "the full CatData+0xC48 save key survives its high bits");
        check(o.cat_seed == m.cat_seed, "the CatData+0 seed survives its high bits");
        check(o.level_step == UINT32_MAX, "the maximum level step round-trips");
        check(r.pos == r.len, "consumed exactly the frame");
        // Pin the appended field order independently of dec_choice.
        Reader fields(buf, n); fields.pos = 23 + (uint32_t)strlen(m.name);
        check(fields.u64v() == m.cat_id && fields.u64v() == m.cat_seed &&
              fields.u32v() == m.level_step && fields.u8v() == 0 && fields.pos == fields.len,
              "wire suffix is cat identity and empty page name after the option name");
        check(kProtoVersion >= 44, "the party order in ENTERNODE requires protocol 44 or later");
    }
    {
        for (uint8_t kind : {kChapterReady, kChapterSelect}) {
            ChapterMsg m{};
            m.kind = kind; m.generation = 42;
            if (kind == kChapterSelect) { m.act = 3; m.difficulty = 4; }
            uint8_t bytes[11]{};
            check(enc_chapter(bytes, sizeof(bytes), m) == 11, "chapter control fits exact wire size");
            Reader r(bytes, sizeof(bytes)); r.u8v(); ChapterMsg copy{};
            check(dec_chapter(r, copy) && copy.generation == 42 && copy.act == m.act &&
                  copy.difficulty == m.difficulty, "chapter/ready fields round-trip");
            for (uint32_t cut = 1; cut < 11; ++cut) {
                Reader short_r(bytes, cut); short_r.u8v();
                check(!dec_chapter(short_r, copy), "truncated chapter command rejected");
            }
            m.generation = 0;
            check(!enc_chapter(bytes, sizeof(bytes), m), "chapter requires setup generation");
        }
        ChapterMsg bad{}; bad.generation = 1; bad.kind = kChapterSelect;
        check(!enc_chapter(buf, sizeof(buf), bad), "chapter zero rejected");
        bad.act = 4; check(!enc_chapter(buf, sizeof(buf), bad), "chapter four rejected");
        bad.act = 1; bad.difficulty = -1;
        check(!enc_chapter(buf, sizeof(buf), bad), "negative difficulty rejected");
        bad.difficulty = 1001;
        check(!enc_chapter(buf, sizeof(buf), bad), "excessive difficulty rejected");
        bad.kind = kChapterReady; bad.difficulty = 0;
        check(!enc_chapter(buf, sizeof(buf), bad), "ready cannot carry chapter selection");
    }
    {
        uint8_t cat0[3] = {1, 2, 3};
        uint8_t cat1[2] = {4, 5};
        SetupMsg m{};
        m.kind = kSetupResumeExport;
        m.checkpoint = 0x123456789abcdef0ull;
        m.count = 2;
        m.generation = 17;
        m.members=4;for(unsigned i=0;i<4;++i){m.peers[i]=(uint8_t)(i+1);m.requests[i]=101+i;}
        m.cats[0].id = 0x101;
        m.cats[0].size = sizeof(cat0);
        m.cats[0].hash = 0xAAAA;
        m.cats[0].data = cat0;
        m.cats[1].id = 0x202;
        m.cats[1].size = sizeof(cat1);
        m.cats[1].hash = 0xBBBB;
        m.cats[1].data = cat1;
        uint32_t n = enc_setup(buf, setup_frame_size(m), m);
        check(n > 0, "setup export encodes");
        check(n == setup_frame_size(m), "actual send allocation fits every cat header");
        Reader r(buf, n);
        check(r.u8v() == MSG_SETUP, "setup message type");
        SetupMsg o{};
        check(dec_setup(r, o), "setup export decodes");
        check(o.kind == kSetupResumeExport && o.count == 2 && o.generation == 17 && o.checkpoint == m.checkpoint,
              "setup header round-trips");
        check(o.members==4 && !memcmp(o.peers,m.peers,sizeof(m.peers)) && !memcmp(o.requests,m.requests,sizeof(m.requests)),"all participant identities and export generations round-trip");
        check(o.cats[0].id == m.cats[0].id && o.cats[0].size == 3 &&
              memcmp(o.cats[0].data, cat0, 3) == 0,
              "first setup cat bytes round-trip");
        check(o.cats[1].id == m.cats[1].id && o.cats[1].size == 2 &&
              memcmp(o.cats[1].data, cat1, 2) == 0,
              "second setup cat bytes round-trip");
        check(r.pos == r.len, "setup frame consumed exactly");
        free(o.cats[0].data);
        free(o.cats[1].data);
        for (uint32_t cut = 0; cut < n; ++cut) {
            Reader short_frame(buf, cut);
            if (short_frame.len) short_frame.u8v(); // position past MSG_SETUP
            SetupMsg bad{};
            check(!dec_setup(short_frame, bad), "truncated setup frame is rejected");
            for (uint8_t i = 0; i < kSetupMaxCats; ++i) free(bad.cats[i].data);
        }
    }
    {
        uint8_t payload[1024] = {};
        SetupMsg m{};
        m.count = kSetupMaxCats;
        for (uint8_t i = 0; i < m.count; ++i) {
            m.cats[i] = { uint64_t(i + 1), sizeof(payload), uint64_t(i), payload };
        }
        const uint32_t need = setup_frame_size(m);
        uint8_t* frame = (uint8_t*)malloc(need);
        check(need == 15 + 1 + 5 * kMaxPeers + kSetupMaxCats * (20 + sizeof(payload)), "setup frame includes membership and all cat metadata");
        check(enc_setup(frame, need, m) == need, "sixteen-cat send buffer encodes");
        check(enc_setup(frame, need - 1, m) == 0, "undersized sixteen-cat send fails");
        Reader r(frame, need); r.u8v();
        SetupMsg decoded{};
        check(dec_setup(r, decoded) && decoded.count == kSetupMaxCats, "sixteen-cat frame decodes");
        for (auto& c : decoded.cats) free(c.data);
        free(frame);
    }
    {
        // An event choice carries the stat key rather than a type, and kind 0
        // must round-trip as cleanly as kind 1.
        ChoiceMsg m{};
        m.kind = kChoiceEvent; m.index = 0; m.count = 2; m.node_seed = 0x1234;
        m.level_step = 0; strcpy_s(m.name, "con"); strcpy_s(m.event_name, "StacyMutant1");
        uint32_t n = enc_choice(buf, sizeof(buf), m);
        Reader r(buf, n); r.u8v();
        ChoiceMsg o{};
        o.cat_id = UINT64_MAX; o.cat_seed = UINT64_MAX; o.level_step = UINT32_MAX;
        check(dec_choice(r, o), "an event choice decodes");
        check(o.kind == kChoiceEvent && o.aux == 0, "kind 0, no aux");
        check(strcmp(o.name, "con") == 0, "the stat key survives");
        // Zero is a legitimate value meaning "the sender did not know which
        // node it was in", and the receiver must read it as unknown rather
        // than as a mismatch -- see apply_pending.
        check(o.node_seed == 0x1234, "event node seed round-trips");
        check(o.cat_id == 0 && o.cat_seed == 0 && o.level_step == 0 &&
              strcmp(o.event_name, "StacyMutant1") == 0,
              "event identity, page name and level step round-trip");
        check(r.pos == r.len, "event consumes the appended zero fields");
    }
    {
        for (uint32_t count : { 1u, 64u }) {
            ChoiceMsg m{};
            m.kind = kChoiceLevelUp; m.count = count; m.index = count - 1;
            m.node_seed = 1; m.cat_id = count == 1 ? 1 : UINT64_MAX - 1;
            uint32_t n = enc_choice(buf, sizeof(buf), m);
            Reader r(buf, n); r.u8v();
            ChoiceMsg o{}; o.cat_seed = 1; o.level_step = 1;
            check(dec_choice(r, o), "level-up accepts boundary counts and valid boundary ids");
            check(o.count == count && o.index == count - 1 && o.cat_id == m.cat_id,
                  "boundary count, index and full cat id round-trip");
            check(o.cat_seed == 0 && o.level_step == 0,
                  "zero cat seed and initial level step are legal");
            check(r.pos == r.len, "empty option name leaves appended fields aligned");
        }
    }
    {
        for (uint8_t kind : { kChoiceEvent, kChoiceLevelUp }) {
            ChoiceMsg m{}; m.kind = kind; m.index = 1; m.count = 3;
            m.node_seed = 1;
            if (kind == kChoiceLevelUp) {
                m.cat_id = 0xFEDCBA9876543210ULL;
                m.cat_seed = UINT64_MAX; m.level_step = 1;
            } else strcpy_s(m.event_name, "StacyMutant1");
            strcpy_s(m.name, "leave");
            uint32_t n = enc_choice(buf, sizeof(buf), m);
            Reader full(buf, n); full.u8v();
            ChoiceMsg decoded{};
            check(n > 0 && dec_choice(full, decoded), "complete CHOICE before truncation is valid");
            check(decoded.cat_seed == m.cat_seed && decoded.level_step == m.level_step,
                  "all-ones cat seed and consecutive level step are preserved");
            for (uint32_t cut = 0; cut < n; ++cut) {
                Reader r(buf, cut); r.u8v();
                ChoiceMsg o{};
                char what[80];
                sprintf_s(what, "CHOICE kind %u truncated to %u bytes is rejected",
                          (unsigned)kind, (unsigned)cut);
                check(!dec_choice(r, o), what);
            }
        }
    }
    {
        auto rejected = [&](const ChoiceMsg& m, const char* what) {
            uint32_t n = enc_choice(buf, sizeof(buf), m);
            Reader r(buf, n); r.u8v();
            ChoiceMsg o{};
            check(n > 0 && !dec_choice(r, o), what);
        };
        for (uint8_t kind : { kChoiceEvent, kChoiceLevelUp }) {
            ChoiceMsg valid{}; valid.kind = kind; valid.count = 4; valid.index = 2;
            valid.node_seed = 1;
            if (kind == kChoiceLevelUp) valid.cat_id = 1;
            for (uint8_t bad : { (uint8_t)2, (uint8_t)255 }) {
                ChoiceMsg m = valid; m.kind = bad;
                rejected(m, "unknown CHOICE kind is rejected");
            }
            for (uint32_t bad : { 0u, 65u, UINT32_MAX }) {
                ChoiceMsg m = valid; m.count = bad; m.index = 0;
                rejected(m, "zero or excessive CHOICE count is rejected");
            }
            for (uint32_t bad : { 4u, 5u, UINT32_MAX }) {
                ChoiceMsg m = valid; m.index = bad;
                rejected(m, "CHOICE index at or above count is rejected");
            }
            if (kind == kChoiceLevelUp) {
                ChoiceMsg m = valid; m.node_seed = 0;
                rejected(m, "level-up with an unknown node is rejected");
                for (uint64_t bad : { uint64_t(0), UINT64_MAX }) {
                    m = valid; m.cat_id = bad;
                    rejected(m, "level-up with a zero or sentinel cat id is rejected");
                }
            } else {
                ChoiceMsg m = valid; m.cat_id = 1;
                rejected(m, "event with a cat id is rejected");
                m = valid; m.cat_seed = 1;
                rejected(m, "event with a cat seed is rejected");
                m = valid; m.level_step = 1;
                rejected(m, "event with a level step is rejected");
            }
        }
    }

    printf("\n-- NodeHashMsg: the meta layer's per-node check --\n");
    {
        NodeHashMsg m{};
        m.node_seed = 0xABCAAC1E04A8C2C0ull; m.node_index = 6;
        m.point = kNodePointEvent;
        m.rng[0] = 1; m.rng[1] = 2; m.rng[2] = 3; m.rng[3] = 4;
        m.hist_hash = 0x1111222233334444ull;
        m.cats_hash = 0x5555666677778888ull;
        m.cat_count = 29;
        m.inv_hash  = 0x9999AAAABBBBCCCCull;
        strcpy_s(m.event, "data/events/alley_cat.gon");

        uint32_t n = enc_nodehash(buf, sizeof(buf), m);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_NODEHASH, "message type is NODEHASH");
        NodeHashMsg o{};
        check(dec_nodehash(r, o), "decodes");
        check(o.node_seed == m.node_seed, "the node seed is the identity");
        check(o.point == kNodePointEvent, "the sample point");
        // All four words, not just the first: the log prints rng[0] but the
        // COMPARISON is over the whole 32-byte state, so a codec that carried
        // one word would agree on streams that had genuinely diverged.
        check(o.rng[0] == 1 && o.rng[1] == 2 && o.rng[2] == 3 && o.rng[3] == 4,
              "all four words of the simulation stream survive");
        check(o.hist_hash == m.hist_hash, "the run-history hash");
        check(o.cats_hash == m.cats_hash && o.cat_count == 29, "the cat roster");
        check(o.inv_hash == m.inv_hash, "the inventory hash");
        check(strcmp(o.event, m.event) == 0, "the chosen event's name");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        NodeHashMsg m{}; m.node_seed = 7; m.point = kNodePointEnter;
        uint32_t n = enc_nodehash(buf, sizeof(buf), m);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            NodeHashMsg o{};
            dec_nodehash(r, o);
        }
        check(true, "truncated NODEHASH frames parse without faulting");
    }

    printf("\n-- EnterNodeMsg: the host's party order --\n");
    {
        EnterNodeMsg m{}; m.seed0 = 5; m.seed[0] = 5; m.familiar_count = 1; m.familiar_ids[0] = 0x71000001ull;
        m.party_count = 3; m.party_ids[0] = 0x70000002ull; m.party_ids[1] = 0x70000001ull; m.party_ids[2] = 0x70000003ull;
        uint8_t frame[1024];
        uint32_t n = enc_enter_node(frame, sizeof(frame), m);
        Reader r(frame, n); r.u8v();
        EnterNodeMsg o{};
        check(n && dec_enter_node(r, o) && o.party_count == 3 && o.party_ids[0] == 0x70000002ull &&
              o.party_ids[1] == 0x70000001ull && o.party_ids[2] == 0x70000003ull, "party ids keep their ORDER");
        // An empty party ("not known") round-trips; a frame that ends before the field is refused.
        EnterNodeMsg none = m; none.party_count = 0;
        n = enc_enter_node(frame, sizeof(frame), none);
        Reader re(frame, n); re.u8v();
        EnterNodeMsg eo{}; eo.party_count = 9;
        check(dec_enter_node(re, eo) && eo.party_count == 0 && eo.familiar_count == 1, "an unknown party (0) round-trips");
        Reader ro(frame, n - 1); ro.u8v();
        EnterNodeMsg oo{};
        check(!dec_enter_node(ro, oo), "a frame that ends before the party field is refused");
        EnterNodeMsg dup = m; dup.party_ids[1] = dup.party_ids[0];
        check(!enc_enter_node(frame, sizeof(frame), dup), "duplicate party ids are not encoded");
        EnterNodeMsg many = m; many.party_count = 17;
        check(!enc_enter_node(frame, sizeof(frame), many), "more than 16 party ids are not encoded");
    }

    printf("\n-- CatDigestMsg: one (id, hash, size) per cat --\n");
    {
        uint8_t big[2 + kCatDigestMax * 20];
        CatDigestMsg m{};
        m.count = (uint8_t)kCatDigestMax;
        for (uint32_t i = 0; i < kCatDigestMax; ++i) {
            m.ids[i] = 0x70000001ull + ((uint64_t)(i / 16) << 24) + i;
            m.hashes[i] = 0xF00DF00D00000000ull ^ (i * 0x9E3779B97F4A7C15ull);
            m.sizes[i] = 4000 + i;
        }
        uint32_t n = enc_catdigest(big, sizeof(big), m);
        check(n == sizeof(big), "a full digest fits exactly the sender's buffer");
        Reader r(big, n);
        check(r.u8v() == MSG_CATDIGEST, "message type is CATDIGEST");
        CatDigestMsg o{};
        check(dec_catdigest(r, o), "decodes");
        check(o.count == m.count && memcmp(o.ids, m.ids, sizeof(m.ids)) == 0 &&
              memcmp(o.hashes, m.hashes, sizeof(m.hashes)) == 0 &&
              memcmp(o.sizes, m.sizes, sizeof(m.sizes)) == 0, "every id, hash and size");
        bool refused = true;
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader t(big, cut); t.u8v();
            CatDigestMsg x{};
            if (dec_catdigest(t, x)) refused = false;
        }
        check(refused, "every truncated digest is refused");
        CatDigestMsg bad = m; bad.ids[3] = 0;
        check(enc_catdigest(big, sizeof(big), bad) == 0, "an id of 0 is not encoded");
        bad = m; bad.count = kCatDigestMax + 1;
        check(enc_catdigest(big, sizeof(big), bad) == 0, "more than 64 cats is not encoded");
        CatDigestMsg empty{};
        n = enc_catdigest(big, sizeof(big), empty);
        Reader e(big, n); e.u8v(); CatDigestMsg eo{}; eo.count = 9;
        check(n == 2 && dec_catdigest(e, eo) && eo.count == 0, "an empty digest round-trips");
    }

    printf("\n-- RunHistMsg: the used-event list --\n");
    {
        uint8_t body[64];
        for (uint32_t i = 0; i < sizeof(body); ++i) body[i] = (uint8_t)(i * 7 + 1);
        RunHistMsg m{};
        m.size = sizeof(body);
        m.data = body;
        m.hash = savefile_hash(body, sizeof(body));

        uint8_t big[256];
        uint32_t n = enc_runhist(big, sizeof(big), m);
        check(n > 0, "encodes");
        Reader r(big, n);
        check(r.u8v() == MSG_RUNHIST, "message type is RUNHIST");
        RunHistMsg o{};
        check(dec_runhist(r, o), "decodes");
        check(o.size == sizeof(body), "size");
        check(o.data != nullptr && memcmp(o.data, body, sizeof(body)) == 0,
              "the serialized bytes survive intact");
        check(o.hash == m.hash, "the hash rides along");
        free(o.data);

        // A zero-length or absurd length must be refused rather than turned
        // into an allocation -- same contract as dec_catdata.
        RunHistMsg bad{}; bad.size = 0; bad.data = body;
        check(enc_runhist(big, sizeof(big), bad) == 0, "a zero-length history is refused");
    }

    printf("\n-- StateDumpMsg: the desync dump --\n");
    {
        // 40 records of 26 bytes is not the real CatState; the point is that
        // the codec carries whatever the sender packs and refuses anything
        // whose count, stride and size do not agree with each other.
        const uint32_t kCount = 40, kStride = 26;
        uint8_t body[kCount * kStride];
        for (uint32_t i = 0; i < sizeof(body); ++i) body[i] = (uint8_t)(i * 11 + 3);

        StateDumpMsg m{};
        m.battle_id = 0xdeadbeefcafe1234ull;
        m.turn = 21; m.count = kCount; m.stride = kStride;
        m.size = kCount * kStride; m.data = body;

        uint8_t big[2048];
        uint32_t n = enc_statedump(big, sizeof(big), m);
        check(n > 0, "encodes");
        Reader r(big, n);
        check(r.u8v() == MSG_STATEDUMP, "message type is STATEDUMP");
        StateDumpMsg o{};
        check(dec_statedump(r, o), "decodes");
        check(o.battle_id == m.battle_id, "battle id");
        check(o.turn == 21, "the turn the table was hashed at");
        check(o.count == kCount && o.stride == kStride, "count and stride");
        check(o.data != nullptr && memcmp(o.data, body, sizeof(body)) == 0,
              "every cat record survives intact");
        free(o.data);

        // The check that stops two builds from confidently diffing nonsense:
        // a size that does not equal count * stride is malformed, not a hint.
        uint8_t bad[2048];
        memcpy(bad, big, n);
        bad[1 + 8 + 4 + 4 + 4] ^= 0x01;   // corrupt the low byte of `size`
        Reader rb(bad, n); rb.u8v();
        StateDumpMsg ob{};
        check(!dec_statedump(rb, ob), "size != count * stride is refused");
        check(ob.data == nullptr, "and it allocated nothing");

        StateDumpMsg empty{}; empty.data = body;
        check(enc_statedump(big, sizeof(big), empty) == 0,
              "an empty dump is refused rather than sent");
    }

    printf("\n-- a truncated frame fails, it does not read past the end --\n");
    {
        ActionMsg a{}; a.type = 2; a.turn = 9; strcpy_s(a.gon, "Spit");
        uint32_t n = enc_action(buf, sizeof(buf), a);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            ActionMsg b{};
            dec_action(r, b);          // must not crash; result is don't-care
        }
        check(true, "every truncation length parsed without faulting");

        Reader r(buf, n - 1); r.u8v();
        ActionMsg b{};
        check(!dec_action(r, b), "one byte short is rejected");
    }

    printf("\n-- an over-long string is truncated without desynchronising --\n");
    {
        // A 300-byte name is malformed input, not a transport error. The cursor
        // must still advance by the whole field or every later field shifts.
        Writer w(buf, sizeof(buf));
        w.u8v(MSG_ACTION);
        w.u64v(9);                      // battle_id, ahead of turn on the wire
        w.u32v(1); w.u8v(0); w.u8v(2); w.u8v(0); w.u8v(0); w.u8v(0); w.u8v(0);
        w.i32v(0); w.i32v(0); w.i32v(0); w.i32v(0);
        char big[200]; memset(big, 'x', sizeof(big)); big[199] = 0;
        w.str(big);                     // Writer clamps to 255
        w.u32v(0xFEEDFACE);             // a sentinel after the string

        Reader r(buf, w.len); r.u8v();
        ActionMsg a{};
        check(dec_action(r, a), "decodes");
        check(a.battle_id == 9 && a.turn == 1, "battle_id and turn survive ahead of it");
        check(strlen(a.gon) == sizeof(a.gon) - 1, "name truncated to the field");
        check(r.u32v() == 0xFEEDFACE, "the cursor still lands on the next field");
    }

    printf("\n-- SAVEFILE carries the bytes, and owns them on arrival --\n");
    {
        // A stand-in for the real thing: the shipped saves are sqlite3 files
        // that open with this literal, and the transfer is byte-for-byte, so
        // the header is the cheapest end-to-end sanity check there is.
        uint8_t save[4096];
        memcpy(save, "SQLite format 3", 16);
        for (size_t i = 16; i < sizeof(save); ++i) save[i] = (uint8_t)(i * 31u);

        SaveFileMsg out{};
        out.slot = 2;
        out.size = (uint32_t)sizeof(save);
        out.hash = savefile_hash(save, out.size);
        out.fresh = 1;
        strcpy(out.name, "steamcampaign03.sav");
        out.data = save;

        uint32_t cap = savefile_frame_size(out);
        uint8_t* frame = (uint8_t*)malloc(cap);
        uint32_t n = enc_savefile(frame, cap, out);
        check(n > out.size, "encodes to more than the payload");

        Reader r(frame, n);
        check(r.u8v() == MSG_SAVEFILE, "type byte");
        SaveFileMsg in{};
        check(dec_savefile(r, in), "decodes");
        check(in.slot == 2, "slot survives");
        check(in.size == out.size, "size survives");
        check(strcmp(in.name, "steamcampaign03.sav") == 0, "name survives");
        check(in.data != nullptr, "the decoder allocated the payload");
        check(in.data != save, "and it is a copy, not the sender's buffer");
        check(memcmp(in.data, save, out.size) == 0, "bytes survive intact");
        check(savefile_hash(in.data, in.size) == out.hash, "hash agrees with the sender's");
        check(in.fresh == 1, "fresh survives");
        free(in.data);
        free(frame);
    }

    // `fresh` sits between the name and the payload, so a wrong offset would
    // shift the whole blob. Both values are round-tripped rather than just the
    // interesting one: a decoder that ignored the byte and left the field at
    // its default would pass a test that only ever checked fresh = 0.
    printf("\n-- SAVEFILE's `fresh` byte round-trips both ways --\n");
    {
        uint8_t save[64];
        for (size_t i = 0; i < sizeof(save); ++i) save[i] = (uint8_t)(i + 7u);

        for (int want = 0; want <= 1; ++want) {
            SaveFileMsg out{};
            out.slot  = 1;
            out.size  = (uint32_t)sizeof(save);
            out.hash  = savefile_hash(save, out.size);
            out.fresh = (uint8_t)want;
            strcpy(out.name, "steamcampaign02.sav");
            out.data = save;

            uint32_t cap = savefile_frame_size(out);
            uint8_t* frame = (uint8_t*)malloc(cap);
            uint32_t n = enc_savefile(frame, cap, out);

            Reader r(frame, n);
            r.u8v();
            SaveFileMsg in{};
            check(dec_savefile(r, in), "decodes");
            check(in.fresh == (uint8_t)want, want ? "fresh = 1 survives"
                                                  : "fresh = 0 survives");
            check(memcmp(in.data, save, out.size) == 0,
                  "the payload is still aligned behind it");
            free(in.data);
            free(frame);
        }
    }

    // The other half of the same story as `fresh`: that field says the host is
    // STARTING a run, this message says it has left one. It carries nothing but
    // a diagnostic scene name, so the only thing worth pinning is that an empty
    // one is legal -- a host whose scene walk came back blank must still be able
    // to say it left.
    printf("\n-- HOSTLEFT round-trips, empty scene name included --\n");
    {
        const char* names[] = { "House", "MainMenu", "" };
        for (const char* want : names) {
            HostLeftMsg out{};
            strcpy(out.scene, want);

            uint8_t  frame[128];
            uint32_t n = enc_hostleft(frame, sizeof(frame), out);
            check(n > 0, "encodes");

            Reader r(frame, n);
            check(r.u8v() == MSG_HOSTLEFT, "type byte");
            HostLeftMsg in{};
            check(dec_hostleft(r, in), "decodes");
            check(strcmp(in.scene, want) == 0, "the scene name survives");
        }
    }

    printf("\n-- a truncated or oversized SAVEFILE allocates nothing --\n");
    {
        uint8_t payload[64] = {};
        SaveFileMsg out{};
        out.slot = 0; out.size = sizeof(payload);
        out.hash = savefile_hash(payload, out.size);
        strcpy(out.name, "steamcampaign01.sav");
        out.data = payload;

        uint8_t frame[512];
        uint32_t n = enc_savefile(frame, sizeof(frame), out);
        check(n != 0, "encodes");

        // Chop the last byte off: the header still parses, the body does not.
        Reader r(frame, n - 1); r.u8v();
        SaveFileMsg in{};
        check(!dec_savefile(r, in), "a short frame is rejected");
        check(in.data == nullptr, "and nothing is left allocated");

        // A length nobody could honour must be refused before the malloc.
        Writer w(frame, sizeof(frame));
        w.u8v(MSG_SAVEFILE);
        w.u32v(0); w.u32v(kMaxSaveBytes + 1); w.u64v(0); w.str("x.sav");
        Reader r2(frame, w.len); r2.u8v();
        SaveFileMsg in2{};
        check(!dec_savefile(r2, in2), "an oversized length is rejected");
        check(in2.data == nullptr, "and allocates nothing");

        // Zero-length is not a save file, and neither is a null one.
        SaveFileMsg empty{};
        empty.size = 0; empty.data = payload;
        check(enc_savefile(frame, sizeof(frame), empty) == 0, "enc refuses size 0");
        SaveFileMsg nodata{};
        nodata.size = 16; nodata.data = nullptr;
        check(enc_savefile(frame, sizeof(frame), nodata) == 0, "enc refuses a null payload");
    }

    printf("\n-- INVENTORY: three blobs, and an empty one is not an error --\n");
    {
        // The distinction that matters. dec_catdata treats size 0 as malformed
        // because a cat always serializes to something; an inventory bucket is
        // legitimately empty, and a decoder that rejected that would refuse to
        // sync a run whose trash can happened to be empty -- while an encoder
        // that dropped it would leave the client's old contents in place,
        // because the client APPLIES a bucket by clearing and repopulating.
        static uint8_t back[300], store[7];
        for (size_t i = 0; i < sizeof(back);  ++i) back[i]  = (uint8_t)(i * 5 + 1);
        for (size_t i = 0; i < sizeof(store); ++i) store[i] = (uint8_t)(0xA0 + i);

        InventoryMsg out{};
        out.coins = -3; out.food = 41; out.boxes = 0;   // negative: it is an int
        out.hash  = 0x0123456789ABCDEFull;
        out.size[0] = sizeof(back);  out.data[0] = back;
        out.size[1] = sizeof(store); out.data[1] = store;
        out.size[2] = 0;             out.data[2] = nullptr;   // empty trash

        static uint8_t frame[4096];
        uint32_t n = enc_inventory(frame, sizeof(frame), out);
        check(n != 0, "encodes");
        check(n <= inventory_frame_size(out), "fits the advertised frame size");

        Reader r(frame, n);
        check(r.u8v() == MSG_INVENTORY, "type byte");
        InventoryMsg in{};
        check(dec_inventory(r, in), "decodes");
        check(in.coins == -3 && in.food == 41 && in.boxes == 0, "scalars survive");
        check(in.hash == out.hash, "hash survives");
        check(in.size[0] == sizeof(back) && in.size[1] == sizeof(store), "sizes");
        check(in.size[2] == 0 && in.data[2] == nullptr, "an empty bucket stays empty");
        check(in.data[0] && memcmp(in.data[0], back, sizeof(back)) == 0, "backpack bytes");
        check(in.data[1] && memcmp(in.data[1], store, sizeof(store)) == 0, "storage bytes");
        check(r.pos == n, "consumes the whole frame");
        for (uint32_t i = 0; i < kInvBuckets; ++i) free(in.data[i]);

        // All three empty is a real inventory too -- a fresh run. Its own
        // buffer, so the truncation checks below still measure the full frame.
        static uint8_t frame_empty[128];
        InventoryMsg none{};
        uint32_t n2 = enc_inventory(frame_empty, sizeof(frame_empty), none);
        check(n2 != 0, "an entirely empty inventory still encodes");
        Reader r2(frame_empty, n2); r2.u8v();
        InventoryMsg in2{};
        check(dec_inventory(r2, in2), "and decodes");
        check(in2.size[0] == 0 && in2.size[1] == 0 && in2.size[2] == 0, "as empty");

        // Truncation and absurd lengths must be refused before any malloc.
        Reader r3(frame, n - 1); r3.u8v();
        InventoryMsg in3{};
        check(!dec_inventory(r3, in3), "a short frame is rejected");
        for (uint32_t i = 0; i < kInvBuckets; ++i)
            check(in3.data[i] == nullptr, "and leaves nothing allocated");

        Writer w(frame, sizeof(frame));
        w.u8v(MSG_INVENTORY);
        w.i32v(0); w.i32v(0); w.i32v(0); w.u64v(0);
        w.u32v(8); w.u32v(kMaxInvBytes + 1); w.u32v(0);
        Reader r4(frame, w.len); r4.u8v();
        InventoryMsg in4{};
        check(!dec_inventory(r4, in4), "an oversized bucket length is rejected");
        for (uint32_t i = 0; i < kInvBuckets; ++i)
            check(in4.data[i] == nullptr, "including the bucket before it");

        // A length with no bytes behind it is an encoder bug, not a wire state.
        InventoryMsg bad{};
        bad.size[0] = 16; bad.data[0] = nullptr;
        check(enc_inventory(frame, sizeof(frame), bad) == 0,
              "enc refuses a length with a null payload");
    }

    printf("\n-- the encoders refuse a buffer that is too small --\n");
    {
        uint8_t tiny[4];
        Hello h{};
        check(enc_hello(tiny, sizeof(tiny), h) == 0, "enc_hello returns 0, not garbage");
        ActionMsg a{}; a.type = 2;
        check(enc_action(tiny, sizeof(tiny), a) == 0, "enc_action returns 0");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
