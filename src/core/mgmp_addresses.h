// mgmp_addresses.h -- hook targets, pinned to one exact build.
//
//   Mewgenics.exe
//   SHA256      4127cd6a792ae528bca6f65a8873dd61789591937d87656c2b586a5e30eb77ea
//   imagebase   0x140000000
//   SizeOfImage 0x1574000
//
// Every RVA below was read out of Mewgenics.exe.i64 for that build. The 16-byte
// prologue signature is the guard: if the bytes at an RVA don't match, the build
// is not the one these addresses were derived from and we refuse to hook rather
// than splicing a jump into the middle of some unrelated function.
#pragma once

#include <cstdint>
#include <cstring>

namespace mgmp {

enum Target : int {
    T_InitSystems = 0,   // glaiel::ApplicationBase::initSystems()
    T_NextTurn,          // glaiel::TurnControl::NextTurn()
    T_GetChoice,         // glaiel::Brain::GetChoice() -> TurnAction (by value)
    T_DoAction,          // glaiel::Character::DoAction(TurnAction, bool)
    T_Trigger,           // glaiel::Ability::trigger(TurnAction)
    T_BeginTurn,         // glaiel::Character::BeginTurn(int)
    T_EndTurn,           // glaiel::Character::EndTurn()
    T_FrameBegin,        // glaiel::ApplicationBase::FrameBegin()  -- off by default
    T_ApplyAction,       // glaiel::TurnControl::ApplyTurnAction(TurnAction)

    // --- phase 2: the recorder -------------------------------------------
    T_QueueDecision,     // glaiel::TurnControl::QueueDecision(TurnAction)
    T_RandInt,           // randint(int, u64*)      -- hot
    T_RandFloat,         // randfloat(double, u64*) -- hot
    T_Rand2,             // rand2(double, u64*)     -- hot, draws twice
    T_RollChance,        // RollChance(double,double,u64*) -- the proc/crit gate

    // --- phase 5: the meta layer -----------------------------------------
    T_MapUpdate,         // glaiel::MapScreen::update()   -- the follow tick
    T_EnterNode,         // glaiel::MapScreen::EnterNode(MapScreen*, MapNode*)
    T_SaveSelUpdate,     // glaiel::SaveSelection::update() -- the auto-continue tick
    T_SaveSlotClick,     // SaveSelection "continue save slot N"
    T_MewDirectorInit,   // glaiel::MewDirector::init(std::string savefile)

    // --- the run inventory -----------------------------------------------
    // These two are the MewSaveFile blob accessors. They are hooked NOT to
    // observe the game but to divert our own calls: the inventory serializer
    // has no seam that hands us bytes, so we call the game's bucket driver and
    // intercept the store/load it makes at the bottom. See mgmp_invsync.h.
    T_SFStoreBlob,       // MewSaveFile::Store(key, ByteStream&) -- host capture
    T_SFLoadBlob,        // MewSaveFile::Load(key, ByteStream&)  -- client supply

    // --- peer cursors ----------------------------------------------------
    T_StatusMenuUpdate,  // glaiel::StatusMenu::update -- the battle HUD tick

    // --- the decision screens (choice replication) ------------------------
    // The meta layer's command boundaries. Both screens offer a 240-byte-stride
    // option array and commit through a single function, so one hook per screen
    // both CAPTURES the host's pick and SWALLOWS the client's.
    T_EventChoice,       // sub_140937F30 -- WorldEvent option commit
    T_EventUpdate,       // glaiel::WorldEvent::update (slot 10) -- the apply tick
    T_LevelSelect,       // glaiel::LevelUpScreen::select_option(LevelUpOption)
    T_LevelUpdate,       // glaiel::LevelUpScreen::update (slot 9) -- apply tick

    // --- the hooks that CHANGE the game rather than observing it ---------
    T_SaveScumPenalty,   // ApplySaveScumPenalty(RunState*, int) -- no-op'd
    T_TimeDelayTick,     // TimeDelayStatusApplication vtable slot 11 -- the
                         // dt countdown, converted to a turn countdown

    // --- QoL: grey the combat menu out on a cat this peer does not own -----
    T_CombatMenuUpdate,  // glaiel::CombatMenu::update -- the ability bar tick
    T_ButtonUpdate,      // glaiel::Button::update (slot 11) -- scoped to the
                         // buttons the tick above drives, see mgmp_combatlock.h
    T_UpdateDecision,    // glaiel::Brain::UpdateDecision -- freezes the aim
                         // preview's write to Character+0x388, which is
                         // simulation state that backstab damage reads.
    T_HighlightRefresh,  // sub_140151CE0 -- the status half of the ability
                         // highlight. Swallowed, and ONLY while this peer is
                         // drawing another player's aim. See mgmp_aim.h.
    T_SelectAct,         // ActSelectionScreen::SelectAct(int), before fade/map construction
    T_ChapterLower,
    T_ChapterRaise,
    T_EquipmentClick,   // InventoryItemButton native equip/unequip entry
    T_EndRunFinalize,   // native return-home settlement, after queued story scenes
    T_PostBattlePromote, // Post-battle familiar promotion (0x3B3D60) -- forces party to 4
    T_TryAbandon,        // PauseMenu::TryAbandonRun -- "Abandon Adventure" becomes a vote, see mgmp_abandon.h
    T_SaveScumWarn,      // PauseMenu's quit-mid-battle gate: Steven's warning. Skipped in multiplayer only.
    T_TryDepart,         // the House's departure box: "set off with these cats" -- the party-size limit, mgmp_room.h
    T_IsKitten,          // CatData::is_kitten -- "no" in a room once the run is under way (a kitten cannot fight)
    T_LoadChar,          // Character: fill a freshly made character from its definition -- enemy health / armor x2, mgmp_balance.h
    T_EndRunDefeat,      // MewDirector end of a LOST / abandoned run (0x3B4750) -- the ownership filter and the clones' return
    T_GainCat,           // the event effect gain_cat_familiar (0x929B80): a new cat joins the run -- ui.block_new_cats makes it do nothing
    T_EquipDone,         // the gear screen's done-closure (0x3CBFB0): the chapter page, or -- chapter 2 locked -- chapter 1 outright
    T_PropGetInt,        // the save-properties int getter (0x22C5E0): while a chapter map is generated, mapflag_* answer with the flags EVERY player has
    T_GenerateMap,       // MapScreen::generate_map (0x21CA60): the stream is re-seeded from the host's committed seed right at its entry, on every peer
    T_IsAbilityUnlocked, // MewSaveFile: is this ability available (0x232860) -- while a battle is built a client answers like the host
    T_IsPassiveUnlocked, // ... passive (0x232920)
    T_IsItemUnlocked,    // ... item (0x1402329E0)
    T_IsLevelUnlocked,   // ... level group (0x232AA0): which levels a battle may be built from
    T_IsBossAvailable,   // select_boss_level's predicate lambda (0x399B80): bool(closure, const GonObject& boss) -- locked_bosses
    T_SpawnSort,         // the battle's party sort (0x364760): CatData** (CatData** first, CatData** last) -- the order the party is placed in
    T_BattleBuild,       // the battle build (0x35AAF0): (builder, ?) -- terrain, statics, the party, enemies, pickups; in a room it starts from a shared stream
    T_LevelPick,         // the level picker (0x394450): std::string* (self, std::string* out, const std::string& kind, MapNode* node, bool) -- which level a battle uses
    T_BuildStatics,        // the build stage for the level's static objects (0x14035FFA0): in a room the shared stream is reset when it returns
    T_BuildTerrain,        // the build stage for the terrain (0x14035C070): in a room the shared stream is reset when it returns
    T_BuildProps,          // the build stage for props (0x14035E1F0): in a room the shared stream is reset when it returns
    T_BuildParty,          // the build stage for the player's party (0x14035C3E0): in a room the shared stream is reset when it returns
    T_BuildAllies,         // the build stage for allies (0x14035DCC0): in a room the shared stream is reset when it returns
    T_BuildExtra,          // the build stage for extra spawns (also called by a mid-battle spawner: only the build window is touched) (0x14035E5E0): in a room the shared stream is reset when it returns
    T_BuildEnemies,        // the build stage for the enemy list (0x14035D0B0): in a room the shared stream is reset when it returns
    T_RandomPool,          // WorldEvent `random_pool` handler (0x1409310B0): (event state, gon node) -- draws one weighted entry of an event outcome from the shared stream
    T_EvWeatherRoll,   // WorldEvent `weather_roll` handler (0x140935ED0): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvPoolLuck,   // WorldEvent `random_pool_consider_luck` handler (0x140931130): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvRandomChance,   // WorldEvent `random_chance` handler (0x140930750): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvReward,   // WorldEvent `reward` handler (0x140930860): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvDisorderPool,   // WorldEvent `gain_disorder_from_pool` handler (0x14092A640): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvLearnAbility,   // WorldEvent `learn_ability_from_pool` handler (0x140924AA0): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvLearnPassive,   // WorldEvent `learn_passive_from_pool` handler (0x140924DD0): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvMutSet,   // WorldEvent `random_mutation_from_set` handler (0x140932770): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvMut,   // WorldEvent `random_mutation` handler (0x140933270): draws from the shared stream -- the stream is reset from the node first (unlocks_event_roll)
    T_EvStatCheck,   // WorldEvent stat check of an option (0x14091BE20, `stat int` ...): ONE draw from the shared stream against the chosen cat's stat decides good/bad -- reset first (unlocks_event_roll)
    T_EvSetup,            // WorldEvent constructor (0x140913A90: this, event name, flag): the shared stream is reset first, so the event's own draws (difficulty, the subject cat) are the same on every peer
    T_PickRandom,         // the "random element of a vector" helper (0x1400AB770, 41 callers): hooked ONLY for the call that picks an event's subject cat (return address kRet_EventSubject)
    T_PropSetInt,         // the save-properties int setter (0x140938F40: std::string* name [destroyed by the callee], int value): increment/decrement_legacy_counter write through it -- seen inside an event window, so the answers follow the event's own writes
    T_HouseSave,          // the House state writer (0x1401E6810, `files.house_state`): it writes each House cat entity's OWN id (entity+0x80) -- the ids of cats swapped with their clones are put right first (mgmp_catsync)
    T_EvItemPick,         // the pool item picker (0x1408DEC40: pool, out, node, luck in xmm3) that get_item_from_pool & co. call: hooked ONLY for calls made by the WorldEvent code (return address in 0x9173A0-0x919752 / 0x91F5F0-0x9205F7) -- the stream is reset from the node first (unlocks_event_roll), so what the party_damage etc. before it drew on each peer's own party cannot change the pick (Baphomet, 2026-10-03)
    T_COUNT
};

struct TargetDesc {
    uint32_t    rva;
    const char* name;      // short tag used in the log
    const char* symbol;    // full symbol, for the startup banner
};

// Order must match enum Target.
static const TargetDesc kTargets[T_COUNT] = {
    { 0x009B5380, "INIT",     "glaiel::ApplicationBase::initSystems" },

    { 0x008E3C60, "NEXTTURN", "glaiel::TurnControl::NextTurn" },

    { 0x00138590, "CHOICE",   "glaiel::Brain::GetChoice" },

    { 0x0010E550, "DOACTION", "glaiel::Character::DoAction" },

    { 0x000320A0, "TRIGGER",  "glaiel::Ability::trigger" },

    { 0x0010A9B0, "BEGINTURN","glaiel::Character::BeginTurn" },

    { 0x0010BC60, "ENDTURN",  "glaiel::Character::EndTurn" },

    // Hot: one call per frame. Off by default, throttled by frame_log_every.
    // Kept because it is the cheapest end-to-end proof that the hook path works
    // without having to reach a battle, and because a frame fence is on the
    // roadmap anyway.
    { 0x009B7870, "FRAME",    "glaiel::ApplicationBase::FrameBegin" },

    // The command boundary. Every decision -- ability use, end turn, and the
    // std::function kind -- is queued by TurnControl::QueueDecision and lands
    // here exactly once. Character::DoAction only sees the ability case, which
    // is why an end-turn does not show up there.
    { 0x008E4DE0, "APPLY",    "glaiel::TurnControl::ApplyTurnAction" },

    // Where deferred reactions enter the ring. 54 callers, all of them passive/
    // status callbacks -- the return address recorded here names which one, and
    // a type-6 or type-7 that appears in one run and not another is the earliest
    // available desync signal.
    { 0x008E3870, "QUEUE",    "glaiel::TurnControl::QueueDecision" },

    // The three xoshiro256 entry points. These are the hot ones -- see
    // mgmp_rng.h for the ABI, the TLS global-stream filter, and the important
    // caveat that inlined draw sites are invisible to them.
    { 0x0094F340, "RANDINT",  "randint(int, u64*)" },

    { 0x001595A0, "RANDFLT",  "randfloat(double, u64*)" },

    { 0x0094F4C0, "RAND2",    "rand2(double, u64*)" },

    // glaiel::RollChance(double p, double scale, u64* state) -- THE proc/crit
    // gate, and the fourth RNG entry point. It calls rand2, which calls
    // randfloat, so one logical proc roll lands two EV_RNG records whose return
    // addresses are both INSIDE the RNG module: the passive that actually
    // rolled sits one frame further up and was invisible in every capture taken
    // before this hook existed. That is the real explanation for the
    // long-standing "combat takes no RNG" puzzle -- the draws were never
    // absent, they were misattributed.
    //
    // 38 call sites, all lockstep-critical: Character::ReceiveDamage__inner_0
    // (6), ChanceToBlockAndCounter, CounterAttack, ChanceToBackflip, Brittle,
    // Confusion, DejaVu, DelayedFury, Shop::roll_item, combat rewards.
    //
    // Takes NO draw when p >= 1.0, so stream position depends on which procs
    // were POSSIBLE, not merely which fired -- both peers must agree on that.
    { 0x0094F7E0, "ROLL",     "glaiel::RollChance(double,double,u64*)" },

    // glaiel::MapScreen::update -- virtual-only (0 code xrefs; its two data
    // xrefs are one vftable at 0x140F0D100 and .pdata), 0x1B9E bytes and
    // uniquely named, so not an ICF-folding candidate. Hooked for two reasons:
    // it is the only reliable source of a live MapScreen* on a peer that has
    // not clicked anything, and its tail is the safest place to inject the
    // host's node choice -- the native call arrives from a UI callback fired
    // inside this same update, so injecting here is a few frames later at the
    // same point in the frame rather than from an unrelated thread.
    { 0x0038F3A0, "MAPUPD",   "glaiel::MapScreen::update" },

    // glaiel::MapScreen::EnterNode(MapScreen*, MapNode*) -- THE META-LAYER
    // COMMAND BOUNDARY, and the reason the map half of phase 5 is tractable at
    // all. Exactly one code caller (a std::function _Do_call at 0x14039A5B0,
    // reached only from a UI vftable) and its only data xref is .pdata unwind
    // info, so it is not virtual: every map decision passes through here once,
    // the same shape that made TurnControl::ApplyTurnAction work.
    //
    // Its prologue also settles a question this project had only measured:
    //     movups xmm0, [rdi+118h] ; movups [TLS+0x178], xmm0
    //     movups xmm1, [rdi+128h] ; movups [TLS+0x188], xmm1
    // Every MapNode carries its own 32-byte xoshiro256 seed at +0x118 (written
    // by MapScreen::generate_map @ 0x14021C558), and entering the node copies
    // it into the simulation stream. THAT is why "entering a battle does not
    // re-seed" and why two peers that had drifted apart at the menu agreed by
    // the first turn boundary. The protocol therefore never sends a seed --
    // entering the same node is what makes the streams equal.
    //
    // Caveat worth knowing before trusting it: the FIRST thing it does is
    // SDL_GetScancodeFromKey(SDLK_LSHIFT) and, if shift is down, tail-call
    // sub_1403923F0 instead. That is local keyboard state inside a command
    // boundary, so a client holding shift takes a different path than the host.
    { 0x00391C20, "ENTERNODE","glaiel::MapScreen::EnterNode(MapScreen*,MapNode*)" },

    // glaiel::SaveSelection::update -- virtual (its only .rdata xref is slot 10
    // of the SaveSelection vftable at 0x140EDFD38; everything else is .pdata),
    // 0xD10 bytes and uniquely named, so not an ICF-folding candidate. It is
    // the only reliable source of a live SaveSelection* on a peer that has not
    // clicked anything, and its tail is where the client's slot choice is
    // injected -- the native click arrives from a Button callback fired inside
    // this same update, so injecting here is the same point in the frame.
    { 0x001BB780, "SAVESEL",  "glaiel::SaveSelection::update" },

    // sub_1401BCE90 -- THE SAVE-FILE COMMAND BOUNDARY, and the counterpart of
    // MapScreen::EnterNode one screen earlier:
    //
    //     ContinueSlot(SaveSelection* this, int slot, bool play_sound)
    //
    // Every route from the save-selection screen into a run passes through it.
    // Two callers, both Button callbacks (sub_1401BBA70 -- which plays
    // "SaveFile_Continue" and sets the slot's SWF state to _DEFAULT before
    // calling this -- and sub_1401BE840); its only data xref is .pdata unwind
    // info, so it is not virtual.
    //
    // What it does with the slot index is the useful part. It packs
    // {SaveSelection*, int slot} into the capture of the ContinueFile
    // transition lambda, and when that transition completes (sub_1401BFD10)
    // the lambda reads:
    //
    //     mov  rdi, [rcx+8]        ; the SaveSelection
    //     movsxd rbx, [rcx+10h]    ; the slot
    //     shl  rbx, 5              ; * sizeof(std::string)
    //     add  rbx, [rdi+38h]      ; + SaveSelection's name vector
    //     ...
    //     call sub_1401BD780       ; CreateMewDirector(scene, parent, &name)
    //
    // So SaveSelection+0x38 is a std::vector<std::string> of the slot
    // filenames -- "steamcampaign01.sav" and friends, assigned three at a time
    // by SaveSelection::init -- and MewDirector::init(std::string) is what
    // finally opens one. Calling this function with a slot index IS clicking
    // that slot, which is how the client is put on the host's save without a
    // human touching the screen.
    { 0x001BD8B0, "SAVESLOT", "glaiel::SaveSelection::ContinueSlot(int,bool)" },

    // glaiel::MewDirector::init(std::string) -- the function that finally opens
    // a save file, reached only as CreateMewDirector(scene, parent, &name) from
    // the ContinueFile transition. Hooked for ONE job on the client: to swap
    // which file is opened.
    //
    // WHY THAT IS SAFE, from the signature alone. The parameter is taken BY
    // VALUE, so under the MSVC x64 ABI it arrives as a hidden pointer that the
    // callee may read but does not own -- the caller destroys it. The game
    // proves this itself: sub_1401BD780 passes &names[slot], a live element of
    // the SaveSelection's own vector, and the SaveSelection's scene is marked
    // for destruction two instructions earlier. If init retained that pointer
    // it would dangle in the shipped game. So it copies, and a pointer to a
    // 32-byte std::string image on our own side is exactly as valid as the
    // pointer the game passes.
    //
    // That one substitution is what keeps the client's own saves intact: the
    // host's run is written to a filename the game otherwise never uses, and
    // this redirects the load to it, instead of overwriting whichever
    // steamcampaignNN.sav the host happened to be playing.
    { 0x003A6B90, "MEWDIR",   "glaiel::MewDirector::init(std::string)" },

    // sub_14022C4D0 -- MewSaveFile::Store(std::string key, ByteStream&). The
    // bottom of the inventory WRITE path:
    //
    //   sub_1402E10D0 (driver) -> sub_1402E15B0 (one bucket) ->
    //   sub_14022CBD0 (serialize the item vector into a fresh ByteStream) ->
    //   THIS (hand the finished blob to the sqlite "files" table)
    //
    // Hooked so that the host can call the game's own bucket serializer and
    // take the bytes at the bottom instead of letting them reach sqlite. The
    // blob is read straight off the ByteStream argument, which arrives in
    // WRITE mode from that path -- len at +0x0C, buffer at +0x10. (It also
    // accepts a read-mode stream, +0x24/+0x18, which is the branch the client
    // uses to push bytes back in if suppression is ever turned off.)
    //
    // NOTE it takes the key std::string BY VALUE and destroys it, exactly like
    // MewDirector::init takes its filename. Suppressing the call therefore
    // means destroying that string ourselves or leaking 32 bytes of the GAME's
    // heap per bucket per push -- which is what C_GameStrDtor is for.
    { 0x0022CF40, "SFSTORE",  "glaiel::MewSaveFile::Store(std::string,ByteStream&)" },

    // sub_14022C620 -- the exact mirror, and the bottom of the READ path:
    //
    //   sub_1402E1370 (driver) -> sub_1402E1740 (one bucket) ->
    //   sub_14022DDF0 (deserialize a ByteStream into item records) ->
    //   THIS (glaiel::SQLSaveFile::Retrieve into that ByteStream)
    //
    // Its success path is the template for what the client's supply hook does:
    //
    //   if (bs[+0x20]) free(bs[+0x18]);   // drop a read buffer it owned
    //   bs[+0x18] = block;  bs[+0x24] = len;  bs[+0x20] = 1;
    //   bs[+0x00] = 0;                    // mode <- READ
    //   bs[+0x28] = 0;                    // read pos (and write pos)
    //
    // We do the same with our own pointer and bs[+0x20] left at 0, so the game
    // reads our buffer and never frees it -- the borrowing trick mgmp_catsync
    // already relies on. A miss leaves the stream in write mode with length 0,
    // which sub_14022DDF0 correctly reads as "empty bucket", so supplying an
    // empty bucket needs nothing more than mode 0 and length 0.
    { 0x0022D090, "SFLOAD",   "glaiel::MewSaveFile::Load(std::string,ByteStream&)" },

    // sub_140817320 -- glaiel::StatusMenu::update, slot 11 of the StatusMenu
    // vftable at 0x141118CD8. THE BATTLE HUD TICK, and the only hook the peer
    // cursors need -- it is simultaneously where the local cursor is READ and
    // where the remote ones are DRAWN.
    //
    // Reading. StatusMenu caches the board tile under the mouse at +124 (an
    // unaligned qword, packed iVec2D) every frame, right after bounds-checking
    // it against the grid at +72 (width +184, height +188) and hiding both of
    // its own cursor pips when it falls outside. So the mouse -> isometric tile
    // projection never has to be reimplemented; the game does it and leaves the
    // answer in a field.
    //
    // Drawing. Its own two pips (RendererIso components at +80 and +88, built
    // by StatusMenu::init from the animations "GroundMouseCursorPip" and
    // "MouseCursorPip3D") are the local player's, one per peer is not an option
    // -- but update also submits IMMEDIATE-MODE pieces through the same
    // ImmediateModeGameUI that Brain::UpdateDecision draws its "target" cursor
    // on, and those cost nothing to add. See mgmp_cursor.h.
    //
    // Virtual-only and safe to splice: 14849 bytes, its sole .rdata xref is
    // that one vtable slot and everything else is .pdata unwind info, so it is
    // not ICF-folded with anything and is never called non-virtually.
    { 0x0081AEE0, "STATUSMENU", "glaiel::StatusMenu::update (slot 11)" },

    // sub_140937F30 -- THE WORLD-EVENT COMMAND BOUNDARY. Three statements, and
    // the middle one IS the choice:
    //
    //     *(u8*)(MewDirector + 1812) = 1;                 // a choice was made
    //     *(void**)(WorldEvent + 256) = capture[2];       // the chosen option
    //     return sub_14091AA00(WorldEvent);               // run it
    //
    // capture is {vftable, WorldEvent*, option entry*} and the entry is an
    // element of the 240-byte-stride array at WorldEvent+224..+232 -- so the
    // host's pick is an index, and injecting one is synthesising 24 bytes.
    //
    // Two xrefs: the single code caller inside setupActionChoice__inner_1, and
    // one _Func_impl vftable (it doubles as a lambda body). Splicing the
    // function catches both routes.
    //
    // NOTE the prologue is `mov rax, cs:qword_1413D1970` -- a 7-byte
    // RIP-relative load. MinHook relocates it into the trampoline correctly,
    // but it is the reason this signature does not look like the others.
    { 0x0093C1C0, "EVTCHOICE","glaiel::WorldEvent option commit (sub_140937F30)" },

    // glaiel::WorldEvent::update -- vtable slot 10, virtual-only (its only
    // .rdata xref is that slot). The client's apply tick: a CHOICE can arrive
    // before this peer's event screen exists, so it is held and applied from
    // here, where a live WorldEvent* is the `this` pointer.
    { 0x00916340, "EVTUPD",   "glaiel::WorldEvent::update (slot 10)" },

    // glaiel::LevelUpScreen::select_option(LevelUpOption) -- non-virtual, two
    // code callers, and BOTH of them are the button handlers
    // (sub_140386810 / sub_140386CE0), which resolve
    //     *(LevelUpScreen+864) + 240 * index
    // and copy-construct the option before calling in. Hooking here catches
    // both banks of buttons with one splice.
    //
    // The option arrives as a COPY, not a pointer into the array, so the host
    // recovers the index by matching content (type at +0, name at +200) rather
    // than by pointer arithmetic.
    { 0x003835D0, "LVLSELECT","glaiel::LevelUpScreen::select_option(LevelUpOption)" },

    // glaiel::LevelUpScreen::update -- vtable slot 9. LevelUpScreen has NO
    // update at the slot StatusMenu and WorldEvent use; slots 10/11/12 are all
    // the ICF-folded empty virtual, and slot 9 is the only per-frame body it
    // owns. Same job as EVTUPD: hold a CHOICE until there is a live screen.
    { 0x00383210, "LVLUPD",   "glaiel::LevelUpScreen::update (slot 9)" },


    // sub_1408DD9C0 -- the save-scum penalty. Not a guess: the ONLY write it
    // makes to the run object is `++*(u32*)(run+0xE0)`, the scum counter, and
    // every other reference to that object in its 4124 bytes is a read of that
    // same counter to pick an escalation tier (1, 2, 3, >=100). Everything else
    // it does is bump per-cat penalty fields and queue the Steven NPC scripts
    // ("steven_savescum_100", "steven_savescum_houseboss_100"). void return,
    // both call sites in MewDirector::ContinueAdventure, and its only data xref
    // is .pdata unwind info -- so it is not virtual and no-oping it is safe.
    //
    // OFF BY DEFAULT, and deliberately so: this is the first hook in the
    // harness that changes behaviour instead of watching it, and a capture
    // taken with it on is not a capture of the shipped game.
    { 0x008E1610, "SCUM",     "ApplySaveScumPenalty(RunState*, int)" },

    // glaiel::TimeDelayStatusApplication -- vtable slot 11 (the per-update
    // tick). This is the ONE place in the battle sim measured to advance on
    // wall-clock time rather than on turns:
    //
    //   mov  rax, [rcx+18h]            ; context
    //   movsd xmm0, [rax+10h]          ; frame delta-time
    //   mulsd xmm0, [rcx+30h]          ; * this->rate
    //   mov  rax, [rcx+28h]
    //   mulsd xmm0, [rax+28h]          ; * scene timescale
    //   movsd xmm1, [rcx+0F8h]         ; the countdown, seconds
    //   subsd xmm1, xmm0
    //   movsd [rcx+0F8h], xmm1
    //   comisd xmm1, xmm0
    //   jnb  epilogue                  ; >= 0 -> do nothing at all
    //                                  ; <  0 -> apply the statuses
    //
    // The countdown at +0xF8 is seeded from the GON key `delay` by slot 33
    // (0x0024F0A0), and the shipped values are fractional seconds -- .1, .25,
    // 1.13333, 3 -- so it cannot be read as a turn count. That makes it a real
    // desync vector: two peers at different frame rates cross zero on different
    // TURNS, and the effect lands at a different point in the action sequence.
    //
    // Blast radius is exactly this class. Both functions appear in precisely
    // one vtable slot each (0x140F22A38 = slot 11, 0x140F22AE8 = slot 33); the
    // only other xrefs are .pdata unwind records, so neither is called
    // non-virtually from anywhere else.
    //
    // Content using it is nearly nonexistent -- 4 uses across all 447 shipped
    // .gon files, and only two carry simulation payloads:
    //   AZ_LoseHead          (AstroZombie miniboss) delay 3      Cleanse+FullHeal
    //   DestroyerThrowShield (final boss)           delay 1.1333 FormChange
    //   MegaGuppy_SlamRight  (final boss)           delay .25    screen shake only
    //   DbgBackgroundTransitionTest (test content)  delay .1     spawn + music
    //
    // OFF BY DEFAULT: the second hook in the harness that changes the game.
    { 0x0024FC30, "TDELAY",   "glaiel::TimeDelayStatusApplication::update (slot 11)" },

    // sub_1402B12F0 -- glaiel::CombatMenu::update. THE ABILITY BAR'S TICK, and
    // the only place that knows both which cat the bar belongs to and which
    // Buttons it is made of:
    //
    //   CombatMenu+240/+248  std::vector<Button*>{begin,end}, stride 8. Every
    //                        option button AND the end-turn button; update
    //                        walks it, recomputes each button's state, and
    //                        calls the button's own update at the end of the
    //                        iteration ((*(vtbl+88))(button)).
    //   CombatMenu+280/+288  Ref<Character>{ptr, generation}, assigned by
    //                        CombatMenu::show's third argument through
    //                        sub_1400A1B30. The subject of the bar.
    //
    // Non-virtual as far as we care (it is reached through its own vtable slot
    // and nothing else calls it), 5375 bytes, so it is not ICF-folded.
    { 0x002B1DC0, "COMBATMENU", "glaiel::CombatMenu::update" },

    // sub_140975F00 -- glaiel::Button::update, slot 11 of the glaiel::Button
    // vftable at 0x140ED9E88 and the ONLY vtable it appears in (its four other
    // .rdata references are unaligned relocation noise, not slots), so it is
    // neither ICF-folded nor shadowed by a subclass override.
    //
    // Why this is hooked at all, rather than writing the button state from the
    // CombatMenu hook: the state field is recomputed by CombatMenu::update
    // BEFORE it calls each button's update, and it is the button's update that
    // turns that state into a SWF frame. A write made after CombatMenu::update
    // returns is therefore undone on the next tick before it can ever be drawn
    // -- CombatMenu::update lifts a disabled button back out of state 4 with
    // sub_1409767B0(button, true) and the bar renders "up" every frame. The
    // only point that sits between the recompute and the draw is the entry to
    // this function.
    { 0x0097E330, "BUTTON",    "glaiel::Button::update (slot 11)" },

    // The aim preview turns the acting cat toward where it is being aimed, and
    // it does that by writing the SAME field the simulation reads. Exactly one
    // Character::Face call sits inside this function, at 0x1401377C9:
    //
    //     cmp    dword ptr [rdi+220h], 2      ; a type-2 decision is cached
    //     jnz    skip
    //     comisd xmm7, qword ptr [rdi+2A8h]   ; a WALL-CLOCK dt timer
    //     jnb    skip                         ; already elapsed -> no Face
    //     mov    rdx, [rdi+238h]              ; the pending decision's direction
    //     mov    rcx, [rdi+38h]               ; Brain+0x38 = the Character
    //     call   glaiel::Character::Face
    //
    // Two peers at different frame rates land on opposite sides of that `jnb`,
    // so the field diverges by frame rate alone -- and `sub_14011C6F0`, the
    // backstab test called from Character::receive_damage_passives, reads
    // Character+0x388 against the incoming direction. Facing therefore decides
    // DAMAGE. Measured 2026-08-26: 49 turns of byte-identical hashes, then one
    // melee that landed for 12 on the host and 9 on the client with the rng
    // hash identical, the two peers' cat 13 differing only in facing.
    //
    // Brain+0x38 is read twice over here: this site, and 0x1401377D3 taking
    // [rdi+38h] -> +0x60 -> +0x48, which is Character -> TacticsObject -> tile.
    { 0x00137EF0, "PREVIEWFACE", "glaiel::Brain::UpdateDecision" },

    // sub_140151CE0 -- the status half of the ability highlight, and the reason
    // sub_140138A10 is dangerous to call one-sidedly. Four call sites inside
    // it (0x140138C7A, D4A, E3A, E9A), each preceded by a loop that writes
    // `[obj+0x118]`; on a flag change it calls glaiel::apply_status on a real
    // Character* and pushes the returned Status* onto a vector.
    //
    // The hook is a SUPPRESSOR, not an observer, and it is scoped to a few
    // instructions: it returns 0 without calling the original only while
    // mgmp_aim has the highlight guard raised, which happens only around the
    // draw of ANOTHER player's aim, on the peer that does not own that cat.
    // Outside that window it is one predictable branch on a global.
    { 0x00152700, "HILITE",    "ability highlight status refresh (sub_140151CE0)" },
    { 0x00069A70, "SELECTACT", "glaiel::ActSelectionScreen::SelectAct(int)" },
    { 0x0006A260, "ACTLOWER", "chapter difficulty lower callback" },
    { 0x0006A0A0, "ACTRAISE", "chapter difficulty raise callback" },
    { 0x0034EB20, "EQUIPCLICK", "InventoryItemButton::click" },
    { 0x003B5430, "ENDFINAL", "MewDirector return-home settlement" },
    { 0x003B3D60, "PROMOTE", "Post-battle familiar promotion to party" },
    // The pause menu's "Abandon Adventure" (Button_PauseMenu_GiveUp) callback -- found 2026-09-30 by the
    // button's name string: SetupMainSidebar's lambda calls this, and it does nothing but build the
    // confirmation popup ({POPUP_TRY_ABANDON_RUN} / {POPUP_TRY_ABANDON_HOUSEBOSS} / the tutorial text) whose
    // OK callback is the abandon itself. Swallowing it swallows the click.
    { 0x00296FB0, "TRYABANDON", "PauseMenu::TryAbandonRun" },

    // sub_1402972A0 -- bool(PauseMenu*, std::function<void()> on_quit BY VALUE), found 2026-10-01. The three
    // quit callbacks of the pause menu (to menu / to desktop) go through it. It runs `on_quit` at once and
    // returns true when MewDirector+0x710 (where a save-scum would count: 1 battle, 2/3 the house boss) is 0
    // or MewDirector+0x714 (it counts right now) is 0; otherwise it builds Steven's warning popup
    // ({POPUP_TRY_SAVESCUM_WARN...}, by the scum counter RunState+0xE0) whose OK both runs `on_quit` and
    // writes the save property `savescumlocation` (sub_1403BBE60) -- the property that makes the next load
    // (0x1403BD063) call ApplySaveScumPenalty and queue Steven's scenes. When Steven has not been met yet it
    // writes the property and quits without asking. So with +0x714 at 0 there is neither a warning nor a
    // penalty to come -- which is how mgmp_hooks skips it in multiplayer.
    { 0x002972A0, "SCUMWARN", "PauseMenu save-scum quit warning" },

    // sub_1400ADBF0 -- bool(DepartureBox*), found 2026-10-01. The House's two door signs share one callback
    // (0x1401F8EA0): it calls this, and when it returns FALSE it ENDS THE DAY instead (0x1401EF470). This
    // gathers the party from the box's four slots -- *(box+0x100) is an array of four slot pointers, null
    // when empty, each holding the cat's id at +0x80 -- returns false for an empty box, and otherwise sets
    // off (MewDirector::set_party_and_go, 0x1403B13F0, writes the ids into +1464/+1480). So a departure is
    // refused by returning TRUE without calling it: handled, nobody left, and the day is not ended.
    { 0x000ADBF0, "TRYDEPART", "House departure box: set off" },

    // sub_1400D3130 -- bool(CatData*), "is this cat a kitten", found 2026-10-01 in the 3-player halt (two thieves
    // had stats 2 lower on the host than on the clients from byte-identical cat data, measured live by calling
    // CatData::compute_stats inside both processes). compute_stats ends with
    //     if (is_kitten(cat) && !arg5) every stat -= 2         (0x1400C1761..0x1400C178A)
    // and is_kitten is true for a passive-derived reason (sub_1400BF530) or when
    //     MewDirector+0x580 (the CURRENT DAY) - CatData+0xC38 (birth day)  <= 1
    // (CatData+0xC40, when not -1, replaces the current day). The current day is the player's own save's day --
    // 178 on the host, 180 on both clients in that run -- so a cat born on day 179 was a kitten on one peer and an
    // adult on the others: -2 CON = 8 HP less, and the turn-0 state hash halted. mgmp_hooks answers "no" in a room
    // whenever the player is in the run itself (a kitten cannot be taken out; the preparation pages keep the game's
    // own answer).
    { 0x000D3130, "ISKITTEN", "CatData::is_kitten" },

    // sub_1400F70F0 -- void(Character*, name, bool, bool), found 2026-10-01: fills a new Character from its
    // gon definition (23 KB). It is the ONLY place the definition's numbers come in (the one other body that
    // fills a character, sub_1400FCB50, is for a player's own cat and reads CatData). Reached through
    // sub_1400F6DF0 <- the character factory sub_140366630 <- the battle's spawn list (0x14035D0B0), and
    // again for a form change / transformation (sub_1408D3C20 builds the NEXT phase through the factory, so
    // every phase of a boss comes through here). What it writes, in the order that matters here:
    //     +0x350  faction (string -> enum: none 0, allies 1, enemies 2, ... cavemen 8, sabertooths 9, mammoths 10)
    //     +0x5A8  constitution from the definition
    //     +0x5E8  "health" - 4*constitution          (so max health = 4*CON + +0x5E8, see recompute_stats)
    //     +0x5EC  the same number again -- the BASE that +0x5E8 is rebuilt from whenever passives change
    //     +0x4B4  "shield" (the armor the enemy starts with), times the difficulty multiplier at +0xCE8 when
    //             the character is flagged (+0xCE0) -- the game's own boss_health_multiplier path
    //     +0x4B0  only when the definition has "initial_health"; otherwise the character is made with a
    //             huge sentinel and the first recompute clamps it to the maximum
    // mgmp_hooks calls the original and then, in a room, multiplies an enemy's health, maximum and armor.
    { 0x000F70F0, "LOADCHAR", "Character: fill from definition" },

    // sub_1403B4750 -- void(MewDirector*), found 2026-10-01 (review finding F2). The END OF A RUN THAT WAS NOT
    // WON: a lost battle (0x364B50 calls it) and the pause menu's abandon (its "defeat" transition ends in the
    // same lambda). It is the twin of ENDFINAL (0x3B5430): it walks the same three lists -- party (+0x5C0, count
    // +0x5BC), familiars (+0x648, +0x644) and the death records (+0x5E0, +0x5DC, 24-byte entries) -- ORs 0x10 / 0x20
    // into each cat's +0xBF8, calls 0xD2D40 (which clears the on-adventure bit 0x80000) and then clears the run's
    // properties. ENDFINAL was hooked to keep a peer's session copies out of that walk; this one was not, so a lost
    // or abandoned run settled the OTHER players' copies in every registry too.
    { 0x003B4750, "ENDDEFEAT", "MewDirector end of a lost/abandoned run" },
    // The event effect interpreter (0x9173A0) compares the effect name and calls this for gain_cat_familiar: it builds a cat and files
    // its id in the party (or the familiars when the party is full). rcx = the event context.
    { 0x00929B80, "GAINCAT", "event effect gain_cat_familiar" },
    // MewDirector::DoHouseStorageEquipFlow (0x3B0BB0) opens the gear screen ("StorageItems") with a closure that runs when the player locks
    // it in: if mapflag_DesertUnlocked is set it makes the chapter page ("ActSelection"), otherwise it starts "alley.gon" -- chapter 1 --
    // straight away. The closure is a std::function body: rcx = the closure, +0x8 = that flag, +0x10 = the director.
    { 0x003CBFB0, "EQUIPDONE", "gear screen done: chapter page or chapter 1" },
    // MewSaveFile property getter: int64(props, std::string key BY VALUE, int64 fallback). The chapter map's nodes (hard path, the sewers and
    // junkyard exits, ...) are locked or open by the mapflag_* properties of the save that generates it.
    { 0x0022C5E0, "PROPGETINT", "save properties: integer getter" },
    // MapScreen::init (0x38D150) calls it once; it retries try_generate_map (0x21D880) until a map is accepted, and writes every node's 32-byte seed
    // (MapNode+0x118) from the shared stream. Anything that draws from that stream between the chapter commit and this call (the two peers reach it
    // by different routes when one has no chapter page, and after a fade of a different length) shifts every seed.
    { 0x0021CA60, "GENMAP", "MapScreen::generate_map" },
    // MewSaveFile "is X unlocked" checks: bool(MewSaveFile*, std::string name BY VALUE). Debug unlock byte -> true; the name is in the unlocked set ->
    // true; the name is on locked_content.gon's blacklist -> false; otherwise true. The four differ only in which two sets they read.
    { 0x00232860, "ISABILITY", "MewSaveFile: ability unlocked?" },
    { 0x00232920, "ISPASSIVE", "MewSaveFile: passive unlocked?" },
    { 0x002329E0, "ISITEM", "MewSaveFile: item unlocked?" },
    { 0x00232AA0, "ISLEVEL", "MewSaveFile: level group unlocked?" },
    // lambda_1 of MapScreen::select_boss_level (a std::function body): rdx = the boss's GonObject, its name a std::string at +0x88; the closure is unused.
    // Debug byte -> true; name in the unlocked-bosses set (save +0x2A0, rebuilt from the vector at +0x158) -> true; on the blacklist (+0x428) -> false; else true.
    { 0x00399B80, "ISBOSS", "select_boss_level: boss available?" },
    // sub_14035C3E0 (the battle's player spawn) lists the party's CatData* in party order (sub_1403B2F30) and, for up to 32, insertion-sorts
    // them with this (its only caller): key = the cat's computed stats (sub_1400C1820) +0x10, SPEED, highest first; ties keep the party order.
    { 0x00364760, "SPAWNSORT", "battle: sort the party for placement" },
    // sub_14035AAF0, called once per battle (from 0x140897BC0): reads the difficulty (sub_1403B94F0), then 0x35FFA0 / 0x35C070 / 0x35E1F0, the
    // player spawn 0x35C3E0, 0x35DCC0 / 0x35E5E0, the enemy list 0x35D0B0 ... -- every draw that places the board happens inside it.
    { 0x0035AAF0, "BATTLEBUILD", "battle: build the board" },
    // sub_140394450: picks the level a battle node is played on and returns its name in `out`. Called from MapScreen::EnterNode's battle branch
    // (0x392AD8: kind "easy" / "hard" by upgrade_basic_combats_to_hard, node = the MapNode, whose seed is at +0x118) and from two std::function
    // bodies (0x39D579 / 0x39DCFB, node = null). It filters by the level groups (0x232AA0) and draws from the shared stream.
    { 0x00394450, "LEVELPICK", "map: pick the level of a battle node" },
    // The callees of the battle build (0x35AAF0), in call order: statics 0x35FFA0, terrain 0x35C070, props 0x35E1F0, party 0x35C3E0, allies 0x35DCC0,
    // extra 0x35E5E0, enemies 0x35D0B0 -- then the 20% bird roll (0x35B54A..0x35B584, MewDirector+0x70C == 0 && +0x6DC != 0) and the pickups. All take
    // the builder in rcx. A definition load's own draws (a random cat for a Kitten) differ per save, so each stage ends by resetting the stream.
    { 0x0035FFA0, "BUILDSTATICS", "battle build stage: the level's static objects" },
    { 0x0035C070, "BUILDTERRAIN", "battle build stage: the terrain" },
    { 0x0035E1F0, "BUILDPROPS", "battle build stage: props" },
    { 0x0035C3E0, "BUILDPARTY", "battle build stage: the player's party" },
    { 0x0035DCC0, "BUILDALLIES", "battle build stage: allies" },
    { 0x0035E5E0, "BUILDEXTRA", "battle build stage: extra spawns (also called by a mid-battle spawner: only the build window is touched)" },
    { 0x0035D0B0, "BUILDENEMIES", "battle build stage: the enemy list" },
    // events/*.gon `random_pool [ {...} {...} ]` (the result of Tragedy, ...): this 113-byte handler calls sub_14094FF80(node, "weight", stream) -- one draw from the shared
    // simulation stream at TLS+0x178 -- and runs the entry it returns through sub_1409173A0 (the event interpreter: set_frame, prompt, self_damage [a b] ...).
    { 0x009310B0, "RANDOMPOOL", "event: random_pool outcome" },
    { 0x00935ED0, "EVWEATHERROLL", "event: weather_roll" },
    { 0x00931130, "EVPOOLLUCK", "event: random_pool_consider_luck" },
    { 0x00930750, "EVRANDOMCHANCE", "event: random_chance" },
    { 0x00930860, "EVREWARD", "event: reward" },
    { 0x0092A640, "EVDISORDERPOOL", "event: gain_disorder_from_pool" },
    { 0x00924AA0, "EVLEARNABILITY", "event: learn_ability_from_pool" },
    { 0x00924DD0, "EVLEARNPASSIVE", "event: learn_passive_from_pool" },
    { 0x00932770, "EVMUTSET", "event: random_mutation_from_set" },
    { 0x00933270, "EVMUT", "event: random_mutation" },
    { 0x0091BE20, "EVSTATCHECK", "event: option stat check (good/bad)" },
    { 0x00913A90, "EVSETUP", "event: WorldEvent constructor" },
    { 0x000AB770, "PICKRANDOM", "random element of a vector (event subject cat)" },
    { 0x00938F40, "PROPSETINT", "save property int setter (event legacy counters)" },
    { 0x001E6810, "HOUSESAVE", "House state writer (house_state)" },
    { 0x008DEC40, "EVITEMPICK", "event: pick an item from a pool (get_item_from_pool)" },
};

// Coarse module guard, checked before the per-target signatures.
static const uint32_t kExpectedSizeOfImage = 0x1574000;
// The PE timestamp of the build every STRUCTURE OFFSET in this mod was measured on (CatData +0xBF8, Character +0x4B0,
// MewDirector +0x5A8 ...). Signatures find the functions after an update, but nothing re-checks the layouts they work on:
// on any other build the mod refuses to hook (see hooks_verify_module) unless debug.allow_unpinned_build is set.
static const uint32_t kExpectedTimeStamp = 0x6A9C51E5;

// ---------------------------------------------------------------------------
// Injection park point.
//
// The loader cannot inject into a freshly suspended process: nothing but ntdll
// and the exe is mapped yet. It cannot inject at the PE entry point either --
// measured on this build, a remote thread created there faults during
// DLL_THREAD_ATTACH (a loaded DLL's thread callback touches state that only
// exists once the game has initialized), which kills the process.
//
// So the loader parks the game at ApplicationBase::FrameBegin instead: the
// first call happens after all startup is done but before a single frame has
// been simulated, which is early enough for every hook we care about. The only
// thing that ordering costs is initSystems, which has already run by then.
static const uint32_t kParkRva = 0x009B7870;   // glaiel::ApplicationBase::FrameBegin
static const uint8_t  kParkSig[16] = {
    0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20,0x55
};

// ---------------------------------------------------------------------------
// Functions we CALL rather than hook.
//
// Same guard, different reason. A hook splices a jump and a wrong address
// corrupts an unrelated function; a call just runs one, and a wrong address
// runs whatever happens to live there -- with our arguments, on the game's
// stack. The prologue check is what makes "the build drifted" a refusal at
// startup instead of a crash three screens later.
//
// These five implement CatData sync (mgmp_catsync.cpp). SerializeCatData is
// BIDIRECTIONAL -- one function reads or writes depending on ByteStream+0x00 --
// which is the whole reason the client can be updated without reimplementing
// anything the inventory screen does.
enum Call : int {
    C_SerializeCatData = 0,  // void(CatData&, ByteStream&, bool)   src: MewSaveFile.cpp
    C_ByteStreamDtor,        // void(ByteStream*)
    C_OfstreamCtor,          // the std::ofstream embedded at ByteStream+0x30
    C_CatDataById,           // CatData*(registry, u64 id)

    // --- the run inventory (mgmp_invsync.cpp) ----------------------------
    C_InvBucketWrite,        // void(unused, MewSaveFile*, std::string key, Bucket*)
    C_InvBucketRead,         // void(unused, MewSaveFile*, std::string key, Bucket*)
    C_GameStrAlloc,          // void*(size_t) -- the game's operator new wrapper
    C_GameStrDtor,           // void(std::string*) -- _Tidy_deallocate

    // --- the element state the damage path branches on (mgmp_lockstep.cpp) ---
    C_AffectingElements,     // ElementList*(Character*, ElementList* out)

    // --- drawing the peers' cursors (mgmp_cursor.cpp) --------------------
    C_ImGameUI,              // ImmediateModeGameUI*(Component*)
    C_ImTilePiece,           // UIPiece*(ui, str id, int layer, str anim, iVec2D tile,
                             //          Color4f* rgba, int frame, Vec3D* scale)

    // --- replaying a decision screen choice (mgmp_choice.cpp) ------------
    C_LevelUpClick,          // sub_140386810(capture{_, LevelUpScreen*, int idx})

    // --- flushing the host's live run to disk (mgmp_savefile.cpp) --------
    C_SaveAdventure,         // void(MewDirector*) -- sub_1403B9CE0

    // --- the run history (mgmp_runhist.cpp) ------------------------------
    C_RunHistSerialize,      // void(RunHistory*, ByteStream*) -- sub_1408DD2F0
    C_DrawAbilityAOE,        // void(Brain*, Ability*, iVec2D, iVec2D, int)
    C_AbilityHighlight,      // void(Brain*, Ability*, int) -- sub_140138A10.
                             // NOT a draw. Only ever called with the highlight
                             // suppressor raised; read the block below the
                             // table before touching it.

    // --- the attack range a Move preview shows (mgmp_aim.cpp) ------------
    // Both are ORDINARY SIMULATION CALLS. They are in this table because the
    // game's own move preview displaces the cat and puts it back; see the
    // second block below the table.
    C_TacticsMove,           // void(TacticsObject*, iVec2D tile, bool, bool)
    C_RecomputeStats,        // void(Character*, Ability*, bool)

    // --- pressing Play for a client that is stuck on the menu ------------
    C_ButtonClick,           // void(Button*, bool force) -- sub_1409764B0
    C_CatAlloc,
    C_CatCtor,
    C_CatPostLoad,
    C_CatParents,
    C_CatIdAppend,
    C_CatStatusRangeDtor,
    C_EquipmentById,
    C_EquipmentLocation,
    C_EquipmentLegacyQuest,
    C_EquipmentRefresh,
    C_CatMaxHealth,          // int(CatData*, const StatFilter*) -- 4 x effective CON + base health, min 1
    C_StartTransition,       // void(MewDirector*, std::string* name, std::function<void()>* done, double delay)
    C_PropGetInt,            // int64(MewSaveFile* props, std::string key BY VALUE, int64 default) -- sub_14022C5E0, reads one `properties` row
    C_CatStats,              // int* (CatData*, int out[7], const int filter[7] (16-aligned, -1 = all), bool full, bool no_kitten_penalty) -- sub_1400C1820
    C_SaveGame,              // void(MewDirector*) -- sub_1403BDCF0: writes the whole save (cats table, house_state, properties). The native settlement calls it as its last step (0x1403B5F6E), BEFORE the clone merge
    C_COUNT
};

struct CallDesc {
    uint32_t    rva;
    const char* name;
};

// THE PROLOGUE GUARD AND ITS SECOND WINDOW ARE GONE. Both are subsumed by the
// uniqueness requirement in mgmp_sigscan.h, and it is worth recording what they
// were for so nobody reintroduces them.
//
// `sig[16]` compared the first sixteen bytes at a FIXED rva. That answers "is
// this still the pinned build" and cannot answer "where is this function", so
// the day the game patches it is useless. Worse, sixteen bytes does not even
// name a function reliably: MSVC emits the same register-save prologue for
// every large function in a translation unit, and the two prologues this mod
// depends on most turn out to occur SIX times each in the image.
//
// `sig2` existed for one specific near-miss: sub_1403B9CE0 (save_adventure) is
// byte-identical to MewDirector::ContinueAdventure for its first TWENTY-ONE
// bytes, diverging only in the displacement of the `lea rbp, [rax-N]` that
// sizes the frame. A 16-byte guard on the first happily accepted the second --
// the function whose first statement destroys the House scene and which frees
// the entire live cat registry.
//
// A resolver that refuses anything matching more than once cannot make that
// mistake, because "these two functions are indistinguishable" and "this
// pattern matched twice" are the same fact. See mgmp_resolve.h.

// Order must match enum Call.
static const CallDesc kCalls[C_COUNT] = {
    { 0x0022F410, "SerializeCatData" },
    // `cmp byte ptr [rcx+20h], 0` right in the prologue -- the "owns the read
    // buffer" flag. Lending it our own buffer with that byte 0 is what keeps
    // the game from freeing memory we allocated on a different heap.
    { 0x009BF900, "ByteStream::~ByteStream" },
    { 0x0032DC60, "std::ofstream::ofstream" },
    { 0x000D7220, "CatData::by_id" },

    // sub_1402E15B0 / sub_1402E1740 -- one inventory bucket, out and in. These
    // are the two functions mgmp_invsync actually drives; everything below
    // them (the intrusive-list walk, the per-Equipment field loop, the item
    // construction) is the game's and is not reimplemented.
    //
    // First parameter is DEAD. The game's own calls pass whatever happened to
    // be in rcx -- one call site passes an uninitialised local, another passes
    // the literal 101 -- and the decompiled body never reads it. We pass null.
    { 0x002E20D0, "Inventory::write_bucket" },
    { 0x002E2260, "Inventory::read_bucket" },

    // The game's string heap, needed because two of the three bucket keys are
    // longer than the 15-char MSVC small-string buffer ("inventory_backpack"
    // is 18, "inventory_storage" 17) and the callee destroys the key it is
    // given. A key allocated by OUR /MT CRT and freed by the game's is the
    // same two-heap bug ByteStream+0x20 exists to avoid, one layer up.
    { 0x00052A40, "glaiel::str_alloc(size_t)" },
    // std::string::_Tidy_deallocate: frees only when capacity > 15, so calling
    // it on a small-string image is a no-op rather than a wrong free.
    { 0x00052780, "std::string::_Tidy_deallocate" },

    // sub_14011A840 -- glaiel::Character::get_affecting_elements(ElementList& out).
    // The union of three sources, ORed together in this order:
    //   1. Character::get_elements     -- passives and statuses (Character+3320 seed)
    //   2. Character::get_standing_tile_elements -- the TILES the cat is standing on
    //   3. the equipment list at *(Character+128)+264, via vtable slot 81
    //
    // It is called FOUR times inside Character::receive_damage_passives
    // @ 0x14010E68B/69F/6FB/70F, where it gates status application (a wet cat
    // hit by ice gets Freeze). So this 8-byte value is not incidental state --
    // it is an INPUT to damage resolution, and it is the only such input that
    // lives on the grid rather than on the character.
    //
    // ElementList is two u32 bitmasks. The lambda inside
    // get_standing_tile_elements is the whole proof of the tile half:
    //     out[0] |= *(u32*)(tile + 304);   // TacticsTile+0x130
    //     out[1] |= *(u32*)(tile + 308);   // TacticsTile+0x134
    { 0x0011B250, "Character::get_affecting_elements" },

    // sub_14013C570 -- Component* -> ImmediateModeGameUI*. Generic: it only
    // touches Component+0x18 (the scene) and walks scene -> +32 -> +18080 to a
    // {cap,count,data} vector whose first element is the UI, with a fallback
    // through the parent scene when this one has none. Brain and StatusMenu
    // both call it with `this`, which is why one accessor covers both.
    // Returns null off the battle screen; every caller must check.
    { 0x0013CF90, "Component::im_game_ui" },

    // sub_14033FFD0 -- the immediate-mode "draw this sprite on that tile" call,
    // and the reason peer cursors need no components, no lifetimes and no
    // cleanup. Brain::UpdateDecision uses it for the local target cursor
    // (id "TargetCursor", anim "target", layer 6, frame -1); DrawAbilityAOE
    // uses it for "AreaIndicator" and "KnockbackArrow".
    //
    //   rcx  ImmediateModeGameUI*
    //   rdx  std::string* id     -- the immediate-mode identity. The same id on
    //                              consecutive frames is the SAME piece, so it
    //                              persists and animates; stop passing it and
    //                              the piece goes away by itself.
    //   r8d  int layer
    //   r9   std::string* anim   -- a SWF animation name
    //   [5]  iVec2D tile         -- BY VALUE, packed {i32 x, i32 y}; the callee
    //                              adds 0.5 to each to centre it on the square
    //   [6]  float rgba[4]       -- by pointer. The consumer premultiplies rgb
    //                              by a and writes a into Renderer+0x60, so
    //                              element 3 IS the alpha and nothing else has
    //                              to be touched to make a cursor translucent.
    //   [7]  int frame           -- -1 for "no particular frame"
    //   [8]  double scale[3]     -- by pointer, {1,1,1} for native size
    //
    // BOTH std::strings are CONSUMED: the callee runs _Tidy_deallocate on each
    // before returning. Keeping every name at 15 characters or fewer makes that
    // a no-op on a small-string image, which is why the ids below are short --
    // the same two-heap hazard mgmp_invsync handles with C_GameStrDtor.
    { 0x00340B90, "ImmediateModeGameUI::tile_piece" },
    // sub_140386810 -- the LevelUpScreen button callback, called with a capture
    // of {vftable, LevelUpScreen*, int index}. We CALL it rather than
    // reimplementing select_option's argument, because it is the thing that
    // resolves *(LevelUpScreen+864) + 240*index and copy-constructs the option
    // through sub_14037BBB0. Injecting a choice is therefore the game's own
    // click path with our index, not a hand-built LevelUpOption.
    //
    // Only two xrefs: one _Func_impl vftable and .pdata. It is never called
    // directly by the game, so there is no risk of colliding with a live caller.
    { 0x003873E0, "LevelUpScreen button click" },

    // sub_1403B9CE0 -- "save the adventure", void(MewDirector*).
    //
    // One argument in rcx; three of its four representative call sites pass
    // cs:qword_1413D1970 straight through, and MewDirector::ReturnToMap's
    // inner body passes the same pointer in rdi. Twelve code callers, and its
    // only data xref is .pdata -- not virtual, so calling it is calling the
    // thing the game calls at every return to the map.
    //
    // It is WRITE-THROUGH TO DISK, which is the property this module needs:
    // it stores `chapter_map`, `adventure_state`, `trollengine_state`,
    // `tutorial_tokens`, `on_adventure` and `savescumlocation` through
    // MewSaveFile::Store @ 0x14022C4D0, which reaches
    // `INSERT OR REPLACE INTO <table> VALUES (:key, :data);` via
    // SQLSaveFile::SQL @ 0x140A02BE0. So calling it makes the .sav on disk
    // equal to the host's live run rather than its last checkpoint.
    //
    // `chapter_map` is the part that matters for lockstep: the map loader
    // sub_140227ED0 restores each MapNode+0x118/+0x128 from that blob with
    // the same paired `movups` MapScreen::EnterNode uses to copy it OUT into
    // TLS+0x178. The per-node seeds ride in the save; the save is why two
    // peers roll the same battle.
    //
    // THIS IS THE ENTRY THE SECOND SIGNATURE WINDOW EXISTED FOR. Sixteen bytes
    // cannot tell it apart from ContinueAdventure -- byte 21 (0x68 here, 0xD8
    // there) is the first that differs. The signature resolver needs no special
    // case: its pattern is grown until it matches exactly once, so it either
    // names this function or resolves nothing at all.
    { 0x003BABB0, "MewDirector::save_adventure" },

    // sub_1408DD2F0 -- the run history's serializer, void(RunHistory*,
    // ByteStream*), BIDIRECTIONAL in exactly the way SerializeCatData is: every
    // field branches on ByteStream+0x00, and its own version tag (3) is the
    // first thing in the stream.
    //
    // Its two callers are save_adventure and ContinueAdventure, and both pass
    // `*(MewDirector + 1424)` -- the pointer AT the field, dereferenced
    // (`mov rcx, [rsi+590h]`), not the address of it. Getting that backwards
    // would hand the game a pointer to a pointer.
    //
    // NO SECOND WINDOW, and unusually this is provable rather than assumed:
    // these sixteen bytes occur EXACTLY ONCE in the shipped image. Checked
    // against Mewgenics.exe itself, not against IDA -- the rule save_adventure
    // taught, where a 16-byte guard would have happily accepted
    // ContinueAdventure.
    { 0x008E0F40, "RunHistory::serialize" },

    // glaiel::Brain::DrawAbilityAOE -- the range / AOE / knockback tiles a
    // solo player sees while aiming. Immediate mode, so it must be called every
    // frame and there is nothing to free; it resolves its own
    // ImmediateModeGameUI and draws AreaIndicator / KnockbackArrow /
    // PathIndicator, which is why mgmp_aim needs no art of its own.
    //
    // The ABI is read off the game's own call at 0x140137798, which is the only
    // one that matters because it is the one we are copying:
    //     rcx = Brain*, rdx = Ability*, r8 = iVec2D target BY VALUE,
    //     r9 = iVec2D direction BY VALUE, [rsp+0x20] = int 0x13
    // Both iVec2D are 8 bytes, so they go in registers whole -- the MSVC x64
    // rule that sends aggregates by hidden pointer starts above 8 bytes.
    //
    // Its own first instruction is `test rdx, rdx / jz` -- it null-checks the
    // Ability before touching it, which is one less thing for us to get wrong.
    { 0x0013AA50, "Brain::DrawAbilityAOE" },

    // sub_140138A10 -- the reachable/in-range tile highlight. THE TILES ARE A
    // SIDE EFFECT OF STATUSES; read the block below before calling it.
    { 0x00139430, "ability highlight (sub_140139430)" },

    // glaiel::TacticsObject::Move(iVec2D, bool, bool) and
    // glaiel::Character::recompute_stats(Ability*, bool) -- the two halves of
    // the game's own "where could I attack from there" preview. ABI read off
    // the game's own displacement at 0x14077738B / 0x1407773AD:
    //     rcx = TacticsObject*, rdx = iVec2D tile BY VALUE, r8b = 1, r9d = 0
    //     rcx = Character*,     edx = 0 (no ability),       r8b = 1
    { 0x00832160, "TacticsObject::Move" },
    { 0x00102520, "Character::recompute_stats" },

    // sub_1409764B0 -- glaiel::Button::Click(Button*, bool force).
    //
    // CALLING IT IS PRESSING THAT BUTTON, the same claim as
    // SaveSelection::ContinueSlot, and established the same way -- by reading
    // what the game's own click path does. Button::update @ 0x140975F00
    // reaches it at 0x14097644D as `sub_1409764B0(this, 0)`, at the bottom of
    // the hover-and-mouse-up branch, and that is the ONLY call site.
    //
    // The body is the whole click: it plays `<button name>_Click`, then
    //
    //     v8 = *(Button + 240);
    //     if (v8) (*(*(_QWORD*)v8 + 16))(v8);      // std::function _Do_call
    //
    // -- the callback MenuPanel::register_button stored on the button -- and
    // finishes with the radio-group and focus bookkeeping. For the main menu's
    // Play button that callback is the lambda at 0x1401BEBE0, which starts the
    // game's own fade transition into the SaveSelectionScreen scene.
    //
    // WE PASS force = 0, matching the game's call exactly. `force` only
    // bypasses the Button+89 guard; the other three -- Button+764 != 1, the
    // Button+96 <= Button+104 timer, and sub_1409766C0 -- still apply, so a
    // button the player genuinely could not click is one we do not click
    // either. A press that does not take is logged rather than forced, because
    // "the button refused" and "the button is not there" need different fixes
    // and forcing would hide both.
    //
    // The button is identified by the std::string at Button+504 (kBtn_Name),
    // never by pointer or by position in a panel.
    { 0x0097E8E0, "glaiel::Button::Click" },
    { 0x00D3E0A4, "CatData allocation" },
    { 0x0005D6B0, "CatData constructor" },
    { 0x007393E0, "CatData post-load" },
    { 0x00772AC0, "CatData parent registration" },
    { 0x00048010, "Cat ID vector append" },
    { 0x00061780, "CatData saved-status range destructor" },
    // Native inventory lookups, followed by quest_item && legacy_quest.
    { 0x0034BDD0, "InventoryScreen::equipment_by_id" },
    { 0x0034BCB0, "InventoryScreen::equipment_location" },
    { 0x002DE0A0, "Equipment::is_legacy_quest" },
    { 0x0034BB50, "InventoryScreen::refresh_equipment_locations" },

    // CatData::max_health @ 0x1400C2360 -- found 2026-09-30 by reading the game's own clamp of a cat's
    // health (0x1400C2CD0), which does exactly this and then clamps campaign hp (CatData+0x7A8) to it:
    //
    //     stats = CatData::compute_stats(this, &filter, true)   ; 0x1400C1820, effective CatStats
    //     base  = CatData::base_health(this, &filter)           ; 0x1400C1C60
    //     return max(1, 4 * stats.con + base)
    //
    // `rdx` is a 32-byte filter read with MOVAPS, so it must be 16-aligned; the game's own callers
    // pass the constant 0xFF x 32 ("no filter"). Class stat_mods, equipment, passives and mutations are
    // all inside compute_stats -- which is why an estimate from the saved stats alone was wrong.
    { 0x000C2360, "CatData::max_health" },

    // sub_1403C0210 -- starts a named screen transition (fade), running `done` when it completes. The
    // abandon is exactly this call and nothing else: the popup's OK callback (0x14029AE10) builds the
    // std::string "defeat" and a std::function whose body (0x1403C7500) is MewDirector's end-of-run
    // (sub_1403B4750, the same one a lost battle ends in), and calls
    //     sub_1403C0210(MewDirector, &"defeat", &function, 0.0 in xmm3)
    // before it tidies the popup, which is the only part that needs the pause menu. So peers that never
    // opened one can run the abandon by making the same call -- see mgmp_abandon_game.cpp.
    { 0x003C0210, "MewDirector::start_transition" },
    { 0x0022C5E0, "SaveProps::get_int" },
    { 0x000C1820, "CatData::total_stats" },
    { 0x003BDCF0, "MewDirector::SaveGame" },
};

// sub_140138A10 IS NOT A DRAW ROUTINE, and it is in the table above only
// because it is called with T_HighlightRefresh raised. It went in on 2026-08-26
// as "Brain::DrawAbilityRange -- the other half of the preview", killed a run
// within the hour, came out, and came back only once the mutation had a
// suppressor and a fence around it.
//
// It collects a set of objects, sets `[obj+0x118]` on each, and calls
// sub_140151CE0 four times (0x140138C7A, D4A, E3A, E9A). That function, on a
// flag change, runs
//
//     v16 = glaiel::apply_status(&name, v11, v14, v12, v11, nullptr, 1, 1);
//
// on a REAL `Character*` (`v11 = *(a1+56)`) and pushes the returned `Status*`
// onto a vector on the object. The highlighted tiles are a side effect of those
// statuses. `apply_status -> sub_14062F050 -> Character::Face` is a four-hop
// path to a write of `Character+0x388`, which is how it was found.
//
// So calling it on the peer that does NOT own the cat mutates that peer's
// simulation, thousands of times a battle. It cost a run inside the hour: every
// hashed component agreed at turn 20 while cat 31's facing read `(0,-1)` on the
// host and `(-1,0)` on the client, and the next AI decision diverged from an
// identical board. The RNG fence around the call could not see it -- the fence
// guards the stream, and this moved state.
//
// The general rule it earned: **a function reached from a drawing path is not
// therefore a drawing function.** Before calling anything into the game on one
// peer only, walk its callees for `apply_status`, `Face`, `ReceiveDamage` and
// friends, not just for RNG.
//
// WHAT MAKES THE CALL ALLOWED NOW, in the order the guarantees are worth:
//
//   1. `T_HighlightRefresh` swallows `sub_140151CE0` outright while the guard
//      is up, so the status pathway -- the apply, the removal and the vector
//      bookkeeping -- does not execute at all. This is the fix, not a mitigation.
//   2. `mgmp_aim` fences the WHOLE roster's simulation state across the call
//      and puts facing back, exactly as the aim-preview freeze does. That
//      covers what the suppressor does not: `[obj+0x118]` is still written by
//      sub_140138A10 itself, and nobody has proven what reads it.
//   3. Anything the fence catches is logged with the cat and the field. A
//      quiet log is the evidence this is safe; a loud one is the retraction,
//      and either way it arrives in the first battle rather than on turn 49.
//
// "It cannot change the logic anyway" is the exact belief that cost the run.
// The point of the two guards is that the claim is now measured every frame
// instead of assumed once.

// THE ATTACK RANGE UNDER A MOVE -- WHY C_TacticsMove IS IN THE TABLE ABOVE.
//
// Selecting Move shows two things: the green reachable set, and the tiles the
// cat could ATTACK from wherever the mouse is hovering. Only the first comes
// out of sub_140138A10, and the second is not a draw call at all -- the game
// DISPLACES THE CAT AND PUTS IT BACK. Read off PlayerBrain's own update,
// sub_140775EB0 @ 0x14077636B..0x140777403:
//
//     inc  [Character+0xEC0]                     ; a re-entrancy / dirty guard
//     TacticsObject::Move(tobj, hovered, 1, 0)   ; the cat really moves
//     dec  [Character+0xD10]  /  inc [Character+0xD3C]   ; spend the move
//     Character::recompute_stats(char, 0, 1)
//     (*Character+0xD8)->vtable[0xA0](out, hovered)      ; the ATTACK slot's
//                                                        ; range from there
//     inc  [Character+0xD10]  /  dec [Character+0xD3C]
//     TacticsObject::Move(tobj, original, 1, 0)  ; and back
//     Character::recompute_stats(char, 0, 1)
//     dec  [Character+0xEC0]
//
// Two facts fall out of that, and the second is the interesting one.
//
// It is the ATTACK slot (Character+0xD8), not "the longest-ranged ability" --
// the range simply grows with the cat's bonus_range, which is what makes it
// look chosen.
//
// And THE GAME ALREADY DOES THIS ON ONE PEER ONLY. The owner's machine
// displaces the cat every frame the mouse moves; the other machine does not.
// So `TacticsObject+0x50` (the "previous tile" ::Move writes) and the two
// counters above already differ between two peers of a working session, and
// have through every byte-identical 49-turn run measured. That is evidence the
// displacement round-trips cleanly, not a proof -- which is why mgmp_aim
// re-reads the tile after the trip and fences the whole roster across it.
//
// It is worth being clear about the shape of the risk, because it is NOT the
// shape sub_140138A10 had. There the mutation was hidden four hops down a call
// named like a draw. Here it is explicit, symmetric, and authored by the game
// as a preview; the danger is a round trip that fails to complete -- an
// exception between the two Moves, or a Move refused at the destination --
// which is exactly what the state fence reports.

// The MewDirector pointer VARIABLE (not the object). Data, so there is no
// prologue to check -- mgmp_catsync validates it by reading through it and
// refusing on anything implausible.
constexpr uint32_t kRva_MewDirectorPtr = 0x013DAC30;

// Offsets into the MewDirector, all read off sub_1403B2060 and sub_140231180.
constexpr uintptr_t kDir_CatRegistry = 1432;   // ptr, the id -> CatData* map
constexpr uintptr_t kDir_CatIdCap    = 1464;   // u32, cap  -- PROVEN 2026-09-23
constexpr uintptr_t kDir_CatIdCount  = 1468;   // u32, count
constexpr uintptr_t kDir_CatIdData   = 1472;   // u64*, the run's cat ids

// THE CAP WAS BEING READ FROM THE WRONG OFFSET UNTIL 2026-09-23 (mgmp_roster.cpp had
// it at 1460). The hardware watchpoint on +1468/+1472 fired at departure and the raw
// bytes at +1464 read `04 00 00 00 | 04 00 00 00 | 40 56 80 69 5B 02 00 00` -- cap,
// count, pointer, in that order, which is a plain vector<u64> laid out cap/count/data.
// So +1460 is some other field entirely, and every "capacity 499 / 678" this project
// has printed came from it rather than from the vector. The vector is a real std::vector
// and grows (the departure path reallocates it through 0x48FB0), so its cap is a
// current fact and NOT a limit on how many cats a run may hold.

// THE MIRROR, 16 BYTES ON (2026-09-23). The departure code assigns {+1464} and
// {+1480} back to back from one routine -- and the raw dump taken the moment it did
// shows BOTH holding the same four ids, their two allocations 0x30 apart in the heap.
// The engine therefore keeps two copies of the run's cat list, and every write in this
// project has only ever touched one: mgmp_roster's own comment says it wrote +1468/+1472
// and never +1464's neighbour. That is the best available explanation of the crash on
// quit above five cats, and it is why write_party now keeps both in step.
constexpr uintptr_t kDir_MirrorCap   = 1480;   // u32, cap
constexpr uintptr_t kDir_MirrorCount = 1484;   // u32, count
constexpr uintptr_t kDir_MirrorData  = 1488;   // u64*, the same ids
constexpr uintptr_t kDir_CatFamiliars = 0x640; // RVA 0x929E4C: gain_cat_familiar appends here when the party is full.

constexpr uintptr_t kChar_CatData   = 0x88;   // RVA 0xF7066 -> 0xFCB8A stores by_id's result; 0xF7C57 reads it.
constexpr uintptr_t kLvl_CatData    = 0xA0;   // RVA 0x383645 and 0x3786F9 read the upgrade subject.
constexpr uintptr_t kCatData_Seed  = 0x00;   // Not the registry key: comparing this to run ids hid the Character link.
constexpr uintptr_t kCatData_SaveId = 0xC48; // RVA 0x230101 stores Load's u64 key; 0xD7CEE copies it.
constexpr uintptr_t kCatData_Flags = 0xBF8;   // u64 status bits, serialized with the cat (SerializeCatData RVA 0x22F6CD)
// PERMANENT DEATH, read from the disassembly (2026-10-02). Character::OnCorpsePop (RVA 0x115E10, at 0x116443) -- the corpse of a player's cat
// destroyed -- writes CatData+0x7AC = 1 and ORs 0x40000 into CatData+0xBF8, then the run's lists lose the id. The `kill` event effect
// (handler 0x9350C0) sets +0x7AC alone. The native settlement (0x3B5430) turns +0x7AC into flag 0x20 on a cat still in the party, and
// 0xD2D40 clears the byte. So "this cat is dead for good" is readable straight off its CatData while it is still registered.
constexpr uintptr_t kCatData_Killed  = 0x7AC;   // u8
constexpr uint64_t  kCatFlag_Perished = 0x40000;
// The instruction after the call that picks a WorldEvent's subject cat (sub_140913A90, `call sub_1400AB770` at 0x1409150F3): T_PickRandom only acts when it returns here.
constexpr uintptr_t kRet_EventSubject = 0x009150F8;
constexpr uint64_t  kCatFlag_OnAdventure = 0x80000; // set by set_party_and_go (0x3B1504) on the cats that leave; 0xD2D40 clears it
constexpr uintptr_t kCatData_StatusEffects = 0x7B8; // RVA 0x22F301 appends; 0xFD44D restores each entry into battle.
constexpr uintptr_t kCatStatusEffectSize = 0xB8; // RVA 0x237368 append stride and 0x617B9 destructor stride.
constexpr uintptr_t kCatData_Items = 0x9B0; // Five optional item records, SerializeCatData RVA 0x22FD28..0x22FD6E.
constexpr uintptr_t kCatItemSize = 0x60;
constexpr uint32_t kCatItemCount = 5;

// The other two fields sub_1403B9CE0 ("save the adventure") reads before it
// calls the inventory writer:  sub_1402E10D0(*(Inventory**)(dir+1416), dir+56).
constexpr uintptr_t kDir_SaveFile  = 56;      // an EMBEDDED MewSaveFile, not a ptr
constexpr uintptr_t kDir_Inventory = 1416;    // Inventory*

// RunHistory*, the object MapScreen::select_event picks an event through.
// A POINTER, dereferenced -- see C_RunHistSerialize.
//
// This file used to record it as "+1424, saved via sub_1408DD2F0, semantics
// unknown". The semantics turned out to matter a great deal: it holds the
// USED-EVENT LIST that the event roller's retry loop skips over, so two peers
// whose copies differ by one entry roll different events from identical RNG.
// See RunHistMsg.
constexpr uintptr_t kDir_RunHistory = 1424;

// WorldEvent+0x1A10 -- the std::string naming the event that was chosen.
// WorldEvent::init writes MapScreen::select_event's result into it
// (0x14090FB87), tests it against the literal "random" (0x14090FBA3) and then
// uses it as a GON key (0x14090FBE9); `this` is r15 throughout, pinned by the
// `mov r15, rcx` at 0x14090FA30. Read-only for us, and only as evidence.
constexpr uintptr_t kEvt_EventName = 0x1A10;

// Inventory layout, from glaiel::Inventory::init @ 0x1403C8310, which builds
// four identical 64-byte buckets and then zeroes the three scalars.
//
// Only the first three buckets are ever serialized. The fourth (+248) is
// allocated the same way and saved by nothing -- see mgmp_invsync.h for what
// that means for us.
constexpr uintptr_t kInv_Backpack = 56;
constexpr uintptr_t kInv_Storage  = 120;
constexpr uintptr_t kInv_Trash    = 184;
constexpr uintptr_t kInv_Equipped = 248;   // NOT serialized by the game
constexpr uintptr_t kInv_Coins    = 328;   // int, GON key `adventure_coins`
constexpr uintptr_t kInv_Food     = 332;   // int, GON key `adventure_food`
constexpr uintptr_t kInv_Boxes    = 336;   // int, `adventure_furniture_boxes`

// Within one bucket: the intrusive list head and its element count, read off
// the walk at the top of sub_1402E15B0 (`v8 = *(bucket+16)` is the count it
// pre-sizes the vector from; `*(bucket+8)` is the sentinel node it walks to).
constexpr uintptr_t kBucket_Head  = 8;
constexpr uintptr_t kBucket_Count = 16;

// StatusMenu layout, read off StatusMenu::init @ 0x140816F60 (which assigns the
// members in order) and StatusMenu::update @ 0x140817320 (which uses them).
//
// +80/+88 are the two cursor pips, RendererIso components created by init from
// the animations "GroundMouseCursorPip" and "MouseCursorPip3D". update sets
// pip+81 (the Renderer visible byte) to 1 or 0 every frame depending on whether
// the mouse is over the board -- so we can dim them, but we must not try to own
// their visibility.
constexpr uintptr_t kSM_Grid      = 72;    // the tactics grid / level
constexpr uintptr_t kSM_PipGround = 80;    // RendererIso*, "GroundMouseCursorPip"
constexpr uintptr_t kSM_Pip3D     = 88;    // RendererIso*, "MouseCursorPip3D"
constexpr uintptr_t kSM_HoverTile = 124;   // iVec2D, UNALIGNED -- read as bytes

// On the grid at kSM_Grid. update bounds-checks the hovered tile against these
// two before it will show a pip, so they are the authoritative board size.
constexpr uintptr_t kGrid_Width   = 184;   // u32
constexpr uintptr_t kGrid_Height  = 188;   // u32

// On any Renderer (set to 1.0 by the base constructor sub_14005A580, which does
// `*(double*)(this+96) = 1.0`). Renderer::render reads it as a double and emits
// the vertex colour {1,1,1,alpha}, so writing it is the whole of "make this
// translucent" -- no shader, no blend state, no second draw.
constexpr uintptr_t kRenderer_Alpha = 96;

// CombatMenu layout, read off CombatMenu::update @ 0x1402B12F0 and
// CombatMenu::show @ 0x1402AE090.
//
// +240/+248 is the button vector. In the disassembly it is `mov rax,[r15+0F0h]`
// / `mov rax,[r15+0F8h]` with `sub` + `sar rax,3` for the count, i.e. a plain
// std::vector<Button*>. Both the ability buttons and the end-turn button are in
// it -- update finds the end-turn one by comparing each entry against the "bEnd"
// node of the name map at +264, not by position.
//
// +280/+288 is a Ref<Character>: sub_1400A1B30 writes {ptr, *(u64*)(ptr-8)}, and
// every read in update re-checks the generation before dereferencing. Copy that
// -- a CombatMenu holding a stale Character is exactly the state the game itself
// treats as "hide the bar".
constexpr uintptr_t kCM_ButtonsBegin = 240;
constexpr uintptr_t kCM_ButtonsEnd   = 248;
constexpr uintptr_t kCM_Subject      = 280;   // Character*
constexpr uintptr_t kCM_SubjectGen   = 288;   // its generation at assignment
constexpr uintptr_t kRefGenOffset    = 8;     // generation lives at ptr-8

// glaiel::Button, sizeof 0x3C0. +752 is the state, and it indexes the array of
// per-state SWF frame names that starts at +768 with a 32-byte stride -- which
// is what pins the enum, because CombatMenu::show writes those names by hand:
// +768 "up", +800 "over", +832 "down", +896 "missing"/"disabled".
//
//   0 up   1 over   2 down   3 selected   4 disabled   5 enable
//
// 4 is the state the game already uses for a spell the cat cannot afford, so
// forcing it is asking for the greyed-out look the player has already seen
// rather than inventing one. Button::update also refuses to dispatch a click
// while the state is 4, so the bar stops eating clicks as a side effect.
constexpr uintptr_t kBtn_State    = 752;
constexpr int32_t   kBtnState_Disabled = 4;

// Button+504 is the button's own name, a std::string, and it is how the client
// finds the main menu's Play button without knowing anything about the menu.
//
// Two readings, both in this build:
//   * MainMenu::init__inner_0 @ 0x1401B85BB assigns it directly --
//     sub_140052080(button + 504, "MainMenu_Button_Play", 20), and the three
//     lines after it do the same for Settings, Quit and Meow;
//   * Button::Click reads the SIZE of that same string as *(Button + 520) to
//     build the "<name>_Click" sound event -- 504 + 16 is exactly where an
//     MSVC std::string keeps its length.
//
// The four names in the main menu are MainMenu_Button_{Play,Settings,Quit,Meow}.
constexpr uintptr_t kBtn_Name     = 504;
constexpr const char kBtnName_MainMenuPlay[] = "MainMenu_Button_Play";

// The pause menu's way out of a run, named the same way and registered by the
// same helper: PauseMenu::SetupMainSidebar @ 0x14028AB50 calls
// MenuPanel::register_button and then sub_140052080(button + 504,
// "Button_PauseMenu_QuitToMenu", 27). Its callback is the lambda whose body is
// PauseMenu::SetupMainSidebar__inner_3 @ 0x14029E7F0, which builds a
// "QuitToMenuTransition" component -- i.e. clicking it IS quitting to the menu,
// fade and all.
//
// THE BUTTON ONLY EXISTS WHILE THE PAUSE MENU IS OPEN, and that is a real limit
// rather than an oversight. PauseMenu::init runs when the player pauses (it
// plays "PauseMenu_Open" as its last statement) and SetupMainSidebar builds the
// buttons from there, so a client that has been told the host left the run
// cannot be dragged out on its own -- see mgmp_leave.h.
//
// The sidebar's five buttons are Button_PauseMenu_{Resume, Settings,
// QuitToMenu, QuitToDesktop, GiveUp}.
constexpr const char kBtnName_PauseQuitToMenu[] = "Button_PauseMenu_QuitToMenu";

// MewDirector+0x580 is the save's current day (int32): the number T_IsKitten's age is measured from.

// Character fields the definition loader (T_LoadChar) fills; mgmp_balance.h scales them for enemies.
constexpr uintptr_t kChr_Hp        = 0x4B0;   // int32 current health
constexpr uintptr_t kChr_Shield    = 0x4B4;   // int32 armor ("shield")
constexpr uintptr_t kChr_Faction   = 0x350;   // int32 faction enum
constexpr uintptr_t kChr_BaseCon   = 0x5A8;   // int32 constitution from the definition
constexpr uintptr_t kChr_HpBonus   = 0x5E8;   // int32: max health = 4*constitution + this
constexpr uintptr_t kChr_HpBonusBase = 0x5EC; // int32: the value kChr_HpBonus is rebuilt from

// The departure box (T_TryDepart): four slot pointers behind +0x100, a slot's cat id at +0x80.
constexpr uintptr_t kDepart_Slots   = 0x100;
constexpr uint32_t  kDepart_SlotCnt = 4;

// MewDirector+0x714 -- "quitting now would count as a save-scum" (bool). See T_SaveScumWarn.
constexpr uintptr_t kDir_ScumArmed = 0x714;

// SaveSelection+0x90 -- "A SLOT HAS BEEN PICKED" (bool), found 2026-09-30 in SaveSelection::update
// (0x1401BB780). A click on a pod sets it (0x1401BBEA6) BEFORE the continue lambda runs ContinueSlot,
// and every other input on the screen is gated on it being 0: the pods' hover (0x1401BBA4D), the next
// pod click (0x1401BBE56) and the way back to the main menu (0x1401BC3C7). The game only ever clears
// it when a pick is cancelled (0x1401BC108), because a pick normally ends the screen. So a ContinueSlot
// the mod swallows leaves the screen dead -- nothing hovers, nothing clicks, Escape does nothing --
// unless the flag is put back. mgmp_hooks does that; and sets it for the loads it issues itself, the
// same as a click would.
constexpr uintptr_t kSaveSel_Picked = 0x90;

// THE DOOR OUT OF THE HOUSE, and the third button this project presses by name.
//
// Named by walking the run's setup flow once with tune::kLogButtons on
// (2026-09-21): the House's own buttons are `HouseButton`, `EndDay_Sign` and
// `Depart_Sign`, and Depart is the one that starts the adventure. Its callback is
// a generic handler SHARED WITH EndDay_Sign (rva 0x1F8EA0, the slot identified
// inside the std::function's capture) -- which is precisely why the name is the
// right handle here and the address would not be.
//
// Why mgmp_savefile presses it: MewDirector::init always ends in the House, so a
// client that has just taken the host's run is standing in it, and the run only
// begins when somebody walks out. That cannot be replicated by calling
// ContinueAdventure (its second statement frees the live cat registry), so it is
// done the way a player does it -- see the phase-2 block in savefile_on_button_update.
constexpr const char kBtnName_DepartSign[] = "Depart_Sign";

// The gear slots the collar screen is made of, named by the same walk-through. The
// party picker (CatSelector_Left / _Right) ticks under ALL of them, so it cannot
// distinguish the collar from the rest; InventoryButton_Collar is the one slot
// whose own button is unique to the first screen. See mgmp_page.h.
constexpr const char kBtnName_InventoryCollar[] = "InventoryButton_Collar";
constexpr const char kBtnName_CatSelectorLeft [] = "CatSelector_Left";

// THE SECOND LEVEL-UP DECISION SCREEN, named the same way (tune::kLogButtons,
// 2026-09-22). Four option cards, all reporting this same button name, with the
// button's own callback at rva 0x3873E0.
//
// ITS CHOICE DOES REPLICATE, and saying otherwise was a mistake worth recording.
// A first reading of "the per-frame tick never printed" concluded that this screen
// was outside mgmp_choice altogether -- and the next run disproved it from both logs:
//
//     host   -> level-up option 0/4 (type 4 'FullMoon')
//     client <- host chose level-up option 0/4 ('FullMoon') on node b27d9efe486633f0
//     client    applied the host's level-up choice: option 0/4 ('FullMoon')
//
// select_option's RVA is 0x3835D0; the old "no UPDATE" claim had no probe in the update path.
constexpr const char kBtnName_LevelupBoon[] = "Levelup_Boon";

// THE CHAPTER PAGE'S TWO BUTTONS, and the handle the barrier is built on.
//
// Named by the same walk-through as Depart_Sign. They are a BETTER handle than
// the scene name for "is this peer on the chapter page": a scene name has to be
// confirmed against the running game (and the names live in the resource symbol
// pool, so they cannot be reached statically -- see the note above), whereas
// these two buttons exist only while that page is up, and the button detour
// already sees every button in the game by name. So the page is detected by its
// own buttons ticking, and the scene name is only LOGGED alongside it, as a
// cross-check for whoever reads the log.
//
// The heartbeat is what makes it safe: a button's update stops when its screen
// goes away, so "seen within the last half second" is the test, not "seen once".
// The gear screen's done-closure (T_EquipDone): +0x8 is "chapter 2 is unlocked, so show the chapter page" (a bool, mapflag_DesertUnlocked or the
// debug flag), +0x10 the director it hands to the level loader. With the flag clear there is NO chapter page for this player.
constexpr uintptr_t kEquipDone_ActSelect = 0x8;
constexpr uintptr_t kEquipDone_Director  = 0x10;

// MewDirector+0x38 is the save's own property store (MewSaveFile, embedded); +0x78D is the debug "everything unlocked" byte the gear lock and the
// chapter page both read next to the two flags. mapflag_DesertUnlocked = chapter 2 may be chosen, mapflag_LabUnlocked = the three-chapter page.
constexpr uintptr_t kDir_SaveProps   = 0x38;
constexpr uintptr_t kDir_DebugUnlock = 0x78D;

constexpr const char kBtnName_ChapterLower[] = "Button_LowerDifficulty";
constexpr const char kBtnName_ChapterRaise[] = "Button_RaiseDifficulty";

// --- the loaded-scene list ---------------------------------------------------
//
// READ, NEVER CALLED. glaiel's own scene lookup is sub_1409CA630(Director*,
// std::string*): a linear scan of a std::vector<Scene*> whose begin/end sit at
// Director+0 and Director+8, comparing the std::string at Scene+1208 (size at
// +1224, capacity at +1232 -- the ordinary MSVC layout). sub_1409CA700 is the
// identical scan with one extra test, Scene+1243 == 0, and that byte is the
// "marked for destruction" flag: sub_1401BFE80 and sub_1401BFDC0 both set it on
// the current scene immediately before getting-or-creating the one they are
// transitioning to.
//
// Walking it ourselves costs three guarded reads per scene and needs no call
// target, no signature and no std::string of ours in the game's allocator. It
// is also self-proving: the names come back in the log, so a moved offset shows
// up as garbage rather than as a confident wrong answer.
//
// Director* is Component+40, with three readings that agree --
// MewDirector::IsTutorialRunActive @ 0x1403AECB0 (*(this+40) -> the "Cutscene"
// lookup), PauseMenu::SetupMainSidebar (*(this+40) -> the "House" lookup), and
// the MainMenu transitions above (*(MainMenu+40) -> get-or-create "MainMenu").
constexpr uintptr_t kDir_SceneDirector   = 40;    // MewDirector+40 = Director*
constexpr uintptr_t kDirector_ScenesBegin = 0;    // Scene**
constexpr uintptr_t kDirector_ScenesEnd   = 8;    // Scene**
constexpr uintptr_t kScene_Name           = 1208; // std::string
constexpr uintptr_t kScene_Destroying     = 1243; // u8, 1 = being torn down

// 'Map' -- THE ADVENTURE MAP, and a scene name that is not in the out-set above
// because being on the map means being INSIDE the run. Read live from the running
// game's scene vector on 2026-09-22 while the host stood on the map:
//
//   [2] scene 0x18C800064A8 'Map' dying=00
//
// It is the second half of "the run has actually started", next to the chapter
// page -- and it is the half that matters for a RESUMED run, which loads straight
// onto the map without ever passing through the House or the chapter screen. The
// first version of that test only knew about the chapter page, so a resumed run
// satisfied nothing and the client waited on the main menu forever.
constexpr const char kScene_Map[] = "Map";

// The three scene names that mean "this peer is NOT inside an adventure". All
// three are used as scene keys by the game itself: "House" and "Cutscene" in
// PauseMenu::SetupMainSidebar's lookups, "MainMenu" in the two transition
// bodies at 0x1401BFDC0 / 0x140756DE0, "SaveSelectionScreen" in sub_1401BFE80.
//
// MewDirector::ContinueAdventure's first statement is DestroyScene("House"),
// which is what makes the House test decisive rather than suggestive: starting
// an adventure explicitly tears that scene down.
constexpr const char kScene_House      [] = "House";
constexpr const char kScene_MainMenu   [] = "MainMenu";
constexpr const char kScene_SaveSelect [] = "SaveSelectionScreen";

// glaiel::Brain -- the Character the brain drives. Read off the two sites in
// Brain::UpdateDecision described in the PREVIEWFACE target above: one passes
// [rdi+38h] straight to Character::Face, the other walks it +0x60 -> +0x48,
// which is the Character -> TacticsObject -> tile chain.
constexpr uintptr_t kBrain_Character = 0x38;

// The engine's cached mouse position: two doubles, x then y, in LOGICAL window
// coordinates. sub_14097E3D0 (the input tick) fills it from SDL_GetMouseState,
// and deliberately reuses the stored value on frames where it does not ask --
// so reading the cache is reading exactly the position the game is acting on,
// which calling SDL again would not be.
constexpr uint32_t kRva_MouseCache = 0x012FBE80;

// SDL_GL_SwapWindow's slot in the SDL_DYNAPI JUMP TABLE, and the reason the
// overlay does not use MinHook at all.
//
// SDL3 is compiled with its dynamic-API shim, so every SDL_* name in the binary
// is a thunk that reads a function pointer out of a table in .data:
//
//   SDL_GL_SwapWindow  @ 0x140B9B7D0   jmp cs:off_1412DE650
//   off_1412DE650      initially ->    sub_140B92850, the DEFAULT stub:
//                                        SDL_InitDynamicAPI();
//                                        return off_1412DE650();   <- the table
//
// The stub exists to force initialisation on first use and then get out of the
// way: SDL_InitDynamicAPI overwrites the whole table with the real
// implementations, so from the second call onward the thunk jumps straight past
// it. Splicing the stub therefore catches at most one call, made long before a
// session exists -- which is exactly what happened: the hook installed cleanly,
// the banner said so, and it never fired again.
//
// Writing the slot is both simpler and more honest than hooking anything. It is
// the indirection SDL_DYNAPI exists to provide, the table is in .data and
// already writable, and "call the previous value" is the trampoline for free.
constexpr uint32_t kRva_SdlSwapSlot = 0x012E7650;

// THE MOUSE AND THE VIEWPORT ARE NOT THE SAME RULER, and these two slots are
// how the overlay finally stopped guessing at the conversion.
//
// SDL_GetMouseState -- which is what the engine's mouse cache holds -- reports
// LOGICAL window coordinates. glGetIntegerv(GL_VIEWPORT) reports FRAMEBUFFER
// pixels. On a display with OS scaling the two differ by the scale factor, so
// dividing a mouse coordinate by the viewport understates the fraction by
// exactly that factor: at 200% scaling the mouse at the bottom of the window
// reports 0.5, and a peer draws the pointer halfway up the screen.
//
// The stopgap this replaces normalised against the largest mouse coordinate
// seen so far, floored at the viewport. That can only ever GROW the divisor, so
// it covered the case where the mouse space is larger than the framebuffer and
// was blind to the case where it is smaller -- which is the common one, because
// OS scaling makes logical coordinates smaller, never larger.
//
// So ask SDL, which is the only component that actually knows both numbers.
// SDL_GetWindowSize gives logical units (the mouse's space) and
// SDL_GetWindowSizeInPixels gives the framebuffer's -- their ratio IS the scale
// factor, and it can be cross-checked against the viewport for free.
//
// CALLING an SDL function has the same trap as hooking one, in mirror image:
// the address behind a thunk STATICALLY is the DEFAULT stub, and the one that
// gets replaced. Resolving SDL_GetWindowSize that way once yielded a stub
// belonging to a NEIGHBOURING function -- every stub shares a prologue, so the
// pinned-build check passed -- which ran the wrong SDL function on our
// arguments and threw std::bad_alloc on the first frame.
//
// The slot is the answer to both problems. Read it at RUNTIME and you get the
// real implementation that SDL_InitDynamicAPI installed; write it and you have
// intercepted the function. Both RVAs below were decoded from the thunk's own
// `jmp cs:off_...` operand, and the same decode reproduces kRva_SdlSwapSlot
// above exactly -- which is what makes them trustworthy rather than counted.
//
//   SDL_GetWindowSize          @ 0x140B9CF10   jmp cs:off_1412DF170
//   SDL_GetWindowSizeInPixels  @ 0x140B9CF20   jmp cs:off_1412DF178
constexpr uint32_t kRva_SdlGetWindowSizeSlot   = 0x012E8170;
constexpr uint32_t kRva_SdlGetWindowSizePxSlot = 0x012E8178;

// --- the game's own mouse cursor -------------------------------------------
//
// The cursor is NOT a SWF clip the way the board pips are: it is a texture in
// textures/cursor/<state>.png, picked by NAME. glaiel::SetCursor @ 0x1409B09B0
// takes (std::string state, int priority) and stores the winner on the Cursor
// singleton; the Cursor's own late-update @ 0x1409B0900 then copies that string
// onto the ApplicationBase every frame and resets the priority to INT_MIN, so
// the state is re-elected from scratch each frame by whoever cares.
//
// That published copy is what we read. It is the one place the CURRENT cursor
// state exists as a plain string, and reading it costs nothing -- no hook, no
// call, and no dependence on which of the ~18 SetCursor call sites won.
//
// The shipped state names are exactly the file names in textures/cursor:
// default, attack, spell, move, invalid, examine, question, grab, grabr, heal,
// btn_over, pet_frame1..4, plus _hastargets variants of attack/spell/move/heal.
constexpr uint32_t  kRva_ApplicationBase = 0x013C4A30;   // ApplicationBase**
constexpr uintptr_t kApp_CursorState  = 3392;  // std::string, the state name
constexpr uintptr_t kApp_CursorShown  = 3424;  // bool, cursor visible this frame

} // namespace mgmp
