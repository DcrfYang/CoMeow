// mgmp_config.h -- mgmp.json, read from the directory the DLL lives in.
//
// Everything the mod can be told at RUNTIME is here, and it is deliberately
// short. The settings that used to be in the file and are not any more live in
// mgmp_tuning.h as constants; that header explains the test they failed.
#pragma once

#include <cstdint>
#include <string>
#include "mgmp_addresses.h"

namespace mgmp {

struct Config {
    // log: path to the text trace, relative to the DLL directory or absolute.
    // Per-peer, because two instances in one directory would otherwise fight
    // over one file.
    wchar_t  log_path[512] = {};

    // --- net -----------------------------------------------------------------

    // net.role = off | host | client. Anything else leaves multiplayer dormant,
    // which is the default: an unconfigured mgmp.json must not open a socket.
    char     net_role[16]  = "off";
    char     net_addr[64]  = "127.0.0.1";   // client only
    uint16_t net_port      = 27600;
    bool     net_steam     = true;     // net.steam: also offer / try Steam peer-to-peer as a way to reach the host (no open port needed)

    // net.control = "auto", or an array of cat indices into the battle's own
    // character list. auto derives the split from the roster and is the only
    // usable setting for a whole run -- explicit indices cannot be known before
    // the battle exists and are stale the moment you fight anything else.
    //
    // An empty array is legal and means "this peer decides for nothing", a pure
    // observer. That is NOT the same as "auto".
    uint8_t  net_control[32]   = {};
    uint32_t net_control_count = 0;
    bool     net_control_auto  = true;

    // --- the lobby / signaling server ----------------------------------------
    //
    // Every instance now launches NEUTRAL. mgmp.json no longer has to say which
    // machine is the host, and the shipped config does not say it: both players
    // run the same file. The role is decided in the lobby instead -- whoever
    // creates a room is the host, everyone who joins it is a client -- and that
    // is the only arrangement in which two identically configured machines can
    // still end up on opposite sides of one session.
    //
    // These keys are the lobby panel's starting values, not a mode.
    bool     signal_on   = true;              // signal.enabled
    char     signal_addr[64] = "127.0.0.1";   // signal.addr -- the server
    uint16_t signal_port = 27700;             // signal.port
    char     signal_name[32] = {};            // signal.name, "" = the computer name
    char     signal_room[32] = {};            // signal.room, "" = no automatic room
    bool     signal_auto_connect = false;     // signal.auto_connect

    // The pre-lobby launch-time role: kept, and OFF.
    //
    // net.role = host|client still means exactly what it always did, but it is
    // only consulted when this is on. The normal path is the lobby, and a file
    // that still names a role is warned about rather than obeyed -- because
    // obeying it is what made the two peers unequal at startup. The one caller
    // that turns it back on is tools/net_test.ps1: a scripted loopback run has
    // nobody to click anything.
    bool     legacy_role = false;             // signal.legacy_role

    // --- debug ---------------------------------------------------------------
    //
    // The four switches below are the ones tools/net_test.ps1 flips, which is
    // the whole reason they are still in the file rather than in mgmp_tuning.h.

    // debug.follow = false stops the client following the host through the map
    // and stops its own map clicks being swallowed -- how you drive the two
    // instances to the same battle by hand. (-NoFollow)
    bool     net_follow = true;

    // debug.follow_delay_ms makes the CLIENT sit on the host's node choice for
    // that long before following it, manufacturing the late-join gap on demand.
    // The only setting in the mod that exists purely to provoke a bug.
    // (-LateClient)
    uint32_t net_follow_delay_ms = 0;

    // debug.roster_shrink drops N cats from the run's battle list, once per
    // process, at the next map tick -- i.e. between nodes, with no battle reading
    // the list. It is the test for mgmp_roster's primitive: the source entry of
    // slot 505 is written and the game's own resync function is called, so if the
    // live list follows, "who fights" is writable and a per-peer party can be
    // built on it.
    //
    // IT EDITS THE RUN and the log says so at Warn, with the counts on both sides
    // of the change. 0 = off, which is the default everywhere. (-RosterShrink N)
    uint32_t roster_shrink = 0;

    // debug.join_barrier = false lets each peer start deciding the moment it
    // has a roster instead of waiting for the other to reach the same battle.
    // A barrier that never lifts is a stall, so being able to take it out of
    // the picture is worth the line. (-NoBarrier)
    bool     net_join_barrier = true;

    // debug.desync_halt = false turns a per-turn hash mismatch from a halt into
    // a report. It answers the one question a halting run structurally cannot,
    // because halting destroys the evidence for it: is the divergence real or
    // transient? A "turn N AGREES again" line after a mismatch is that answer.
    //
    // Scoped to the hash compare only. Every other halt still halts, because
    // those are conditions where continuing means injecting an action into the
    // wrong cat rather than watching two numbers drift. (-NoHalt)
    bool     net_desync_halt = true;

    // --- the recorder --------------------------------------------------------

    // debug.record opens the binary event stream and implies the RNG hooks --
    // a recording without them is a stream of actions and no draws, which is
    // silently useless and only noticed after playing a whole battle.
    bool     record            = false;
    wchar_t  record_path[512]  = {};   // derived from the log name
    char     record_note[256]  = {};   // stored in EV_META

    // debug.replay = "<file>" injects that capture's decisions instead of
    // polling the brain. Empty means no replay. Meant to be used WITH record:
    // run B replays run A while recording its own stream, and the two captures
    // are diffed. Enabling only replay reproduces a battle and measures
    // nothing.
    wchar_t  replay_path[512]  = {};

    // debug.steam_test: "probe" | "host" | "join:<SteamID64>" -- the Steam P2P networking experiment (mgmp_steamtest.h).
    // Developer only: ignored without ui.dev_tools.
    char     steam_test[96]    = {};

    // --- debug mode ----------------------------------------------------------
    //
    // One switch for "this process is a test rig, not a play session". What it
    // turns on is DESTRUCTIVE and it is the only thing in the mod that writes
    // to the player's save directory on its own initiative: on the way out, the
    // slot named by slot_to is overwritten with a copy of slot_from, so the
    // next launch starts from a known baseline.
    //
    // Slots are the game's own filenames -- slot 1 is steamcampaign01.sav -- and
    // the copy is a plain file copy, because a .sav is a self-contained SQLite
    // database with no slot identity inside it.
    //
    // mgmp_loader.exe --debug overrides this for one launch through a marker
    // file; see mgmp_debug.h for the order and for why the marker does not
    // outlive the launch that wrote it.
    bool     debug_mode = false;
    char     debug_slot_from[64] = "steamcampaign01.sav";
    char     debug_slot_to[64]   = "steamcampaign02.sav";

    // --- the debug panel -----------------------------------------------------

    bool     ui         = true;    // ui.enabled
    bool     ui_visible = true;    // whether it starts open; ui.key toggles
    uint32_t ui_key     = 0x70;    // ui.key, "F1" or a raw VK code
    bool     ui_test    = false;   // ui.test_harness -- DEV ONLY, see mgmp_uitest.h

    // --- release safety (2026-10-01, first test release) ------------------------------------------------
    // ui.dev_tools = false (the default) takes every developer instrument out of play: the panel's cheats (999 damage,
    // node jump), debug mode (which overwrites a save slot on exit), roster_shrink, the test aids, the UI test
    // harness, and every debug.* switch that makes the two peers play differently (follow, join_barrier, desync_halt,
    // follow_delay_ms, record, replay). The file may still name them; they are ignored and the log says so.
    bool     dev_tools = false;        // ui.dev_tools
    // ui.beta_notice: show the "this is a test release, back up your saves" dialog at startup. The dialog's "do not show
    // again" box writes false back into mgmp.json (config_set_beta_notice); set it to true there to see it again.
    bool     beta_notice = true;
    // ui.block_new_cats: the event effect "a new cat joins the run" does nothing. Off by default (the game as it is); set from the main menu's
    // mod settings. Part of the handshake's ruleset: both players must agree.
    bool     block_new_cats = false;
    // debug.test_weaken: every enemy to 1 hp at the first usable turn (a test aid). Needs dev_tools, and both peers
    // must agree -- it is part of the ruleset the handshake compares.
    bool     test_weaken = false;
    // debug.allow_unpinned_build: hook a Mewgenics.exe whose PE timestamp is not the one the offsets were measured on.
    // Off: the mod refuses to touch such a build (a patched game with moved structures would be corrupted silently).
    bool     allow_unpinned = false;

    // --- derived, not parsed -------------------------------------------------

    // Whether the load-time hook rules installed the whole co-op surface.
    //
    // It is true for every instance now, because the lobby can make any of them
    // the host and the hooks that a role = off launch used to skip would then
    // have to go in late, from another thread. Kept as a field because the one
    // caller that CARES is the late-install path itself: with this true,
    // hooks_install_late returning zero means "nothing was left to do", and
    // reporting that as a failure -- which is what it used to do, in red, on
    // every single session start -- is worse than saying nothing.
    bool     coop_implied = false;

    // Which hooks are installed. No longer settable from the file: the defaults
    // plus the forcing rules below them are the only combinations that were
    // ever correct, and an ini that turned one off by hand produced failures
    // that looked like missing features (see the hook_sfstoreblob note in
    // mgmp_config.cpp).
    bool     hook[T_COUNT] = {};

    // Anything the parser could not make sense of, printed in the startup
    // banner: a misconfigured mod must announce itself, not fail silently.
    char     parse_warnings[512] = {};
};

// dll_dir must be the directory containing mgmp.dll (no trailing slash).
void        config_load(const wchar_t* dll_dir);
const Config& config();

// THE PANEL'S CONNECT BUTTONS ARE A ROLE, AND THE ROLE IS READ FROM HERE.
//
// net.role decides two separate things at load time: which hooks get installed
// (hooks_implied_by), and -- in savefile, catsync, invsync, runhist and aim --
// whether the module arms at all. The panel's host/join buttons used to change
// neither: they opened a socket and set the LIVE role in mgmp_net, so
// net_role() said "host" while config().net_role still said "off".
//
// The result was a session that looked connected and was half dead. Cursors,
// map-follow, choice and lockstep read net_role() and worked; the save, the
// cats, the inventory and the run history read config().net_role and silently
// stayed off, so the client connected, saw the host's mouse, and never received
// the run. Reported from the wild on 2026-08-28, and it is the shipped default
// path -- mgmp.json.template says "off" and the panel is the obvious way in.
//
// Called by session begin(). Returns true if the role actually changed, which
// is the caller's cue to install the hooks that were skipped at load time.
bool        config_set_role(bool host);

// The loader's --debug / --no-debug marker, applied once at init by mgmp_debug.
// Returns true if it actually changed the value, which is the caller's cue to
// log which source won: "the command line said something different from the
// file" is the first thing a surprising debug run has to be able to explain.
bool        config_set_debug_mode(bool on);

// The beta-notice dialog's "do not show again" box. Updates the live value and rewrites ONLY that one setting in
// mgmp.json in place (comments and everything else are kept; the key or the "ui" block is added when missing, and a
// missing file is created). Returns false if the file could not be written -- the live value is changed regardless, so
// the dialog still stays closed for this run.
bool        config_set_beta_notice(bool on);
// ui.block_new_cats, in memory and in mgmp.json (the same careful rewrite as beta_notice). False when the file could not be written.
bool        config_set_block_new_cats(bool on);
// The text rewrite behind both: `text` with ui.<key> set to `on` (added when absent), or an empty string for a file that is not an object.
std::string config_rewrite_ui_bool(const std::string& text, const char* key, bool on);
// The text rewrite behind it, exposed for tests: returns `text` with ui.beta_notice set to `on`, or an empty string if
// the text is not an object it can edit.
std::string config_rewrite_beta_notice(const std::string& text, bool on);

} // namespace mgmp
