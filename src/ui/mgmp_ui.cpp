#include "mgmp_checkpoint.h"
#include "mgmp_ui.h"
#include "mgmp_menu.h"
#include "mgmp_uitest.h"      // the player-facing menus: drawn in this same frame
#include "mgmp_leave.h"      // the co-op session decision -- see the panel section below

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_opengl3.h"

#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_session.h"
#include "mgmp_signal.h"
#include "mgmp_lockstep.h"
#include "mgmp_cursor.h"     // cursor_hovered_tile, for the debug action
#include "mgmp_battleid.h"   // kNoBattle
#include "mgmp_follow.h"
#include "mgmp_combatlock.h"
#include "mgmp_leave.h"
#include "mgmp_choice.h"     // choice_level_note -- which cat this peer is really levelling

// Declared by imgui_impl_win32.h's owner rather than the header itself.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HDC(__stdcall* fn_wglGetCurrentDC)(void);

namespace mgmp {
namespace {

// --- state ------------------------------------------------------------------

struct UI {
    bool     enabled   = false;   // ui = 1 and the subclass installed
    bool     visible   = true;
    bool     ready     = false;   // ImGui context + backends alive
    bool     failed    = false;   // init tried and lost; do not retry every frame
    uint32_t toggle_vk = 0x70;

    HWND     hwnd      = nullptr;
    WNDPROC  prev_wndproc = nullptr;

    // The pane's own copy of the log. Fed from log_ring_fetch by cursor, so a
    // quiet frame copies nothing.
    std::deque<LogEntry> lines;
    uint32_t cursor  = 0;
    uint32_t dropped = 0;      // lines that fell out of the ring unseen

    // Filters.
    bool show_level[5] = { false, true, true, true, true };   // Trace off by default
    ImGuiTextFilter text_filter;
    // Tags seen so far, in first-seen order, and whether each is shown.
    std::vector<std::string> tags;
    std::vector<bool>        tag_on;
    bool autoscroll = true;

    float font_scale = 1.0f;

    // Seeded from mgmp.json at ui_init, so the fields already hold whatever the
    // launch was configured for and a reconnect is one click.
    char addr[64] = "127.0.0.1";
    int  port     = 27600;

    // Reused every frame so the filtered pass allocates nothing after warm-up.
    std::vector<uint32_t> filtered;

    // Centre the node list on the marker: once automatically when the map first
    // appears, and again whenever the button is pressed. A 100-node map is
    // otherwise a hunt for the one row that matters.
    bool     scroll_to_marker = true;
    uint32_t last_node_count  = 0;
};

UI g;

const Config& cfg() { return config(); }

// --- colours ----------------------------------------------------------------
//
// The five the user reads by shape before they read the words: gray is volume,
// white is the normal run, green is something agreeing or completing, yellow is
// odd-but-continuing, red is a halt or a feature switching itself off.

ImVec4 level_colour(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return ImVec4(0.50f, 0.50f, 0.52f, 1.0f);
        case LogLevel::Good:  return ImVec4(0.40f, 0.85f, 0.45f, 1.0f);
        case LogLevel::Warn:  return ImVec4(0.95f, 0.80f, 0.25f, 1.0f);
        case LogLevel::Error: return ImVec4(1.00f, 0.35f, 0.35f, 1.0f);
        case LogLevel::Info:
        default:              return ImVec4(0.88f, 0.88f, 0.90f, 1.0f);
    }
}

const char* level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Good:  return "good";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        default:              return "info";
    }
}

// A stable colour per tag, so LOCKSTEP and SAVEFILE are told apart at a glance
// in an interleaved log. Hashed rather than tabled: there are 24 tags today and
// a table would need editing every time one is added, which is exactly the kind
// of maintenance that silently stops happening.
ImVec4 tag_colour(const char* tag) {
    static const ImVec4 kPalette[] = {
        ImVec4(0.45f, 0.72f, 1.00f, 1.0f),  ImVec4(0.98f, 0.62f, 0.40f, 1.0f),
        ImVec4(0.60f, 0.85f, 0.60f, 1.0f),  ImVec4(0.85f, 0.60f, 0.95f, 1.0f),
        ImVec4(0.40f, 0.85f, 0.85f, 1.0f),  ImVec4(0.95f, 0.80f, 0.50f, 1.0f),
        ImVec4(0.75f, 0.75f, 0.45f, 1.0f),  ImVec4(0.55f, 0.65f, 0.95f, 1.0f),
        ImVec4(0.95f, 0.55f, 0.65f, 1.0f),  ImVec4(0.50f, 0.90f, 0.70f, 1.0f),
    };
    uint32_t h = 2166136261u;                       // FNV-1a
    for (const char* p = tag; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

// --- pulling the ring -------------------------------------------------------

void note_tag(const char* tag) {
    if (!tag[0]) return;
    for (const auto& t : g.tags) if (t == tag) return;
    g.tags.emplace_back(tag);
    g.tag_on.push_back(true);
}

bool tag_shown(const char* tag) {
    if (!tag[0]) return true;   // banner lines have no tag and are always shown
    for (size_t i = 0; i < g.tags.size(); ++i)
        if (g.tags[i] == tag) return g.tag_on[i];
    return true;
}

void pump_log() {
    LogEntry batch[256];
    uint32_t dropped = 0;
    for (;;) {
        uint32_t n = log_ring_fetch(g.cursor, batch, 256, &dropped);
        g.dropped += dropped;
        for (uint32_t i = 0; i < n; ++i) {
            note_tag(batch[i].tag);
            g.lines.push_back(batch[i]);
        }
        if (n < 256) break;   // drained
    }
    while (g.lines.size() > tune::kUiLogLines) g.lines.pop_front();
}

// --- the log window ---------------------------------------------------------

void draw_log() {
    ImGui::SetNextWindowSize(ImVec2(980, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("mgmp -- log")) { ImGui::End(); return; }

    // Severity row. Each checkbox is drawn in its own colour, so the legend and
    // the control are the same widget.
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, level_colour((LogLevel)i));
        ImGui::Checkbox(level_name((LogLevel)i), &g.show_level[i]);
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    ImGui::Checkbox("autoscroll", &g.autoscroll);
    ImGui::SameLine();
    if (ImGui::Button("clear view")) g.lines.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("(the file keeps everything)");

    g.text_filter.Draw("filter", 240.0f);

    // AND HERE, BECAUSE THIS IS THE WINDOW THE PLAYER IS LOOKING AT (2026-09-23, measured).
    // The note was first drawn in the session window, and the run that followed proved the
    // lesson the hard way: the level-up screen was up, the note had been filled, and the
    // player saw nothing -- their session window was closed and the log window was open.
    // A warning about a panel that lies is worth nothing if it is on a hidden window.
    {
        char note[256] = {};
        if (choice_level_note(note, sizeof(note)))
            ImGui::TextColored(level_colour(LogLevel::Warn), "!! %s", note);
    }

    if (ImGui::TreeNode("tags")) {
        for (size_t i = 0; i < g.tags.size(); ++i) {
            if (i && (i % 6)) ImGui::SameLine();
            bool on = g.tag_on[i];
            ImGui::PushStyleColor(ImGuiCol_Text, tag_colour(g.tags[i].c_str()));
            if (ImGui::Checkbox(g.tags[i].c_str(), &on)) g.tag_on[i] = on;
            ImGui::PopStyleColor();
        }
        ImGui::NewLine();
        if (ImGui::SmallButton("all"))  for (size_t i = 0; i < g.tag_on.size(); ++i) g.tag_on[i] = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("none")) for (size_t i = 0; i < g.tag_on.size(); ++i) g.tag_on[i] = false;
        ImGui::TreePop();
    }

    if (g.dropped) {
        ImGui::PushStyleColor(ImGuiCol_Text, level_colour(LogLevel::Warn));
        ImGui::Text("%u line(s) were written faster than this pane read them and "
                    "fell out of the ring", g.dropped);
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // Rebuilt every frame rather than cached against a filter signature: the
    // predicate is three comparisons over at most ui_log_lines entries, and a
    // cache whose invalidation is wrong shows stale lines during a desync,
    // which is precisely when the pane has to be trusted.
    g.filtered.clear();
    for (uint32_t i = 0; i < (uint32_t)g.lines.size(); ++i) {
        const LogEntry& e = g.lines[i];
        if (!g.show_level[(int)e.level]) continue;
        if (!tag_shown(e.tag)) continue;
        if (g.text_filter.IsActive() &&
            !g.text_filter.PassFilter(e.text) && !g.text_filter.PassFilter(e.tag))
            continue;
        g.filtered.push_back(i);
    }

    ImGui::BeginChild("scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));

    ImGuiListClipper clipper;
    clipper.Begin((int)g.filtered.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const LogEntry& e = g.lines[g.filtered[row]];

            ImGui::TextDisabled("%06u", e.seq);
            ImGui::SameLine();
            ImGui::TextDisabled("%04u", e.turn);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, tag_colour(e.tag));
            ImGui::Text("%-9s", e.tag);
            ImGui::PopStyleColor();
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, level_colour(e.level));
            ImGui::TextUnformatted(e.text);
            ImGui::PopStyleColor();
        }
    }
    clipper.End();

    ImGui::PopStyleVar();
    if (g.autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

// --- the session window -----------------------------------------------------

const char* state_name(NetState s) {
    switch (s) {
        case NetState::Idle:       return "idle";
        case NetState::Listening:  return "listening";
        case NetState::Connecting: return "connecting";
        case NetState::Connected:  return "connected";
        case NetState::Ready:      return "READY";
        case NetState::Failed:     return "FAILED";
        case NetState::Closed:     return "closed";
    }
    return "?";
}

ImVec4 state_colour(NetState s) {
    switch (s) {
        case NetState::Ready:                             return level_colour(LogLevel::Good);
        case NetState::Failed:  case NetState::Closed:    return level_colour(LogLevel::Error);
        case NetState::Idle:                              return level_colour(LogLevel::Trace);
        default:                                          return level_colour(LogLevel::Warn);
    }
}

// The lobby's colour rule is the session's, one level up: connected is good,
// connecting is odd-but-continuing, and a dial that failed is red because the
// next thing the player does about it is retype an address.
ImVec4 signal_colour(SignalState s) {
    switch (s) {
        case SignalState::Connected:  return level_colour(LogLevel::Good);
        case SignalState::Failed:     return level_colour(LogLevel::Error);
        case SignalState::Connecting: return level_colour(LogLevel::Warn);
        case SignalState::Closed:     return level_colour(LogLevel::Warn);
        case SignalState::Off:
        default:                      return level_colour(LogLevel::Trace);
    }
}

void row(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

// The map, and a button to enter any node on it.
//
// This EDITS THE RUN, which nothing else on this panel does, and it is here
// under protest: the reason it exists is that a run can end up standing at a
// node it has already entered but not resolved, unable to go in and unable to
// move on. Re-entering that node is the way out.
//
// Host only -- entering a node is a run decision. It goes through the ordinary
// EnterNode hook, so it publishes ENTERNODE and the client follows exactly as
// if the node had been clicked; and it is applied at the tail of
// MapScreen::update, the site the game's own UI callback uses.
void draw_nodes() {
    if (net_role() == NetRole::Client) return;   // the host owns the run

    const uint32_t count = follow_node_count();
    if (!ImGui::CollapsingHeader("map nodes")) return;

    if (!count) {
        ImGui::TextDisabled("no map screen yet -- the nodes appear once the run "
                            "is on the map");
        return;
    }

    // Three different answers to "which node", and they disagree in exactly the
    // situation the panel exists for.
    //
    //   marker    where the cats are STANDING -- read live from the game, so it
    //             is right for a run that was just loaded from disk
    //   selected  what is highlighted on the map
    //   entered   what this module watched somebody enter THIS SESSION, which
    //             is nothing at all after a reload
    uint32_t marker = 0, selected = 0, entered = 0;
    const bool have_marker   = follow_marker_node(marker);
    const bool have_selected = follow_selected_node(selected);
    const bool have_entered  = follow_current_node(entered);

    // A new map (different node count) re-arms the auto-centre.
    if (count != g.last_node_count) {
        g.last_node_count  = count;
        g.scroll_to_marker = true;
    }

    if (have_marker) {
        ImGui::TextColored(level_colour(LogLevel::Good),
                           "cats are standing at node %u of %u", marker, count);
        ImGui::SameLine();
        if (ImGui::SmallButton("scroll to")) g.scroll_to_marker = true;
    }
    if (!have_marker)
        ImGui::TextDisabled("marker node unknown (map not up, or the offset drifted)");

    if (have_selected && (!have_marker || selected != marker))
        ImGui::Text("selected: node %u", selected);
    if (have_entered && (!have_marker || entered != marker))
        ImGui::TextDisabled("last entered this session: node %u", entered);

    ImGui::TextColored(level_colour(LogLevel::Warn),
                       "entering a node from here is not a move the player made");

    if (!follow_on_map()) {
        ImGui::TextColored(level_colour(LogLevel::Trace),
                           "the map is not ticking -- a node is in progress; "
                           "finish it before jumping");
    }

    ImGui::BeginChild("nodes", ImVec2(0, 200), ImGuiChildFlags_Borders);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t    type = 0;
        uint64_t    seed = 0;
        const char* name = "?";
        if (!follow_node_info(i, type, seed, &name)) continue;

        ImGui::PushID((int)i);
        if (cfg().dev_tools && ImGui::SmallButton("enter")) {       // developer tool: edits the run
            follow_request_jump(i);
            log_line_lvl(LogLevel::Warn, "UI",
                         "panel requested a jump to node %u (%s)", i, name);
        }
        ImGui::SameLine();

        // The marker wins the colour: it is the one that answers "where am I".
        const bool is_marker   = have_marker   && i == marker;
        const bool is_selected = have_selected && i == selected;
        const bool is_entered  = have_entered  && i == entered;

        char mark[40] = {};
        if (is_marker)   strcat_s(mark, "  <== CATS ARE HERE");
        if (is_selected) strcat_s(mark, is_marker ? " (selected)" : "  <- selected");
        if (is_entered && !is_marker) strcat_s(mark, "  (entered)");

        const ImVec4 col = is_marker   ? level_colour(LogLevel::Good)
                         : is_selected ? level_colour(LogLevel::Warn)
                                       : level_colour(LogLevel::Info);
        ImGui::TextColored(col, "%3u  %-16s %016llx%s", i, name,
                           (unsigned long long)seed, mark);
        if (is_marker && g.scroll_to_marker) {
            ImGui::SetScrollHereY(0.5f);
            g.scroll_to_marker = false;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// The connect controls.
//
// Every button here only ever RECORDS a request; session_update applies it at
// the top of the next frame. Calling net_shutdown from inside the present path
// would tear a session down underneath a half-submitted frame.
void draw_connect() {
    // Kept, and NO LONGER THE WAY IN.
    //
    // The lobby decides the role now, so this folded-away path is the manual
    // one: what tools/net_test.ps1 drives, and the fallback when a session has
    // to be started by hand. Removing it would take the only way to point the
    // transport at an address without a server, and leaving it open would put
    // two ways to pick a role on one panel.
    if (!ImGui::TreeNode("manual connect (legacy)")) return;

    const bool busy = session_request_pending();
    const bool live = net_active();

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("addr", g.addr, sizeof(g.addr));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("port", &g.port, 0, 0);
    if (g.port < 1)     g.port = 1;
    if (g.port > 65535) g.port = 65535;

    ImGui::BeginDisabled(busy);

    if (ImGui::Button("host")) session_request_host((uint16_t)g.port);
    ImGui::SetItemTooltip("Listen on this port. If a session is already up it is"
                          " torn down first -- 'host' means start over.");
    ImGui::SameLine();

    if (ImGui::Button("join")) session_request_join(g.addr, (uint16_t)g.port);
    ImGui::SetItemTooltip("Dial addr:port. A client that only lost its socket"
                          " keeps its run and declines the host's save.");
    ImGui::SameLine();

    ImGui::BeginDisabled(!live);
    if (ImGui::Button("disconnect")) session_request_disconnect();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Drop the session. Pair it with 'join' to exercise"
                          " reconnect without restarting the process.");

    ImGui::EndDisabled();

    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("applying...");
    }

    // ARM THE LEAVE PATH BY HAND. This exists because the feature has three
    // stages that fail identically from the outside -- the host never announced,
    // the client declined, or the client armed and never saw the button -- and
    // pressing this skips the first two. If the pause menu is open and this does
    // nothing, the fault is the click; if it works, the fault is upstream.
    if (ImGui::Button("arm leave")) leave_request_local();
    ImGui::SetItemTooltip("Pretend the host announced that it left the run:"
                          " open the pause menu and the mod presses Quit To Menu."
                          " A test of the click half on its own.");
    ImGui::SameLine();
    // Live, beside the button, because every stage of this fails invisibly and
    // reading it out of the log afterwards has already cost two rounds of "it
    // did not work" with no way to say which half.
    char leave[192];
    leave_status(leave, sizeof(leave));
    ImGui::TextDisabled("%s", leave);

    if (session_last_action()[0]) {
        const char* a = session_last_action();
        ImGui::TextColored(a[0] == '!' ? level_colour(LogLevel::Error)
                                       : level_colour(LogLevel::Good),
                           "%s", a);
    }

    // Reconnect is the one open item with no live evidence behind it, and the
    // order below is the part that is easy to get wrong: a client that only
    // lost its socket DECLINES the host's save and keeps its run, so testing
    // the save transfer needs a peer whose process actually restarted.
    if (ImGui::TreeNode("what to expect")) {
        ImGui::TextWrapped(
            "Reconnect (socket only): the client keeps its run and declines the "
            "save. The host flushes its live run to disk, then sends CATDATA, "
            "INVENTORY, an ENTERNODE for the node the run is standing in, and "
            "every ACTION of the battle in progress. A joiner visibly replays "
            "the fight from turn 0.\n\n"
            "If turn 0 disagrees, check first whether CatData is written during "
            "a battle -- the replay rebuilds the fight from the cats as pushed, "
            "and nobody has confirmed the game leaves CatData alone mid-fight.");
        ImGui::TreePop();
    }

    ImGui::TreePop();   // the manual-connect fold
}

void draw_session() {
    ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("mgmp -- session")) { ImGui::End(); return; }

    const NetState st = net_state();
    ImGui::TextDisabled("state");
    ImGui::SameLine();
    ImGui::TextColored(state_colour(st), "%s", state_name(st));
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", session_status());

    if (st == NetState::Failed && net_error()[0])
        ImGui::TextColored(level_colour(LogLevel::Error), "%s", net_error());

    if (lockstep_halted())
        ImGui::TextColored(level_colour(LogLevel::Error),
                           "HALTED -- the peers disagreed and this one stopped");

    // WHICH CAT THIS PEER IS REALLY UPGRADING (2026-09-23). The level-up screen cannot be
    // re-aimed -- its name, numbers and four options are copied by value when it is built --
    // so the panel states the difference instead of pretending there is none: while a
    // level-up is up, the cat named here is the one the upgrade will land on, and that is
    // THIS peer's own cat even when the picture belongs to the shared draw.
    {
        char note[256] = {};
        if (choice_level_note(note, sizeof(note))) {
            const bool split = note[0] && strstr(note, "shared draw") != nullptr;
            ImGui::TextColored(level_colour(split ? LogLevel::Warn : LogLevel::Good),
                               "%s", note);
        }
    }

    // Said here as well as in the log, because this instance is about to
    // overwrite one of the player's save slots when it closes, and with two
    // instances on one screen "which one is the test rig" is a question the
    // panel has to answer without being asked.
    if (cfg().debug_mode)
        ImGui::TextColored(level_colour(LogLevel::Warn),
                           "DEBUG MODE -- on exit, '%s' is restored from '%s'",
                           cfg().debug_slot_to, cfg().debug_slot_from);

    // --- debug actions -------------------------------------------------------
    //
    // The one button here that EDITS THE RUN, and it says so on its own tooltip.
    // Its purpose is testing speed: a TurnControl exists only inside a battle, so
    // any session that needs one has to fight first, and this is how the fight
    // ends in one click.
    // --- the co-op session: "continue alone, and stay alone until a warehouse" ------
    //
    // WIRED TO mgmp_leave's decision (2026-09-22). The buttons are the ONLY way to
    // answer the question the leave path now asks, which is why they had to be wired in
    // the same step as the block: a peer held undecided with no way to answer would be a
    // screen with no exit, which is the failure this project keeps closing.
    //
    // What is wired: the question appears here, the two answers are recorded, and an
    // undecided peer no longer has Quit To Menu pressed behind its back. What is NOT
    // yet: dropping the connection, removing the other player's cats, refusing to
    // re-pair until a warehouse, and blocking map/battle input.
    ImGui::SeparatorText("co-op session");
    ImGui::TextWrapped("Recovery: %s", checkpoint_status());
    {
        char status[192] = {};
        leave_status(status, sizeof(status));

        if (leave_decision_pending()) {
            ImGui::PushStyleColor(ImGuiCol_Text, level_colour(LogLevel::Warn));
            ImGui::TextUnformatted("The other player has left the run. Answer before"
                                   " playing on:");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Continue alone: keep this run and play it with your own"
                               " cats. The other player's cats will be removed, and the"
                               " two of you cannot pair again until both are back at a"
                               " warehouse.");
            ImGui::TextWrapped("Wait for the peer: leave this run as before -- the mod"
                               " presses Quit To Menu for you when you open the pause"
                               " menu, and you are taken back in if the host starts a run.");
            if (ImGui::Button("Continue alone")) leave_decide_continue_alone();
            ImGui::SameLine();
            if (ImGui::Button("Wait for the peer")) leave_decide_wait();
        } else {
            ImGui::TextWrapped("no decision waiting");
        }
        ImGui::Text("leave: %s", status[0] ? status : "?");
        // The latch is a standing condition, not a departure, so it gets its own line
        // rather than riding in leave_status: "continue alone" is answered once and the
        // refusal to pair outlives it until the House. See leave_pairing_allowed.
        if (leave_solo_active())
            ImGui::TextDisabled("pairing: refused until the warehouse (continuing alone)");
    }

    if (cfg().dev_tools) {
    ImGui::SeparatorText("debug actions");
    {
        const int32_t armed = lockstep_armed_enemy_hit();
        int32_t hx = 0, hy = 0;
        const bool hover = cursor_hovered_tile(hx, hy);

        // TWO STEPS, LIKE AN ABILITY: arm it here, then click the target on the
        // board. One step cannot work -- pressing this button puts the mouse on the
        // panel, and the only honest reading of "what is the player pointing at" is
        // the game's own hover field.
        // ALL ENEMIES, NO TARGETING -- added 2026-09-22 because a deathrattle's
        // children cannot be clicked: the arm path resolves a TILE against the
        // roster, and a cat that appeared during the turn is not in it yet.
        if (ImGui::Button("999 damage to EVERY enemy on the field")) {
            lockstep_debug_hit_all(999);
        }
        ImGui::SetItemTooltip(
            "Adopts mid-battle arrivals first (the deathrattle case), then hits every\n"
            "enemy for 999. Each hit is broadcast, so BOTH peers write the same numbers\n"
            "and the session stays readable afterwards.\n\n"
            "Players' cats and the run's familiars are left alone.");

        if (!armed) {
            if (ImGui::Button("999 damage to the enemy: arm, then click it")) {
                lockstep_arm_enemy_hit(999);
            }
            ImGui::SetItemTooltip(
                "Arms a 999-hp hit; the next LEFT CLICK on the board is the target.\n\n"
                "It writes ONE field (hp) and calls nothing: the game's own death"
                " handling does the rest.\n\n"
                "This EDITS THE BATTLE -- the peers' hashes will disagree from the next"
                " turn, because the write is in neither peer's action stream. Use it"
                " while testing, not in a run you care about.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, level_colour(LogLevel::Warn));
            ImGui::Text("ARMED: %d damage -- left-click an enemy on the board", armed);
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("cancel")) lockstep_disarm_enemy_hit();
        }

        if (hover)
            ImGui::TextDisabled("mouse is on tile (%d,%d) -- %s", hx, hy,
                                armed ? "click it" : "arm first, then click it");
        else
            ImGui::TextDisabled("no board under the mouse (not in a battle?)");
    }
    }   // cfg().dev_tools

    ImGui::Separator();

    const NetStats s = net_stats();
    if (ImGui::BeginTable("session", 2, ImGuiTableFlags_SizingStretchProp)) {
        const NetRole r = net_role();
        row("role", "%s", r == NetRole::Host   ? "host"
                        : r == NetRole::Client ? "client" : "off");

        // "id 1" and NOT "1 of 2". The id is an identity, not an ordinal, and
        // the ordinal reading is wrong in a way that looks like a bug: peer 0
        // is ALWAYS the host, so a client correctly showing id 1 reads as
        // "1 of 2" -- which sounds like it is the first of two rather than the
        // second peer. The count belongs on its own row.
        row("this peer", "id %u%s", (unsigned)net_self(),
            net_self() == kHostPeer ? "  (peer 0 is always the host)" : "");

        uint8_t ids[8] = {};
        if (net_peer_ids(ids, 8) && net_peer_count()) {
            char buf[64]; int n = 0;
            for (uint8_t i = 0; i < net_peer_count() && i < 8; ++i)
                n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                                 i ? ", %u" : "%u", (unsigned)ids[i]);
            row("session", "%u peer(s): [%s]", (unsigned)net_peer_count(), buf);
        } else {
            row("session", "%u peer(s)", (unsigned)net_peer_count());
        }

        // Not the id. Once anyone has disconnected the two diverge, and it is
        // this -- the position in the sorted membership list -- that the
        // control split is computed from, so the two are worth showing apart.
        row("split index", "%u", (unsigned)net_peer_pos());

        row("messages", "%u sent / %u received / %u dropped",
            s.sent, s.received, s.dropped);
        row("bytes", "%llu out / %llu in",
            (unsigned long long)s.bytes_sent, (unsigned long long)s.bytes_received);

        ImGui::EndTable();
    }

    ImGui::SeparatorText("battle");

    if (ImGui::BeginTable("battle", 2, ImGuiTableFlags_SizingStretchProp)) {
        const bool active = lockstep_active();
        row("lockstep", "%s", active ? "active" : "idle");
        // The node seed, which is what battle identity IS -- both peers read it
        // out of MapNode+0x118 entering the same node. Two peers showing
        // different ids here are in different battles, and that alone explains
        // most "the battle just sits there" reports.
        const uint64_t bid = lockstep_battle_id();
        if (active && bid != kNoBattle) row("battle id", "%016llx", (unsigned long long)bid);
        else                            row("battle id", "--");
        row("turn", "%u", log_turn());
        row("local actor", "%s", lockstep_local_actor() ? "yes -- this peer decides"
                                                        : "no");
        // Reads the LAST combat-menu tick, not the current frame -- the scope
        // the Button hook runs in is closed by the time the panel draws. Out of
        // a battle it holds whatever the last bar said.
        row("ability bar", "%s", combatlock_engaged()
                                     ? "greyed (a peer owns this cat)" : "live");
        row("map input", "%s", follow_suppresses_local_input()
                                   ? "suppressed (following the host)" : "local");
        ImGui::EndTable();
    }

    // The lobby is where a session starts now, so its state sits beside the
    // session's instead of a window away: "connected to the server, in room
    // AB12 as host" is the first thing that has to be true.
    ImGui::SeparatorText("lobby");
    ImGui::TextDisabled("server");
    ImGui::SameLine();
    ImGui::TextColored(signal_colour(signal_state()), "%s",
                       signal_state_name(signal_state()));
    ImGui::SameLine();
    ImGui::TextDisabled("(title screen > Multiplayer)");
    ImGui::TextDisabled("%s", signal_status());
    if (signal_room()[0])
        ImGui::Text("room %s -- you are the %s", signal_room(), signal_role());

    draw_connect();
    draw_nodes();

    ImGui::SeparatorText("panel");
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("scale", &g.font_scale, 0.75f, 2.5f, "%.2f"))
        ImGui::GetStyle().FontScaleMain = g.font_scale;
    ImGui::TextDisabled("VK 0x%02X toggles this panel", (unsigned)g.toggle_vk);

    ImGui::End();
}

// (The lobby window that lived here moved to the title screen -- see mgmp_menu.cpp.
// This panel keeps the read-only lobby line in the session window and nothing that
// dials a server.)

// --- input ------------------------------------------------------------------

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // The toggle is read before ImGui sees anything, so the panel can always be
    // dismissed even if it has somehow captured the keyboard.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == (WPARAM)g.toggle_vk) {
        g.visible = !g.visible;
        if (g.ready) {
            // Drop any half-finished input state, or a button held when the
            // panel was dismissed stays held when it comes back.
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
        return 0;
    }

    // F2: the log-upload panel (mgmp_menu). Same early read as the toggle above, and for the same reason: it must
    // open whatever ImGui is holding. A held key's repeats (bit 30 of lparam) do not toggle it again.
    if (msg == WM_KEYDOWN && wp == VK_F2 && g.ready) {
        if (!(lp & 0x40000000)) {
            menu_toggle_log_window();
            ImGui::GetIO().ClearInputKeys();
        }
        return 0;
    }

    // THE CONTEXT IS UP WHENEVER THE MOD IS: the player-facing menus (mgmp_menu)
    // are ImGui windows too, and they must take clicks with the F1 panel hidden.
    // ImGui only claims what is actually under its windows (WantCaptureMouse), so
    // an empty screen still belongs entirely to the game.
    if (g.ready) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return 1;

        // Swallow what ImGui is using, so a click on a button does not also land
        // on the board behind it. Only when ImGui actually wants it: the game
        // must keep receiving everything else, and swallowing a button-up it saw
        // the down for would leave it stuck.
        //
        // MOUSE MOVES ARE NOT SWALLOWED. The game reads its pointer from the
        // events it is sent, and its own cursor is a texture drawn from that
        // position; eating the moves froze that cursor at the edge of every
        // window and let it jump on the way out. Passing them through costs
        // nothing -- hover is a look, only a click is an action.
        //
        // Safe for lockstep by construction -- suppressing local input can only
        // ever mean "this player did not act", which the protocol already
        // handles everywhere (Brain::GetChoice returns type=1 and the game
        // waits exactly as it waits for a person).
        const ImGuiIO& io = ImGui::GetIO();
        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:   case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:   case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
            case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
                if (io.WantCaptureMouse) return 1;
                break;
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                if (io.WantCaptureKeyboard) return 1;
                break;
            default: break;
        }
    }

    return CallWindowProcW(g.prev_wndproc, hwnd, msg, wp, lp);
}

// --- bring-up ---------------------------------------------------------------

HWND game_window() {
    HMODULE ogl = GetModuleHandleA("opengl32.dll");
    if (!ogl) return nullptr;
    auto get_dc = (fn_wglGetCurrentDC)GetProcAddress(ogl, "wglGetCurrentDC");
    if (!get_dc) return nullptr;
    HDC dc = get_dc();
    return dc ? WindowFromDC(dc) : nullptr;
}

// Runs on the first swap, because that is the first moment a GL context is
// current -- the font atlas is a texture and cannot be built before one exists.
bool bring_up() {
    g.hwnd = game_window();
    if (!g.hwnd) {
        log_line("UI", "!! no window behind the current GL DC -- the panel is OFF");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // No imgui.ini. The working directory is the game's, and the loopback test
    // runs two instances out of it -- they would fight over one settings file
    // and each would restore the other's window layout.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // The game hides the OS cursor and draws its own from textures/cursor/*.png
    // (see glaiel::SetCursor), so an OS-cursor-driven pointer would be
    // invisible over the panel. ImGui draws its own instead.
    io.MouseDrawCursor = true;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().FontScaleMain = g.font_scale;

    if (!ImGui_ImplWin32_Init(g.hwnd)) {
        log_line("UI", "!! ImGui_ImplWin32_Init failed -- the panel is OFF");
        ImGui::DestroyContext();
        return false;
    }
    // GLSL 150 -- ApplicationBase::RefreshWindow asks for a CORE 3.2 context,
    // and core profile has no compatibility fallback to fall back to.
    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        log_line("UI", "!! ImGui_ImplOpenGL3_Init failed -- the panel is OFF");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    menu_bring_up();

    log_line("UI", "panel up -- imgui %s, win32 + gl3 core, hwnd %p, VK 0x%02X toggles",
             IMGUI_VERSION, (void*)g.hwnd, (unsigned)g.toggle_vk);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

void ui_init() {
    g.enabled   = cfg().ui;
    g.visible   = cfg().ui_visible;
    g.toggle_vk = cfg().ui_key;
    _snprintf_s(g.addr, sizeof(g.addr), _TRUNCATE, "%s", cfg().net_addr);
    g.port = (int)cfg().net_port;

    if (!g.enabled) {
        log_line("UI", "panel OFF (set ui.enabled in mgmp.json)");
        return;
    }

    // The subclass goes on now rather than at bring-up, because the toggle key
    // has to work even if the panel starts hidden -- and a hidden panel never
    // reaches the swap path that would install it.
    HWND hwnd = game_window();
    if (!hwnd) {
        // Not fatal: ui_on_swap retries, and on the very first frames there may
        // be no current DC on this thread yet.
        log_line("UI", "window not resolvable yet -- input hookup deferred to the first swap");
        return;
    }
    g.hwnd = hwnd;
    g.prev_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&wndproc);
    if (!g.prev_wndproc)
        log_line("UI", "!! could not subclass the game window -- the panel will draw"
                       " but will not take input");
}

void ui_shutdown() {
    // The WndProc is deliberately NOT restored, for the same reason
    // overlay_shutdown leaves the SDL slot alone: a session can end and begin
    // again inside one process, and a panel that silently loses its input the
    // second time is the asymmetry that is easy to write and hard to notice.
    // The mod is injected and never unloaded.
    //
    // The ImGui context is left standing too -- destroying it frees GL objects,
    // and shutdown can run from a thread with no current context.
    g.visible = false;
}

void* ui_window() { return g.hwnd; }

bool ui_captures_input() {
    if (!g.ready) return false;
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void ui_on_swap(void* window) {
    (void)window;
    if (!g.enabled || g.failed) return;

    // Deferred from ui_init when there was no current DC then.
    if (!g.prev_wndproc) {
        HWND hwnd = game_window();
        if (hwnd) {
            g.hwnd = hwnd;
            g.prev_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                        (LONG_PTR)&wndproc);
            if (g.prev_wndproc)
                log_line("UI", "input hooked up on the first swap (hwnd %p)", (void*)hwnd);
        }
    }

    // The context is created even with the F1 panel hidden: the menus need it,
    // and hiding the debug windows must not take the player's UI with them.
    if (!g.ready) {
        if (!bring_up()) { g.failed = true; return; }
        g.ready = true;
    }

    // The arrow ImGui draws itself is only for the debug panel. Over the
    // player-facing menus the game's own cursor art is drawn instead (see
    // mgmp_menu), and a second arrow on top of it is the bug this avoids.
    ImGui::GetIO().MouseDrawCursor = g.visible;

    if (g.visible) {
        // Cheap, and it keeps the pane current even on frames where nothing is
        // drawn because a window is collapsed.
        pump_log();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    uitest_before_frame();      // dev harness; a no-op unless ui.test_harness is set
    ImGui::NewFrame();

    if (g.visible) {
        draw_session();
        draw_log();
    }
    menu_draw();

    ImGui::Render();
    // RenderDrawData backs up and restores the GL state it touches, which is
    // the same discipline mgmp_overlay follows by hand -- the game's next frame
    // inherits whatever is left behind.
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    uitest_after_render();
}

} // namespace mgmp
