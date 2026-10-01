// mgmp_listprobe.h -- DEBUG INSTRUMENT: name the code that changes the battle
// character list.
//
// WHY THIS EXISTS, AND WHY IT IS A HARDWARE WATCHPOINT RATHER THAN A HOOK.
//
// The question is "who appends to (or removes from) the battle's character list",
// and it is asked because that list is the one thing a per-battle roster choice
// would have to be built through (see the M0 notes). Three static instruments
// were tried and all three were defeated by the same fact:
//
//   * scanning for the disp32 0x1F90 finds 159 sites, and the ONLY one that takes
//     the address of that field belongs to a DIFFERENT object of a different
//     layout (its +0x1F90 is a 12-byte-element vector; the two helpers it calls,
//     0x140058910 and 0x140052780, are that vector's clear and a std::string
//     reset). So offset scanning cannot even tell which +0x1F90 it is looking at.
//   * there is no literal displacement to scan for in the first place: the
//     compiler folds constants into the base register (`base = obj + 0x1000`,
//     `disp = 0xF90`), which is exactly what makes a displacement-keyed search
//     the wrong key.
//   * hooking "the append helper" needs the helper's address, and the helper is
//     only findable by its shape -- and the shape that was found belongs to the
//     other container type.
//
// A watchpoint sidesteps all of it. The value being watched lives at a KNOWN
// offset of a KNOWN object (the chain mgmp_lockstep already walks), and the trap
// fires on the WRITE, wherever it comes from -- helper, inlined store, or a
// function nobody has ever looked at. The fault's RIP names the instruction that
// did it, and a stack scan names its callers.
//
// WHAT IS WATCHED, AND WHY IT MOVED OFF THE LIST ITSELF
//
// The scene object `sub` carries TWO parallel tables, both indexed by the same
// slot number, and 0x1F90 is slot 505 of each:
//
//   sub+0x20 -- the LIVE table: 16-byte entries, each holding a {container*, valid}
//               pair. A container is {refcount, pad, cap, count, data} and holds
//               Character*. This is what every consumer reads, including the mod.
//   sub+0x18 -- the SOURCE table: 16-byte entries holding {cap, count, array}
//               inline. A one-line disassembly of the caller settled it: before
//               reading the list from the live table, fn 0x35D0B0 calls
//               0x96B470(obj, 505), which releases the old container, allocates a
//               fresh one, and copies the SOURCE entry into it -- filtering each
//               candidate on three bytes of the candidate itself.
//
// So the live list is a per-turn COPY of the source, which makes the SOURCE the
// interesting address: a per-battle roster choice is a change to what the source
// says. The slots are:
//
//   0. holder+0x1F90    (4B, write) the live table's container pointer -- fires
//                       when the battle replaces its list, which is the event
//                       that made an earlier run conclude the list was "rebuilt".
//   1. src+0x1F90+4     (4B, write) the source's count.
//   2. src+0x1F90+8     (4B, write) the source's array pointer.
//   3. live+0x0C        (4B, write) the LIVE container's count.
//
// READ THE the design notes SECTION "Who fights: one authoritative layer and one derived
// copy" BEFORE TRUSTING THE WORD "source" HERE. Both of these tables turned out to
// be DERIVED: rebuilt for each battle and released with it. The layer that actually
// decides is the run's cat id vector at MewDirector+1468/+1472. The instrument found
// the writers of a copy -- which is why its question has an answer that did not
// settle the design. What it did that mattered was to watch the list be rebuilt at
// all, and to say so in a way that could be re-read a day later.
//
// Slots 1..3 used to all watch the live container (cap/count/data). That question
// is answered -- 0x96B470 is the writer and it runs per turn -- so 1 and 2 were
// re-aimed at the one side that is still unknown, whoever writes the SOURCE, and
// 3 was kept on the live count for one reason: if the source turns out to be
// quiet during a battle, slot 3 is what still proves the copy ran and how many
// characters its three filters let through. Without it, a silent round would have
// two readings and no way to choose between them.
//
// Each hit is logged with: the slot, the RIP that followed the write, 16 bytes
// either side of it for offline disassembly, the field values, and the caller
// chain unwound with the image's own .pdata -- which names the exact function and
// offset that made the call, where a raw stack scan cannot tell a return address
// from a saved pointer.
//
// LIMITS, STATED PLAINLY
//
//   * Debug registers are PER THREAD. The probe arms on the game's main thread,
//     which is the one the frame hook runs on and the one a battle runs on. A
//     roster change made from another thread is missed.
//   * The initial fill of the FIRST battle can be missed: arming needs a
//     TurnControl, and the first one is handed over at a turn boundary, by which
//     time that battle is already spawned. Everything after it -- summons,
//     removals, and any later battle whose objects are reused -- is caught.
//   * A HANDLER CANNOT RE-ARM THE REGISTERS. Windows discards DRx changes made by
//     an exception handler for its own thread (the kernel reloads DR7 from the
//     state saved when the #DB was taken), so a race cannot be closed from inside
//     the trap -- and a read-back taken there cannot see the difference, because
//     the pending context reads back as written. Measured in a process of its own
//     (_buildcheck/m0_dr_intrap.cpp). All arming happens in the tick.
//   * The registers can be TAKEN AWAY while the probe runs, and when they are,
//     the log looks exactly like "the game never writes this field". slots_live()
//     re-reads them every half second, re-arms, and says so.
//   * It observes only. It never writes to game memory and never redirects
//     control flow: the handler logs, clears the trap, and continues.
//
// ---------------------------------------------------------------------------
// WHAT IT FOUND -- so nobody runs it again for the same answer
//
// 2026-09-21: the list at `holder+0x1F90` is CLEARED AND REFILLED whenever
// membership changes, not built once; the appender (rva 0x96B52B) filters each
// candidate on three bytes of the candidate itself, which is the game's own
// "still in the world" test; and both the appender and the installer (rva
// 0x96B49A, which takes the field offset in r8) are reached INDIRECTLY, so their
// caller exists only in a running stack. The full write-up, the table of
// addresses and the consequence for a per-battle cat selection are in the design notes
// under "The battle character list is rebuilt, not built".
//
// 2026-09-21, LATER -- the same day, a second run, and the whole shape changed.
// The caller chain of an install hit broke at fn 0x35D0B0, whose first three
// lines gave the mechanism away: `mov edx, 0x1F9` then `call 0x96B470`, and only
// then does it read [sub+0x20]+0x1F90. 0x96B470(obj, index) is:
//
//     release the container in  [obj+0x20] + index*16   (refcount, then free)
//     allocate a fresh 24-byte one, install it, refcount = 1
//     copy every element of     [obj+0x18] + index*16   into it
//     skipping a candidate unless it is non-null and [c+0x10] != 0 and
//            [c+0x0F] == 0 and [c+0x0E] != 0
//
// Both tables hang off the same object the probe's chain already reaches (`sub`),
// and 0x1F90/16 == 505 is the same slot in both. Read live in the running game:
// source {cap 42, count 32} and container {cap 42, count 32} held the same 32
// Character* -- so at any instant the two agree, the copy loop is what enforces
// it, and the SOURCE is the definition the battle list is derived from.
//
// Consequence, and the reason this file's slots were re-aimed: the battle's cat
// list is not where membership is decided. Whoever writes the source entry at
// index 505 decides who fights, and the copy loop's three flag bytes are the last
// filter. That is the layer a per-peer subset has to touch. The instrument now
// watches the source instead of the copy -- and, having learned that a handler
// cannot re-arm anything, it arms only from the tick.
//
// ---------------------------------------------------------------------------
// THREE WAYS IT CAN LIE WHILE IT RUNS
//
// All three were observed on the way to that answer, and all three look exactly
// like "the game never writes this field":
//
//   1. SetThreadContext can accept DR0-DR7 and not apply them. No error, no
//      trap, no log line. program_slots() therefore READS THEM BACK and fails
//      loudly, and listprobe_init's self-test proves on the probe's OWN memory
//      that a watchpoint traps at all before anybody trusts a silence.
//   2. THE DE-DUPE BELOW. It prints a site's first five writes and then one in
//      two hundred -- which silently ate the whole of the second battle's roster
//      fill, because the site had already used its budget in the first.
//      FIXME(before the next run): key the counter on the watched ADDRESS as
//      well as the rip, so each new container gets its own budget.
//   3. A watchpoint left armed on a vector the game has freed reports every
//      unrelated write the allocator makes through those bytes -- 136 of them in
//      one log, from a module that is not even the game. The tick now disarms
//      when the holder is released and sanity-checks the container before arming
//      on it.
//   4. AN INSTRUMENT AIMED AT A SHARED CHUNK FIRES FOR EVERYBODY ELSE TOO, and
//      a *fourth* failure is what that cost: an execute breakpoint on the append
//      entry, added to read the registers it is not passed in, was taken ~27,000
//      times a second by an unrelated hot loop on the main menu alone and HUNG
//      THE GAME. Two lessons, both now enforced: a code address found by
//      observation is checked against its recorded prologue before it is used,
//      and an execute breakpoint is a FAULT whose resume depends on the resume
//      flag -- never point one at a hot path. See tune::kProbeAppend.
//
//   5. THE PROBE'S OWN BELIEF ABOUT ITS REGISTERS. This is the one that cost the
//      most, because it was invisible in the log AND in the read-back: an in-trap
//      re-arm reports success and is then discarded by the kernel, so slot 0 kept
//      firing while slots 1..3 were empty and every later write went unseen. The
//      log said "re-armed INSIDE the trap" in good faith. Fixed by not doing it
//      there (see the measurement note in mgmp_listprobe.cpp) and by verifying
//      DR0..DR3/DR7 from the tick, where a failure is observable.
//
// The replacement for "which caller wrote this" is `log_callers`: the stack is
// unwound with the image's own .pdata (RtlVirtualUnwind), so a hit names the
// exact function and offset that made the call. A raw stack scan cannot do that
// -- it cannot tell a return address from a saved pointer, and it was reporting
// the frame that matched a pattern rather than the frame that called.
//
// It is gated on tune::kRosterProbe, OFF by default, and is meant to be switched
// on only for a question the log cannot otherwise answer.
#pragma once

namespace mgmp {

// Installs the vectored handler. Called once from hooks_install.
void listprobe_init();

// The TurnControl* is in hand in exactly one place -- the NextTurn hook -- so the
// probe is handed it there and remembers it.
void listprobe_set_turn_control(void* turn_control);

// Every frame, from the frame hook: re-walks the chain and re-arms when the
// battle has replaced its holder or its vector.
void listprobe_tick();

// Clears the debug registers. Not called on the normal path; kept so the probe
// can be turned off without leaving watchpoints behind.
void listprobe_shutdown();

// --- WATCH ONE ADDRESS OF SOMEBODY ELSE'S OBJECT (2026-09-23) ------------------------
//
// The roster probe above owns all four slots and re-aims them whenever the battle chain
// moves. This borrows one slot for a question the roster probe has no business in: WHO
// WRITES LevelUpScreen+0xA0, the level-up screen's subject CatData*.
//
// That question is the upstream half of "each peer levels its own cats". Changing the
// pointer from mgmp_choice works for that module's own logic (owner, choose-locally, the
// placeholder stopping) but not for the picture, the stat numbers or the option roll --
// measured: the old subject is held at NOTHING ELSE in the screen object, so all three come
// from the game's pending-level-up state. Whoever writes this field is where they would all
// follow together.
//
// Safe to call every frame: the roster tick only rewrites slots when the chain CHANGES, and
// after a battle the chain is settled. `what` must be a string literal: the probe stores the
// pointer, not the text.
void listprobe_watch_qword(int index, const char* what, const void* addr);

// --- AND ONE EXECUTE BREAKPOINT, FOR THE QUESTION A WATCHPOINT CANNOT ASK (2026-09-23) ---
//
// "Who creates a level-up screen" cannot be answered with a watch: the screen object does
// not exist yet, so there is no address to watch. It can be answered at the instruction that
// INSTALLS the screen's vtable -- `lea rax,[rip+vt]; mov [rcx],rax` at RVA 0x3C6CFB, found
// from LVLUPD's slot (9) in the class's vtable at 0xF160E0. That store runs once per screen,
// so an execute breakpoint there is a cold path, and a hit names the instruction's callers --
// i.e. whoever opens a level-up screen, which is the draw.
void listprobe_watch_exec(int index, const char* what, const void* addr);

// --- THE LEVEL-UP SCREEN'S OWN FIELDS, WATCHED FOR THE WRITE THAT FILLS THEM (2026-09-23) --
//
// What is wanted: make the level-up panel show THIS peer's cat. What is known, all measured:
//
//   * the panel's content is a BY-VALUE COPY (name, numbers, the four options) taken when the
//     screen is built -- the drawn cat is held at "NOTHING ELSE" in the screen object, so the
//     picture cannot be re-aimed afterwards, and a re-point of +0xA0 changes this module's own
//     reading of the screen while the player keeps seeing the drawn cat;
//   * the screen object is REUSED: the client's second level-up ran at the SAME address as its
//     first, which is why the panel builder's execute breakpoint never fired (the object was
//     never built again) and why a write watch on its first qword never fired either (a vtable
//     is only written when an object is created);
//   * therefore, while the panel cannot be re-aimed AFTER the fact, the FIELDS ARE REWRITTEN
//     for each new level-up -- and by then this module knows the screen's address.
//
// So this watches the screen's own layout: +0xA0 (the subject this module already reads),
// +0x360/+0x368 (the option array's begin and end). Whoever writes them is the code that fills
// the panel, named by RIP plus its .pdata caller chain -- the address that six static and
// runtime instruments failed to find, obtained from the inside instead of from outside.
//
// Safe to call repeatedly: the last screen handed over is the one watched, and the slots are
// armed from the probe's tick (the only place a re-arm survives -- see the file's own note).
void listprobe_watch_screen(const void* screen);

// Every button the game updates, once each, by name AND by callback.
//
// A separate switch from the roster probe because it answers a different kind of
// question -- "what are these screens CALLED, and who does the click" rather than
// "who writes this field" -- and because naming the run's setup flow (party,
// equipment, chapter, the one part of starting a game the mod does not touch)
// needs no debug registers at all. Called from the Button::update detour, which
// already fires for every button in the game.
//
// The callback half is the load-bearing part. Those button names turn out to be
// entries in a resource symbol pool -- neighbours of HouseStatusUI and
// object_cattree1, with no code reference anywhere in the image, because the
// engine looks them up by name rather than by address -- so the literal is a dead
// end and `Button + 240` (the std::function MenuPanel::register_button stored,
// whose vtable slot 2 is what the click runs) is the way to the commit function.
// Self-checking: MainMenu_Button_Play must report rva 0x1BEBE0.
void listprobe_on_button(void* button);

} // namespace mgmp
