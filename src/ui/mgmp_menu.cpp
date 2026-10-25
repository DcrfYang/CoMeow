#include "mgmp_menu.h"
#include "mgmp_i18n.h"
#include "mgmp_ui.h"

#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture
#include "stb_image.h"        // implementation lives in mgmp_overlay.cpp

#include "mgmp_config.h"
#include "mgmp_leave.h"
#include "mgmp_lockstep.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"     // addr_of_call: TempHideMouseCursor (the drawing board's pointer)
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_saveslots.h"
#include "mgmp_roomhist.h"
#include "mgmp_chat.h"
#include "mgmp_emote.h"
#include "mgmp_draw.h"
#include "mgmp_unlocks.h"     // settings_room_speed: the combat speed line
#include "mgmp_page.h"
#include "mgmp_perf.h"       // the room panel's latency line
#include "mgmp_mapvote.h"     // the players' picks of the next map node
#include "mgmp_follow.h"      // follow_on_map: the paws only while the map ticks
#include "mgmp_abandon.h"
#include "mgmp_room.h"
#include "mgmp_catview.h"
#include "mgmp_checkpoint.h"
#include "mgmp_catpalette.h"
#include "mgmp_savefile.h"
#include "mgmp_session.h"
#include "mgmp_signal.h"
#include "mgmp_lan.h"
#include "mgmp_upnp.h"
#include "mgmp_steambridge.h"
#include "mgmp_logupload.h"
#include "mgmp_prevrun.h"
#include "mgmp_treasure.h"
#include "mgmp_housecoop.h"
#include "mgmp_areagate.h"

namespace mgmp {
namespace {

// ---------------------------------------------------------------------------
// palette -- read off the game's own screenshots (main menu, save screen, house)
// ---------------------------------------------------------------------------
constexpr ImU32 kCream   = IM_COL32(244, 242, 234, 255);   // menu entries
constexpr ImU32 kCreamHi = IM_COL32(255, 253, 244, 255);   // hovered menu entry
constexpr ImU32 kShadow  = IM_COL32(20, 26, 22, 170);
constexpr ImU32 kInk     = IM_COL32(30, 30, 30, 255);      // outlines and paper text
constexpr ImU32 kInkSoft = IM_COL32(58, 58, 58, 255);
constexpr ImU32 kPaper   = IM_COL32(217, 217, 210, 255);
constexpr ImU32 kPaperHi = IM_COL32(236, 236, 228, 255);
constexpr ImU32 kPaperLo = IM_COL32(190, 190, 182, 255);
constexpr ImU32 kGreen   = IM_COL32(74, 140, 84, 255);
constexpr ImU32 kAmber   = IM_COL32(196, 146, 40, 255);
constexpr ImU32 kRed     = IM_COL32(180, 62, 58, 255);
constexpr ImU32 kYellow  = IM_COL32(222, 164, 10, 255);   // the closed-port notice
constexpr ImU32 kGrey    = IM_COL32(120, 120, 116, 255);
constexpr ImU32 kPink    = IM_COL32(226, 150, 146, 255);   // the save screen's knob

// The layout is authored on a 1920x1080 stage and scaled to the content
// rectangle, the same rectangle the game and the peer cursors use.
constexpr float kStageW = 1920.0f;
constexpr float kStageH = 1080.0f;

// ---------------------------------------------------------------------------
// assets
// ---------------------------------------------------------------------------
struct Tex {
    ImTextureData* td = nullptr;
    int  w = 0, h = 0;
    bool tried = false;
    bool ok() const { return td != nullptr; }
    ImTextureRef ref() const { return td->GetTexRef(); }
};

struct Menu;
Menu* M();

std::string assets_dir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    HMODULE mod = nullptr;
    wchar_t file[MAX_PATH * 2] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&assets_dir), &mod) ||
        !GetModuleFileNameW(mod, file, (DWORD)(sizeof(file) / sizeof(file[0]))))
        return cached;
    std::wstring dir = file;
    dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\assets";
    int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        cached.assign((size_t)n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, &cached[0], n, nullptr, nullptr);
    }
    return cached;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Loaded the first time it is asked for. Returns false (and remembers it) when
// the file is missing, so a missing asset costs one failed open, not one a frame.
bool tex_load(Tex& t, const char* rel) {
    if (t.tried) return t.ok();
    t.tried = true;
    const std::string path = assets_dir() + "\\" + rel;
    // stb_image is built STDIO-less in this DLL (see mgmp_overlay.cpp), so the
    // file is read here and decoded from memory.
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) { log_line("MENU", "asset missing: %s -- drawing a flat stand-in", path.c_str()); return false; }
    std::vector<unsigned char> bytes;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && len < (32L << 20)) {
        bytes.resize((size_t)len);
        if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    }
    fclose(f);
    int w = 0, h = 0, n = 0;
    unsigned char* px = bytes.empty() ? nullptr
                                      : stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) { log_line("MENU", "!! could not decode %s", path.c_str()); return false; }

    ImTextureData* td = IM_NEW(ImTextureData)();
    td->Create(ImTextureFormat_RGBA32, w, h);
    memcpy(td->Pixels, px, (size_t)w * (size_t)h * 4);
    td->UseColors = true;
    td->SetStatus(ImTextureStatus_WantCreate);
    ImGui::RegisterUserTexture(td);
    stbi_image_free(px);
    t.td = td; t.w = w; t.h = h;
    return true;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
enum class Win { None, Multi, Backup, Settings };
enum class LanPhase { None, HostDial, HostCreate, JoinDial, JoinJoin };   // the LAN tab's create / join, step by step
enum class Confirm { None, Overwrite, Load, Delete, Undo, Import };

struct Menu {
    ImFont* font = nullptr;

    Tex paper_panel, paper_tag, cursor_def, cursor_over;
    // 0.1.91: the drawing board's tool is the mouse pointer while it is over the board (draw_board sets it each frame, draw_cursor draws it,
    // menu_board_cursor_tick hides the game's own cursor meanwhile)
    int     board_cursor = 0;          // 0 none, 1 pen, 2 eraser
    ImU32   board_cursor_col = 0;
    float   board_cursor_r = 0;        // the eraser's reach, screen pixels
    ULONGLONG board_cursor_at = 0;
    Tex icon_multi, icon_cats, cat_head, cat_ear_l, cat_ear_r;
    Tex emote_atlas;                  // the chat's emotes (mgmp_emote; assets/emotes, not in git)

    Win  win = Win::None;
    // Written by the button detour, read by menu_draw: the title entries follow the
    // game's own. The main menu's buttons tick only while that page is up, and the
    // pause sidebar's buttons exist only while it is open (and it CAN open over the
    // title -- Esc -- greying the native entries out).
    ULONGLONG native_ms = 0, pause_ms = 0;
    // Set when a room opens, cleared when the press lands: entering a room is the
    // player saying "I am playing with this person", and the next thing they need
    // is a save slot. See auto_play_watch in mgmp_menu.cpp.
    ULONGLONG auto_play_until = 0;
    float     title_a = 0;            // eased visibility of our entries, 0..1
    float     title_dim = 0;          // eased 0..1 while a modal sits over the menu
    bool title_scene = false;         // the MainMenu scene is live (set by menu_draw, read by the button detour)
    bool hovering_button = false;     // any of our buttons under the mouse this frame

    // --- multiplayer window ---
    char addr[64]  = {};
    char pname[32] = {};
    char room[48]  = {};
    char room_name[48] = {};          // the name of the room we are in, as we know it
    char search[48] = {};             // the room list's search box
    char create_pw[40] = {};          // the optional password for a room being created
    int  transport = 0;               // the carrier chosen for a room being created: 0 = not chosen (the best usable one), 1 = direct (the game port), 2 = Steam relay, 3 = the server relay
    bool relay_warn = false;          // the "low-spec server" notice for the server relay is up
    // The password prompt for joining a locked room.
    bool     pw_ask   = false;
    bool     pw_wrong = false;        // the last try was refused: say so
    bool     pw_sent  = false;        // a join with a password is in flight
    char     pw_id[16]   = {};
    char     pw_name[48] = {};
    char     pw_buf[40]  = {};
    uint32_t err_seen = 0;            // the signal client's refusal counter as of the last frame
    // Local network play (the multiplayer window's second tab).
    bool      lan_tab      = false;
    char      lan_room[48] = {};      // the room to create
    char      lan_pw[40]   = {};
    LanPhase  lan_phase    = LanPhase::None;
    double    lan_deadline = 0;       // the step in progress gives up at this time
    bool      lan_dialed   = false;   // the signaling client was told to connect for this attempt
    uint32_t  lan_err0     = 0;       // the refusal counter when the room request went out
    char      lan_t_addr[64] = {};    // the lobby being joined
    uint16_t  lan_t_port   = 0;
    char      lan_t_id[16] = {};
    bool      lan_t_pw     = false;
    double    lan_next_search = 0;
    bool      room_connected = false;   // this client has been linked to the host at least once in the room it is in
    // The F2 log-upload panel.
    bool     log_open   = false;
    bool     log_halt   = false;   // ...and the battle has HALTED: the window leads with what the players can do now
    bool     log_desync = false;   // the window was raised by a battle desync (lockstep_take_desync_notice): no description to write, the summary goes instead
    char     log_desync_text[192] = {};
    bool     log_prev   = false;   // the window shows the PREVIOUS run (it ended badly): its log and dump, not this run's (mgmp_prevrun.h)
    int      prev_stage = 0;       // 0 not looked yet, 1 an automatic upload of it is running, 2 handled
    bool     log_inited = false;
    // 0.1.93: F2's history page -- the earlier runs' logs (log_history), read again every two seconds while it is up
    bool     log_hist   = false;
    LogHistoryEntry hist[kLogHistoryKeep];
    int      hist_n     = 0;
    ULONGLONG hist_at   = 0;
    int      hist_sent  = -1;      // the row the last upload from this page was for
    char     log_addr[64] = {};
    char     log_desc[kLogUploadDescMax + 1] = {};
    // The first-run beta notice.
    bool     beta_closed = false;     // dismissed for this run
    bool     beta_dont   = false;     // its "do not show again" box
    bool     ag_dont     = false;     // the area question's "do not ask again in this game" box
    bool inited    = false;
    double next_list = 0;
    bool  was_in_room = false;
    bool  panel_open  = false;        // the in-game panel
    float panel_w     = 580;          // its width; grows while someone has cat cards to show
    float panel_h     = 520;          // its height on the stage, learned from the last frame
    float panel_t     = 0;            // 0 folded .. 1 out
    double joined_at  = 0;
    SignalRoom rooms[32];
    SignalPeer peers[8];

    // --- the lobby: the room list and, on a page of its own, the form that creates a room ---
    bool create_page = false;

    // --- backup window ---
    char save_name[kSaveNameMax] = {};
    Confirm confirm = Confirm::None;
    int     confirm_idx = -1;
    double  confirm_until = 0;
    // The dialog that asks before a backup is loaded, overwritten or deleted (None = closed). idx 0.. = the twenty, 100.. = the auto saves.
    Confirm dlg = Confirm::None;
    int     dlg_idx = -1;
    // Overwrite asks for the backup's name in the dialog itself (2026-10-10), starting from the name it has; the page's name box is for an empty position.
    char    dlg_name[kSaveNameMax] = {};
    bool    dlg_focus = false;
    // WHICH ACCOUNT'S SAVES (2026-10-15): the folder line of the backup page, the switch's warning, the list of folders
    char        acct_label[200] = {};
    bool        acct_warn = false;       // the switch's warning is up (the backup page's button)
    bool        acct_list = false;       // the list is up for a switch (the title screen's question needs no flag: it shows while the folder is unsure)
    SaveAccount acct[8];
    int         acct_n = 0;
    double      acct_at = -10;           // when the list was read
    double      title_since = 0;         // the title screen up since (the question waits a moment for Steam's id)
    SaveSlotInfo current[kGameSlots];
    double  refreshed_at = 0;
    // The page that restores a slot from a handshake save (inside the backup window); sync_slot/sync_idx = the entry the confirm dialog is about (-1 = closed).
    bool    sync_page = false;
    int     sync_slot = -1, sync_idx = -1;

    // --- toast ---
    char   toast[512] = {};
    double toast_until = 0;
};

Menu g;
Menu* M() { return &g; }

double now() { return ImGui::GetTime(); }

void say(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(g.toast, sizeof(g.toast), _TRUNCATE, fmt, ap);
    va_end(ap);
    g.toast_until = now() + 4.0;
}

// ---------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------
struct View { ImVec2 org; float k; float w, h; };

View view() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    float cw = ds.x, ch = ds.x * 9.0f / 16.0f;
    if (ch > ds.y) { ch = ds.y; cw = ch * 16.0f / 9.0f; }
    View v;
    v.w = cw; v.h = ch;
    v.org = ImVec2((ds.x - cw) * 0.5f, (ds.y - ch) * 0.5f);
    v.k = ch / kStageH;
    return v;
}

ImVec2 P(const View& v, float x, float y) { return ImVec2(v.org.x + x * v.k, v.org.y + y * v.k); }

float ease_out(float t) { t = t < 0 ? 0 : (t > 1 ? 1 : t); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

// ---------------------------------------------------------------------------
// drawing helpers
// ---------------------------------------------------------------------------
ImU32 mul_alpha(ImU32 c, float a) {
    ImU32 A = (c >> IM_COL32_A_SHIFT) & 0xFF;
    A = (ImU32)(A * (a < 0 ? 0 : (a > 1 ? 1 : a)));
    return (c & ~IM_COL32_A_MASK) | (A << IM_COL32_A_SHIFT);
}

uint32_t hash32(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
    return h;
}

float jitter(uint32_t seed, int i, float amp) {
    return (((hash32(seed * 2654435761u + (uint32_t)i * 40503u) & 0xFFFF) / 65535.0f) - 0.5f) * 2.0f * amp;
}

ImVec2 text_size(float size, const char* s) {
    return g.font ? g.font->CalcTextSizeA(size, FLT_MAX, 0.0f, s) : ImGui::CalcTextSize(s);
}

void text_at(ImDrawList* dl, ImVec2 p, float size, ImU32 col, const char* s, bool shadow = false) {
    ImFont* f = g.font ? g.font : ImGui::GetFont();
    if (shadow) dl->AddText(f, size, ImVec2(p.x + size * 0.045f, p.y + size * 0.06f), kShadow, s);
    dl->AddText(f, size, p, col, s);
}

// Centred on x, top at y.
void text_c(ImDrawList* dl, float cx, float y, float size, ImU32 col, const char* s, bool shadow = false) {
    const ImVec2 sz = text_size(size, s);
    text_at(dl, ImVec2(cx - sz.x * 0.5f, y), size, col, s, shadow);
}

void text_r(ImDrawList* dl, float rx, float y, float size, ImU32 col, const char* s) {
    const ImVec2 sz = text_size(size, s);
    text_at(dl, ImVec2(rx - sz.x, y), size, col, s);
}

// A hand-cut rectangle: every edge is broken into short runs and each vertex is
// nudged a pixel or two, deterministically, so a button keeps its shape between
// frames and no two buttons are the same.
void rough_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 line, float thick, uint32_t seed, float amp) {
    ImVec2 pts[40];
    int n = 0;
    const int seg = 4;
    auto add = [&](float x, float y) { pts[n] = ImVec2(x + jitter(seed, n * 2, amp), y + jitter(seed, n * 2 + 1, amp)); ++n; };
    for (int i = 0; i < seg; ++i) add(a.x + (b.x - a.x) * i / seg, a.y);
    for (int i = 0; i < seg; ++i) add(b.x, a.y + (b.y - a.y) * i / seg);
    for (int i = 0; i < seg; ++i) add(b.x - (b.x - a.x) * i / seg, b.y);
    for (int i = 0; i < seg; ++i) add(a.x, b.y - (b.y - a.y) * i / seg);
    if ((fill >> IM_COL32_A_SHIFT) & 0xFF) dl->AddConvexPolyFilled(pts, n, fill);
    dl->AddPolyline(pts, n, line, ImDrawFlags_Closed, thick);
}

// Stretches the middle of a texture and keeps its torn borders. Margins are in
// SOURCE pixels; `scale` is destination pixels per source pixel.
void nine_slice(ImDrawList* dl, Tex& t, ImVec2 p0, ImVec2 p1, float ml, float mt, float mr, float mb,
                float scale, ImU32 col = IM_COL32_WHITE) {
    const float dl_ = ml * scale, dt = mt * scale, dr = mr * scale, db = mb * scale;
    float xs[4] = { p0.x, p0.x + dl_, p1.x - dr, p1.x };
    float ys[4] = { p0.y, p0.y + dt,  p1.y - db, p1.y };
    // A panel smaller than its own borders: shrink the borders, keep the order.
    if (xs[1] > xs[2]) { xs[1] = xs[2] = (p0.x + p1.x) * 0.5f; }
    if (ys[1] > ys[2]) { ys[1] = ys[2] = (p0.y + p1.y) * 0.5f; }
    float us[4] = { 0, ml / t.w, 1.0f - mr / t.w, 1 };
    float vs[4] = { 0, mt / t.h, 1.0f - mb / t.h, 1 };
    for (int j = 0; j < 3; ++j) {
        if (j != 1) {
            for (int i = 0; i < 3; ++i)
                dl->AddImage(t.ref(), ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j + 1]),
                             ImVec2(us[i], vs[j]), ImVec2(us[i + 1], vs[j + 1]), col);
            continue;
        }
        // The middle row is the only one that is ever much taller than its
        // source, and stretching it smears the paper's grain into vertical
        // streaks. Tile it at its natural height, every other tile mirrored so
        // the seams line up.
        const float natural = (t.h - mt - mb) * scale;
        float y = ys[1];
        for (int n = 0; y < ys[2] - 0.5f; ++n) {
            const float y1 = (y + natural < ys[2] - 1.0f) ? y + natural : ys[2];
            const float frac = (y1 - y) / natural;
            float v0 = vs[1], v1 = vs[1] + (vs[2] - vs[1]) * frac;
            if (n & 1) { const float top = vs[2] - (vs[2] - vs[1]) * frac; v0 = vs[2]; v1 = top; }
            for (int i = 0; i < 3; ++i)
                dl->AddImage(t.ref(), ImVec2(xs[i], y), ImVec2(xs[i + 1], y1),
                             ImVec2(us[i], v0), ImVec2(us[i + 1], v1), col);
            y = y1;
        }
    }
}

// The paper every window sits on. Falls back to a flat rough rectangle.
void paper(ImDrawList* dl, const View& v, ImVec2 a, ImVec2 b, float alpha = 1.0f) {
    if (tex_load(g.paper_panel, "ui\\paper_panel.png")) {
        // A soft drop shadow so the sheet lifts off the scene behind it.
        dl->AddRectFilled(ImVec2(a.x + 6 * v.k, a.y + 10 * v.k), ImVec2(b.x + 6 * v.k, b.y + 10 * v.k),
                          mul_alpha(IM_COL32(0, 0, 0, 90), alpha), 10.0f * v.k);
        nine_slice(dl, g.paper_panel, a, b, 30, 62, 30, 50, 0.8f * v.k, mul_alpha(IM_COL32_WHITE, alpha));
    } else {
        rough_rect(dl, a, b, mul_alpha(kPaper, alpha), mul_alpha(kInk, alpha), 3.0f * v.k, 7, 2.0f * v.k);
    }
}

// ---------------------------------------------------------------------------
// widgets. All take SCREEN-space rectangles; the caller converts from the stage.
// ---------------------------------------------------------------------------
enum class Tone { Normal, Good, Danger };

bool paper_button(const char* id, ImVec2 a, ImVec2 b, const char* label, float font, bool enabled = true,
                  Tone tone = Tone::Normal, bool armed = false) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##b", ImVec2(b.x - a.x, b.y - a.y)) && enabled;
    const bool hov = ImGui::IsItemHovered() && enabled;
    const bool held = ImGui::IsItemActive() && enabled;
    ImGui::PopID();
    if (hov) g.hovering_button = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const uint32_t seed = ImHashStr(id);
    ImU32 fill = kPaper;
    if (tone == Tone::Good)   fill = IM_COL32(200, 222, 196, 255);
    if (tone == Tone::Danger) fill = IM_COL32(230, 200, 194, 255);
    if (armed)                fill = IM_COL32(240, 214, 150, 255);
    if (!enabled)             fill = IM_COL32(200, 200, 194, 150);
    else if (held)            fill = kPaperLo;
    else if (hov)             fill = kPaperHi;

    // Hovering lifts the tag a little, the way the house screen's tags do.
    const float grow = hov ? 1.5f : 0.0f;
    ImVec2 pa(a.x - grow, a.y - grow - (held ? 0 : (hov ? 2.0f : 0.0f)));
    ImVec2 pb(b.x + grow, b.y + grow - (held ? 0 : (hov ? 2.0f : 0.0f)));
    dl->AddRectFilled(ImVec2(pa.x + 3, pa.y + 4), ImVec2(pb.x + 3, pb.y + 4), IM_COL32(0, 0, 0, enabled ? 55 : 25), 2.0f);
    rough_rect(dl, pa, pb, fill, enabled ? kInk : IM_COL32(30, 30, 30, 110), 2.4f, seed, 1.4f);
    const ImU32 tc = enabled ? kInk : IM_COL32(30, 30, 30, 120);
    // A label that does not fit is set smaller rather than spilling over the edge: the other languages run
    // well past the Chinese the buttons were sized for.
    ImVec2 sz = text_size(font, label);
    const float room = (b.x - a.x) - 14.0f;
    if (sz.x > room && sz.x > 0) { font *= room / sz.x; sz = text_size(font, label); }
    text_at(dl, ImVec2((pa.x + pb.x - sz.x) * 0.5f, (pa.y + pb.y - sz.y) * 0.5f), font, tc, label);
    return clicked;
}

bool overlay_window_begin(const char* id, ImVec2 pos, ImVec2 size, bool focus = true, bool inputs = true) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
    if (!focus) fl |= ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!inputs) fl |= ImGuiWindowFlags_NoInputs;          // drawn only: the mouse goes through it to what is under it
    const bool open = ImGui::Begin(id, nullptr, fl);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return open;
}

// InputText in paper colours.
bool paper_input(const char* id, ImVec2 a, ImVec2 b, char* buf, size_t cap, const char* hint, float font,
                 ImGuiInputTextFlags flags = 0) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.96f, 0.95f, 0.90f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.12f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.12f, 0.12f, 0.11f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.12f, 0.12f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.55f, 0.72f, 0.60f, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    const float pad = (b.y - a.y - font) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, pad > 1 ? pad : 1));
    ImGui::PushFont(g.font ? g.font : ImGui::GetFont(), font);
    ImGui::SetNextItemWidth(b.x - a.x);
    const bool changed = ImGui::InputTextWithHint(id, hint, buf, cap, flags);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) g.hovering_button = true;
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return changed;
}

void status_dot(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(c, r, col, 14);
    dl->AddCircle(c, r, kInk, 14, 1.8f);
}

// A padlock: a body and a shackle, in ink.
void draw_lock(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float bw = s * 0.78f, bh = s * 0.55f;
    const ImVec2 b0(c.x - bw * 0.5f, c.y - bh * 0.05f), b1(c.x + bw * 0.5f, c.y - bh * 0.05f + bh);
    const float r = bw * 0.30f;
    dl->PathArcTo(ImVec2(c.x, b0.y), r, 3.14159f, 6.28318f, 12);   // the shackle: an arch over the body
    dl->PathStroke(col, 0, s * 0.11f);
    dl->AddLine(ImVec2(c.x - r, b0.y), ImVec2(c.x - r, b0.y + bh * 0.2f), col, s * 0.11f);
    dl->AddLine(ImVec2(c.x + r, b0.y), ImVec2(c.x + r, b0.y + bh * 0.2f), col, s * 0.11f);
    dl->AddRectFilled(b0, b1, col, s * 0.08f);
    dl->AddCircleFilled(ImVec2(c.x, (b0.y + b1.y) * 0.5f), s * 0.07f, IM_COL32(255, 255, 250, 255), 8);
}

// Case-insensitive (ASCII) substring test; any other byte compares as itself, so Chinese and the like match exactly.
bool contains_ci(const char* hay, const char* needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    auto low = [](unsigned char c) { return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); };
    for (const char* h = hay; *h; ++h) {
        const char* a = h; const char* b = needle;
        while (*a && *b && low((unsigned char)*a) == low((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

// Word-wraps `text` into [x, x+maxw) and returns the y below the last line. It breaks at a space when the line has
// one and between characters otherwise (Chinese and Japanese have none), always on a UTF-8 boundary.
float wrap_text(ImDrawList* dl, float x, float y, float maxw, float fs, float line_h, ImU32 col, const char* text) {
    const char* s = text;
    while (*s) {
        if (*s == '\n') { y += line_h; ++s; continue; }   // an empty line ("\n\n")
        const char* e = s;
        const char* fit = s;
        const char* space = nullptr;   // just after the last space that still fits
        while (*e && *e != '\n') {
            const unsigned char c = (unsigned char)*e;
            const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            const char* nx = e + len;
            std::string part(s, nx);
            if (text_size(fs, part.c_str()).x > maxw) break;
            fit = nx; e = nx;
            if (c == ' ') space = nx;
        }
        const char* cut = fit;
        if (*e && *e != ' ' && *e != '\n' && space && space > s) cut = space;   // broke inside a word: back up to the last space
        if (cut == s) cut = s + 1;
        std::string ln(s, cut);
        while (!ln.empty() && ln.back() == ' ') ln.pop_back();
        if (dl) text_at(dl, ImVec2(x, y), fs, col, ln.c_str());   // no draw list: only measured
        y += line_h;
        s = cut;
        if (*s == '\n') ++s;                                // the line's own end
        else while (*s == ' ') ++s;
    }
    return y;
}

// Opens a whole-screen window that dims what is behind it and swallows the mouse, so nothing underneath (ours or the
// game's) can be clicked while a dialog is up. The dialog itself is drawn on it. Returns false (after End) if hidden.
bool modal_begin(const char* id) {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (!overlay_window_begin(id, ImVec2(0, 0), ds)) { ImGui::End(); return false; }
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, 120));
    return true;
}

// A multi-line InputText in paper colours (ImGui has no hint text for these, so an empty box draws its own).
bool paper_multiline(const char* id, ImVec2 a, ImVec2 b, char* buf, size_t cap, const char* hint, float font) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.96f, 0.95f, 0.90f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.12f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.12f, 0.12f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.55f, 0.72f, 0.60f, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    ImGui::PushFont(g.font ? g.font : ImGui::GetFont(), font);
    const bool changed = ImGui::InputTextMultiline(id, buf, cap, ImVec2(b.x - a.x, b.y - a.y), ImGuiInputTextFlags_None);
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || active) g.hovering_button = true;
    if (!buf[0] && !active)
        wrap_text(ImGui::GetWindowDrawList(), a.x + 12.0f, a.y + 9.0f, (b.x - a.x) - 24.0f, font * 0.85f, font * 1.15f,
                  IM_COL32(30, 30, 28, 110), hint);
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    return changed;
}

// ---------------------------------------------------------------------------
// the title screen
// ---------------------------------------------------------------------------

// The game's paw, traced from the native hover (measured 2026-09-30 at 2560x1600
// from a screenshot: 118 px wide, 87 px tall; connected-component boxes of the five
// white blobs). Units are fractions of the paw's width, from the centre of its box.
void draw_paw(ImDrawList* dl, ImVec2 c, float w, ImU32 col, float angle = 0.0f) {
    struct Blob { float x, y, rx, ry; };
    static const Blob b[5] = {
        {  0.000f,  0.153f, 0.237f, 0.216f },   // pad
        { -0.373f,  0.000f, 0.127f, 0.114f },   // toes, left to right
        { -0.127f, -0.233f, 0.119f, 0.119f },
        {  0.199f, -0.246f, 0.114f, 0.123f },
        {  0.394f,  0.008f, 0.106f, 0.097f },
    };
    // The entry's window clips to the label; the paw starts bigger and further left
    // than that, and a clipped left toe is what it looked like.
    dl->PushClipRectFullScreen();
    const float ca = cosf(angle), sa = sinf(angle);
    for (const Blob& e : b) {
        const float ox = e.x * w, oy = e.y * w;
        dl->AddEllipseFilled(ImVec2(c.x + ox * ca - oy * sa, c.y + ox * sa + oy * ca), ImVec2(e.rx * w, e.ry * w), col, angle, 32);
    }
    dl->PopClipRect();
}

// One text-only entry, following the game's own (compared frame by frame against
// "Start game", 2026-09-30): plain white, no shadow, no scale change; on hover a
// paw swells in at the left -- big, then settling -- while the label slides right
// by one paw. Both ease over about a tenth of a second, and back out faster.
bool menu_entry(const View& v, const char* id, const char* label, float cy) {
    const float size = 80.0f * v.k;
    const ImVec2 sz = text_size(size, label);
    const float shift_max = 86.0f * v.k;
    const ImVec2 pos(v.org.x + 64.0f * v.k, v.org.y + cy * v.k - sz.y * 0.5f);
    const ImVec2 a(pos.x - 14 * v.k, pos.y - 6 * v.k), b(pos.x + shift_max + sz.x + 24 * v.k, pos.y + sz.y + 6 * v.k);

    bool clicked = false;
    // While the menu is not interactive (a modal over it, or the page is going
    // away) the entry is drawn but takes no input, like the native one.
    const bool live = g.title_dim < 0.05f && g.title_a > 0.9f;
    if (overlay_window_begin(id, a, ImVec2(b.x - a.x, b.y - a.y), false)) {
        ImGui::SetCursorScreenPos(a);
        clicked = ImGui::InvisibleButton("##e", ImVec2(b.x - a.x, b.y - a.y)) && live;
        const bool hov = ImGui::IsItemHovered() && live;
        if (hov) g.hovering_button = true;

        ImGuiStorage* st = ImGui::GetStateStorage();
        const ImGuiID key = ImGui::GetID("hover_t");
        float t = st->GetFloat(key, 0.0f);
        const float dt = ImGui::GetIO().DeltaTime;
        t += hov ? dt / 0.11f : -dt / 0.07f;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        st->SetFloat(key, t);
        const float e = ease_out(t);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* f = g.font ? g.font : ImGui::GetFont();
        const float vis = g.title_a * (1.0f - 0.55f * g.title_dim);
        const ImU32 col = mul_alpha(IM_COL32(255, 255, 255, 255), vis);
        const float shift = shift_max * e;
        dl->AddText(f, size, ImVec2(pos.x + shift, pos.y), col, label);
        if (t > 0.001f) {
            // Starts about twice its size and a little to the left, settles to 1.
            const float w = 117.0f * v.k * (1.0f + 0.9f * (1.0f - e));
            const ImVec2 c(pos.x + (31.0f - 30.0f * (1.0f - e)) * v.k, pos.y + sz.y * 0.5f + 2.6f * v.k);
            draw_paw(dl, c, w, mul_alpha(IM_COL32(255, 255, 255, 255), vis * (0.35f + 0.65f * e)));
        }
    }
    ImGui::End();
    return clicked;
}

// ---------------------------------------------------------------------------
// multiplayer state helpers
// ---------------------------------------------------------------------------
bool in_room() { return signal_state() == SignalState::Connected && signal_room()[0]; }

struct Status { const char* text; ImU32 col; };

Status session_state_text() {
    switch (net_state()) {
        case NetState::Ready:      return { tr(Tx::NET_READY), kGreen };
        case NetState::Listening:  return { tr(Tx::NET_LISTEN), kAmber };
        case NetState::Connecting: return { tr(Tx::NET_CONNECTING), kAmber };
        case NetState::Connected:  return { tr(Tx::NET_HANDSHAKE), kAmber };
        case NetState::Failed:     return { tr(Tx::NET_FAILED), kRed };
        case NetState::Closed:     return { tr(Tx::NET_CLOSED), kRed };
        case NetState::Idle:
        default:                   return { tr(Tx::NET_IDLE), kGrey };
    }
}

// Split "host" or "host:port" -> host, port. A trailing :digits is a port;
// anything else (an IPv6 literal, a name with a colon) is left alone.
void split_addr(const char* in, char* host, size_t cap, uint16_t& port) {
    strncpy_s(host, cap, in, _TRUNCATE);
    // Trim blanks.
    size_t n = strlen(host);
    while (n && host[n - 1] == ' ') host[--n] = 0;
    char* s = host; while (*s == ' ') ++s;
    if (s != host) memmove(host, s, strlen(s) + 1);
    char* colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon && colon[1]) {
        bool digits = true;
        for (char* p = colon + 1; *p; ++p) if (*p < '0' || *p > '9') digits = false;
        const long v = digits ? strtol(colon + 1, nullptr, 10) : 0;
        if (digits && v >= 1 && v <= 65535) { port = (uint16_t)v; *colon = 0; }
    }
    if (!host[0]) strncpy_s(host, cap, "localhost", _TRUNCATE);
}

void ensure_inited() {
    if (g.inited) return;
    g.inited = true;
    // "localhost" is the default the player sees, whatever mgmp.json says: the
    // file's 127.0.0.1 is a developer default, and a stranger sees a box with a
    // number in it that they have no reason to trust. A configured address that
    // is NOT the loopback is kept, so a shipped config still wins.
    const Config& c = config();
    const bool loop = !c.signal_addr[0] || strcmp(c.signal_addr, "127.0.0.1") == 0;
    strncpy_s(g.addr, sizeof(g.addr), loop ? "localhost" : c.signal_addr, _TRUNCATE);
    // 0.1.89: the name this player last connected with (ui.player_name), else the config's
    strncpy_s(g.pname, sizeof(g.pname), c.ui_player_name[0] ? c.ui_player_name : c.signal_name, _TRUNCATE);
}

// ---------------------------------------------------------------------------
// the multiplayer window (title screen)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// local network play (no server): the LAN tab of the multiplayer window
//
// Hosting starts the lobby that is built into the mod (mgmp_lan.h), connects the ordinary signaling client to it on
// 127.0.0.1 and creates the room; joining connects the same client to the lobby that answered the search and joins. Both are
// a few steps that wait for the signaling client, so they are a small state machine run once a frame (lan_tick), whether or
// not the window is open.
// ---------------------------------------------------------------------------

// One step of "be connected to this lobby": 1 = connected, 0 = still working, -1 = it failed.
int lan_connect_step(const char* addr, uint16_t port) {
    char want[96];
    _snprintf_s(want, sizeof(want), _TRUNCATE, "%s:%u", addr, (unsigned)port);
    const SignalState st = signal_state();
    if (st == SignalState::Connected && !strcmp(signal_server(), want)) return 1;
    if (signal_request_pending() || st == SignalState::Connecting) return 0;
    if (st == SignalState::Connected) {                  // connected to some other lobby: leave it first
        g.lan_dialed = false;
        signal_request_disconnect();
        return 0;
    }
    if (!g.lan_dialed) {
        signal_request_connect(addr, port, g.pname);
        g.lan_dialed = true;
        if (config_set_player_name(g.pname)) log_line("MENU", "the player name '%s' is remembered for the next start (ui.player_name)", g.pname);
        log_line("MENU", "LAN: connecting to %s as '%s'", want, g.pname);
        return 0;
    }
    return -1;                                           // we dialled and it is Off/Failed/Closed again
}

void lan_fail(const char* why) {
    log_line_lvl(LogLevel::Warn, "MENU", "!! LAN: %s", why);
    say("%s", tr(Tx::LAN_FAILED));
    g.lan_phase = LanPhase::None;
    g.pw_ask = false; g.pw_sent = false; g.pw_buf[0] = 0;
    if (signal_state() != SignalState::Off) signal_request_disconnect();
}

void lan_tick() {
    if (g.lan_phase == LanPhase::None) {
        // The lobby we host lives only as long as our own connection to it.
        if (lan_host_running()) {
            char want[48];
            _snprintf_s(want, sizeof(want), _TRUNCATE, "127.0.0.1:%u", (unsigned)lan_host_port());
            const SignalState st = signal_state();
            const bool on_it = (st == SignalState::Connected || st == SignalState::Connecting) && !strcmp(signal_server(), want);
            if (!on_it && !signal_request_pending()) lan_host_stop();
        }
        return;
    }
    if (now() > g.lan_deadline) { lan_fail("timed out"); return; }

    switch (g.lan_phase) {
    case LanPhase::HostDial: {
        const int r = lan_connect_step("127.0.0.1", lan_host_port());
        if (r < 0) { lan_fail("could not reach the local lobby"); break; }
        if (r == 0) break;
        g.lan_err0 = signal_error_seq();
        signal_request_create(g.lan_room, g.lan_pw);
        strncpy_s(g.room_name, sizeof(g.room_name), g.lan_room, _TRUNCATE);
        log_line("MENU", "LAN: creating room '%s'%s", g.lan_room, g.lan_pw[0] ? " (password)" : "");
        g.lan_phase = LanPhase::HostCreate;
        g.lan_deadline = now() + 12.0;
        break;
    }
    case LanPhase::HostCreate:
        if (in_room()) { g.lan_phase = LanPhase::None; g.lan_pw[0] = 0; }
        else if (signal_state() != SignalState::Connected) lan_fail("lost the local lobby");
        else if (signal_error_seq() != g.lan_err0) lan_fail("the lobby refused the room");
        break;
    case LanPhase::JoinDial: {
        const int r = lan_connect_step(g.lan_t_addr, g.lan_t_port);
        if (r < 0) { lan_fail("could not reach the host's lobby"); break; }
        if (r == 0) break;
        g.lan_err0 = signal_error_seq();
        if (g.lan_t_pw) {
            // A room with a password: the same prompt as the online list's.
            g.pw_ask = true; g.pw_wrong = false; g.pw_sent = false; g.pw_buf[0] = 0;
            strncpy_s(g.pw_id, sizeof(g.pw_id), g.lan_t_id, _TRUNCATE);
            strncpy_s(g.pw_name, sizeof(g.pw_name), g.room_name, _TRUNCATE);
        } else {
            signal_request_join(g.lan_t_id);
        }
        log_line("MENU", "LAN: joining room %s at %s:%u", g.lan_t_id, g.lan_t_addr, (unsigned)g.lan_t_port);
        g.lan_phase = LanPhase::JoinJoin;
        g.lan_deadline = now() + 15.0;
        break;
    }
    case LanPhase::JoinJoin:
        if (in_room()) { g.lan_phase = LanPhase::None; }
        else if (signal_state() != SignalState::Connected) lan_fail("lost the host's lobby");
        else if (g.lan_t_pw) {
            // Typing the password takes as long as it takes; closing the prompt ends the attempt (the lobby stays connected).
            if (g.pw_ask || g.pw_sent || signal_request_pending()) g.lan_deadline = now() + 15.0;
            else g.lan_phase = LanPhase::None;
        }
        else if (signal_error_seq() != g.lan_err0) lan_fail("the room is gone or full");
        break;
    default: break;
    }
}

// "192.168.1.5:27700" (a few, comma separated) -- what a friend types when the search finds nothing. Looked up every few seconds.
const char* lan_addr_line() {
    static char line[160];
    static double at = -100;
    if (now() - at > 3.0) {
        at = now();
        const std::vector<std::string> ips = lan_local_addresses();
        std::string s;
        for (size_t i = 0; i < ips.size() && i < 3; ++i) {
            if (i) s += ", ";
            s += ips[i] + ":" + std::to_string((unsigned)lan_host_port());
        }
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s", s.c_str());
    }
    return line;
}

void draw_lan_tabs(ImDrawList* dl, const View& v, ImVec2 a, float W) {
    (void)dl;
    const float k = v.k;
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    const bool free = g.lan_phase == LanPhase::None;
    if (paper_button("tab_online", ImVec2(X(70), Y(118)), ImVec2(X(W / 2 - 5), Y(170)), tr(Tx::LAN_TAB_ONLINE), 28 * k, free,
                     g.lan_tab ? Tone::Normal : Tone::Good))
        g.lan_tab = false;
    if (paper_button("tab_lan", ImVec2(X(W / 2 + 5), Y(118)), ImVec2(X(W - 70), Y(170)), tr(Tx::LAN_TAB_LAN), 28 * k, free,
                     g.lan_tab ? Tone::Good : Tone::Normal)) {
        if (!g.lan_tab) { g.lan_tab = true; lan_search_start(); g.lan_next_search = now() + 8.0; }
    }
}

void draw_lan_tab(ImDrawList* dl, const View& v, ImVec2 a, float W, float H) {
    const float k = v.k;
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    const bool busy = signal_request_pending();
    const bool free = g.lan_phase == LanPhase::None && !busy && !g.pw_ask;

    // this player
    text_at(dl, ImVec2(X(70), Y(204)), 28 * k, kInk, tr(Tx::PLAYER_NAME));
    paper_input("##lan_name", ImVec2(X(290), Y(190)), ImVec2(X(W - 70), Y(250)), g.pname, sizeof(g.pname), tr(Tx::NAME_HINT), 28 * k);

    // --- host ---------------------------------------------------------------
    text_at(dl, ImVec2(X(50), Y(270)), 30 * k, kInk, tr(Tx::LAN_HOST_TITLE));
    text_at(dl, ImVec2(X(50), Y(328)), 26 * k, kInk, tr(Tx::ROOM_NAME));
    paper_input("##lan_room", ImVec2(X(190), Y(316)), ImVec2(X(W - 320), Y(368)), g.lan_room, sizeof(g.lan_room), tr(Tx::ROOM_NAME_HINT), 26 * k);
    text_at(dl, ImVec2(X(50), Y(390)), 26 * k, kInk, tr(Tx::ROOM_PW));
    paper_input("##lan_pw", ImVec2(X(190), Y(378)), ImVec2(X(W - 320), Y(430)), g.lan_pw, sizeof(g.lan_pw), tr(Tx::ROOM_PW_HINT), 26 * k,
                ImGuiInputTextFlags_Password);
    if (paper_button("lan_create", ImVec2(X(W - 300), Y(316)), ImVec2(X(W - 50), Y(430)), tr(Tx::LAN_CREATE), 28 * k, free, Tone::Good)) {
        std::string err;
        if (!lan_host_start(err)) {
            say(tr(Tx::LAN_START_FAIL), err.c_str());
        } else {
            g.lan_phase = LanPhase::HostDial;
            g.lan_deadline = now() + 12.0;
            g.lan_dialed = false;
            log_line("MENU", "LAN: hosting '%s' on port %u", g.lan_room, (unsigned)lan_host_port());
        }
    }

    // --- join ---------------------------------------------------------------
    text_at(dl, ImVec2(X(50), Y(462)), 30 * k, kInk, tr(Tx::LAN_JOIN_TITLE));
    const LanSearch ls = lan_search_state();
    if (paper_button("lan_search", ImVec2(X(W - 330), Y(450)), ImVec2(X(W - 50), Y(504)), tr(Tx::LAN_SEARCH_BTN), 24 * k,
                     free && ls != LanSearch::Searching)) {
        lan_search_start();
        g.lan_next_search = now() + 8.0;
    }
    if (free && ls == LanSearch::Done && now() >= g.lan_next_search) { lan_search_start(); g.lan_next_search = now() + 8.0; }

    const std::vector<LanRoom> found = lan_search_results();
    if (ls == LanSearch::Searching) {
        text_at(dl, ImVec2(X(50), Y(508)), 22 * k, kGrey, tr(Tx::LAN_SEARCHING));
    } else if (!found.empty()) {
        char msg[64]; _snprintf_s(msg, sizeof(msg), _TRUNCATE, tr(Tx::LAN_FOUND), (unsigned)found.size());
        text_at(dl, ImVec2(X(50), Y(508)), 22 * k, kGreen, msg);
    }

    const float lx0 = X(50), lx1 = X(W - 50), ly0 = Y(540), ly1 = Y(724);
    rough_rect(dl, ImVec2(lx0, ly0), ImVec2(lx1, ly1), IM_COL32(255, 255, 250, 70), kInk, 2.2f, 93, 1.2f);
    if (found.empty()) {
        if (ls != LanSearch::Searching) wrap_text(dl, lx0 + 24 * k, ly0 + 40 * k, lx1 - lx0 - 48 * k, 22 * k, 27 * k, kInkSoft, tr(Tx::LAN_NONE));
    } else {
        text_at(dl, ImVec2(lx0 + 20 * k, ly0 + 8 * k), 20 * k, kGrey, tr(Tx::COL_ROOM));
        text_at(dl, ImVec2(X(400), ly0 + 8 * k), 20 * k, kGrey, tr(Tx::COL_HOST));
        text_at(dl, ImVec2(X(640), ly0 + 8 * k), 20 * k, kGrey, tr(Tx::COL_PLAYERS));
        ImGui::SetCursorScreenPos(ImVec2(lx0, ly0 + 34 * k));
        ImGui::BeginChild("##lan_rooms", ImVec2(lx1 - lx0, ly1 - ly0 - 38 * k), false,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float rowh = 58 * k;
        for (size_t i = 0; i < found.size(); ++i) {
            const LanRoom& r = found[i];
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            float name_x = rp.x + 20 * k;
            if (r.pw) { draw_lock(cdl, ImVec2(name_x + 14 * k, rp.y + rowh * 0.5f), 28 * k, kInk); name_x += 40 * k; }
            text_at(cdl, ImVec2(name_x, rp.y + 14 * k), 26 * k, kInk, r.name.empty() ? r.id.c_str() : r.name.c_str());
            text_at(cdl, ImVec2(X(400), rp.y + 5 * k), 24 * k, kInkSoft, r.host.c_str());
            text_at(cdl, ImVec2(X(400), rp.y + 33 * k), 18 * k, kGrey, r.addr.c_str());
            char cnt[16]; _snprintf_s(cnt, sizeof(cnt), _TRUNCATE, "%d/%d", r.players, r.cap);
            text_at(cdl, ImVec2(X(640), rp.y + 14 * k), 26 * k, kInkSoft, cnt);
            const bool full = r.players >= r.cap;
            char bid[48]; _snprintf_s(bid, sizeof(bid), _TRUNCATE, "lanjoin_%s_%s", r.addr.c_str(), r.id.c_str());
            if (paper_button(bid, ImVec2(X(730), rp.y + 5 * k), ImVec2(X(W - 70), rp.y + rowh - 5 * k),
                             r.locked ? tr(Tx::ROOM_LOCKED_BTN) : full ? tr(Tx::FULL) : tr(Tx::JOIN), 24 * k, !r.locked && !full && free, Tone::Good)) {
                strncpy_s(g.lan_t_addr, sizeof(g.lan_t_addr), r.addr.c_str(), _TRUNCATE);
                strncpy_s(g.lan_t_id, sizeof(g.lan_t_id), r.id.c_str(), _TRUNCATE);
                g.lan_t_port = r.port;
                g.lan_t_pw = r.pw;
                strncpy_s(g.room_name, sizeof(g.room_name), r.name.empty() ? r.id.c_str() : r.name.c_str(), _TRUNCATE);
                g.lan_phase = LanPhase::JoinDial;
                g.lan_deadline = now() + 12.0;
                g.lan_dialed = false;
            }
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(lx1 - lx0, rowh));
        }
        ImGui::EndChild();
    }

    wrap_text(dl, X(50), Y(736), (W - 100) * k, 19 * k, 23 * k, kGrey, tr(Tx::LAN_FIREWALL));

    if (paper_button("lan_back", ImVec2(X(70), Y(H - 84)), ImVec2(X(W - 70), Y(H - 24)), tr(Tx::BACK), 30 * k))
        g.win = Win::None;
}

void draw_room_info(ImDrawList* dl, const View& v, ImVec2 o, float w, float& y_out, bool compact);

// THE CREATE-ROOM PAGE (2026-10-05): the connection method, the room's name and an optional password, on a page of their own -- they shared the room list's page and it was too crowded.
void draw_create_page(ImDrawList* dl, const View& v, ImVec2 a, float W, float H, bool busy) {
    const float k = v.k;
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(150), 36 * k, kInk, tr(Tx::CREATE_ROOM));

    // THE CARRIER (2026-10-03): the game port (direct) or Steam's relay. "Direct" is only offered once the server has seen the port from outside.
    RoomTransport chosen = RoomTransport::Any;
    float y = 200;
    if (!signal_server_private()) {      // a lobby on this network needs no choosing
        const DirectCheck ds = signal_direct_check();
        const bool direct_ok = ds == DirectCheck::Open || ds == DirectCheck::NoAnswer;
        const bool steam_ok = signal_steam_usable();
        int relay_used = 0, relay_max = 0;
        const bool relay_known = signal_relay_info(&relay_used, &relay_max);
        const bool relay_ok = relay_known && relay_used < relay_max;
        int eff = g.transport;
        if (eff == 1 && !direct_ok) eff = 0;
        if (eff == 2 && !steam_ok) eff = 0;
        if (eff == 3 && !relay_ok) eff = 0;
        if (eff == 0) eff = direct_ok ? 1 : (steam_ok ? 2 : 0);
        const unsigned port = (unsigned)config().net_port;
        char lbl[96];
        text_at(dl, ImVec2(X(50), Y(y + 14)), 26 * k, kInk, tr(Tx::TR_LABEL));
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, tr(Tx::TR_DIRECT), port);
        if (paper_button("tr_direct", ImVec2(X(190), Y(y)), ImVec2(X(400), Y(y + 52)), lbl, 22 * k, direct_ok && !busy, eff == 1 ? Tone::Good : Tone::Normal)) g.transport = 1;
        if (paper_button("tr_steam", ImVec2(X(410), Y(y)), ImVec2(X(620), Y(y + 52)), tr(Tx::TR_STEAM), 22 * k, steam_ok && !busy, eff == 2 ? Tone::Good : Tone::Normal)) g.transport = 2;
        text_c(dl, X(515), Y(y + 56), 18 * k, kGreen, tr(Tx::TR_RECOMMENDED));   // the small "recommended" under the Steam button (user request 2026-10-11)
        if (relay_known) _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "%s %u/%u", tr(Tx::TR_RELAY), (unsigned)relay_used, (unsigned)relay_max);
        else             _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "%s", tr(Tx::TR_RELAY));
        if (paper_button("tr_relay", ImVec2(X(630), Y(y)), ImVec2(X(W - 50), Y(y + 52)), lbl, 22 * k, relay_ok && !busy && !g.relay_warn, eff == 3 ? Tone::Good : Tone::Normal)) {
            // The project's own server is small: say so before the server is chosen as the carrier
            if (strncmp(signal_server(), "49.233.209.67:", 14) == 0) g.relay_warn = true;
            else g.transport = 3;
        }
        char hint[400] = {};
        ImU32 col = kInkSoft;
        if (eff == 3) {
            _snprintf_s(hint, sizeof(hint), _TRUNCATE, tr(Tx::TR_RELAY_SEL), (unsigned)relay_used, (unsigned)relay_max);
        } else {
            switch (ds) {
                case DirectCheck::Unknown:
                case DirectCheck::Checking: _snprintf_s(hint, sizeof(hint), _TRUNCATE, tr(Tx::TR_CHECKING), port); break;
                case DirectCheck::Open:     _snprintf_s(hint, sizeof(hint), _TRUNCATE, tr(Tx::TR_OPEN), port); col = kGreen; break;
                case DirectCheck::Closed:   _snprintf_s(hint, sizeof(hint), _TRUNCATE, tr(Tx::TR_CLOSED), port); col = kYellow; break;
                case DirectCheck::Busy:     _snprintf_s(hint, sizeof(hint), _TRUNCATE, tr(Tx::TR_BUSY), port); col = kRed; break;
                case DirectCheck::NoAnswer: _snprintf_s(hint, sizeof(hint), _TRUNCATE, "%s", tr(Tx::TR_NOANSWER)); col = kAmber; break;
            }
            if (!steam_ok) { const size_t n = strlen(hint); _snprintf_s(hint + n, sizeof(hint) - n, _TRUNCATE, " %s", tr(Tx::TR_STEAM_NA)); }
        }
        if (!relay_known) { const size_t n = strlen(hint); _snprintf_s(hint + n, sizeof(hint) - n, _TRUNCATE, " %s", tr(Tx::TR_RELAY_NA)); }
        else if (!relay_ok) { const size_t n = strlen(hint); char full[200]; _snprintf_s(full, sizeof(full), _TRUNCATE, tr(Tx::TR_RELAY_FULL), (unsigned)relay_used, (unsigned)relay_max); _snprintf_s(hint + n, sizeof(hint) - n, _TRUNCATE, " %s", full); }
        wrap_text(dl, X(50), Y(y + 88), (W - 100 - 180) * k, 22 * k, 26 * k, col, hint);
        if (paper_button("tr_recheck", ImVec2(X(W - 190), Y(y + 84)), ImVec2(X(W - 50), Y(y + 134)), tr(Tx::TR_RECHECK), 22 * k, ds != DirectCheck::Checking && !busy)) signal_request_direct_check();
        chosen = eff == 1 ? RoomTransport::Direct : eff == 2 ? RoomTransport::Steam : eff == 3 ? RoomTransport::Relay : RoomTransport::Any;
        y += 178;
    }

    // the name, and an optional password (empty = an open room)
    text_at(dl, ImVec2(X(50), Y(y + 12)), 26 * k, kInk, tr(Tx::ROOM_NAME));
    paper_input("##room", ImVec2(X(190), Y(y)), ImVec2(X(W - 50), Y(y + 52)), g.room, sizeof(g.room), tr(Tx::ROOM_NAME_HINT), 26 * k);
    text_at(dl, ImVec2(X(50), Y(y + 84)), 26 * k, kInk, tr(Tx::ROOM_PW));
    paper_input("##roompw", ImVec2(X(190), Y(y + 72)), ImVec2(X(W - 50), Y(y + 124)), g.create_pw, sizeof(g.create_pw), tr(Tx::ROOM_PW_HINT), 26 * k, ImGuiInputTextFlags_Password);

    if (paper_button("create", ImVec2(X(70), Y(H - 130)), ImVec2(X(W / 2 - 10), Y(H - 58)), tr(Tx::CREATE_ROOM), 34 * k, !busy && !g.pw_ask, Tone::Good)) {
        signal_request_create(g.room, g.create_pw, chosen);
        strncpy_s(g.room_name, sizeof(g.room_name), g.room, _TRUNCATE);
        log_line("MENU", "create requested: '%s'%s, carrier %s", g.room, g.create_pw[0] ? " (password)" : "",
                 chosen == RoomTransport::Direct ? "direct" : chosen == RoomTransport::Steam ? "Steam" : chosen == RoomTransport::Relay ? "server relay" : "any");
    }
    if (paper_button("create_back", ImVec2(X(W / 2 + 10), Y(H - 130)), ImVec2(X(W - 70), Y(H - 58)), tr(Tx::BACK), 34 * k)) g.create_page = false;
}

void draw_multi_window(const View& v) {
    ensure_inited();
    const SignalState st0 = signal_state();
    const bool disconnected = st0 == SignalState::Off || st0 == SignalState::Failed || st0 == SignalState::Closed;
    if (in_room() || st0 != SignalState::Connected) g.create_page = false;      // the form belongs to the connected lobby
    const float W = 900, H = in_room() ? (lan_host_running() ? 886.0f : 860.0f) : (st0 == SignalState::Connected ? (g.create_page ? (signal_server_private() ? 560.0f : 700.0f) : 780.0f) : (disconnected && g.lan_tab ? 880.0f : 760.0f));
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f + 20);
    const ImVec2 sz(W * v.k, H * v.k);
    if (!overlay_window_begin("##mgmp_multi", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));

    const float k = v.k;
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    text_c(dl, X(W / 2), Y(46), 52 * k, kInk, tr(Tx::MENU_MULTI));

    const SignalState st = signal_state();
    const bool busy = signal_request_pending();

    if (in_room()) {
        // Reopened while a room is open: show it instead of the connect form.
        float top = 130;
        if (lan_host_running() && lan_addr_line()[0]) {
            char al[200]; _snprintf_s(al, sizeof(al), _TRUNCATE, tr(Tx::LAN_YOUR_ADDR), lan_addr_line());
            text_c(dl, X(W / 2), Y(116), 24 * k, kGreen, al);     // the title ends at about 108
            top = 156;
        }
        float y = 0;
        draw_room_info(dl, v, ImVec2(X(60), Y(top)), (W - 120) * k, y, false);
        if (room_is_host()) {
            const bool locked = room_locked();
            const bool can = locked ? room_can_unlock() : true;
            if (paper_button("lock", ImVec2(X(60), Y(H - 172)), ImVec2(X(W - 60), Y(H - 112)),
                             locked ? (can ? tr(Tx::UNLOCK) : tr(Tx::UNLOCK_WHEN)) : tr(Tx::LOCK),
                             30 * k, true, locked ? Tone::Normal : Tone::Good))
                room_request_lock(!locked);
        }
        if (paper_button("leave", ImVec2(X(60), Y(H - 100)), ImVec2(X(W / 2 - 10), Y(H - 40)), tr(Tx::LEAVE_ROOM), 30 * k, !busy, Tone::Danger))
            signal_request_leave();
        if (paper_button("back", ImVec2(X(W / 2 + 10), Y(H - 100)), ImVec2(X(W - 60), Y(H - 40)), tr(Tx::BACK), 30 * k))
            g.win = Win::None;
        ImGui::End();
        return;
    }

    if (st == SignalState::Off || st == SignalState::Failed || st == SignalState::Closed) {
        draw_lan_tabs(dl, v, a, W);
        if (g.lan_tab) { draw_lan_tab(dl, v, a, W, H); ImGui::End(); return; }
        text_c(dl, X(W / 2), Y(186), 26 * k, kInkSoft, tr(Tx::CONNECT_FIRST));

        text_at(dl, ImVec2(X(70), Y(252)), 30 * k, kInk, tr(Tx::SERVER_ADDR));
        paper_input("##addr", ImVec2(X(290), Y(240)), ImVec2(X(W - 70), Y(302)), g.addr, sizeof(g.addr), "localhost", 30 * k);
        text_at(dl, ImVec2(X(70), Y(342)), 30 * k, kInk, tr(Tx::PLAYER_NAME));
        paper_input("##name", ImVec2(X(290), Y(330)), ImVec2(X(W - 70), Y(392)), g.pname, sizeof(g.pname), tr(Tx::NAME_HINT), 30 * k);

        const char* err = signal_error();
        if (st == SignalState::Failed && err[0]) {
            text_c(dl, X(W / 2), Y(470), 26 * k, kRed, tr(Tx::NET_FAILED));
            text_c(dl, X(W / 2), Y(508), 22 * k, kRed, err);
        } else if (st == SignalState::Closed) {
            text_c(dl, X(W / 2), Y(470), 26 * k, kAmber, tr(Tx::SERVER_LOST));
        }

        if (paper_button("connect", ImVec2(X(70), Y(H - 130)), ImVec2(X(W / 2 - 10), Y(H - 58)), tr(Tx::CONNECT), 34 * k, !busy, Tone::Good)) {
            char host[64]; uint16_t port = config().signal_port;
            split_addr(g.addr, host, sizeof(host), port);
            signal_request_connect(host, port, g.pname);
            log_line("MENU", "connect requested: %s:%u as '%s'", host, (unsigned)port, g.pname);
            if (config_set_player_name(g.pname)) log_line("MENU", "the player name '%s' is remembered for the next start (ui.player_name)", g.pname);
        }
        if (paper_button("back", ImVec2(X(W / 2 + 10), Y(H - 130)), ImVec2(X(W - 70), Y(H - 58)), tr(Tx::BACK), 34 * k))
            g.win = Win::None;
        ImGui::End();
        return;
    }

    if (st == SignalState::Connecting) {
        const int dots = (int)(now() * 3.0) % 4;
        char msg[48]; _snprintf_s(msg, sizeof(msg), _TRUNCATE, tr(Tx::CONNECTING_SERVER), dots, "...");
        text_c(dl, X(W / 2), Y(280), 40 * k, kInk, msg);
        if (paper_button("cancel", ImVec2(X(W / 2 - 150), Y(H - 130)), ImVec2(X(W / 2 + 150), Y(H - 58)), tr(Tx::CANCEL), 34 * k)) {
            g.lan_phase = LanPhase::None;
            signal_request_disconnect();
        }
        ImGui::End();
        return;
    }

    // --- connected, no room: the list -----------------------------------------
    if (now() >= g.next_list && !busy) { signal_request_list(); g.next_list = now() + 3.0; }

    char who[120];
    _snprintf_s(who, sizeof(who), _TRUNCATE, tr(Tx::CONNECTED_AS), signal_server(), signal_name());
    text_c(dl, X(W / 2), Y(112), 26 * k, kGreen, who);

    if (g.create_page) { draw_create_page(dl, v, a, W, H, busy); ImGui::End(); return; }

    // search
    text_at(dl, ImVec2(X(50), Y(150)), 26 * k, kInk, tr(Tx::SEARCH));
    paper_input("##search", ImVec2(X(190), Y(138)), ImVec2(X(W - 50), Y(190)), g.search, sizeof(g.search), tr(Tx::SEARCH_HINT), 26 * k);

    const float lx0 = X(50), lx1 = X(W - 50), ly0 = Y(204), ly1 = Y(H - 140);
    rough_rect(dl, ImVec2(lx0, ly0), ImVec2(lx1, ly1), IM_COL32(255, 255, 250, 70), kInk, 2.2f, 91, 1.2f);
    const uint32_t total = signal_rooms(g.rooms, 32);
    // The rows that match the search: open rooms first, locked ones after them (the server sends them that way
    // already; sorting here as well keeps the list right whatever sent it).
    // THE ROOMS JOINED WITHIN A DAY, AND THE ROOMS OF THEIR HOSTS, come first (and are highlighted); inside each group the open rooms are before the locked ones.
    uint32_t order[32]; bool recent[32] = {}; uint32_t n = 0;
    for (uint32_t i = 0; i < total && i < 32; ++i) recent[i] = roomhist_recent(g.rooms[i].id, g.rooms[i].host);
    // a room whose players have started (locked) comes after every room that can be joined
    for (int lk = 0; lk < 2; ++lk)
    for (int grp = 0; grp < 2; ++grp)
        for (int pass = 0; pass < 2; ++pass)
            for (uint32_t i = 0; i < total && i < 32; ++i)
                if (g.rooms[i].locked == (lk == 1) && recent[i] == (grp == 0) && g.rooms[i].pw == (pass == 1) &&
                    (contains_ci(g.rooms[i].name, g.search) || contains_ci(g.rooms[i].host, g.search) || contains_ci(g.rooms[i].id, g.search)))
                    order[n++] = i;
    if (n == 0) {
        text_c(dl, X(W / 2), Y(300), 30 * k, kInkSoft, total == 0 ? tr(Tx::NO_ROOMS) : tr(Tx::NO_MATCH));
    } else {
        text_at(dl, ImVec2(lx0 + 20 * k, ly0 + 10 * k), 22 * k, kGrey, tr(Tx::COL_ROOM));
        text_at(dl, ImVec2(X(420), ly0 + 10 * k), 22 * k, kGrey, tr(Tx::COL_HOST));
        text_at(dl, ImVec2(X(640), ly0 + 10 * k), 22 * k, kGrey, tr(Tx::COL_PLAYERS));
        ImGui::SetCursorScreenPos(ImVec2(lx0, ly0 + 44 * k));
        ImGui::BeginChild("##rooms", ImVec2(lx1 - lx0, ly1 - ly0 - 50 * k), false,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float rowh = 62 * k;
        for (uint32_t oi = 0; oi < n; ++oi) {
            const SignalRoom& r = g.rooms[order[oi]];
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            if (recent[order[oi]]) {      // joined within a day, or its host's other room: a tinted band with a bar at its left edge
                cdl->AddRectFilled(ImVec2(rp.x + 2 * k, rp.y + 2 * k), ImVec2(rp.x + (lx1 - lx0) - 2 * k, rp.y + rowh - 2 * k), IM_COL32(120, 200, 120, 70), 6 * k);
                cdl->AddRectFilled(ImVec2(rp.x + 2 * k, rp.y + 2 * k), ImVec2(rp.x + 10 * k, rp.y + rowh - 2 * k), IM_COL32(60, 150, 70, 200), 4 * k);
            }
            float name_x = rp.x + 20 * k;
            if (r.pw) { draw_lock(cdl, ImVec2(name_x + 14 * k, rp.y + rowh * 0.5f), 30 * k, kInk); name_x += 40 * k; }
            text_at(cdl, ImVec2(name_x, rp.y + 10 * k), 28 * k, kInk, r.name[0] ? r.name : r.id);
            text_at(cdl, ImVec2(X(420), rp.y + 12 * k), 26 * k, kInkSoft, r.host);
            char cnt[16]; _snprintf_s(cnt, sizeof(cnt), _TRUNCATE, "%u/%u", (unsigned)r.players, (unsigned)r.cap);
            text_at(cdl, ImVec2(X(640), rp.y + 12 * k), 26 * k, kInkSoft, cnt);
            const bool full = r.players >= r.cap;
            if (paper_button(r.id, ImVec2(X(730), rp.y + 6 * k), ImVec2(X(W - 70), rp.y + rowh - 6 * k),
                             r.locked ? tr(Tx::ROOM_LOCKED_BTN) : full ? tr(Tx::FULL) : tr(Tx::JOIN), 26 * k, !r.locked && !full && !busy && !g.pw_ask, Tone::Good)) {
                strncpy_s(g.room_name, sizeof(g.room_name), r.name[0] ? r.name : r.id, _TRUNCATE);
                roomhist_set_pending(r.id, r.host);        // noted once the lobby has really put this peer in it
                if (r.pw) {
                    // A locked room: ask for the password first.
                    g.pw_ask = true; g.pw_wrong = false; g.pw_sent = false; g.pw_buf[0] = 0;
                    strncpy_s(g.pw_id, sizeof(g.pw_id), r.id, _TRUNCATE);
                    strncpy_s(g.pw_name, sizeof(g.pw_name), g.room_name, _TRUNCATE);
                    log_line("MENU", "room %s (%s) is locked -- asking for its password", r.id, r.name);
                } else {
                    signal_request_join(r.id);
                    log_line("MENU", "join requested: room %s (%s)", r.id, r.name);
                }
            }
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(lx1 - lx0, rowh));   // an item after the cursor moves, or ImGui asserts on the child's End
        }
        ImGui::EndChild();
    }

    // the bottom row: create (opens the form on its own page), refresh, disconnect, back
    {
        const float bw = (W - 100 - 30) / 4;
        auto bx = [&](int i) { return 50 + i * (bw + 10); };
        if (paper_button("open_create", ImVec2(X(bx(0)), Y(H - 120)), ImVec2(X(bx(0) + bw), Y(H - 56)), tr(Tx::CREATE_ROOM), 28 * k, !busy && !g.pw_ask, Tone::Good)) g.create_page = true;
        if (paper_button("refresh", ImVec2(X(bx(1)), Y(H - 120)), ImVec2(X(bx(1) + bw), Y(H - 56)), tr(Tx::REFRESH), 28 * k, !busy)) {
            signal_request_list(); g.next_list = now() + 3.0;
        }
        if (paper_button("disc", ImVec2(X(bx(2)), Y(H - 120)), ImVec2(X(bx(2) + bw), Y(H - 56)), tr(Tx::DISCONNECT_SERVER), 28 * k, !busy, Tone::Danger))
            signal_request_disconnect();
        if (paper_button("back", ImVec2(X(bx(3)), Y(H - 120)), ImVec2(X(bx(3) + bw), Y(H - 56)), tr(Tx::BACK), 28 * k))
            g.win = Win::None;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// the room: shared by the title-screen window and the in-game panel
// ---------------------------------------------------------------------------

// The page of the k-th player row that is NOT this peer, by handing the session's
// ids out in ascending order. The room list and the PEERS membership both start at
// the host, so the two line up while the membership is stable.
// Now by ROW of the room list and by name (mgmp_room.h): the ascending-id pairing broke the moment a
// player reconnected and came back with a higher id than someone who joined after it.
uint8_t peer_for_row(uint32_t row) { return room_peer_for_row(row); }

PageState page_for_row(uint32_t k) {
    const uint8_t id = peer_for_row(k);
    return id == 0xFF ? PageState::Unknown : page_of(id);
}

// The cat cards of the player on a row (self = -1 row index handled by the caller).
uint32_t cards_for_row(bool is_me, uint32_t row, const CatBrief** cats) {
    *cats = nullptr;
    if (is_me) return catview_page_shows((uint8_t)page_self()) ? catview_self(cats) : 0;
    const uint8_t id = peer_for_row(row);
    if (id == 0xFF || !catview_page_shows((uint8_t)page_of(id))) return 0;
    return catview_of(id, cats);
}

// Does any player in the room have cards up? The panel is widened for them.
bool any_cards() {
    const CatBrief* c = nullptr;
    if (cards_for_row(true, 0, &c)) return true;
    for (uint32_t i = 0; i < kMaxPeers; ++i)
        if (cards_for_row(false, i, &c)) return true;
    return false;
}

ImU32 palette_col(int row, int col, ImU32 fallback) {
    if (row < 0 || row >= kPaletteRows) return fallback;
    const uint32_t c = kCatPalette[row][col];
    return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
}

// A cat's head: the artwork in assets\ui\cat_head.png (black outline, white inside, transparent outside),
// its white multiplied by a colour. WHICH colour (class_view_col): a classed cat is painted in its CLASS
// palette (measured against the game's own map panel), a collarless cat keeps its inherited fur row. The
// dark classes were all lifted to one grey and could not be told apart (2026-10-01), so they are now
// chosen by hand -- Monk light grey, Druid brown, Necromancer slate teal -- and any other dark colour is
// brightened with its hue kept. The Jester has no colour of its own: it wears three (see draw_cat_head).
constexpr ImU32 kJesterRed   = IM_COL32(214, 64, 56, 255);
constexpr ImU32 kJesterBlue  = IM_COL32(64, 112, 214, 255);
constexpr ImU32 kJesterGreen = IM_COL32(66, 164, 84, 255);

ImU32 class_view_col(const CatBrief& cat) {
    switch (cat.klass) {
        case 7:  return IM_COL32(196, 196, 196, 255);   // Monk: palette grey 120 -> a light grey
        case 9:  return IM_COL32(168, 116, 78, 255);    // Druid: palette near-black brown -> a warm brown
        case 11: return IM_COL32(72, 72, 80, 255);      // Necromancer: palette black -> the darkest grey that still shows the outline
        case 13: return kJesterGreen;                   // Jester: the face (ears are drawn over it)
        default: break;
    }
    const int row = cat.coat >= 0 ? cat.coat : cat.fur;
    const ImU32 col = palette_col(row, 0, IM_COL32(222, 214, 196, 255));
    float R = (float)((col >> IM_COL32_R_SHIFT) & 255), G = (float)((col >> IM_COL32_G_SHIFT) & 255),
          B = (float)((col >> IM_COL32_B_SHIFT) & 255);
    const float mx = R > G ? (R > B ? R : B) : (G > B ? G : B);
    // Too dark for the outline to read against: scale it up so its brightest channel reaches 158 -- the
    // hue stays (a dark green becomes a green, not a grey).
    if (mx < 158.0f) {
        const float f = mx < 8.0f ? 0.0f : 158.0f / mx;
        if (f == 0.0f) { R = G = B = 112.0f; }
        else { R = R * f > 255 ? 255 : R * f; G = G * f > 255 ? 255 : G * f; B = B * f > 255 ? 255 : B * f; }
    }
    return IM_COL32((int)R, (int)G, (int)B, 255);
}

void draw_cat_head(ImDrawList* dl, ImVec2 c, float r, const CatBrief& cat) {
    const ImU32 col = class_view_col(cat);
    const float w = 2.35f * r;               // the art is 1.107 : 1
    const float h = w / 1.107f;
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f);
    if (tex_load(g.cat_head, "ui\\cat_head.png")) {
        dl->AddImage(g.cat_head.ref(), a, ImVec2(a.x + w, a.y + h), ImVec2(0, 0), ImVec2(1, 1), col);
        if (cat.klass == 13) {
            // The two ears over the green face: cat_ear_l / cat_ear_r are the head picture cut down to the
            // white inside of each ear (they are separate shapes, so no rectangle could split them from the
            // face), drawn over it in red and blue.
            if (tex_load(g.cat_ear_l, "ui\\cat_ear_l.png"))
                dl->AddImage(g.cat_ear_l.ref(), a, ImVec2(a.x + w, a.y + h), ImVec2(0, 0), ImVec2(1, 1), kJesterRed);
            if (tex_load(g.cat_ear_r, "ui\\cat_ear_r.png"))
                dl->AddImage(g.cat_ear_r.ref(), a, ImVec2(a.x + w, a.y + h), ImVec2(0, 0), ImVec2(1, 1), kJesterBlue);
        }
    } else {
        dl->AddCircleFilled(c, r, col, 24);
        dl->AddCircle(c, r, kInk, 24, 2.0f);
    }
}

// One cat: head and name/level on top, the health bar, then class and health value.
void draw_cat_card(ImDrawList* dl, ImVec2 p, float k, float w, const CatBrief& cat) {
    const float hr = 17.0f * k;                          // head radius
    draw_cat_head(dl, ImVec2(p.x + hr + 3 * k, p.y + hr + 4 * k), hr, cat);

    const float tx = p.x + 2 * hr + 12 * k;
    ImDrawList* d = dl;
    d->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + w, p.y + 46 * k), true);
    text_at(d, ImVec2(tx, p.y + 1 * k), 16 * k, kInk, cat.name[0] ? cat.name : "?");
    char lv[24];
    _snprintf_s(lv, sizeof(lv), _TRUNCATE, "Lv %d", cat.level);
    text_at(d, ImVec2(tx, p.y + 20 * k), 18 * k, kInkSoft, lv);
    d->PopClipRect();

    // health
    const bool full = cat.hp >= kBriefFullHp;
    int maxhp = cat.maxhp > 0 ? cat.maxhp : (full ? 1 : cat.hp);
    if (!full && cat.hp > maxhp) maxhp = cat.hp;          // the estimate is a floor
    float frac = full ? 1.0f : (maxhp > 0 ? (float)cat.hp / (float)maxhp : 0.0f);
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
    const ImVec2 b0(p.x + 2 * k, p.y + 46 * k), b1(p.x + w - 2 * k, p.y + 57 * k);
    dl->AddRectFilled(b0, b1, IM_COL32(70, 64, 60, 200), 3.0f);
    if (frac > 0.0f)
        dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * frac, b1.y),
                          frac > 0.5f ? kGreen : (frac > 0.25f ? kAmber : kRed), 3.0f);
    dl->AddRect(b0, b1, kInk, 3.0f, 0, 1.6f);

    // class, in the class's own palette colour, and the health value
    const ImU32 cc = (cat.klass < kClassCount || cat.coat >= 0) ? class_view_col(cat) : kGrey;
    const float cy = p.y + 62 * k;
    if (cat.klass == 13) {
        // Red, blue and green thirds: the Jester's three colours.
        const ImVec2 dc(p.x + 8 * k, cy + 9 * k); const float dr = 6.0f * k;
        const ImU32 thirds[3] = { kJesterRed, kJesterBlue, kJesterGreen };
        for (int i = 0; i < 3; ++i) {
            dl->PushClipRect(ImVec2(dc.x - dr + i * (2 * dr / 3.0f), dc.y - dr - 1),
                             ImVec2(dc.x - dr + (i + 1) * (2 * dr / 3.0f), dc.y + dr + 1), true);
            dl->AddCircleFilled(dc, dr, thirds[i], 12);
            dl->PopClipRect();
        }
    } else {
        dl->AddCircleFilled(ImVec2(p.x + 8 * k, cy + 9 * k), 6.0f * k, cc, 12);
    }
    dl->AddCircle(ImVec2(p.x + 8 * k, cy + 9 * k), 6.0f * k, kInk, 12, 1.4f);
    text_at(dl, ImVec2(p.x + 18 * k, cy), 17 * k, kInk, tr_class(cat.klass));
    if (!full) {
        char hp[16];
        _snprintf_s(hp, sizeof(hp), _TRUNCATE, "%d", cat.hp);
        const float hw = text_size(16 * k, hp).x;
        text_at(dl, ImVec2(p.x + w - hw - 2 * k, cy + 1 * k), 16 * k, kInkSoft, hp);
    }
}

// The cards to the right of a player's name, four across.
void draw_cat_cards(ImDrawList* dl, ImVec2 p, float k, float w, const CatBrief* cats, uint32_t n) {
    const float gap = 5 * k;
    const float cw = (w - gap * 3) / 4.0f;
    for (uint32_t i = 0; i < n && i < 4; ++i)
        draw_cat_card(dl, ImVec2(p.x + i * (cw + gap), p.y), k, cw, cats[i]);
}

void draw_room_info(ImDrawList* dl, const View& v, ImVec2 o, float w, float& y_out, bool compact) {
    const float k = v.k;
    const bool host = _stricmp(signal_role(), "host") == 0;
    float y = o.y;

    // The room's display name: the server's list when it still has us in it,
    // else the name this peer typed or clicked on the way in.
    const char* room_label = g.room_name[0] ? g.room_name : signal_room();
    SignalRoom rooms[32];
    const uint32_t nr = signal_rooms(rooms, 32);
    uint32_t cap = 0;
    for (uint32_t i = 0; i < nr; ++i)
        if (strcmp(rooms[i].id, signal_room()) == 0) {
            if (rooms[i].name[0]) room_label = rooms[i].name;
            cap = rooms[i].cap;
            break;
        }

    char line[200];
    text_at(dl, ImVec2(o.x, y), 32 * k, kInk, room_label);
    y += 44 * k;
    _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::ROOM_LINE), signal_room(), host ? tr(Tx::ROLE_HOST) : tr(Tx::ROLE_PLAYER));
    text_at(dl, ImVec2(o.x, y), 22 * k, kInkSoft, line);
    y += 34 * k;

    // Players.
    y += 8 * k;
    dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + w, y), kInk, 2.0f);
    y += 10 * k;
    const uint32_t n = signal_peers(g.peers, 8);
    if (cap) _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::PLAYERS_OF), (unsigned)n, (unsigned)cap);
    else     _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::PLAYERS_N), (unsigned)n);
    text_at(dl, ImVec2(o.x, y), 22 * k, kGrey, line);
    y += 36 * k;

    const Status me = session_state_text();
    const bool ready = net_state() == NetState::Ready;
    const float rowh = 100 * k;
    for (uint32_t i = 0; i < n; ++i) {
        const SignalPeer& p = g.peers[i];
        const bool is_me = strcmp(p.name, signal_name()) == 0;
        const bool is_host = _stricmp(p.role, "host") == 0;
        const CatBrief* cats = nullptr;
        const bool away = room_row_away(i);
        const uint32_t nc = away ? 0 : cards_for_row(is_me, i, &cats);
        const float name_col = 270 * k;
        if (nc) dl->PushClipRect(ImVec2(o.x, y - 4 * k), ImVec2(o.x + name_col - 8 * k, y + rowh), true);
        // Connection state colours the dot; it is not written out any more.
        ImU32 col;
        if (away)         col = kAmber;
        else if (is_me)   col = me.col;
        else if (ready)   col = kGreen;
        else              col = kAmber;
        status_dot(dl, ImVec2(o.x + 14 * k, y + rowh * 0.34f), 10 * k, col);
        // P1..P4 in the room list's order (host first), each in its own colour.
        {
            static const ImU32 kPlayer[4] = { IM_COL32(205, 58, 52, 255), IM_COL32(58, 108, 205, 255),
                                              IM_COL32(214, 168, 24, 255), IM_COL32(56, 150, 68, 255) };
            char pn[8];
            _snprintf_s(pn, sizeof(pn), _TRUNCATE, "P%u", (unsigned)(i + 1));
            // Centred on the dot's centre line, not hung from the top of the row.
            text_at(dl, ImVec2(o.x + 34 * k, y + rowh * 0.34f - text_size(30 * k, pn).y * 0.5f), 30 * k, kPlayer[i & 3], pn);
        }
        const float name_x = o.x + 84 * k;
        char nm[80];
        _snprintf_s(nm, sizeof(nm), _TRUNCATE, "%s%s", p.name, is_me ? tr(Tx::YOU_SUFFIX) : "");
        text_at(dl, ImVec2(name_x, y + 2 * k), 28 * k, kInk, nm);

        // WHAT that player is looking at -- which a connection state cannot say. Resolved by handing out
        // the session's ids to the rows that are not us, in ascending order: both lists start at the
        // host, so the pairing holds while the membership does. After a disconnect a label can be
        // stale, never wrong about who it is describing.
        {
            const PageState pg = is_me ? page_self() : page_for_row(i);
            if (away)
                text_at(dl, ImVec2(name_x, y + 42 * k), 24 * k, kAmber, tr(Tx::AWAY_BOSS));
            else if (pg != PageState::Unknown)
                text_at(dl, ImVec2(name_x, y + 42 * k), 24 * k, kInkSoft, page_name(pg));
            else if (!is_me)
                text_at(dl, ImVec2(name_x, y + 42 * k), 24 * k, kGrey, tr(Tx::UNKNOWN));
        }
        // 0.1.85: this player's round trip to that one (mgmp_perf's 2 s ping; between two clients it is the path through the host, the one their messages take)
        if (!is_me && !away) {
            uint32_t ms = 0;
            bool via = false;
            if (perf_peer_latency_ms(peer_for_row(i), &ms, &via)) {
                char lat[96];
                _snprintf_s(lat, sizeof(lat), _TRUNCATE, tr(via ? Tx::LATENCY_VIA_HOST : Tx::LATENCY_MS), (unsigned)ms);
                text_at(dl, ImVec2(name_x, y + 72 * k), 20 * k, ms < 150 ? kGreen : ms < 500 ? kAmber : kRed, lat);
            }
        }
        if (nc) {
            dl->PopClipRect();
            draw_cat_cards(dl, ImVec2(o.x + name_col, y + 8 * k), k, w - name_col, cats, nc);
        }
        y += rowh;
    }
    if (n == 0) {
        text_at(dl, ImVec2(o.x + 6 * k, y + 4 * k), 24 * k, kGrey, tr(Tx::FETCHING_PLAYERS));
        y += 44 * k;
    }

    // The lock: until it is on nobody may pick a save; while it is on the room is hidden and closed.
    text_at(dl, ImVec2(o.x, y), 22 * k, room_locked() ? kGreen : kAmber,
            room_locked() ? tr(Tx::LOCKED_LINE)
                          : (host ? tr(Tx::UNLOCKED_HOST) : tr(Tx::UNLOCKED_CLIENT)));
    y += 34 * k;

    // The combat speed every player's battles run at (the host's, proto 82; user request 2026-10-11).
    {
        float sp = 0.0f;
        char val[48];
        // the game's speeds are 1, 2, 3 and 4 (the setting holds 1.0 .. 4.0): shown as the step, the way the game names it
        if (settings_room_speed(sp)) _snprintf_s(val, sizeof(val), _TRUNCATE, sp == (float)(int)sp ? "%.0f" : "%g", (double)sp);
        else                         strncpy_s(val, sizeof(val), tr(Tx::SPEED_UNKNOWN), _TRUNCATE);
        _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::SPEED_LINE), val);
        text_at(dl, ImVec2(o.x, y), 22 * k, kInkSoft, line);
        y += 34 * k;
    }

    // (The notice that the host's game port is closed to the outside, with the router-forwarding advice, is gone for good (2026-10-04): it misled -- the server relay and Steam carry a room without the port.)

    // Session line.
    y += 2 * k;
    dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + w, y), kInk, 2.0f);
    y += 12 * k;
    if (lockstep_halted()) {
        text_at(dl, ImVec2(o.x, y), 24 * k, kRed, tr(Tx::HALTED));
        y += 34 * k;
    }
    if (net_state() == NetState::Failed && net_error()[0]) {
        text_at(dl, ImVec2(o.x, y), 20 * k, kRed, net_error());
        y += 30 * k;
    }
    y_out = y;
}

// ---------------------------------------------------------------------------
// the in-game tag + panel
// ---------------------------------------------------------------------------
void draw_mp_tag(const View& v) {
    if (!in_room()) { g.panel_open = false; g.panel_t = 0; g.was_in_room = false; return; }
    if (!g.was_in_room) { g.was_in_room = true; g.joined_at = now(); }

    // Ease the fold toward its target.
    const float target = g.panel_open ? 1.0f : 0.0f;
    const float dt = ImGui::GetIO().DeltaTime;
    if (g.panel_t < target) { g.panel_t += dt / 0.22f; if (g.panel_t > target) g.panel_t = target; }
    if (g.panel_t > target) { g.panel_t -= dt / 0.18f; if (g.panel_t < target) g.panel_t = target; }
    const float e = ease_out(g.panel_t);

    const float k = v.k;
    // Right edge, a quarter of the way down: below the house screen's resource
    // banner (which ends at 19% of the height) and clear of the map's own tabs.
    const float tw = 118, th = 96;
    const ImVec2 ta(v.org.x + v.w - (tw - 10) * k, v.org.y + 0.205f * v.h);
    const ImVec2 tb(ta.x + tw * k, ta.y + th * k);

    if (g.panel_t > 0.001f) {

        // The panel unfolds to the left of the tag, hanging from the same top edge,
        // and is exactly as tall as what it says.
        // Wider while somebody has cat cards up: four cards need about 440 stage units.
        {
            const float want = any_cards() ? 830.0f : 580.0f;
            g.panel_w += (want - g.panel_w) * (dt * 10.0f > 1.0f ? 1.0f : dt * 10.0f);
            if (fabsf(want - g.panel_w) < 0.5f) g.panel_w = want;
        }
        const float PW = g.panel_w;
        const float px1 = ta.x + 18 * k;   // tucked under the tag, which is drawn last
        const float slide = (1.0f - e) * (PW * 0.35f) * k;
        const ImVec2 pa(px1 - PW * k + slide, ta.y - 8 * k);
        const ImVec2 pb(pa.x + PW * k, pa.y + g.panel_h * k);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
        if (overlay_window_begin("##mgmp_panel", pa, ImVec2(pb.x - pa.x, pb.y - pa.y), false)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            text_c(dl, (pa.x + pb.x) * 0.5f, pa.y + 34 * k, 42 * k, kInk, tr(Tx::MENU_MULTI));
            float y = 0;
            draw_room_info(dl, v, ImVec2(pa.x + 56 * k, pa.y + 104 * k), (PW - 112) * k, y, true);
            const bool busy = signal_request_pending();
            float by0 = y + 14 * k, by1 = by0 + 56 * k;
            if (room_is_host()) {
                const bool locked = room_locked();
                const bool can = locked ? room_can_unlock() : true;
                if (paper_button("lock", ImVec2(pa.x + 56 * k, by0), ImVec2(pb.x - 56 * k, by1),
                                 locked ? (can ? tr(Tx::UNLOCK) : tr(Tx::UNLOCK_WHEN)) : tr(Tx::LOCK),
                                 26 * k, true, locked ? Tone::Normal : Tone::Good))
                    room_request_lock(!locked);
                by0 = by1 + 12 * k; by1 = by0 + 56 * k;
            }
            if (paper_button("leave", ImVec2(pa.x + 56 * k, by0), ImVec2(pa.x + (PW / 2 - 8) * k, by1),
                             tr(Tx::LEAVE_ROOM), 26 * k, !busy, Tone::Danger)) {
                signal_request_leave();
                g.panel_open = false;
            }
            if (paper_button("fold", ImVec2(pa.x + (PW / 2 + 8) * k, by0), ImVec2(pb.x - 56 * k, by1),
                             tr(Tx::FOLD), 26 * k))
                g.panel_open = false;
            const float h = (by1 - pa.y) / k + 46.0f;      // bottom margin for the torn edge
            g.panel_h = h;
            dl->ChannelsSetCurrent(0);
            paper(dl, v, pa, ImVec2(pb.x, pa.y + h * k), e);
            dl->ChannelsMerge();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    bool clicked = false, hov = false;
    if (overlay_window_begin("##mgmp_tag", ta, ImVec2(tb.x - ta.x, tb.y - ta.y), false)) {
        ImGui::SetCursorScreenPos(ta);
        clicked = ImGui::InvisibleButton("##t", ImVec2(tb.x - ta.x, tb.y - ta.y));
        hov = ImGui::IsItemHovered();
        if (hov) g.hovering_button = true;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // The tag leans away from the edge when hovered, like the house tags.
        const float lift = (hov ? 10.0f : 0.0f) * k * (g.panel_open ? 0.0f : 1.0f) + e * 6.0f * k;
        const ImVec2 c((ta.x + tb.x) * 0.5f - lift, (ta.y + tb.y) * 0.5f);
        const float rot = (hov ? -0.06f : 0.0f);
        auto rotp = [&](float dx, float dy) {
            const float cs = cosf(rot), sn = sinf(rot);
            return ImVec2(c.x + dx * cs - dy * sn, c.y + dx * sn + dy * cs);
        };
        const float hw = tw * 0.5f * k * 1.1f, hh = th * 0.5f * k * 1.1f;
        // shadow
        dl->AddRectFilled(ImVec2(c.x - hw + 4 * k, c.y - hh + 6 * k), ImVec2(c.x + hw + 4 * k, c.y + hh + 6 * k),
                          IM_COL32(0, 0, 0, 70), 6.0f * k);
        if (tex_load(g.paper_tag, "ui\\paper_tag.png")) {
            dl->AddImageQuad(g.paper_tag.ref(), rotp(-hw, -hh), rotp(hw, -hh), rotp(hw, hh), rotp(-hw, hh),
                             ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
        } else {
            rough_rect(dl, ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), kPaper, kInk, 3.0f * k, 33, 2.0f * k);
        }
        // Icon: the mod's own multiplayer glyph if one has been dropped into
        // assets/ui, otherwise the game's cat-and-magnifier stand-in.
        Tex* icon = nullptr;
        if (tex_load(g.icon_multi, "ui\\icon_multiplayer.png")) icon = &g.icon_multi;
        else if (tex_load(g.icon_cats, "ui\\icon_cats.png"))    icon = &g.icon_cats;
        if (icon) {
            const float isz = 93.0f * k;   // 1.5x the first size
            const float ar = (float)icon->w / (float)icon->h;
            const float iw = ar >= 1 ? isz : isz * ar, ih = ar >= 1 ? isz / ar : isz;
            const ImVec2 ic(c.x + 6 * k, c.y - 2 * k);
            dl->AddImageQuad(icon->ref(),
                             ImVec2(ic.x - iw / 2, ic.y - ih / 2), ImVec2(ic.x + iw / 2, ic.y - ih / 2),
                             ImVec2(ic.x + iw / 2, ic.y + ih / 2), ImVec2(ic.x - iw / 2, ic.y + ih / 2),
                             ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
        }
        // A pulse dot while the session is not yet paired, so a player who has
        // not opened the panel still sees that something is waiting.
        if (net_state() != NetState::Ready) {
            const float pulse = 0.6f + 0.4f * sinf((float)now() * 5.0f);
            status_dot(dl, ImVec2(tb.x - 30 * k, ta.y + 14 * k), 8 * k, mul_alpha(kAmber, pulse));
        } else {
            status_dot(dl, ImVec2(tb.x - 30 * k, ta.y + 14 * k), 8 * k, kGreen);
        }
    }
    ImGui::End();
    if (clicked) g.panel_open = !g.panel_open;
}

// ---------------------------------------------------------------------------
// the save-backup window
// ---------------------------------------------------------------------------
void fmt_slots(const SaveBackup& b, char* out, size_t cap) {
    out[0] = 0;
    for (int s = 0; s < kGameSlots; ++s) {
        char part[96], t[24];
        if (!b.slot[s].present) {
            _snprintf_s(part, sizeof(part), _TRUNCATE, tr(Tx::SLOT_EMPTY), s + 1);
        } else {
            saveslots_format_time(b.slot[s].timer, t, sizeof(t));
            if (b.slot[s].percent >= 0)
                _snprintf_s(part, sizeof(part), _TRUNCATE, tr(Tx::SLOT_PCT), s + 1, b.slot[s].percent, t);
            else
                _snprintf_s(part, sizeof(part), _TRUNCATE, tr(Tx::SLOT_NOPCT), s + 1, t);
        }
        strncat_s(out, cap, part, _TRUNCATE);
        if (s + 1 < kGameSlots) strncat_s(out, cap, "   ", _TRUNCATE);
    }
}

// THE FILE DIALOG RUNS ON ITS OWN THREAD. The menu draws from inside the game's buffer swap; a modal
// dialog there would stop the game's frame (and, in a room, its network pump) for as long as the player
// browses. The worker owns the dialog; the backup window picks the answer up on a later frame.
struct FileDialog {
    volatile LONG state = 0;           // 0 idle, 1 open, 2 answered
    bool     save = false;
    int      index = -1;
    bool     ok = false;
    wchar_t  path[MAX_PATH * 2] = {};
    wchar_t  title[128] = {};
    wchar_t  filter[256] = {};
};
FileDialog g_fd;

DWORD WINAPI file_dialog_thread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = (HWND)ui_window();
    ofn.lpstrFilter = g_fd.filter;
    ofn.lpstrFile   = g_fd.path;
    ofn.nMaxFile    = (DWORD)(sizeof(g_fd.path) / sizeof(g_fd.path[0]));
    ofn.lpstrTitle  = g_fd.title;
    ofn.lpstrDefExt = L"mgmpsave";
    ofn.Flags       = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                      (g_fd.save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    g_fd.ok = g_fd.save ? GetSaveFileNameW(&ofn) != 0 : GetOpenFileNameW(&ofn) != 0;
    CoUninitialize();
    InterlockedExchange(&g_fd.state, 2);
    return 0;
}

void file_dialog_open(bool save, int index, const char* default_name) {
    if (InterlockedCompareExchange(&g_fd.state, 0, 0) != 0) return;
    g_fd.save = save; g_fd.index = index; g_fd.ok = false;
    g_fd.path[0] = 0;
    if (default_name && default_name[0]) {
        std::wstring n = widen(default_name);
        for (wchar_t& c : n) if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_';
        wcsncpy_s(g_fd.path, (n + L".mgmpsave").c_str(), _TRUNCATE);
    }
    wcsncpy_s(g_fd.title, widen(tr(save ? Tx::EXPORT_TITLE : Tx::IMPORT_TITLE)).c_str(), _TRUNCATE);
    // "<label>\0*.mgmpsave\0\0"
    const std::wstring label = widen(tr(Tx::FILE_FILTER));
    memset(g_fd.filter, 0, sizeof(g_fd.filter));
    const size_t n = label.size() < 200 ? label.size() : 200;
    wmemcpy(g_fd.filter, label.c_str(), n);
    wmemcpy(g_fd.filter + n + 1, L"*.mgmpsave", 10);
    InterlockedExchange(&g_fd.state, 1);
    HANDLE h = CreateThread(nullptr, 0, file_dialog_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else InterlockedExchange(&g_fd.state, 0);
}

// The answer, once: export or import the position the dialog was opened for.
bool file_dialog_take() {
    if (InterlockedCompareExchange(&g_fd.state, 0, 0) != 2) return false;
    if (g_fd.ok && g_fd.path[0]) {
        if (g_fd.save && g_fd.index >= 100) saveslots_auto_export(g_fd.index - 100, g_fd.path);     // an auto save
        else if (g_fd.save)                 saveslots_export(g_fd.index, g_fd.path);
        else           saveslots_import(g_fd.index, g_fd.path);
    }
    InterlockedExchange(&g_fd.state, 0);
    return true;
}

// ---------------------------------------------------------------------------
// WHICH ACCOUNT'S SAVES (2026-10-15). On a PC where several accounts have Mewgenics saves and the Steam id names none of the folders, the
// title screen asks which one is the player's (mgmp_savefile remembers the answer); the backup page shows the folder in use and can switch it,
// behind a warning. Never in a room.
// ---------------------------------------------------------------------------
void acct_display(const SaveAccount& a, char* out, size_t cap) {
    if (a.persona[0]) { strncpy_s(out, cap, a.persona, _TRUNCATE); return; }
    char id[80] = {};
    WideCharToMultiByte(CP_UTF8, 0, a.name, -1, id, sizeof(id), nullptr, nullptr);
    _snprintf_s(out, cap, _TRUNCATE, "%s (%s)", tr(Tx::ACCT_UNKNOWN), id);
}
const char* acct_how_text() {
    const char* how = savefile_dir_how();
    if (!strcmp(how, "chosen")) return tr(Tx::ACCT_HOW_CHOSEN);
    if (!strcmp(how, "steam"))  return tr(Tx::ACCT_HOW_STEAM);
    if (!strcmp(how, "only"))   return tr(Tx::ACCT_HOW_ONLY);
    if (!strcmp(how, "guess"))  return tr(Tx::ACCT_HOW_GUESS);
    return "?";
}
void acct_refresh() {
    g.acct_n = savefile_accounts(g.acct, 8);
    g.acct_at = now();
    g.acct_label[0] = 0;
    for (int i = 0; i < g.acct_n; ++i) {
        if (!g.acct[i].current) continue;
        char who[120];
        acct_display(g.acct[i], who, sizeof(who));
        _snprintf_s(g.acct_label, sizeof(g.acct_label), _TRUNCATE, tr(Tx::ACCT_NOW), who, acct_how_text());
    }
}

// The switch's warning (the backup page's button): what it is for, then the list.
void draw_acct_warn(const View& v) {
    if (in_room()) { g.acct_warn = false; return; }
    const float k = v.k;
    const float W = 860, H = 420;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    if (!modal_begin("##mgmp_acct_warn")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + W * k, a.y + H * k));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(36), 40 * k, kInk, tr(Tx::ACCT_SWITCH_BTN));
    wrap_text(dl, X(60), Y(110), (W - 120) * k, 26 * k, 38 * k, kRed, tr(Tx::ACCT_WARN));
    if (paper_button("acct_go", ImVec2(X(60), Y(H - 100)), ImVec2(X(W / 2 - 10), Y(H - 36)), tr(Tx::ACCT_CONTINUE), 30 * k, true, Tone::Danger)) {
        g.acct_warn = false; g.acct_list = true;
        acct_refresh();
        log_line("MENU", "save folder switch: the warning was confirmed -- the list of folders is shown");
    }
    if (paper_button("acct_no", ImVec2(X(W / 2 + 10), Y(H - 100)), ImVec2(X(W - 60), Y(H - 36)), tr(Tx::CANCEL), 30 * k)) g.acct_warn = false;
    ImGui::End();
}

// The list of account folders. `forced`: the title screen's question (the folder is unsure); otherwise the backup page's switch.
void draw_acct_list(const View& v, bool forced) {
    if (in_room()) { g.acct_list = false; return; }
    if (now() - g.acct_at > 2.0) acct_refresh();
    const float k = v.k;
    const int shown = g.acct_n < 6 ? g.acct_n : 6;
    const float rowh = 108;
    const float W = 1040, H = 100 + (forced ? 190 : 20) + rowh * (shown ? shown : 1) + 120;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    if (!modal_begin(forced ? "##mgmp_acct_ask" : "##mgmp_acct_list")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + W * k, a.y + H * k));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(36), 40 * k, kInk, tr(Tx::ACCT_TITLE));
    float y = 100;
    if (forced) { wrap_text(dl, X(60), Y(y), (W - 120) * k, 24 * k, 34 * k, kInk, tr(Tx::ACCT_ASK)); y += 190; }
    else y += 20;
    for (int i = 0; i < shown; ++i) {
        const SaveAccount& s = g.acct[i];
        rough_rect(dl, ImVec2(X(50), Y(y)), ImVec2(X(W - 50), Y(y + rowh - 12)), s.current ? IM_COL32(120, 190, 130, 60) : IM_COL32(255, 255, 250, 50), kInk, 1.8f, 300 + i, 1.0f);
        char who[160];
        acct_display(s, who, sizeof(who));
        if (s.current) { const size_t n = strlen(who); _snprintf_s(who + n, sizeof(who) - n, _TRUNCATE, "  [%s]", tr(Tx::ACCT_CURRENT)); }
        text_at(dl, ImVec2(X(72), Y(y + 10)), 30 * k, kInk, who);
        char when[40] = {};
        if (s.newest) {
            FILETIME ft{}, lt{}; SYSTEMTIME st{};
            ft.dwLowDateTime = (DWORD)(s.newest & 0xFFFFFFFFull); ft.dwHighDateTime = (DWORD)(s.newest >> 32);
            FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
            _snprintf_s(when, sizeof(when), _TRUNCATE, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        } else strncpy_s(when, sizeof(when), tr(Tx::ACCT_NO_SAVE), _TRUNCATE);
        char id[80] = {};
        WideCharToMultiByte(CP_UTF8, 0, s.name, -1, id, sizeof(id), nullptr, nullptr);
        char meta[260];
        _snprintf_s(meta, sizeof(meta), _TRUNCATE, tr(Tx::ACCT_META), id, when, s.saves);
        text_at(dl, ImVec2(X(72), Y(y + 52)), 22 * k, kGrey, meta);
        char bid[24]; _snprintf_s(bid, sizeof(bid), _TRUNCATE, "acct_use_%d", i);
        if (paper_button(bid, ImVec2(X(W - 300), Y(y + 18)), ImVec2(X(W - 72), Y(y + rowh - 30)), tr(Tx::ACCT_USE), 26 * k, true, Tone::Good)) {
            if (savefile_choose_account(s.name)) {
                char disp[160]; acct_display(s, disp, sizeof(disp));
                say(tr(Tx::ACCT_DONE), disp);
                g.acct_list = false;
                acct_refresh();
                if (g.win == Win::Backup) { saveslots_refresh(); saveslots_current(g.current); g.refreshed_at = now(); }
            } else say("%s", tr(Tx::ACCT_FAIL));
        }
        y += rowh;
    }
    if (paper_button(forced ? "acct_later" : "acct_cancel", ImVec2(X(W / 2 - 160), Y(H - 96)), ImVec2(X(W / 2 + 160), Y(H - 36)),
                     forced ? tr(Tx::ACCT_LATER) : tr(Tx::CANCEL), 28 * k)) {
        if (forced) savefile_dir_dismiss_choice();
        g.acct_list = false;
    }
    ImGui::End();
}

void refresh_backup_view() {
    saveslots_refresh();
    saveslots_current(g.current);
    acct_refresh();
    g.refreshed_at = now();
    g.confirm = Confirm::None;
}

bool confirm_armed(Confirm kind, int idx) {
    return g.confirm == kind && g.confirm_idx == idx && now() < g.confirm_until;
}
void arm(Confirm kind, int idx) { g.confirm = kind; g.confirm_idx = idx; g.confirm_until = now() + 3.0; }
void ask(Confirm kind, int idx) {   // open the confirm dialog (draw_backup_confirm)
    g.dlg = kind; g.dlg_idx = idx;
    if (kind == Confirm::Overwrite && idx >= 0 && idx < kSaveBackupCount) {
        strncpy_s(g.dlg_name, sizeof(g.dlg_name), saveslots_get(idx).name, _TRUNCATE);
        g.dlg_focus = true;
    }
}

// ---------------------------------------------------------------------------
// RESTORE FROM A HANDSHAKE SAVE (2026-10-05): a page of its own inside the backup window. The three game slots side by side, each with every handshake save made for it (newest first); "Restore" asks, then
// puts that save over the slot (saveslots_sync_restore takes the undo point first).
// ---------------------------------------------------------------------------
void fmt_when(int64_t at, char* out, size_t cap) {
    out[0] = 0;
    if (!at) return;
    time_t t = (time_t)at; tm lt{};
    localtime_s(&lt, &t);
    strftime(out, cap, "%Y-%m-%d %H:%M", &lt);
}

void fmt_one_slot(const SaveSlotInfo& si, int s, char* out, size_t cap) {
    char t[24];
    if (!si.present) { _snprintf_s(out, cap, _TRUNCATE, tr(Tx::SLOT_EMPTY), s + 1); return; }
    saveslots_format_time(si.timer, t, sizeof(t));
    if (si.percent >= 0) _snprintf_s(out, cap, _TRUNCATE, tr(Tx::SLOT_PCT), s + 1, si.percent, t);
    else                 _snprintf_s(out, cap, _TRUNCATE, tr(Tx::SLOT_NOPCT), s + 1, t);
}

void draw_sync_page(ImDrawList* dl, const View& v, ImVec2 a, float W, float H, bool locked) {
    const float k = v.k;
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    text_c(dl, X(W / 2), Y(38), 50 * k, kInk, tr(Tx::SYNC_TITLE));
    wrap_text(dl, X(60), Y(112), (W - 120) * k, 22 * k, 30 * k, kGrey, tr(Tx::SYNC_HINT));
    if (locked) text_c(dl, X(W / 2), Y(186), 24 * k, kRed, tr(Tx::ROOM_NO_WRITE));

    const float colw = 406, gap = 20, top = 222, bot = H - 156;
    for (int s = 0; s < kGameSlots; ++s) {
        const float cx = 50 + s * (colw + gap);
        char head[64]; _snprintf_s(head, sizeof(head), _TRUNCATE, tr(Tx::SYNC_SLOT_H), s + 1);
        text_at(dl, ImVec2(X(cx + 8), Y(top - 38)), 30 * k, kInk, head);
        char cur[96]; fmt_one_slot(g.current[s], s, cur, sizeof(cur));
        text_r(dl, X(cx + colw - 6), Y(top - 30), 18 * k, kInkSoft, cur);
        rough_rect(dl, ImVec2(X(cx), Y(top)), ImVec2(X(cx + colw), Y(bot)), IM_COL32(255, 255, 250, 60), kInk, 2.2f, 17 + s, 1.2f);
        ImGui::SetCursorScreenPos(ImVec2(X(cx + 4), Y(top) + 4 * k));
        char cid[24]; _snprintf_s(cid, sizeof(cid), _TRUNCATE, "##sync%d", s);
        ImGui::BeginChild(cid, ImVec2((colw - 8) * k, (bot - top) * k - 8 * k), false, ImGuiWindowFlags_NoBackground);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float rowh = 112 * k, rw = (colw - 8) * k;
        const int n = saveslots_sync_count(s);
        if (!n) {
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            text_at(cdl, ImVec2(rp.x + 16 * k, rp.y + 20 * k), 26 * k, kGrey, tr(Tx::SYNC_NONE));
            ImGui::Dummy(ImVec2(rw, rowh));
        }
        for (int i = 0; i < n; ++i) {
            const SyncSave& e = saveslots_sync_get(s, i);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            cdl->AddLine(ImVec2(rp.x + 8 * k, rp.y + rowh - 2), ImVec2(rp.x + rw - 8 * k, rp.y + rowh - 2), IM_COL32(30, 30, 30, 60), 1.5f);
            char when[40]; fmt_when(e.saved_at, when, sizeof(when));
            text_at(cdl, ImVec2(rp.x + 14 * k, rp.y + 8 * k), 26 * k, kInk, when);
            char d1[96];
            if (e.stage) _snprintf_s(d1, sizeof(d1), _TRUNCATE, "%s", tr(e.stage == kStagePrep ? Tx::SYNC_STAGE_PREP : Tx::SYNC_STAGE_READY));
            else         _snprintf_s(d1, sizeof(d1), _TRUNCATE, tr(Tx::SYNC_DETAIL), (unsigned)e.seq, (unsigned)e.players);
            text_at(cdl, ImVec2(rp.x + 14 * k, rp.y + 44 * k), 20 * k, kInkSoft, d1);
            char d2[128] = {};
            if (e.after_boss) strncat_s(d2, sizeof(d2), tr(Tx::SYNC_BOSS), _TRUNCATE);
            if (e.day >= 0) {
                char part[40]; _snprintf_s(part, sizeof(part), _TRUNCATE, tr(Tx::SYNC_DAY), e.day);
                if (d2[0]) strncat_s(d2, sizeof(d2), " | ", _TRUNCATE);
                strncat_s(d2, sizeof(d2), part, _TRUNCATE);
            }
            if (e.percent >= 0) {
                char part[16]; _snprintf_s(part, sizeof(part), _TRUNCATE, "%d%%", e.percent);
                if (d2[0]) strncat_s(d2, sizeof(d2), " | ", _TRUNCATE);
                strncat_s(d2, sizeof(d2), part, _TRUNCATE);
            }
            if (d2[0]) text_at(cdl, ImVec2(rp.x + 14 * k, rp.y + 72 * k), 20 * k, kInkSoft, d2);
            char id[32]; _snprintf_s(id, sizeof(id), _TRUNCATE, "syr%d_%d", s, i);
            if (paper_button(id, ImVec2(rp.x + rw - 118 * k, rp.y + 24 * k), ImVec2(rp.x + rw - 14 * k, rp.y + 80 * k), tr(Tx::SYNC_DO), 24 * k, !locked, Tone::Good)) {
                g.sync_slot = s; g.sync_idx = i;
            }
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(rw, rowh));
        }
        ImGui::EndChild();
    }

    const char* m = saveslots_message();
    if (m[0]) {
        char line[300];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s", m[0] == '!' ? m + 1 : m, saveslots_message_is_load() ? tr(Tx::LOADED_HINT) : "");
        text_c(dl, X(W / 2), Y(H - 136), 26 * k, m[0] == '!' ? kRed : kGreen, line);
    }
    if (paper_button("sync_back", ImVec2(X(W / 2 - 150), Y(H - 96)), ImVec2(X(W / 2 + 150), Y(H - 36)), tr(Tx::BACK), 30 * k)) {
        g.sync_page = false; g.sync_slot = g.sync_idx = -1;
    }
}

// The question before a handshake save goes over a slot: which save, which slot, and what happens first (the undo point).
void draw_sync_confirm(const View& v) {
    if (g.sync_slot < 0) return;
    const bool in_range = g.sync_idx >= 0 && g.sync_idx < saveslots_sync_count(g.sync_slot);
    if (!in_range || g.win != Win::Backup || !g.sync_page || in_room()) { g.sync_slot = g.sync_idx = -1; return; }
    const SyncSave& e = saveslots_sync_get(g.sync_slot, g.sync_idx);
    const float k = v.k;
    const float W = 900, H = 500;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!modal_begin("##mgmp_sync_ask")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    text_c(dl, X(W / 2), Y(40), 50 * k, kRed, tr(Tx::SYNC_TITLE));
    char when[40]; fmt_when(e.saved_at, when, sizeof(when));
    char line[480];
    _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::DLG_SYNC_B), when, g.sync_slot + 1);
    const float ey = wrap_text(dl, X(60), Y(140), (W - 120) * k, 30 * k, 42 * k, kInk, line);
    char d1[96];
    if (e.stage) _snprintf_s(d1, sizeof(d1), _TRUNCATE, "%s", tr(e.stage == kStagePrep ? Tx::SYNC_STAGE_PREP : Tx::SYNC_STAGE_READY));
    else         _snprintf_s(d1, sizeof(d1), _TRUNCATE, tr(Tx::SYNC_DETAIL), (unsigned)e.seq, (unsigned)e.players);
    wrap_text(dl, X(60), ey + 14 * k, (W - 120) * k, 22 * k, 30 * k, kInkSoft, d1);

    const float by0 = H - 120, by1 = H - 44;
    bool done = false;
    if (paper_button("sync_yes", ImVec2(X(60), Y(by0)), ImVec2(X(W / 2 - 14), Y(by1)), tr(Tx::SYNC_DO), 34 * k, true, Tone::Danger)) {
        log_line("MENU", "handshake save restore: slot %d, entry %d (node %u) confirmed", g.sync_slot + 1, g.sync_idx + 1, (unsigned)e.seq);
        saveslots_sync_restore(g.sync_slot, g.sync_idx);
        done = true;
    }
    if (paper_button("sync_no", ImVec2(X(W / 2 + 14), Y(by0)), ImVec2(X(W - 60), Y(by1)), tr(Tx::CANCEL), 34 * k)) done = true;
    ImGui::End();
    if (done) {
        g.sync_slot = g.sync_idx = -1;
        saveslots_sync_refresh();
        saveslots_current(g.current);
    }
}

void draw_backup_window(const View& v) {
    const float W = 1360, H = 980;
    const ImVec2 a = P(v, 430.0f, (kStageH - H) * 0.5f + 24);   // right of the game's own menu column
    const ImVec2 sz(W * v.k, H * v.k);
    if (!overlay_window_begin("##mgmp_backup", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    const float k = v.k;
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    if (now() - g.refreshed_at > 2.0) refresh_backup_view();   // the game may have written a slot meanwhile
    if (file_dialog_take()) refresh_backup_view();
    const bool dialog_open = InterlockedCompareExchange(&g_fd.state, 0, 0) != 0;

    if (g.sync_page) { draw_sync_page(dl, v, a, W, H, in_room()); ImGui::End(); return; }

    text_c(dl, X(W / 2), Y(38), 50 * k, kInk, tr(Tx::MENU_BACKUP));

    const bool locked = in_room();

    // The three slots as they are right now.
    text_at(dl, ImVec2(X(60), Y(112)), 24 * k, kGrey, tr(Tx::CURRENT_SAVES));
    {
        char cur[256]; SaveBackup tmp; for (int s = 0; s < kGameSlots; ++s) tmp.slot[s] = g.current[s];
        fmt_slots(tmp, cur, sizeof(cur));
        text_at(dl, ImVec2(X(190), Y(108)), 26 * k, kInk, cur);
    }

    // Name box + global actions.
    text_at(dl, ImVec2(X(60), Y(170)), 26 * k, kInk, tr(Tx::BACKUP_NAME));
    paper_input("##sname", ImVec2(X(190), Y(158)), ImVec2(X(500), Y(214)), g.save_name, sizeof(g.save_name),
                tr(Tx::BACKUP_NAME_HINT), 26 * k);
    // The handshake saves of the three slots, on a page of their own.
    if (paper_button("sync_open", ImVec2(X(520), Y(158)), ImVec2(X(810), Y(214)), tr(Tx::SYNC_BTN), 24 * k)) {
        g.sync_page = true; g.sync_slot = g.sync_idx = -1;
        saveslots_sync_refresh();
        saveslots_current(g.current);
    }
    if (paper_button("undo", ImVec2(X(830), Y(158)), ImVec2(X(1090), Y(214)),
                     confirm_armed(Confirm::Undo, 0) ? tr(Tx::CLICK_AGAIN) : tr(Tx::UNDO_LOAD), 24 * k,
                     saveslots_undo_available() && !locked, Tone::Normal, confirm_armed(Confirm::Undo, 0))) {
        if (confirm_armed(Confirm::Undo, 0)) {
            saveslots_undo();
            refresh_backup_view();
        } else arm(Confirm::Undo, 0);
    }
    if (paper_button("close", ImVec2(X(1110), Y(158)), ImVec2(X(W - 60), Y(214)), tr(Tx::BACK), 26 * k))
        g.win = Win::None;

    if (locked) {
        text_c(dl, X(W / 2), Y(232), 24 * k, kRed, tr(Tx::ROOM_NO_WRITE));
    } else {
        // the save folder in use and how it was decided; the switch is for a PC where several accounts have saves (2026-10-15)
        if (!g.acct_label[0] && now() - g.acct_at > 2.0) acct_refresh();
        text_at(dl, ImVec2(X(60), Y(230)), 22 * k, kGrey, g.acct_label);
        if (paper_button("acct_switch", ImVec2(X(W - 330), Y(222)), ImVec2(X(W - 60), Y(262)), tr(Tx::ACCT_SWITCH_BTN), 22 * k, !dialog_open)) g.acct_warn = true;
    }

    // List.
    const float ly0 = Y(268), ly1 = Y(H - 110);
    rough_rect(dl, ImVec2(X(50), ly0), ImVec2(X(W - 50), ly1), IM_COL32(255, 255, 250, 60), kInk, 2.2f, 17, 1.2f);
    ImGui::SetCursorScreenPos(ImVec2(X(56), ly0 + 4 * k));
    ImGui::BeginChild("##list", ImVec2((W - 112) * k, ly1 - ly0 - 8 * k), false, ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const float rowh = 80 * k;
    const float rx = X(56), rw = (W - 112) * k;
    // THE AUTO SAVE QUEUE, ABOVE THE TWENTY: the newest first, taken each time the player is back in the warehouse after a settled run; the oldest is replaced by the next one. Load and export only -- an
    // auto save can be neither overwritten nor deleted by the player.
    {
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        text_at(cdl, ImVec2(hp.x + 18 * k, hp.y + 10 * k), 24 * k, kInk, tr(Tx::AUTO_HEADER));
        ImGui::SetCursorScreenPos(hp);
        ImGui::Dummy(ImVec2(rw, 48 * k));
        for (int i = 0; i < kAutoSaveCount; ++i) {
            const SaveBackup& b = saveslots_auto_get(i);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            cdl->AddLine(ImVec2(rp.x + 10 * k, rp.y + rowh - 2), ImVec2(rp.x + rw - 10 * k, rp.y + rowh - 2), IM_COL32(30, 30, 30, 60), 1.5f);
            char idx[8]; _snprintf_s(idx, sizeof(idx), _TRUNCATE, "A%d", i + 1);
            text_at(cdl, ImVec2(rp.x + 18 * k, rp.y + 20 * k), 36 * k, b.used ? kInk : kGrey, idx);
            if (b.used) {
                text_at(cdl, ImVec2(rp.x + 92 * k, rp.y + 5 * k), 30 * k, kInk, b.name);
                char sl[256]; fmt_slots(b, sl, sizeof(sl));
                text_at(cdl, ImVec2(rp.x + 92 * k, rp.y + 46 * k), 20 * k, kInkSoft, sl);
            } else {
                text_at(cdl, ImVec2(rp.x + 92 * k, rp.y + 22 * k), 28 * k, kGrey, tr(Tx::EMPTY_POS));
            }
            if (b.used) {
                const float by0 = rp.y + 12 * k, by1 = rp.y + rowh - 14 * k;
                char id[24];
                _snprintf_s(id, sizeof(id), _TRUNCATE, "al%d", i);
                if (paper_button(id, ImVec2(rp.x + 904 * k, by0), ImVec2(rp.x + 1008 * k, by1), tr(Tx::LOAD), 24 * k, !locked, Tone::Good))
                    ask(Confirm::Load, 100 + i);
                _snprintf_s(id, sizeof(id), _TRUNCATE, "ax%d", i);
                if (paper_button(id, ImVec2(rp.x + 1014 * k, by0), ImVec2(rp.x + 1118 * k, by1), tr(Tx::EXPORT), 24 * k, !dialog_open))
                    file_dialog_open(true, 100 + i, b.name);       // an index of 100 and up in the file dialog is an auto save
            }
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(rw, rowh));
        }
        const ImVec2 dp = ImGui::GetCursorScreenPos();
        cdl->AddLine(ImVec2(dp.x + 10 * k, dp.y + 6 * k), ImVec2(dp.x + rw - 10 * k, dp.y + 6 * k), IM_COL32(30, 30, 30, 140), 2.5f);
        ImGui::SetCursorScreenPos(dp);
        ImGui::Dummy(ImVec2(rw, 20 * k));
    }
    for (int i = 0; i < kSaveBackupCount; ++i) {
        const SaveBackup& b = saveslots_get(i);
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        cdl->AddLine(ImVec2(rp.x + 10 * k, rp.y + rowh - 2), ImVec2(rp.x + rw - 10 * k, rp.y + rowh - 2), IM_COL32(30, 30, 30, 60), 1.5f);

        char idx[8]; _snprintf_s(idx, sizeof(idx), _TRUNCATE, "%02d", i + 1);
        text_at(cdl, ImVec2(rp.x + 18 * k, rp.y + 20 * k), 36 * k, b.used ? kInk : kGrey, idx);

        if (b.used) {
            text_at(cdl, ImVec2(rp.x + 92 * k, rp.y + 5 * k), 30 * k, kInk, b.name);
            char when[40] = {};
            if (b.saved_at) {
                time_t t = (time_t)b.saved_at; tm lt{};
                localtime_s(&lt, &t);
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &lt);
            }
            text_r(cdl, rp.x + 784 * k, rp.y + 9 * k, 22 * k, kGrey, when);   // the text column ends where the buttons begin (x 794)
            char sl[256]; fmt_slots(b, sl, sizeof(sl));
            text_at(cdl, ImVec2(rp.x + 92 * k, rp.y + 46 * k), 20 * k, kInkSoft, sl);
        } else {
            text_at(cdl, ImVec2(rp.x + 92 * k, rp.y + 22 * k), 28 * k, kGrey, tr(Tx::EMPTY_POS));
        }

        const float by0 = rp.y + 12 * k, by1 = rp.y + rowh - 14 * k;
        char id[24];
        // save / overwrite
        _snprintf_s(id, sizeof(id), _TRUNCATE, "s%d", i);
        if (paper_button(id, ImVec2(rp.x + 794 * k, by0), ImVec2(rp.x + 898 * k, by1),
                         b.used ? tr(Tx::OVERWRITE) : tr(Tx::SAVE), 24 * k, !locked, b.used ? Tone::Normal : Tone::Good)) {
            if (b.used) ask(Confirm::Overwrite, i);       // a used position asks first (the dialog)
            else {
                saveslots_save(i, g.save_name);
                refresh_backup_view();
            }
        }
        if (b.used) {
            _snprintf_s(id, sizeof(id), _TRUNCATE, "l%d", i);
            if (paper_button(id, ImVec2(rp.x + 904 * k, by0), ImVec2(rp.x + 1008 * k, by1), tr(Tx::LOAD), 24 * k, !locked, Tone::Good))
                ask(Confirm::Load, i);
            // One file out: the whole position (see saveslots_export).
            _snprintf_s(id, sizeof(id), _TRUNCATE, "x%d", i);
            if (paper_button(id, ImVec2(rp.x + 1014 * k, by0), ImVec2(rp.x + 1118 * k, by1), tr(Tx::EXPORT), 24 * k,
                             !dialog_open))
                file_dialog_open(true, i, b.name);
            _snprintf_s(id, sizeof(id), _TRUNCATE, "d%d", i);
            if (paper_button(id, ImVec2(rp.x + 1124 * k, by0), ImVec2(rp.x + 1228 * k, by1), tr(Tx::DELETE_BTN), 24 * k, true, Tone::Danger))
                ask(Confirm::Delete, i);
        } else {
            // One file in: an exported position becomes this one (then "Load" it as usual).
            _snprintf_s(id, sizeof(id), _TRUNCATE, "i%d", i);
            if (paper_button(id, ImVec2(rp.x + 904 * k, by0), ImVec2(rp.x + 1008 * k, by1), tr(Tx::IMPORT), 24 * k,
                             !dialog_open, Tone::Normal))
                file_dialog_open(false, i, nullptr);
        }
        ImGui::SetCursorScreenPos(rp);
        ImGui::Dummy(ImVec2(rw, rowh));   // see the room list: no bare SetCursorScreenPos at a child's end
    }

    ImGui::EndChild();

    // Feedback line.
    const char* m = saveslots_message();
    if (m[0]) {
        char line[300];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s", m[0] == '!' ? m + 1 : m,
                    saveslots_message_is_load() ? tr(Tx::LOADED_HINT) : "");
        text_c(dl, X(W / 2), Y(H - 84), 26 * k, m[0] == '!' ? kRed : kGreen, line);
    }
    text_c(dl, X(W / 2), Y(H - 46), 22 * k, kGrey, tr(Tx::BACKUP_FOOT));
    ImGui::End();
}

// THE QUESTION BEFORE A BACKUP IS LOADED, OVERWRITTEN OR DELETED (2026-10-04): a dimmed whole-screen dialog with the action in its title, what it does to which backup, and a button for it and one to
// cancel -- in place of the old button that only turned into "confirm" for three seconds and was easy to hit twice without reading. The window under it cannot be clicked meanwhile.
void draw_backup_confirm(const View& v) {
    if (g.dlg == Confirm::None) return;
    const bool is_auto = g.dlg_idx >= 100;
    const int pos = is_auto ? g.dlg_idx - 100 : g.dlg_idx;
    const bool in_range = pos >= 0 && pos < (is_auto ? kAutoSaveCount : kSaveBackupCount);
    const SaveBackup* b = !in_range ? nullptr : (is_auto ? &saveslots_auto_get(pos) : &saveslots_get(pos));
    // gone: the position emptied meanwhile, the window closed, or a room opened (loading and overwriting are refused in a room)
    if (!b || !b->used || g.win != Win::Backup || (in_room() && g.dlg != Confirm::Delete)) { g.dlg = Confirm::None; return; }

    const float k = v.k;
    const bool ow = g.dlg == Confirm::Overwrite;
    const float W = 900, H = ow ? 600 : 500;   // overwrite: room for the name box
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!modal_begin("##mgmp_backup_ask")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    const bool load = g.dlg == Confirm::Load, del = g.dlg == Confirm::Delete;
    const char* title = load ? tr(Tx::DLG_LOAD_T) : del ? tr(Tx::DLG_DEL_T) : tr(Tx::DLG_OW_T);
    const char* body  = load ? tr(Tx::DLG_LOAD_B) : del ? tr(Tx::DLG_DEL_B) : tr(Tx::DLG_OW_B);
    const char* act   = load ? tr(Tx::LOAD)       : del ? tr(Tx::DELETE_BTN) : tr(Tx::OVERWRITE);
    text_c(dl, X(W / 2), Y(40), 50 * k, load ? kInk : kRed, title);

    char line[400];
    _snprintf_s(line, sizeof(line), _TRUNCATE, body, b->name);
    const float ey = wrap_text(dl, X(60), Y(140), (W - 120) * k, 30 * k, 42 * k, kInk, line);
    // what the backup holds, so the right one is certain
    char sl[256]; fmt_slots(*b, sl, sizeof(sl));
    const float sy = wrap_text(dl, X(60), ey + 14 * k, (W - 120) * k, 22 * k, 30 * k, kInkSoft, sl);

    // Overwrite: the name the backup will carry, starting from the one it has (Enter confirms, like the button).
    bool enter = false;
    if (ow) {
        text_at(dl, ImVec2(X(60), sy + 30 * k), 26 * k, kInk, tr(Tx::BACKUP_NAME));
        if (g.dlg_focus) { ImGui::SetKeyboardFocusHere(); g.dlg_focus = false; }
        enter = paper_input("##ow_name", ImVec2(X(190), sy + 18 * k), ImVec2(X(W - 60), sy + 74 * k), g.dlg_name, sizeof(g.dlg_name),
                            tr(Tx::BACKUP_NAME_HINT), 26 * k, ImGuiInputTextFlags_EnterReturnsTrue);
    }

    const float by0 = H - 120, by1 = H - 44;
    bool done = false;
    if (paper_button("bk_yes", ImVec2(X(60), Y(by0)), ImVec2(X(W / 2 - 14), Y(by1)), act, 34 * k, true, load ? Tone::Good : Tone::Danger) || enter) {
        if (g.dlg == Confirm::Load)           { if (is_auto) saveslots_auto_load(pos); else saveslots_load(pos); }
        else if (g.dlg == Confirm::Overwrite) saveslots_save(pos, g.dlg_name);
        else if (g.dlg == Confirm::Delete)    saveslots_delete(pos);
        log_line("MENU", "backup %s%d: %s confirmed", is_auto ? "A" : "", pos + 1, load ? "load" : del ? "delete" : "overwrite");
        done = true;
    }
    if (paper_button("bk_no", ImVec2(X(W / 2 + 14), Y(by0)), ImVec2(X(W - 60), Y(by1)), tr(Tx::CANCEL), 34 * k)) done = true;
    ImGui::End();
    if (done) { g.dlg = Confirm::None; refresh_backup_view(); }
}

// ---------------------------------------------------------------------------
// cursor + toast
// ---------------------------------------------------------------------------
void icon_pen(ImDrawList* dl, ImVec2 c, float s, ImU32 tip);
void icon_eraser(ImDrawList* dl, ImVec2 c, float s);

void draw_cursor(const View& v) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDrawCursor) return;                 // the debug panel is drawing its own arrow
    if (io.MousePos.x < -1e5f) return;
    // 0.1.91: a drawing tool over the board IS the pointer -- the pen with its nib on the hotspot, the eraser on its circle (the game's own cursor
    // is hidden meanwhile: menu_board_cursor_tick). Drawn whatever WantCaptureMouse says: the board's canvas is ours.
    if (g.board_cursor) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float s = 40.0f * (v.h / 720.0f);
        const ImVec2 m = io.MousePos;
        if (g.board_cursor == 1) icon_pen(fg, ImVec2(m.x + 0.40f * s, m.y - 0.40f * s), s, g.board_cursor_col);   // the nib (-0.40, 0.40) on the mouse
        else                     icon_eraser(fg, m, (g.board_cursor_r > 0 ? g.board_cursor_r * 2.0f : s) * 0.95f);
        return;
    }
    if (!io.WantCaptureMouse) return;               // the game's cursor is showing and is right

    // The overlay is drawn AFTER the game's cursor, so over a sheet of paper the
    // game's arrow would be underneath it. Draw the same arrow on top, sized the
    // way the peer cursors are: 34 px of ink at a 720-high content, hotspot (34,7).
    Tex& t = g.hovering_button ? g.cursor_over : g.cursor_def;
    if (!tex_load(t, g.hovering_button ? "ui\\cursor_over.png" : "ui\\cursor_default.png")) return;
    const float ink = 106.0f;
    const float s = 34.0f * (v.h / 720.0f) / ink;
    const float hx = 34.0f, hy = 7.0f;
    const ImVec2 p0(io.MousePos.x - hx * s, io.MousePos.y - hy * s);
    const ImVec2 p1(p0.x + 128.0f * s, p0.y + 128.0f * s);
    ImGui::GetForegroundDrawList()->AddImage(t.ref(), p0, p1);
}

// ---------------------------------------------------------------------------
// THE ROOM CHAT (mgmp_chat). Enter opens the input with the whole history on a dimmed screen (the game gets neither mouse nor keyboard meanwhile); Enter again sends what was typed and closes it (an empty
// box only closes). Whenever a line is sent or arrives, the history is shown at the lower left of every screen for ten seconds and then fades.
// ---------------------------------------------------------------------------

// The same breaking rule as wrap_text (a space when the line has one, between characters otherwise, always on a UTF-8 boundary), into lines instead of onto the screen.
void wrap_split(const char* text, float fs, float maxw, std::vector<std::string>& out) {
    const char* s = text;
    while (*s) {
        const char* e = s;
        const char* fit = s;
        const char* space = nullptr;
        while (*e) {
            const unsigned char c = (unsigned char)*e;
            const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            const char* nx = e + len;
            std::string part(s, nx);
            if (text_size(fs, part.c_str()).x > maxw) break;
            fit = nx; e = nx;
            if (c == ' ') space = nx;
        }
        const char* cut = fit;
        if (*e && *e != ' ' && space && space > s) cut = space;
        if (cut == s) cut = s + 1;
        std::string ln(s, cut);
        while (!ln.empty() && ln.back() == ' ') ln.pop_back();
        out.push_back(ln);
        s = cut;
        while (*s == ' ') ++s;
    }
}

// THE EMOTES (0.1.70, mgmp_emote): The Binding of Isaac's online emotes (0.1.90: without its speech bubble). The atlas is 4x the game's pixels (scratch/make_emote_atlas.py),
// so the linear filter keeps them sharp.
bool emotes_ready() {
    return tex_load(g.emote_atlas, "emotes\\emotes.png") && g.emote_atlas.w == kEmoteAtlasCols * 128 && g.emote_atlas.h >= (kEmoteAtlasCells / kEmoteAtlasCols) * 128;
}
void emote_image(ImDrawList* dl, ImVec2 a, ImVec2 b, int cell, ImU32 tint) {
    if (cell < 0 || cell >= kEmoteAtlasCells || !g.emote_atlas.ok()) return;
    const float cw = 128.0f / (float)g.emote_atlas.w, ch = 128.0f / (float)g.emote_atlas.h;
    const ImVec2 uv0((cell % kEmoteAtlasCols) * cw, (cell / kEmoteAtlasCols) * ch);
    dl->AddImage(g.emote_atlas.ref(), a, b, uv0, ImVec2(uv0.x + cw, uv0.y + ch), tint);
}
// `s` screen pixels per game pixel; `top_left` is the corner of the emote's place in the row (34 x 36 game pixels, the emote 32 x 32 centred in it).
// 0.1.90: the emote alone -- the speech bubble that was drawn behind it (emotes/bubble.png) is gone, at the user's request ("only the emote").
// Returns the width taken.
float draw_emote(ImDrawList* dl, ImVec2 top_left, float s, int cell, float alpha) {
    const ImVec2 c(top_left.x + 17 * s, top_left.y + 18 * s);
    emote_image(dl, ImVec2(c.x - 16 * s, c.y - 16 * s), ImVec2(c.x + 16 * s, c.y + 16 * s), cell, mul_alpha(IM_COL32_WHITE, alpha));
    return 34 * s;
}

// One chat line as "name: text", wrapped, in the colour of its sender's P1..P4 tag in the room panel (the room list's order, host first; the same four colours).
// A line that is one emote is a row of its own, "name:" and the emote, two lines tall.
struct ChatRow { std::string text; ImU32 col; Emote emote; uint64_t at = 0; };
constexpr float kEmoteRowLines = 2.0f;
float chat_row_h(const ChatRow& r, float lh) { return r.emote.pick >= 0 ? lh * kEmoteRowLines : lh; }
void chat_rows(float fs, float maxw, int first, std::vector<ChatRow>& rows) {
    static const ImU32 kPlayer[4] = { IM_COL32(205, 58, 52, 255), IM_COL32(58, 108, 205, 255), IM_COL32(214, 168, 24, 255), IM_COL32(56, 150, 68, 255) };
    SignalPeer peers[8];
    const uint32_t np = signal_peers(peers, 8);
    for (int i = first; i < chat_count(); ++i) {
        const ChatLine& l = chat_line(i);
        ImU32 col = kInk;                                           // somebody no longer in the room: plain ink
        for (uint32_t pi = 0; pi < np; ++pi) if (strcmp(peers[pi].name, l.name) == 0) { col = kPlayer[pi & 3]; break; }
        Emote e;
        if (emote_parse(l.text, e) && emotes_ready()) {             // no pictures: the token stays text
            ChatRow r{ std::string(l.name) + ":", col, e, l.at };
            rows.push_back(r);
            continue;
        }
        std::string full = std::string(l.name) + ": " + l.text;
        std::vector<std::string> parts;
        wrap_split(full.c_str(), fs, maxw, parts);
        for (const std::string& p : parts) rows.push_back({ p, col, Emote{}, l.at });
    }
}

// ---------------------------------------------------------------------------
// THE DRAWING BOARD (mgmp_draw, proto 87): the pen, the eraser and the bin in the bottom-right corner while the map or a battle is
// showing; the lines of every player in their P colour. The lines are drawn on the background list -- over the game, under every one
// of the mod's own windows -- and while the pen or the eraser is in hand a window as large as the game's picture, kept BEHIND the
// mod's other windows, takes the mouse so the clicks that draw never reach the game (the window procedure swallows what ImGui wants).
// ---------------------------------------------------------------------------
ImU32 board_color(uint8_t owner) {
    static const ImU32 kPlayer[4] = { IM_COL32(205, 58, 52, 255), IM_COL32(58, 108, 205, 255), IM_COL32(214, 168, 24, 255), IM_COL32(56, 150, 68, 255) };
    const int row = room_row_of_peer(owner);
    return row >= 0 ? kPlayer[row & 3] : kInk;
}

ImU32 board_mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int s) { return (int)(((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t); };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), 255);
}

// A quill lying from the top right to the bottom left (user's reference, 2026-10-11): the vane, in the player's drawing colour (`tip`),
// with a few notches and a pale shaft, and a dark steel nib.
void icon_pen(ImDrawList* dl, ImVec2 c, float s, ImU32 tip) {
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    const float nx0 = -0.40f, ny0 = 0.40f;          // the nib's point
    const float DX = 0.7071f, DY = -0.7071f;        // along the quill, toward its top
    const float NX = 0.7071f, NY = 0.7071f;         // across it, toward the lower right
    const float A0 = 0.20f, A1 = 1.10f;             // the vane, along the axis (the nib is 0..A0)
    auto axis = [&](float a, float off) {           // a point at `a` along the (slightly bent) axis, `off` across it
        float t = (a - A0) / (A1 - A0);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float bend = 0.035f * sinf(3.14159265f * t);
        return P(nx0 + DX * a + NX * (bend + off), ny0 + DY * a + NY * (bend + off));
    };
    struct Notch { float t, depth, len; };
    static const Notch kNotch[2][2] = { { { 0.34f, 0.05f, 0.10f }, { 0.56f, 0.05f, 0.10f } },      // upper-left side
                                        { { 0.44f, 0.04f, 0.09f }, { 0.68f, 0.035f, 0.09f } } };   // lower-right side
    auto width = [&](float t, int side) {           // side 0: upper left (the wide side), 1: lower right
        const float wmax = side == 0 ? 0.185f : 0.12f;
        float w = wmax * powf(sinf(3.14159265f * fminf(1.0f, powf(t, 0.75f))), 0.8f);
        if (t < 0.1f && w < 0.018f) w = 0.018f;
        for (const Notch& n : kNotch[side])
            if (t >= n.t - n.len && t < n.t) w -= n.depth * (t - (n.t - n.len)) / n.len;
        return w > 0 ? w : 0.0f;
    };
    // the samples along the vane: an even run plus both sides of every notch's step
    float ts[64];
    int nt = 0;
    for (int i = 0; i <= 40; ++i) ts[nt++] = i / 40.0f;
    for (const auto& side : kNotch) for (const Notch& n : side) { ts[nt++] = n.t - 0.002f; ts[nt++] = n.t; }
    std::sort(ts, ts + nt);
    ImVec2 outline[128];
    int no = 0;
    for (int i = 0; i < nt; ++i) { const float a = A0 + ts[i] * (A1 - A0); outline[no++] = axis(a, -width(ts[i], 0)); }
    for (int i = nt - 1; i >= 0; --i) { const float a = A0 + ts[i] * (A1 - A0); outline[no++] = axis(a, width(ts[i], 1)); }
    dl->AddConcavePolyFilled(outline, no, tip);
    dl->AddPolyline(outline, no, kInk, 0.03f * s, ImDrawFlags_Closed);
    // the shaft
    ImVec2 shaft[22];
    shaft[0] = axis(A0 - 0.02f, 0);
    for (int i = 0; i <= 20; ++i) shaft[i + 1] = axis(A0 + (A1 - A0) * 0.86f * i / 20.0f, 0);
    dl->AddPolyline(shaft, 22, IM_COL32(252, 244, 222, 255), 0.028f * s);
    // the nib: the point, the shoulders, the base under the vane; its slit and hole
    auto nib = [&](float a, float w) { return P(nx0 + DX * a + NX * w, ny0 + DY * a + NY * w); };
    const ImVec2 nb[5] = { nib(0, 0), nib(0.12f, 0.06f), nib(A0 + 0.02f, 0.035f), nib(A0 + 0.02f, -0.035f), nib(0.12f, -0.06f) };
    dl->AddConvexPolyFilled(nb, 5, IM_COL32(36, 46, 84, 255));
    dl->AddPolyline(nb, 5, kInk, 0.025f * s, ImDrawFlags_Closed);
    dl->AddLine(nib(0.02f, 0), nib(0.10f, 0), IM_COL32(200, 205, 225, 255), 0.014f * s);
    dl->AddCircleFilled(nib(0.115f, 0), 0.016f * s, IM_COL32(200, 205, 225, 255));
}

// An eraser on the slant, in two colours.
void icon_eraser(ImDrawList* dl, ImVec2 c, float s) {
    const ImVec2 u(0.7071f * s, -0.7071f * s), w(0.7071f * s, 0.7071f * s);
    auto Q = [&](float a, float b) { return ImVec2(c.x + u.x * a + w.x * b, c.y + u.y * a + w.y * b); };
    const float L = 0.36f, W = 0.17f, split = 0.06f;
    dl->AddQuadFilled(Q(-L, -W), Q(split, -W), Q(split, W), Q(-L, W), kPink);
    dl->AddQuadFilled(Q(split, -W), Q(L, -W), Q(L, W), Q(split, W), IM_COL32(120, 160, 205, 255));
    dl->AddQuad(Q(-L, -W), Q(L, -W), Q(L, W), Q(-L, W), kInk, 0.035f * s);
    dl->AddLine(Q(split, -W), Q(split, W), kInk, 0.025f * s);
}

// A bin: the lid and its handle, the body with three ribs.
void icon_bin(ImDrawList* dl, ImVec2 c, float s) {
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    dl->AddQuadFilled(P(-0.27f, -0.17f), P(0.27f, -0.17f), P(0.21f, 0.37f), P(-0.21f, 0.37f), IM_COL32(150, 156, 160, 255));
    dl->AddQuad(P(-0.27f, -0.17f), P(0.27f, -0.17f), P(0.21f, 0.37f), P(-0.21f, 0.37f), kInk, 0.035f * s);
    for (float x : { -0.1f, 0.0f, 0.1f }) dl->AddLine(P(x * 1.1f, -0.08f), P(x, 0.29f), kInk, 0.025f * s);
    dl->AddRectFilled(P(-0.34f, -0.29f), P(0.34f, -0.2f), IM_COL32(170, 176, 180, 255), 0.02f * s);
    dl->AddRect(P(-0.34f, -0.29f), P(0.34f, -0.2f), kInk, 0.02f * s, 0, 0.035f * s);
    dl->AddRect(P(-0.1f, -0.38f), P(0.1f, -0.29f), kInk, 0.02f * s, 0, 0.035f * s);
}

// One of the three tool buttons. `kind` 0 pen, 1 eraser, 2 bin.
bool board_button(const char* id, ImVec2 a, ImVec2 b, int kind, bool active, ImU32 mine, const char* tip, float k) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(b.x - a.x, b.y - a.y));
    const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImGui::PopID();
    if (hov) g.hovering_button = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = active ? board_mix(kPaperHi, mine, 0.42f) : held ? kPaperLo : hov ? kPaperHi : kPaper;
    const float lift = hov && !held ? 2.0f : 0.0f;
    const ImVec2 pa(a.x, a.y - lift), pb(b.x, b.y - lift);
    dl->AddRectFilled(ImVec2(pa.x + 3, pa.y + 4), ImVec2(pb.x + 3, pb.y + 4), IM_COL32(0, 0, 0, 60), 2.0f);
    rough_rect(dl, pa, pb, fill, kInk, active ? 3.2f : 2.2f, ImHashStr(id), 1.2f);
    const ImVec2 c((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f);
    const float s = (b.x - a.x) * 0.92f;
    if (kind == 0) icon_pen(dl, c, s, mine);
    else if (kind == 1) icon_eraser(dl, c, s);
    else icon_bin(dl, c, s);
    if (hov && tip) {   // the name of the tool to the left of the column, on a slip of paper
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float fs = 22 * k;
        const ImVec2 ts = text_size(fs, tip);
        const float cy = (a.y + b.y) * 0.5f;
        const ImVec2 ta(a.x - ts.x - 26 * k, cy - ts.y * 0.5f - 7 * k), tb(a.x - 12 * k, cy + ts.y * 0.5f + 7 * k);
        rough_rect(fg, ta, tb, kPaperHi, kInk, 1.8f, ImHashStr(tip), 1.0f);
        text_at(fg, ImVec2(ta.x + 7 * k, ta.y + 7 * k), fs, kInk, tip);
    }
    return clicked;
}

void draw_board(const View& v) {
    g.board_cursor = 0;
    draw_tick();
    uint64_t scope = 0;
    const int layer = draw_visible_layer(&scope);
    if (layer < 0) return;
    const float k = v.k;
    const ImVec2 o = v.org;
    const ImU32 mine = board_color(net_self());

    // the lines
    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        const float th = 5.0f * k;
        std::vector<ImVec2> pts;
        for (int i = 0; i < draw_stroke_count(); ++i) {
            const DrawStroke& s = draw_stroke(i);
            if ((int)s.layer != layer || (layer == kDrawLayerBattle && s.scope != scope) || s.pts.empty()) continue;
            const ImU32 col = board_color(s.owner);
            // one polyline per run: the eraser cuts a stroke into pieces (DrawStroke::cut)
            for (size_t r0 = 0; r0 < s.pts.size();) {
                size_t r1 = r0 + 1;
                while (r1 < s.pts.size() && !(r1 < s.cut.size() && s.cut[r1])) ++r1;
                pts.resize(r1 - r0);
                for (size_t j = r0; j < r1; ++j) pts[j - r0] = ImVec2(o.x + s.pts[j].x / 65535.0f * v.w, o.y + s.pts[j].y / 65535.0f * v.h);
                r0 = r1;
                if (pts.size() == 1) { bg->AddCircleFilled(pts[0], th * 0.6f, col, 12); continue; }
                bg->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(0, 0, 0, 55), ImDrawFlags_None, th + 2.5f * k);
                bg->AddPolyline(pts.data(), (int)pts.size(), col, ImDrawFlags_None, th);
                if (pts.size() < 600) { for (const ImVec2& p : pts) bg->AddCircleFilled(p, th * 0.5f, col, 8); }   // round joins
                else { bg->AddCircleFilled(pts.front(), th * 0.5f, col, 8); bg->AddCircleFilled(pts.back(), th * 0.5f, col, 8); }
            }
        }
    }

    // the canvas: the mouse belongs to the board while a tool is in hand (right click puts it down)
    const DrawTool tool = draw_tool();
    if (tool != DrawTool::None) {
        if (overlay_window_begin("##mgmp_board_canvas", o, ImVec2(v.w, v.h), false)) {
            ImGui::SetCursorScreenPos(o);
            ImGui::InvisibleButton("##canvas", ImVec2(v.w, v.h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float fx = (m.x - o.x) / v.w, fy = (m.y - o.y) / v.h;
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                draw_set_tool(DrawTool::None);
            } else if (tool == DrawTool::Pen) {
                static ImVec2 last;
                if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { draw_pen_down(fx, fy); last = m; }
                else if (ImGui::IsItemActive() && draw_pen_active()) {
                    const float dx = m.x - last.x, dy = m.y - last.y;
                    if (dx * dx + dy * dy >= 2.5f * 2.5f * k * k) { draw_pen_move(fx, fy); last = m; }
                }
                if (ImGui::IsItemDeactivated() || (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && draw_pen_active())) draw_pen_up();
                if (hovered) { g.board_cursor = 1; g.board_cursor_col = mine; g.board_cursor_at = GetTickCount64(); }   // the pen is the pointer (draw_cursor)
            } else if (tool == DrawTool::Eraser) {
                const float r = 20.0f;   // stage pixels
                if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) draw_erase_at(fx, fy, r);
                if (hovered) {
                    fg->AddCircle(m, r * k, IM_COL32(255, 255, 255, 220), 24, 3.0f * k);
                    fg->AddCircle(m, r * k, kInk, 24, 1.5f * k);
                    g.board_cursor = 2; g.board_cursor_r = r * k; g.board_cursor_at = GetTickCount64();      // and the eraser is the pointer (draw_cursor)
                }
            }
        }
        ImGui::End();
    }

    // the three tools, a column on the right edge of the game's picture above the battle's "end turn" button (2026-10-11: a row in
    // the bottom-right corner covered it), stage x 1858..1914, y 707..891 -- clear of the map's furniture too
    const float bs = 56 * k, gap = 8 * k;
    const ImVec2 a0(o.x + (kStageW - 6 - 56) * k, o.y + 707 * k);
    const ImVec2 b1(a0.x + bs, a0.y + 3 * bs + 2 * gap);
    if (overlay_window_begin("##mgmp_board_tools", ImVec2(a0.x - 8 * k, a0.y - 8 * k), ImVec2(b1.x - a0.x + 14 * k, b1.y - a0.y + 16 * k))) {
        const DrawTool t = draw_tool();
        ImVec2 p = a0;
        if (board_button("bd_pen", p, ImVec2(p.x + bs, p.y + bs), 0, t == DrawTool::Pen, mine, tr(Tx::DRAW_PEN), k))
            draw_set_tool(t == DrawTool::Pen ? DrawTool::None : DrawTool::Pen);
        p.y += bs + gap;
        if (board_button("bd_eraser", p, ImVec2(p.x + bs, p.y + bs), 1, t == DrawTool::Eraser, mine, tr(Tx::DRAW_ERASER), k))
            draw_set_tool(t == DrawTool::Eraser ? DrawTool::None : DrawTool::Eraser);
        p.y += bs + gap;
        if (board_button("bd_bin", p, ImVec2(p.x + bs, p.y + bs), 2, false, mine, tr(Tx::DRAW_CLEAR), k))
            draw_clear_own();
    }
    ImGui::End();
}

void draw_chat(const View& v) {
    const float k = v.k;
    bool open = chat_input_open();
    if (open && !chat_available()) { chat_set_input_open(false); open = false; }       // the room ended under it
    static char buf[kChatTextMax];
    static bool was_open = false;
    static int seen = 0;

    if (open) {
        if (!was_open) { was_open = true; buf[0] = 0; seen = -1; }
        g.hovering_button = true;
        // Enter again: send what was typed (an empty box only closes), then the history is shown for ten seconds on every screen. Esc closes without sending.
        const bool enter = chat_take_enter_while_open();
        const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (enter || esc) {
            if (enter) chat_submit(buf);
            buf[0] = 0; was_open = false;
            chat_set_input_open(false);
            ImGui::GetIO().ClearInputKeys();
            return;
        }
        if (!modal_begin("##mgmp_chat")) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool emotes = emotes_ready();
        const float W = 1100, H = emotes ? 790.0f : 700.0f;
        const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
        paper(dl, v, a, ImVec2(a.x + W * k, a.y + H * k));
        auto X = [&](float x) { return a.x + x * k; };
        auto Y = [&](float y) { return a.y + y * k; };
        text_c(dl, X(W / 2), Y(22), 40 * k, kInk, tr(Tx::CHAT_TITLE));

        const float fs = 28 * k, lh = 38 * k, cw = (W - 120) * k;
        std::vector<ChatRow> rows;
        chat_rows(fs, cw, 0, rows);
        const float hy0 = Y(84), hy1 = Y(H - (emotes ? 210 : 120));
        rough_rect(dl, ImVec2(X(40), hy0), ImVec2(X(W - 40), hy1), IM_COL32(255, 255, 250, 60), kInk, 2.2f, 31, 1.2f);
        ImGui::SetCursorScreenPos(ImVec2(X(48), hy0 + 6 * k));
        ImGui::BeginChild("##chat_hist", ImVec2((W - 96) * k, hy1 - hy0 - 12 * k), false, ImGuiWindowFlags_NoBackground);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const uint64_t tnow = GetTickCount64();
        if (rows.empty()) text_at(cdl, ImVec2(cp.x + 12 * k, cp.y + 10 * k), fs, kGrey, tr(Tx::CHAT_EMPTY));
        float ry = cp.y + 6 * k;
        for (size_t i = 0; i < rows.size(); ++i) {
            const ChatRow& r = rows[i];
            text_at(cdl, ImVec2(cp.x + 12 * k, ry), fs, r.col, r.text.c_str());
            if (r.emote.pick >= 0) {
                const float rh = chat_row_h(r, lh), s = (rh - 4 * k) / 36.0f;
                draw_emote(cdl, ImVec2(cp.x + 12 * k + text_size(fs, r.text.c_str()).x + 10 * k, ry + 2 * k), s, emote_cell(r.emote, tnow - r.at), 1.0f);
            }
            ry += chat_row_h(r, lh);
        }
        ImGui::Dummy(ImVec2(cw, (rows.empty() ? lh : ry - cp.y - 6 * k) + 12 * k));
        if (seen != chat_count()) { seen = chat_count(); ImGui::SetScrollHereY(1.0f); }      // the newest line is the one in view
        ImGui::EndChild();

        // The emotes: one click sends one as a line of its own. With nothing typed the box closes, as Enter would; typed text stays for Enter.
        // NOT an ImGui button (0.1.71): the input box below takes the keyboard focus back every frame (SetKeyboardFocusHere), which makes IT the active item, so a
        // button pressed here lost its active state the next frame and never saw its release -- the click went nowhere. The press itself is the click.
        if (emotes) {
            const float cell = 46, gap = 2, x0 = (W - (cell + gap) * kEmoteCount + gap) * 0.5f, y0 = H - 196;
            for (int i = 0; i < kEmoteCount; ++i) {
                const ImVec2 ea(X(x0 + (cell + gap) * i), Y(y0)), eb(ea.x + cell * k, ea.y + cell * k);
                const bool hov = ImGui::IsMouseHoveringRect(ea, eb, false);
                const bool click = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
                if (hov) { dl->AddRectFilled(ea, eb, IM_COL32(255, 255, 250, 150), 6 * k); dl->AddRect(ea, eb, kInk, 6 * k, 0, 1.6f * k); }
                const float pad = hov ? 0 : 3 * k;
                emote_image(dl, ImVec2(ea.x + pad, ea.y + pad), ImVec2(eb.x - pad, eb.y - pad), emote_icon_cell(i), IM_COL32_WHITE);
                if (click) log_line("CHAT", "emote %s picked", emote_name(i));
                if (click && chat_submit_emote(i) && !buf[0]) {
                    was_open = false;
                    chat_set_input_open(false);
                    ImGui::GetIO().ClearInputKeys();
                    ImGui::End();
                    return;
                }
            }
        }

        ImGui::SetKeyboardFocusHere();                                    // the box keeps the keyboard, so the game never gets a key while this is open
        paper_input("##chat_in", ImVec2(X(40), Y(H - 100)), ImVec2(X(W - 40), Y(H - 44)), buf, sizeof(buf), tr(Tx::CHAT_HINT), 28 * k);
        ImGui::End();
        return;
    }
    was_open = false;

    // Not typing: the history for five seconds after the last line, then a fade. NO BACKGROUND (2026-10-06): the lines stand on the game itself; a soft light halo behind each letter keeps them
    // readable on a dark scene.
    const uint64_t until = chat_show_until();
    if (!until || !chat_available()) return;
    const uint64_t t = GetTickCount64();
    if (t >= until + kChatFadeMs) return;
    const float a = t <= until ? 1.0f : 1.0f - (float)(t - until) / (float)kChatFadeMs;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float fs = 26 * k, lh = 34 * k, cw = 820 * k;
    std::vector<ChatRow> rows;
    chat_rows(fs, cw, chat_count() > 24 ? chat_count() - 24 : 0, rows);
    if (rows.empty()) return;
    const float maxh = lh * 16;                                     // sixteen lines' height (an emote row counts as two)
    size_t first = rows.size();
    float h = 0;
    while (first > 0 && h + chat_row_h(rows[first - 1], lh) <= maxh) h += chat_row_h(rows[--first], lh);
    if (first == rows.size()) return;
    h += 24 * k;
    const ImVec2 pa(v.org.x + 28 * k, v.org.y + v.h - 150 * k - h);
    ImFont* f = g.font ? g.font : ImGui::GetFont();
    const ImU32 halo = mul_alpha(IM_COL32(240, 238, 228, 200), a);
    const float o = 1.5f * k;
    float ry = pa.y + 12 * k;
    for (size_t i = first; i < rows.size(); ++i) {
        const ImVec2 at(pa.x + 20 * k, ry);
        const char* s = rows[i].text.c_str();
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) dl->AddText(f, fs, ImVec2(at.x + dx * o, at.y + dy * o), halo, s);
        dl->AddText(f, fs, at, mul_alpha(rows[i].col, a), s);
        if (rows[i].emote.pick >= 0) {
            const float rh = chat_row_h(rows[i], lh), sc = (rh - 4 * k) / 36.0f;
            draw_emote(dl, ImVec2(at.x + text_size(fs, s).x + 10 * k, ry + 2 * k), sc, emote_cell(rows[i].emote, t - rows[i].at), a);
        }
        ry += chat_row_h(rows[i], lh);
    }
}

void draw_toast(const View& v) {
    if (now() >= g.toast_until || !g.toast[0]) return;
    const float left = (float)(g.toast_until - now());
    const float a = left < 0.6f ? left / 0.6f : 1.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float size = 30 * v.k;
    const ImVec2 sz = text_size(size, g.toast);
    const float cx = v.org.x + v.w * 0.5f, y = v.org.y + v.h * 0.90f;
    const ImVec2 pa(cx - sz.x * 0.5f - 34 * v.k, y - 14 * v.k), pb(cx + sz.x * 0.5f + 34 * v.k, y + sz.y + 14 * v.k);
    rough_rect(dl, pa, pb, mul_alpha(kPaper, a), mul_alpha(kInk, a), 2.6f, 5, 1.4f);
    ImFont* f = g.font ? g.font : ImGui::GetFont();
    dl->AddText(f, size, ImVec2(cx - sz.x * 0.5f, y), mul_alpha(kInk, a), g.toast);
}

} // namespace

// ---------------------------------------------------------------------------

void menu_bring_up() {
    // The host's handshake choice is a panel; it may only wait for a click that can be made.
    checkpoint_set_manual(true);
    ImGuiIO& io = ImGui::GetIO();
    const std::string dir = assets_dir();
    const std::string ttf = dir + "\\fonts\\ui.ttf";
    // The game's own menu face, exported from its SWF. Sizes are chosen per draw
    // call (ImGui 1.92 rasterises a glyph at the size asked for), so one font
    // object serves every text in the menus.
    if (!dir.empty() && GetFileAttributesW(widen(ttf).c_str()) != INVALID_FILE_ATTRIBUTES)
        g.font = io.Fonts->AddFontFromFileTTF(ttf.c_str(), 0.0f);
    if (!g.font) {
        // Windows ships a CJK face in every locale we care about. It is not the
        // game's, but the menus stay readable and the log says why.
        char windir[MAX_PATH] = {};
        GetWindowsDirectoryA(windir, MAX_PATH);
        const char* faces[] = { "msyh.ttc", "simhei.ttf", "msgothic.ttc" };
        for (const char* face : faces) {
            char f[MAX_PATH * 2];
            _snprintf_s(f, sizeof(f), _TRUNCATE, "%s\\Fonts\\%s", windir, face);
            if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) continue;
            g.font = io.Fonts->AddFontFromFileTTF(f, 0.0f);
            if (g.font) { log_line("MENU", "menu font: %s (the game's ui.ttf was not found under %s)", f, dir.c_str()); break; }
        }
    } else {
        log_line("MENU", "menu font: %s", ttf.c_str());
    }
    if (!g.font) log_line("MENU", "!! no CJK font could be loaded -- Chinese labels will show as '?'");

    // THE OTHER NINE LANGUAGES (2026-10-01). ui.ttf is the game's Simplified Chinese face: basic Latin and
    // CJK, but no accented Latin (French, German...), no Cyrillic, no Hangul and no kana. Windows faces are
    // merged BEHIND it, so a glyph the game's face has is still drawn in it and only the missing ones fall
    // through: Segoe UI (Latin + Cyrillic), Malgun Gothic (Hangul), Yu Gothic / MS Gothic (kana, kanji).
    if (g.font) {
        char windir[MAX_PATH] = {};
        GetWindowsDirectoryA(windir, MAX_PATH);
        const char* merge[] = { "segoeui.ttf", "arial.ttf", "malgun.ttf", "YuGothM.ttc", "msgothic.ttc" };
        int merged = 0;
        for (const char* face : merge) {
            char f[MAX_PATH * 2];
            _snprintf_s(f, sizeof(f), _TRUNCATE, "%s\\Fonts\\%s", windir, face);
            if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.DstFont = g.font;
            if (io.Fonts->AddFontFromFileTTF(f, 0.0f, &cfg)) ++merged;
        }
        log_line("MENU", "menu font: %d fallback face(s) merged for the other languages", merged);
    }
}

// The game's language, read from its own settings.txt (one folder above the saves) about once a second:
// its options screen writes the file the moment the language changes. See mgmp_i18n.h.
void poll_language() {
    static ULONGLONG next = 0;
    static FILETIME  seen = {};
    const ULONGLONG t = GetTickCount64();
    if (t < next) return;
    next = t + 1000;
    wchar_t dir[MAX_PATH * 2] = {};
    if (!savefile_save_dir(dir, sizeof(dir) / sizeof(dir[0]))) return;
    std::wstring p(dir);
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t cut = p.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return;
    p = p.substr(0, cut) + L"\\settings.txt";
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &seen) == 0) return;
    seen = fad.ftLastWriteTime;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "current_language ", 17) != 0) continue;
        char* v = line + 17;
        v[strcspn(v, "\r\n ")] = 0;
        if (i18n_set_lang_code(v)) log_line("MENU", "the game's language is '%s' -- the mod's text follows it", v);
        break;
    }
    fclose(f);
}

// --- dev harness hooks ------------------------------------------------------
//
// Both ask for a press that happens from the button detour rather than from the
// caller: a button is only pressable once Button::update has recomputed the state
// its own guards read, and the detour is the one place that has happened.
char g_press[64];
int  g_slot_req = -1;
void menu_request_press(const char* n) { strncpy_s(g_press, n ? n : "", _TRUNCATE); }
void menu_request_slot(int s)          { g_slot_req = s; }
int  menu_take_slot_request()          { const int s = g_slot_req; g_slot_req = -1; return s; }

void menu_on_button(void* button) {
    if (!button) return;
    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name))) return;
    if (!name[0]) return;
    const ULONGLONG t = GetTickCount64();

    // The dev harness's press, first: it names the button it wants and means it.
    if (g_press[0] && strcmp(name, g_press) == 0) {
        log_line("MENU", "pressing '%s' as asked", name);
        g_press[0] = 0;
        savefile_click_button(button);
        return;
    }

    // A room just opened: the player has said who they are playing with, and the
    // next thing the game needs from them is a save slot. Pressing Play is how the
    // save screen comes up -- the same press mgmp_savefile makes for a client, for
    // the same reason (see kBtnName_MainMenuPlay).
    if (g.auto_play_until && strcmp(name, kBtnName_MainMenuPlay) == 0) {
        g.auto_play_until = 0;
        log_line("MENU", "a room is open -- pressing '%s' so the save screen comes up", name);
        savefile_click_button(button);
        return;
    }

    if (!g.title_scene) return;
    if      (strncmp(name, "MainMenu_Button_", 16) == 0)  g.native_ms = t;
    else if (strncmp(name, "Button_PauseMenu_", 17) == 0) g.pause_ms  = t;
}

// ---------------------------------------------------------------------------
// the save-selection stage: who has picked a save, and the host choosing the handshake save
// ---------------------------------------------------------------------------

// The session id of the i-th row of the room list: the list and the session's membership both run
// host first, in ascending id (the same pairing page_for_row relies on).
uint8_t id_for_room_row(uint32_t i) {
    if ((int)i == room_self_row()) return net_self();
    const uint8_t id = room_peer_for_row(i);
    return id == 0xFF ? 7 : id;       // bit 7 is never a player: an unplaced row reads "not picked"
}

// A FILETIME (100 ns since 1601, UTC) as the player's local "YYYY-MM-DD HH:MM:SS".
void format_filetime(uint64_t ft, char* out, size_t cap) {
    FILETIME a, b; SYSTEMTIME st;
    a.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu); a.dwHighDateTime = (DWORD)(ft >> 32);
    if (!ft || !FileTimeToLocalFileTime(&a, &b) || !FileTimeToSystemTime(&b, &st)) {
        _snprintf_s(out, cap, _TRUNCATE, tr(Tx::TIME_UNKNOWN));
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

constexpr uint32_t kSyncRowsShown = 6;   // rows of the handshake list shown at once; the rest scroll

void draw_save_sync(const View& v) {
    SaveSyncView sv;
    static bool was_up = false;
    if (!in_room() || !checkpoint_sync_view(sv)) { was_up = false; return; }
    // The room panel would sit over the right of this one; fold it the moment this one appears (the
    // player can still open it again from its tag).
    if (!was_up) { was_up = true; g.panel_open = false; }

    const float k = v.k;
    SignalPeer peers[8];
    const uint32_t np = signal_peers(peers, 8);
    const bool show_rows = sv.phase == kSyncWaiting || sv.phase == kSyncGuestWaiting;
    const bool choosing  = sv.phase == kSyncHostChoosing;
    const uint32_t items = choosing ? sv.n + (sv.prep ? 1u : 0u) : 0u;

    const float W = 780;
    float H = 300;
    if (sv.phase == kSyncInvalid && sv.selected) H = (sv.selected & kHoldersNoRecord) ? 500 : 400;   // the no-record text is longer
    if (show_rows)     H = 170 + (np ? np : 1) * 62 + 40 + 76;
    else if (choosing) H = 190 + (items ? (items < kSyncRowsShown ? items : kSyncRowsShown) : 1) * 76 + 40 + 76;   // more than that scroll
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_sync", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    if (show_rows) {
        text_c(dl, X(W / 2), Y(40), 44 * k, kInk,
               sv.phase == kSyncGuestWaiting ? tr(Tx::SYNC_WAIT_HOST) : tr(Tx::SYNC_WAIT_OTHERS));
        text_c(dl, X(W / 2), Y(102), 22 * k, kInkSoft,
               sv.phase == kSyncGuestWaiting ? tr(Tx::SYNC_ALL_PICKED) : tr(Tx::SYNC_THEN_HOST));
        float y = 158;
        for (uint32_t i = 0; i < np; ++i) {
            const bool me = strcmp(peers[i].name, signal_name()) == 0;
            const bool done = (sv.selected >> (id_for_room_row(i) & 7)) & 1;
            status_dot(dl, ImVec2(X(70), Y(y + 22)), 10 * k, done ? kGreen : kAmber);
            char nm[80];
            _snprintf_s(nm, sizeof(nm), _TRUNCATE, "%s%s", peers[i].name, me ? tr(Tx::YOU_SUFFIX) : "");
            text_at(dl, ImVec2(X(96), Y(y + 4)), 28 * k, kInk, nm);
            const char* st = done ? tr(Tx::PICKED) : tr(Tx::NOT_PICKED);
            const float sw = text_size(26 * k, st).x;
            text_at(dl, ImVec2(X(W - 60) - sw, Y(y + 6)), 26 * k, done ? kGreen : kGrey, st);
            y += 62;
        }
        // A pick is not a promise: anybody may start the round over (everyone then picks again).
        if (paper_button("hs_repick", ImVec2(X(W / 2 - 150), Y(H - 96)), ImVec2(X(W / 2 + 150), Y(H - 40)),
                         tr(Tx::REPICK), 26 * k))
            room_abort_selection();
    } else if (choosing) {
        text_c(dl, X(W / 2), Y(40), 44 * k, kInk, tr(Tx::CHOOSE_SYNC));
        text_c(dl, X(W / 2), Y(102), 22 * k, kInkSoft, tr(Tx::CHOOSE_SYNC_HINT));
        // THE LIST SCROLLS (2026-10-04): the journal keeps the whole run now, so it can be far longer than the window. The wheel or the bar moves it.
        const uint32_t shown = items ? (items < kSyncRowsShown ? items : kSyncRowsShown) : 1;
        ImGui::SetCursorScreenPos(ImVec2(X(44), Y(146)));
        ImGui::BeginChild("##hs_list", ImVec2((W - 88) * k, (shown * 76 - 4) * k), false, ImGuiWindowFlags_NoBackground);
        const float lx = ImGui::GetCursorScreenPos().x + 6 * k, lw = (W - 100 - 24) * k;
        auto row = [&](const char* id, const char* label, Tone tone, bool armed) {
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const bool hit = paper_button(id, ImVec2(lx, rp.y + 2 * k), ImVec2(lx + lw, rp.y + 66 * k), label, 22 * k, true, tone, armed);
            ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + 76 * k));
            ImGui::Dummy(ImVec2(1, 0));
            return hit;
        };
        if (sv.prep && row("hs_prep", tr(Tx::PREP_STAGE), Tone::Good, false)) checkpoint_host_pick(-1);
        for (uint32_t i = 0; i < sv.n && i < kCheckpointCandidates; ++i) {
            char when[40], label[220], id[24], tail[160];
            format_filetime(sv.entry[i].stamp, when, sizeof(when));
            const bool boss = (sv.entry[i].flags & kCheckpointAfterBoss) != 0;      // right after the chapter boss: next floor or home from here
            _snprintf_s(tail, sizeof(tail), _TRUNCATE, "%s%s", i == 0 ? tr(Tx::LATEST) : "", boss ? tr(Tx::SYNC_AFTER_BOSS) : "");
            const uint8_t stage = checkpoint_stage_of_seq(sv.entry[i].seq);       // one of the two saves from before the map: the run starts over from that page
            if (stage) _snprintf_s(label, sizeof(label), _TRUNCATE, tr(Tx::SYNC_ENTRY_STAGE), i + 1, tr(stage == kStagePrep ? Tx::SYNC_STAGE_PREP : Tx::SYNC_STAGE_READY), when, tail);
            else       _snprintf_s(label, sizeof(label), _TRUNCATE, tr(Tx::SYNC_ENTRY), i + 1, (unsigned long long)sv.entry[i].seq, when, tail);
            _snprintf_s(id, sizeof(id), _TRUNCATE, "hs_%u", i);
            if (row(id, label, Tone::Normal, boss)) checkpoint_host_pick((int)i);
        }
        ImGui::EndChild();
        if (paper_button("hs_repick", ImVec2(X(W / 2 - 150), Y(H - 96)), ImVec2(X(W / 2 + 150), Y(H - 40)),
                         tr(Tx::REPICK), 26 * k))
            room_abort_selection();
    } else {
        text_c(dl, X(W / 2), Y(50), 44 * k, kRed, tr(Tx::COMBO_INVALID));
        text_c(dl, X(W / 2), Y(122), 26 * k, kInk, tr(Tx::COMBO_INVALID_1));
        text_c(dl, X(W / 2), Y(160), 26 * k, kInk, tr(Tx::COMBO_INVALID_2));
        if (sv.selected) {   // for this phase the mask names the players whose save carries an unfinished co-op record
            char who[160] = "";
            for (uint32_t i = 0; i < np; ++i)
                if ((sv.selected >> (id_for_room_row(i) & 7)) & 1) {
                    const size_t at = strlen(who);
                    _snprintf_s(who + at, sizeof(who) - at, _TRUNCATE, "%s%s", at ? ", " : "", peers[i].name);
                }
            char line[480];
            // bit 7: every one of them is on the map with no co-op record to recover it (kHoldersNoRecord) -- the way out is back to the warehouse, not "load the original save"
            _snprintf_s(line, sizeof(line), _TRUNCATE, tr((sv.selected & kHoldersNoRecord) ? Tx::COMBO_INVALID_4 : Tx::COMBO_INVALID_3), who[0] ? who : "?");
            wrap_text(dl, X(50), Y(204), (W - 100) * k, 22 * k, 28 * k, kInkSoft, line);
        }
        // OK starts the round over on EVERY peer: nobody is left holding a pick the host refused.
        if (paper_button("hs_ok", ImVec2(X(W / 2 - 150), Y(H - 90)), ImVec2(X(W / 2 + 150), Y(H - 34)),
                         tr(Tx::OK_REPICK), 26 * k, true, Tone::Normal)) {
            checkpoint_sync_dismiss();
            room_abort_selection();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// the abandon-adventure vote (mgmp_abandon.h): the question the others see, and the proposer's wait
// ---------------------------------------------------------------------------

void draw_abandon(const View& v) {
    AbandonView av;
    if (!abandon_view(av)) return;
    static const ImU32 kPlayerCol[4] = { IM_COL32(205, 58, 52, 255), IM_COL32(58, 108, 205, 255),
                                         IM_COL32(214, 168, 24, 255), IM_COL32(56, 150, 68, 255) };
    const float k = v.k;
    const float W = 760;
    const float H = 330 + av.n * 46.0f;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f - 40);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_abandon", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    text_c(dl, X(W / 2), Y(36), 44 * k, kInk, tr(Tx::AB_TITLE));
    char pn[8];
    if (av.phase == AbandonPhase::Asking) {
        // "P2 想要放弃冒险，是否同意？" -- the P in its own colour, the line centred as a whole.
        _snprintf_s(pn, sizeof(pn), _TRUNCATE, "P%u", (unsigned)(av.proposer_pos + 1));
        const char* rest = tr(Tx::AB_ASK);
        const float w1 = text_size(30 * k, pn).x, w2 = text_size(30 * k, rest).x;
        const float x0 = X(W / 2) - (w1 + w2) * 0.5f;
        text_at(dl, ImVec2(x0, Y(110)), 30 * k, kPlayerCol[av.proposer_pos & 3], pn);
        text_at(dl, ImVec2(x0 + w1, Y(110)), 30 * k, kInk, rest);
    } else if (av.phase == AbandonPhase::Answered) {
        text_c(dl, X(W / 2), Y(110), 30 * k, kInk, tr(Tx::AB_AGREED_WAIT));
    } else {
        text_c(dl, X(W / 2), Y(110), 30 * k, kInk, tr(Tx::AB_WAIT));
    }

    float y = 170;
    for (uint32_t i = 0; i < av.n; ++i) {
        const bool yes = av.agreed[i];
        status_dot(dl, ImVec2(X(90), Y(y + 22)), 10 * k, yes ? kGreen : kAmber);
        _snprintf_s(pn, sizeof(pn), _TRUNCATE, "P%u", (unsigned)(i + 1));
        text_at(dl, ImVec2(X(120), Y(y + 4)), 28 * k, kPlayerCol[i & 3], pn);
        const char* who = i == av.self_pos ? tr(Tx::YOU_SUFFIX) : "";
        text_at(dl, ImVec2(X(120) + text_size(28 * k, pn).x + 6 * k, Y(y + 4)), 28 * k, kInk, who);
        const char* st = i == av.proposer_pos ? tr(Tx::AB_PROPOSER) : (yes ? tr(Tx::AB_AGREED) : tr(Tx::AB_WAITING));
        const float sw = text_size(26 * k, st).x;
        text_at(dl, ImVec2(X(W - 90) - sw, Y(y + 6)), 26 * k, yes ? kGreen : kGrey, st);
        y += 46;
    }

    char cd[48];
    _snprintf_s(cd, sizeof(cd), _TRUNCATE, tr(Tx::AB_TIMER), (unsigned)av.seconds_left);
    text_c(dl, X(W / 2), Y(y + 8), 20 * k, kInkSoft, cd);

    const float by0 = H - 96, by1 = H - 34;
    if (av.phase == AbandonPhase::Asking) {
        if (paper_button("ab_yes", ImVec2(X(60), Y(by0)), ImVec2(X(W / 2 - 10), Y(by1)), tr(Tx::AB_YES), 30 * k, true, Tone::Good))
            abandon_vote(true);
        if (paper_button("ab_no", ImVec2(X(W / 2 + 10), Y(by0)), ImVec2(X(W - 60), Y(by1)), tr(Tx::AB_NO), 30 * k, true, Tone::Danger))
            abandon_vote(false);
    } else if (av.phase == AbandonPhase::Proposing) {
        if (paper_button("ab_cancel", ImVec2(X(W / 2 - 130), Y(by0)), ImVec2(X(W / 2 + 130), Y(by1)), tr(Tx::AB_CANCEL), 30 * k, true, Tone::Danger))
            abandon_withdraw();
    }
    ImGui::End();
}

// The room's notices (a player dropped, the house boss is played alone): one at a time, with an OK.
void draw_room_notice(const View& v) {
    char text[1024];
    if (!room_notice(text, sizeof(text))) return;
    const float k = v.k;
    const float W = 820;
    // room for the drop notice plus its fight sentence in the longer languages -- and taller for a longer text (the house boss co-op's notices)
    const float text_h = k > 0 ? wrap_text(nullptr, 0, 0, (W - 120) * k, 26 * k, 38 * k, kInk, text) / k : 0;
    const float H = text_h + 210 > 410 ? text_h + 210 : 410;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f - 60);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_room_notice", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(34), 40 * k, kInk, tr(Tx::NOTICE_TITLE));
    wrap_text(dl, X(60), Y(100), (W - 120) * k, 26 * k, 38 * k, kInk, text);
    if (paper_button("rn_ok", ImVec2(X(W / 2 - 110), Y(H - 88)), ImVec2(X(W / 2 + 110), Y(H - 32)), tr(Tx::OK), 28 * k))
        room_notice_dismiss();
    ImGui::End();
}

// EXPERIMENTAL house boss co-op (mgmp_housecoop): the helper's question, and the waiting lines of the helped player and of a held gear screen.
void draw_housecoop(const View& v) {
    HouseCoopView hv;
    if (!housecoop_view(hv)) return;
    const float k = v.k;
    const bool ask = hv.panel == HcPanel::Ask;
    const bool button = ask || hv.panel == HcPanel::WaitVotes || hv.panel == HcPanel::WaitHouse;
    char text[1024];
    switch (hv.panel) {
        case HcPanel::Ask:       _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::HC_ASK), hv.px_row, hv.px_row, hv.px_row); break;
        case HcPanel::WaitVotes: _snprintf_s(text, sizeof(text), _TRUNCATE, "%s", tr(Tx::HC_WAIT_VOTES)); break;
        case HcPanel::WaitHouse: _snprintf_s(text, sizeof(text), _TRUNCATE, "%s", tr(Tx::HC_WAIT_HOUSE)); break;
        case HcPanel::Voted:     _snprintf_s(text, sizeof(text), _TRUNCATE, tr(Tx::HC_VOTED), hv.px_row); break;
        case HcPanel::GearWait:  _snprintf_s(text, sizeof(text), _TRUNCATE, "%s", tr(Tx::HC_GEAR_WAIT)); break;
        default: text[0] = 0; break;
    }
    const float W = ask ? 880.0f : 760.0f;
    const float text_h = k > 0 ? wrap_text(nullptr, 0, 0, (W - 100) * k, 25 * k, 36 * k, kInk, text) / k : 0;
    const float H = 84 + text_h + (button ? 120.0f : 36.0f);
    const ImVec2 a = ask ? P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f - 40) : P(v, (kStageW - W) * 0.5f, 96);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_housecoop", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(32), 36 * k, kInk, tr(Tx::HC_TITLE));
    wrap_text(dl, X(50), Y(84), (W - 100) * k, 25 * k, 36 * k, kInk, text);
    if (ask) {
        if (paper_button("hc_yes", ImVec2(X(60), Y(H - 92)), ImVec2(X(W / 2 - 10), Y(H - 34)), tr(Tx::HC_YES), 30 * k, true, Tone::Good)) housecoop_answer(true);
        if (paper_button("hc_no", ImVec2(X(W / 2 + 10), Y(H - 92)), ImVec2(X(W - 60), Y(H - 34)), tr(Tx::HC_NO), 30 * k, true, Tone::Danger)) housecoop_answer(false);
    } else if (button) {
        if (paper_button("hc_solo", ImVec2(X(W / 2 - 150), Y(H - 84)), ImVec2(X(W / 2 + 150), Y(H - 30)), tr(Tx::HC_SOLO), 28 * k, true, Tone::Danger)) housecoop_fight_alone();
    }
    ImGui::End();
}

// THE AREA QUESTION (0.1.53, mgmp_areagate): the host is about to lead the room into an area another player has not unlocked.
const char* area_name(int area) {
    static const Tx kNames[] = { Tx::AREA_SEWERS, Tx::AREA_JUNKYARD, Tx::AREA_CAVES, Tx::AREA_BONEYARD, Tx::AREA_MEATWORLD, Tx::AREA_DESERT,
                                 Tx::AREA_BUNKER, Tx::AREA_CRATER, Tx::AREA_CORE, Tx::AREA_MOON, Tx::AREA_DIMENSIONX, Tx::AREA_LAB,
                                 Tx::AREA_ICEAGE, Tx::AREA_FUTURE, Tx::AREA_JURASSIC, Tx::AREA_THEEND, Tx::AREA_ENDOFTIME };   // mgmp_areagate's kAreas order
    return area >= 0 && area < (int)(sizeof(kNames) / sizeof(kNames[0])) ? tr(kNames[area]) : "?";
}
void draw_area_gate(const View& v) {
    AreaGateView av;
    if (!areagate_view(av)) { g.ag_dont = false; return; }
    const float k = v.k;
    char who[240] = {}; size_t w = 0;
    for (uint8_t i = 0; i < av.n; ++i) {
        const int n = _snprintf_s(who + w, sizeof(who) - w, _TRUNCATE, "%s%s", i ? ", " : "", av.names[i]);
        if (n > 0) w += (size_t)n;
    }
    char detail[512];
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, tr(Tx::AG_DETAIL), area_name(av.area), who);
    const float W = 900;
    const float text_w = (W - 120) * k;
    const float h1 = k > 0 ? wrap_text(nullptr, 0, 0, text_w, 27 * k, 39 * k, kInk, tr(Tx::AG_TEXT)) / k : 0;
    const float h2 = k > 0 ? wrap_text(nullptr, 0, 0, text_w, 24 * k, 35 * k, kInk, detail) / k : 0;
    const float H = 100 + h1 + 24 + h2 + 40 + 44 + 30 + 100;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f - 30);
    const ImVec2 sz(W * k, H * k);
    if (!modal_begin("##mgmp_area_gate")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(36), 40 * k, kInk, tr(Tx::BETA_TITLE));
    float y = 100;
    wrap_text(dl, X(60), Y(y), text_w, 27 * k, 39 * k, kInk, tr(Tx::AG_TEXT));
    y += h1 + 24;
    wrap_text(dl, X(60), Y(y), text_w, 24 * k, 35 * k, IM_COL32(150, 40, 30, 255), detail);
    y += h2 + 40;

    // "do not ask again in this game"
    const ImVec2 b0(X(60), Y(y)), b1(X(60 + 44), Y(y + 44));
    const char* lab = tr(Tx::AG_DONT_ASK);
    const float lw = text_size(26 * k, lab).x;
    ImGui::SetCursorScreenPos(b0);
    ImGui::PushID("ag_dont");
    const bool clicked = ImGui::InvisibleButton("##c", ImVec2(b1.x - b0.x + 16 * k + lw, b1.y - b0.y));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) g.hovering_button = true;
    if (clicked) g.ag_dont = !g.ag_dont;
    rough_rect(dl, b0, b1, hov ? kPaperHi : IM_COL32(255, 255, 250, 200), kInk, 2.4f, 317, 1.2f);
    if (g.ag_dont) {
        dl->AddLine(ImVec2(b0.x + 9 * k, b0.y + 23 * k), ImVec2(b0.x + 19 * k, b0.y + 34 * k), kInk, 4.5f * k);
        dl->AddLine(ImVec2(b0.x + 19 * k, b0.y + 34 * k), ImVec2(b0.x + 36 * k, b0.y + 10 * k), kInk, 4.5f * k);
    }
    text_at(dl, ImVec2(b1.x + 16 * k, b0.y + 9 * k), 26 * k, kInk, lab);

    const float by0 = H - 96, by1 = H - 34;
    if (paper_button("ag_go", ImVec2(X(60), Y(by0)), ImVec2(X(W / 2 - 14), Y(by1)), tr(Tx::AG_GO), 30 * k, true, Tone::Danger)) {
        areagate_answer(true, g.ag_dont);
        g.ag_dont = false;
    } else if (paper_button("ag_no", ImVec2(X(W / 2 + 14), Y(by0)), ImVec2(X(W - 60), Y(by1)), tr(Tx::CANCEL), 30 * k, true, Tone::Good)) {
        areagate_answer(false, g.ag_dont);
        g.ag_dont = false;
    }
    ImGui::End();
}

// The first-run notice: this is a test release, back your saves up. Shown over the title screen until dismissed;
// "do not show again" writes ui.beta_notice = false into mgmp.json.
// ---------------------------------------------------------------------------
// the mod settings window (title screen)
// ---------------------------------------------------------------------------
void draw_settings_window(const View& v) {
    // Laid out top to bottom: each part starts below the text of the one before (the notes wrap to a different number of lines per language).
    const float W = 1200, H = 1060;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * v.k, H * v.k);
    if (!overlay_window_begin("##mgmp_settings", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    const float k = v.k;
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    auto stage_y = [&](float screen_y) { return k > 0 ? (screen_y - a.y) / k : 0.0f; };
    text_c(dl, X(W / 2), Y(40), 48 * k, kInk, tr(Tx::MENU_SETTINGS));

    // Each setting: a box and its words, and a note under it. Not while a room is open -- the HOST's setting is the room's (its HELLO carries it).
    // An experimental one carries the warning in amber after its words (or on its own line when they do not fit together). Returns the y after it.
    const bool locked = in_room() || net_active();
    bool clicked = false;
    auto box = [&](const char* id, float y, bool on, const char* lab, const char* note, uint32_t seed, bool experimental) -> float {
        const ImVec2 b0(X(70), Y(y)), b1(X(70 + 48), Y(y + 48));
        const float lw = text_size(30 * k, lab).x;
        ImGui::SetCursorScreenPos(b0);
        ImGui::PushID(id);
        clicked = ImGui::InvisibleButton("##c", ImVec2(b1.x - b0.x + 18 * k + lw, b1.y - b0.y)) && !locked;
        const bool hov = ImGui::IsItemHovered() && !locked;
        ImGui::PopID();
        if (hov) g.hovering_button = true;
        rough_rect(dl, b0, b1, hov ? kPaperHi : IM_COL32(255, 255, 250, locked ? 120 : 200), kInk, 2.4f, seed, 1.2f);
        if (on) {
            dl->AddLine(ImVec2(b0.x + 10 * k, b0.y + 25 * k), ImVec2(b0.x + 21 * k, b0.y + 37 * k), locked ? kGrey : kInk, 5 * k);
            dl->AddLine(ImVec2(b0.x + 21 * k, b0.y + 37 * k), ImVec2(b0.x + 39 * k, b0.y + 11 * k), locked ? kGrey : kInk, 5 * k);
        }
        text_at(dl, ImVec2(b1.x + 18 * k, b0.y + 8 * k), 30 * k, locked ? kGrey : kInk, lab);
        float note_y = y + 62;
        if (experimental) {
            const char* warn = tr(Tx::SET_EXP_WARN);
            const float wx = b1.x + 18 * k + lw + 20 * k;
            if (wx + text_size(22 * k, warn).x <= X(W - 50)) text_at(dl, ImVec2(wx, b0.y + 14 * k), 22 * k, kAmber, warn);
            else { text_at(dl, ImVec2(X(70), Y(y + 60)), 22 * k, kAmber, warn); note_y += 32; }
        }
        return stage_y(wrap_text(dl, X(70), Y(note_y), (W - 140) * k, 22 * k, 29 * k, kInkSoft, note)) + 18;
    };
    float y = 96;
    {   // THE ENEMY MULTIPLIER (2026-10-19): health and armor of every enemy in a room, x2 .. x8 in steps of 0.5 -- a slider; the file is written when it is let go
        static bool s_drag = false;
        const uint8_t cur = config().enemy_scale_half;
        char head[160];
        _snprintf_s(head, sizeof(head), _TRUNCATE, "%s:  x%g", tr(Tx::SET_ENEMY_SCALE), cur / 2.0);
        text_at(dl, ImVec2(X(70), Y(y + 4)), 30 * k, locked ? kGrey : kInk, head);
        const float tx0 = 100, tx1 = W - 100, ty = y + 70;          // the track
        const int steps = 12;                                       // 4 .. 16 halves
        dl->AddLine(ImVec2(X(tx0), Y(ty)), ImVec2(X(tx1), Y(ty)), locked ? kGrey : kInk, 3 * k);
        for (int i = 0; i <= steps; ++i) {
            const float x = tx0 + (tx1 - tx0) * i / steps;
            const bool whole = i % 2 == 0;
            dl->AddLine(ImVec2(X(x), Y(ty - (whole ? 10 : 6))), ImVec2(X(x), Y(ty + (whole ? 10 : 6))), locked ? kGrey : kInkSoft, 2 * k);
            if (whole) { char t[8]; _snprintf_s(t, sizeof(t), _TRUNCATE, "%d", 2 + i / 2); text_c(dl, X(x), Y(ty + 28), 20 * k, kInkSoft, t); }
        }
        ImGui::SetCursorScreenPos(ImVec2(X(tx0 - 20), Y(ty - 24)));
        ImGui::InvisibleButton("##enemy_scale", ImVec2((tx1 - tx0 + 40) * k, 48 * k));
        const bool hov = ImGui::IsItemHovered() && !locked;
        if (hov) g.hovering_button = true;
        if (ImGui::IsItemActive() && !locked && k > 0) {
            float f = (ImGui::GetIO().MousePos.x - X(tx0)) / ((tx1 - tx0) * k);
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            const uint8_t want = (uint8_t)(4 + (int)(f * steps + 0.5f));
            if (want != config().enemy_scale_half) config_set_enemy_scale_half(want, false);
            s_drag = true;
        } else if (s_drag) {
            s_drag = false;
            const bool saved = config_set_enemy_scale_half(config().enemy_scale_half, true);
            log_line("MENU", "ui.enemy_scale = %.1f (%s)", config().enemy_scale_half / 2.0, saved ? "mgmp.json updated" : "could NOT write mgmp.json -- only for this run");
        }
        const uint8_t now_half = config().enemy_scale_half;
        const float kx = tx0 + (tx1 - tx0) * (now_half - 4) / (float)steps;
        rough_rect(dl, ImVec2(X(kx - 12), Y(ty - 18)), ImVec2(X(kx + 12), Y(ty + 18)), hov || s_drag ? kPaperHi : IM_COL32(255, 255, 250, locked ? 120 : 230),
                   locked ? kGrey : kInk, 2.4f, 359, 1.2f);
        y = stage_y(wrap_text(dl, X(70), Y(ty + 50), (W - 140) * k, 22 * k, 29 * k, kInkSoft, tr(Tx::SET_ENEMY_SCALE_NOTE))) + 18;
    }
    // THE SETTINGS RUN FROM VERIFIED TO EXPERIMENTAL (the rule since 0.1.106): a feature that has been tested enough is listed here, ahead of the experimental section,
    // with no warning (and usually on by default); the experimental section holds only what is still being tried out. A feature that is verified later moves up.
    {   // the shared treasure chest (mgmp_treasure)
        const bool on = config().exp_treasure;
        y = box("set_exp_treasure", y, on, tr(Tx::SET_EXP_TREASURE), tr(Tx::SET_EXP_TREASURE_NOTE), 347, false);
        if (clicked) {
            const bool saved = config_set_exp_treasure(!on);
            log_line("MENU", "ui.exp_treasure = %s (%s)", !on ? "true" : "false", saved ? "mgmp.json updated" : "could NOT write mgmp.json -- only for this run");
        }
    }
    {   // each player's own random events (mgmp_indepevt)
        const bool on = config().exp_indep_events;
        y = box("set_exp_indep_events", y, on, tr(Tx::SET_EXP_INDEP_EVENTS), tr(Tx::SET_EXP_INDEP_EVENTS_NOTE), 359, false);
        if (clicked) {
            const bool saved = config_set_exp_indep_events(!on);
            log_line("MENU", "ui.exp_indep_events = %s (%s)", !on ? "true" : "false", saved ? "mgmp.json updated" : "could NOT write mgmp.json -- only for this run");
        }
    }
    // THE EXPERIMENTAL FEATURES (2026-10-18): a section of their own, one box each, all off by default. A new experimental feature gets its
    // own box here (and its own ui.exp_* key and HELLO rules bit).
    {
        dl->AddLine(ImVec2(X(60), Y(y)), ImVec2(X(W - 60), Y(y)), kInkSoft, 2 * k);
        text_at(dl, ImVec2(X(70), Y(y + 14)), 36 * k, kInk, tr(Tx::SET_EXP_SECTION));
        y = stage_y(wrap_text(dl, X(70), Y(y + 60), (W - 140) * k, 22 * k, 29 * k, kInkSoft, tr(Tx::SET_EXP_SECTION_NOTE))) + 16;
    }
    {   // helping the one player whose house boss is today (mgmp_housecoop)
        const bool on = config().exp_house_coop;
        y = box("set_exp_house_coop", y, on, tr(Tx::SET_EXP_HOUSE_COOP), tr(Tx::SET_EXP_HOUSE_COOP_NOTE), 353, true);
        if (clicked) {
            const bool saved = config_set_exp_house_coop(!on);
            log_line("MENU", "ui.exp_house_coop = %s (%s)", !on ? "true" : "false", saved ? "mgmp.json updated" : "could NOT write mgmp.json -- only for this run");
        }
    }
    if (locked) text_at(dl, ImVec2(X(70), Y(H - 112)), 24 * k, kAmber, tr(Tx::SET_IN_ROOM));

    if (paper_button("set_back", ImVec2(X(W / 2 - 150), Y(H - 78)), ImVec2(X(W / 2 + 150), Y(H - 22)), tr(Tx::BACK), 32 * k))
        g.win = Win::None;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// 0.1.95 THE MAP: EVERY PLAYER'S PICK OF THE NEXT NODE (mgmp_mapvote). A small paw in the player's colour at a corner of the node it picked: P1 top left, P2 top right,
// P3 bottom left, P4 bottom right (the room list's order, host first -- the colours of the drawing board). The picture is the game's own paw, the one it shows
// beside a menu entry under the mouse (traced for the mod's own title entries: draw_paw), outlined and filled in the player's colour.
// Nothing here takes the mouse: the click is the game's (and swallowed by mgmp_mapvote).
// ---------------------------------------------------------------------------
void draw_mapvote(const View& v) {
    if (page_self() != PageState::InGame) return;
    { const ImVec2 mp = ImGui::GetIO().MousePos; if (mp.x > -1e6f && mp.y > -1e6f) mapvote_note_mouse((mp.x - v.org.x) / v.k, (mp.y - v.org.y) / v.k); }
    // The paws show while the map itself ticks (MapScreen::update, seen by mapvote_tick) -- that is what a screen over the map (the level-up page: measured, the map
    // stops) or a battle takes away. NOT draw_visible_layer(): after the chapter boss it said "battle" for good (the fight's last unit, a dove, was still standing,
    // so the lockstep never left the battle) and 0.1.103 hid the paws on the whole next floor.
    {
        static int s_state = -1, s_lines = 0;
        const bool map_on = mapvote_map_ticking(), lvl_tick = mapvote_level_ticking(), lvl_btn = page_button_live("Levelup_");
        const bool pause = page_button_live("Button_PauseMenu_");
        const int layer = draw_visible_layer(nullptr);
        const int st = (map_on ? 1 : 0) | (lvl_tick ? 2 : 0) | (lvl_btn ? 4 : 0) | (pause ? 8 : 0) | ((layer + 1) << 4);
        if (st != s_state && s_lines < 80) {
            s_state = st; ++s_lines;
            log_line("MAPVOTE", "overlay: map ticking %s, level-up screen ticking %s, level-up buttons %s, pause menu %s, board layer %d -> paws %s", map_on ? "yes" : "NO", lvl_tick ? "yes" : "no",
                     lvl_btn ? "up" : "not up", pause ? "up" : "not up", layer, map_on && !pause ? "shown" : "hidden");
        }
        if (!map_on || pause) return;
    }
    MapVoteMark marks[kMaxPeers];
    const int n = mapvote_marks(marks, kMaxPeers);
    unsigned have = 0, total = 0;
    bool mine = false;
    const bool counts = mapvote_counts(have, total, mine);
    if (!n && !counts) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float k = v.k;

    for (int i = 0; i < n; ++i) {
        const MapVoteMark& m = marks[i];
        int row = room_row_of_peer(m.peer);
        if (row < 0) row = m.peer;
        const int corner = row & 3;
        const float dx = (corner & 1) ? 1.0f : -1.0f, dy = (corner & 2) ? 1.0f : -1.0f;
        const float px = m.cx + dx * m.half * 0.72f, py = m.cy + dy * m.half * 0.72f;
        const ImU32 col = board_color(m.peer);
        float w = m.half * 0.45f;                                   // the paw's width in stage units: about half what it was (0.1.96), a fifth of the node
        w = w < 17.0f ? 17.0f : (w > 36.0f ? 36.0f : w);
        const ImVec2 c = P(v, px, py);
        draw_paw(dl, c, w * k * 1.2f, kInk, m.angle);              // the dark outline: the same paw, a little larger
        draw_paw(dl, c, w * k, col, m.angle);
    }
    if (counts) {
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::MV_STATUS), have, total);
        text_c(dl, P(v, 960, 0).x, P(v, 0, 1000).y, 26 * k, kCreamHi, line, true);
    }
}

// 0.1.87: an item's description (the game's own text, made plain by treasure_desc_plain) in a paper box whose top edge is centred on (cx, top), in stage
// units; kept on the stage. A line marked '\x01' is the game's small print: smaller and grey.
void draw_item_desc(const View& v, float cx, float top, const char* text, uint32_t seed) {
    if (!text || !text[0]) return;
    const float k = v.k;
    const float W = 560, pad = 22;
    const float fs = 24 * k, lh = 32 * k, fs_s = 20 * k, lh_s = 27 * k;
    const float maxw = (W - 2 * pad) * k;
    // the lines, and the height they take
    std::vector<std::pair<std::string, bool>> lines;
    for (const char* s = text; *s;) {
        const char* e = strchr(s, '\n');
        std::string ln = e ? std::string(s, e) : std::string(s);
        const bool fine_print = !ln.empty() && ln[0] == '\x01';
        if (fine_print) ln.erase(0, 1);
        lines.emplace_back(ln, fine_print);
        if (!e) break;
        s = e + 1;
    }
    float h = 0;
    for (auto& l : lines) h += l.first.empty() ? (l.second ? lh_s : lh) * 0.5f : wrap_text(nullptr, 0, 0, maxw, l.second ? fs_s : fs, l.second ? lh_s : lh, kInk, l.first.c_str());
    const float H = h / k + 2 * pad - 6;
    float x0 = cx - W * 0.5f;
    if (x0 < 20) x0 = 20;
    if (x0 + W > kStageW - 20) x0 = kStageW - 20 - W;
    float y0 = top;
    if (y0 + H > kStageH - 20) y0 = kStageH - 20 - H;
    const ImVec2 a = P(v, x0, y0);
    if (!overlay_window_begin("##mgmp_item_desc", a, ImVec2(W * k, H * k), false, false)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    rough_rect(dl, a, ImVec2(a.x + W * k, a.y + H * k), IM_COL32(244, 242, 234, 240), kInk, 1.8f, 970 + seed, 1.0f);
    float y = a.y + (pad - 3) * k;
    for (auto& l : lines) {
        if (l.first.empty()) { y += (l.second ? lh_s : lh) * 0.5f; continue; }
        y = wrap_text(dl, a.x + pad * k, y, maxw, l.second ? fs_s : fs, l.second ? lh_s : lh, l.second ? kGrey : kInk, l.first.c_str());
    }
    ImGui::End();
}

// EXPERIMENTAL: the shared treasure chest, its items shown by the game in the treasure room itself (mgmp_treasure: a row of the room's own item
// clips). Here only what the game cannot draw: an invisible hit area over each item (hover lifts it and names it in the room's banner, a click
// asks for it), a tag under an item that is taken or being asked for, and "take nothing".
void draw_treasure_row(const View& v, const TreasureView& tv) {
    const float k = v.k;
    const float h = tv.half;
    bool any = false;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (uint8_t i = 0; i < tv.n; ++i) {
        if (!tv.shown[i]) continue;
        if (!any) { x0 = x1 = tv.cx[i]; y0 = y1 = tv.cy[i]; any = true; }
        x0 = tv.cx[i] < x0 ? tv.cx[i] : x0; x1 = tv.cx[i] > x1 ? tv.cx[i] : x1;
        y0 = tv.cy[i] < y0 ? tv.cy[i] : y0; y1 = tv.cy[i] > y1 ? tv.cy[i] : y1;
    }
    if (!any || h <= 0) return;
    // One window over the row; inside it, one hit area per item still in the row, exactly over its picture (mgmp_treasure works the
    // centres out every frame from the clips themselves). Every player takes one: there is no "take nothing".
    const float wx0 = x0 - h - 20, wx1 = x1 + h + 20, wy0 = y0 - h - 20, wy1 = y1 + h + (tv.mismatch ? 110.0f : 70.0f);
    const ImVec2 a = P(v, wx0, wy0);
    const ImVec2 sz((wx1 - wx0) * k, (wy1 - wy0) * k);
    if (!overlay_window_begin("##mgmp_treasure_row", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int hovered = -1;
    for (uint8_t i = 0; i < tv.n; ++i) {
        if (!tv.shown[i]) continue;
        const ImVec2 r0 = P(v, tv.cx[i] - h, tv.cy[i] - h), r1 = P(v, tv.cx[i] + h, tv.cy[i] + h);
        char id[16];
        _snprintf_s(id, sizeof(id), _TRUNCATE, "##tr_slot%u", (unsigned)i);
        ImGui::SetCursorScreenPos(r0);
        const bool can = tv.pending < 0 && !tv.busy;
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(r1.x - r0.x, r1.y - r0.y)) && can;
        if (ImGui::IsItemHovered()) { hovered = i; if (can) g.hovering_button = true; }
        if (clicked) treasure_take(i);
        if (tv.pending == (int)i) {   // asked for, waiting for the host's answer
            const char* tag = tr(Tx::TP_WAIT);
            const float fs = 24 * k;
            const ImVec2 ts = text_size(fs, tag);
            const ImVec2 c = P(v, tv.cx[i], tv.cy[i] + h + 26);
            const ImVec2 t0(c.x - ts.x * 0.5f - 12 * k, c.y - ts.y * 0.5f - 6 * k), t1(c.x + ts.x * 0.5f + 12 * k, c.y + ts.y * 0.5f + 6 * k);
            rough_rect(dl, t0, t1, IM_COL32(236, 236, 228, 235), kInk, 1.8f, 950 + i, 1.0f);
            text_at(dl, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), fs, kInk, tag);
        }
    }
    treasure_hover(hovered);
    if (tv.mismatch) { const ImVec2 m = P(v, (x0 + x1) * 0.5f, y1 + h + 70); text_c(dl, m.x, m.y, 20 * k, kAmber, tr(Tx::TP_MISMATCH)); }
    ImGui::End();
    // 0.1.87: what the hovered item does, under it (below its "waiting" tag when it has one)
    if (hovered >= 0) draw_item_desc(v, tv.cx[hovered], tv.cy[hovered] + h + (tv.pending == hovered ? 56.0f : 14.0f), tv.desc[hovered], (uint32_t)hovered);
}

// EXPERIMENTAL: the shared treasure chest (mgmp_treasure) -- the panel, when the room's own row could not be made. The items every player sees,
// who took which, and a button to take one or nothing. On the right of the treasure room, clear of its chest.
void draw_treasure_pick(const View& v) {
    TreasureView tv;
    if (!treasure_view(tv) || tv.building) return;
    if (tv.native) { draw_treasure_row(v, tv); return; }
    const float k = v.k;
    const float W = 640;
    const float H = 130 + tv.n * 78.0f + (tv.mismatch ? 40.0f : 0.0f) + 30;
    const ImVec2 a = P(v, kStageW - W - 60, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_treasure", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    text_c(dl, X(W / 2), Y(30), 42 * k, kInk, tr(Tx::TP_TITLE));
    float y = 100;
    int hover_row = -1;
    float hover_y = 0;
    if (tv.mismatch) { text_c(dl, X(W / 2), Y(y), 20 * k, kAmber, tr(Tx::TP_MISMATCH)); y += 40; }
    for (uint8_t i = 0; i < tv.n; ++i) {
        const bool open_slot = !tv.taker[i][0];
        rough_rect(dl, ImVec2(X(40), Y(y)), ImVec2(X(W - 40), Y(y + 64)), open_slot ? IM_COL32(250, 248, 238, 200) : IM_COL32(210, 208, 200, 160),
                   IM_COL32(30, 30, 30, 120), 1.6f, 900 + i, 1.0f);
        // the name, set smaller when it would run into the button
        float fs = 28 * k;
        const float room = (W - 40 - 200 - 60) * k;
        const float tw = text_size(fs, tv.name[i]).x;
        if (tw > room && tw > 0) fs *= room / tw;
        text_at(dl, ImVec2(X(60), Y(y + 32) - fs * 0.6f), fs, open_slot ? kInk : kGrey, tv.name[i]);
        char id[16];
        _snprintf_s(id, sizeof(id), _TRUNCATE, "tp_take%u", (unsigned)i);
        if (open_slot) {
            const bool waiting = tv.pending == (int)i;
            const bool can = tv.pending < 0 && !tv.busy;
            if (paper_button(id, ImVec2(X(W - 240), Y(y + 8)), ImVec2(X(W - 56), Y(y + 56)), waiting ? tr(Tx::TP_WAIT) : tr(Tx::TP_TAKE), 26 * k, can, Tone::Good))
                treasure_take(i);
        } else {
            char who[96];
            if (tv.mine[i]) _snprintf_s(who, sizeof(who), _TRUNCATE, "%s", tr(Tx::TP_MINE));
            else            _snprintf_s(who, sizeof(who), _TRUNCATE, tr(Tx::TP_TAKEN), tv.taker[i]);
            float ws = 22 * k;
            const float ww = text_size(ws, who).x, wroom = 200 * k;
            if (ww > wroom && ww > 0) ws *= wroom / ww;
            text_r(dl, X(W - 60), Y(y + 32) - ws * 0.6f, ws, tv.mine[i] ? kGreen : kGrey, who);
        }
        if (ImGui::IsMouseHoveringRect(ImVec2(X(40), Y(y)), ImVec2(X(W - 40), Y(y + 64)), false)) hover_row = i, hover_y = y;
        y += 78;
    }
    ImGui::End();
    // 0.1.87: what the item under the mouse does, left of the panel at its row
    const float panel_x = kStageW - W - 60, panel_y = (kStageH - H) * 0.5f;
    if (hover_row >= 0) draw_item_desc(v, panel_x - 20 - 280, panel_y + hover_y, tv.desc[hover_row], (uint32_t)hover_row);
}

void draw_beta_notice(const View& v) {
    const float k = v.k;
    const float W = 900, H = 880;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!modal_begin("##mgmp_beta")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(40), 44 * k, kInk, tr(Tx::BETA_TITLE));
    wrap_text(dl, X(70), Y(112), (W - 140) * k, 26 * k, 37 * k, kInk, tr(Tx::BETA_TEXT));

    // the "do not show again" box
    const ImVec2 b0(X(70), Y(H - 150)), b1(X(70 + 44), Y(H - 150 + 44));
    const char* lab = tr(Tx::BETA_DONT_SHOW);
    const float lw = text_size(26 * k, lab).x;
    ImGui::SetCursorScreenPos(b0);
    ImGui::PushID("bn_dont");
    const bool clicked = ImGui::InvisibleButton("##c", ImVec2(b1.x - b0.x + 16 * k + lw, b1.y - b0.y));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) g.hovering_button = true;
    if (clicked) g.beta_dont = !g.beta_dont;
    rough_rect(dl, b0, b1, hov ? kPaperHi : IM_COL32(255, 255, 250, 200), kInk, 2.4f, 313, 1.2f);
    if (g.beta_dont) {
        dl->AddLine(ImVec2(b0.x + 9 * k, b0.y + 23 * k), ImVec2(b0.x + 19 * k, b0.y + 34 * k), kInk, 4.5f * k);
        dl->AddLine(ImVec2(b0.x + 19 * k, b0.y + 34 * k), ImVec2(b0.x + 36 * k, b0.y + 10 * k), kInk, 4.5f * k);
    }
    text_at(dl, ImVec2(b1.x + 16 * k, b0.y + 9 * k), 26 * k, kInk, lab);

    if (paper_button("bn_ok", ImVec2(X(W / 2 - 130), Y(H - 84)), ImVec2(X(W / 2 + 130), Y(H - 24)), tr(Tx::OK), 30 * k, true, Tone::Good)) {
        g.beta_closed = true;
        if (g.beta_dont) {
            const bool saved = config_set_beta_notice(false);
            log_line("MENU", "beta notice dismissed, not shown again (%s)", saved ? "mgmp.json updated" : "could NOT write mgmp.json -- only for this run");
        } else {
            log_line("MENU", "beta notice dismissed");
        }
    }
    ImGui::End();
}

// The password prompt for a locked room. Enter or Join sends the join with the typed password.
void draw_pw_prompt(const View& v) {
    const float k = v.k;
    const float W = 760, H = 400;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!modal_begin("##mgmp_pw")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(36), 40 * k, kInk, tr(Tx::PW_TITLE));
    draw_lock(dl, ImVec2(X(W / 2 - 24 - text_size(28 * k, g.pw_name).x / (2 * k)), Y(104)), 30 * k, kInk);
    text_c(dl, X(W / 2 + 14), Y(88), 28 * k, kInkSoft, g.pw_name);

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);   // typing starts at once
    const bool enter = paper_input("##joinpw", ImVec2(X(70), Y(150)), ImVec2(X(W - 70), Y(214)), g.pw_buf, sizeof(g.pw_buf), tr(Tx::ROOM_PW), 30 * k,
                                   ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    if (g.pw_wrong) text_c(dl, X(W / 2), Y(232), 26 * k, kRed, tr(Tx::PW_WRONG));

    const bool busy = signal_request_pending();
    const bool can = g.pw_buf[0] && !busy;
    const bool go = paper_button("pw_join", ImVec2(X(70), Y(H - 100)), ImVec2(X(W / 2 - 10), Y(H - 36)), tr(Tx::JOIN), 30 * k, can, Tone::Good);
    if ((go || (enter && can))) {
        signal_request_join(g.pw_id, g.pw_buf);
        g.pw_sent = true; g.pw_wrong = false;
        g.pw_ask = false;                      // reopened by a refusal; closed for good by joining
        log_line("MENU", "join requested: room %s (%s) with a password", g.pw_id, g.pw_name);
    }
    if (paper_button("pw_cancel", ImVec2(X(W / 2 + 10), Y(H - 100)), ImVec2(X(W - 70), Y(H - 36)), tr(Tx::CANCEL), 30 * k)) {
        g.pw_ask = false; g.pw_wrong = false; g.pw_buf[0] = 0;
    }
    ImGui::End();
}

// The notice before the project's small server is chosen as the carrier.
void draw_relay_warn(const View& v) {
    const float k = v.k;
    const float W = 780, H = 420;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!modal_begin("##mgmp_relaywarn")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(36), 40 * k, kInk, tr(Tx::TR_RELAY));
    wrap_text(dl, X(60), Y(110), (W - 120) * k, 28 * k, 40 * k, kAmber, tr(Tx::TR_RELAY_WARN));
    if (paper_button("rw_ok", ImVec2(X(60), Y(H - 100)), ImVec2(X(W / 2 - 10), Y(H - 36)), tr(Tx::TR_RELAY_OK), 28 * k, true, Tone::Good)) {
        g.transport = 3; g.relay_warn = false;
        log_line("MENU", "server relay chosen after the low-spec notice");
    }
    if (paper_button("rw_cancel", ImVec2(X(W / 2 + 10), Y(H - 100)), ImVec2(X(W - 60), Y(H - 36)), tr(Tx::CANCEL), 30 * k)) g.relay_warn = false;
    ImGui::End();
}

// "68 KB" / "1.2 MB" for the preview line.
void format_size(char* out, size_t cap, uint32_t n) {
    if (n < 1024) _snprintf_s(out, cap, _TRUNCATE, "%u B", (unsigned)n);
    else if (n < (1u << 20)) _snprintf_s(out, cap, _TRUNCATE, "%u KB", (unsigned)(n / 1024));
    else _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", n / 1048576.0);
}

// The files an upload carries: this run's trace log, and the boot log beside it when there is one.
int collect_log_files(wchar_t files[kLogUploadMaxFiles][MAX_PATH]) {
    int n = 0;
    if (g.log_prev) {       // the run that ended badly: its log and its crash dump
        LogUploadRequest pr;
        if (prevrun_request(pr)) { for (int i = 0; i < pr.nfiles; ++i) wcsncpy_s(files[n++], MAX_PATH, pr.files[i], _TRUNCATE); return n; }
        g.log_prev = false;
    }
    wchar_t cur[MAX_PATH] = {};
    if (!log_current_path(cur, MAX_PATH)) return 0;
    wcsncpy_s(files[n++], MAX_PATH, cur, _TRUNCATE);
    wchar_t boot[MAX_PATH] = {};
    wcsncpy_s(boot, cur, _TRUNCATE);
    if (wchar_t* slash = wcsrchr(boot, L'\\')) {
        *(slash + 1) = 0;
        wcsncat_s(boot, L"mgmp_boot.log", _TRUNCATE);
        if (GetFileAttributesW(boot) != INVALID_FILE_ATTRIBUTES && n < kLogUploadMaxFiles) wcsncpy_s(files[n++], MAX_PATH, boot, _TRUNCATE);
    }
    return n;
}

// The last upload's result as one line (both F2 pages).
void log_status_line(const LogUploadStatus& st, char* msg, size_t cap, ImU32& col) {
    msg[0] = 0; col = kInkSoft;
    if (st.state == LogUploadState::Working) { _snprintf_s(msg, cap, _TRUNCATE, tr(Tx::LOG_BUSY), (unsigned)st.percent); return; }
    if (st.state == LogUploadState::Done) { _snprintf_s(msg, cap, _TRUNCATE, tr(Tx::LOG_DONE), st.detail); col = kGreen; return; }
    if (st.state != LogUploadState::Failed) return;
    col = kRed;
    switch (st.error) {
        case LogUploadError::NoFile:    _snprintf_s(msg, cap, _TRUNCATE, "%s", tr(Tx::LOG_FAIL_NOFILE)); break;
        case LogUploadError::Connect:   _snprintf_s(msg, cap, _TRUNCATE, "%s", tr(Tx::LOG_FAIL_CONNECT)); break;
        case LogUploadError::ServerOld: _snprintf_s(msg, cap, _TRUNCATE, "%s", tr(Tx::LOG_FAIL_OLD)); break;
        case LogUploadError::Refused:   _snprintf_s(msg, cap, _TRUNCATE, tr(Tx::LOG_FAIL_REFUSED), st.detail); break;
        default:                        _snprintf_s(msg, cap, _TRUNCATE, "%s", tr(Tx::LOG_FAIL_LOST)); break;
    }
}

// A request addressed like F2's own upload: the server in the window, this player's name, the description typed on the first page.
void log_fill_request(LogUploadRequest& req) {
    char host[64]; uint16_t port = config().signal_port;
    split_addr(g.log_addr, host, sizeof(host), port);
    strncpy_s(req.addr, sizeof(req.addr), host, _TRUNCATE);
    req.port = port;
    const char* who = g.pname[0] ? g.pname : (signal_name()[0] ? signal_name() : "player");
    strncpy_s(req.player, sizeof(req.player), who, _TRUNCATE);
}

// "10-24 19:36:41" (the date only when `with_date`)
void fmt_stamp(char* out, size_t cap, const LogTimeStamp& t, bool with_date) {
    if (with_date) _snprintf_s(out, cap, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    else           _snprintf_s(out, cap, _TRUNCATE, "%02d:%02d:%02d", t.hour, t.minute, t.second);
}
long long stamp_seconds(const LogTimeStamp& t) {   // for a difference only (days since a fixed origin, by the civil-from-days rule)
    int y = t.year, m = t.month;
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + (long long)doe;
    return days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

// 0.1.93: F2's second page -- the logs of the earlier runs (the folder keeps 5 besides this run's), each with when that game started and stopped, how it
// ended, and a button that uploads it (and its crash dump) with the description typed on the first page.
void draw_log_history(const View& v) {
    const ULONGLONG now_ms = GetTickCount64();
    if (!g.hist_at || now_ms - g.hist_at > 2000) { log_flush(); g.hist_n = log_history(g.hist, kLogHistoryKeep); g.hist_at = now_ms; }
    const float k = v.k;
    const float W = 980, rowh = 104;
    const float H = 250 + (g.hist_n ? g.hist_n : 1) * rowh + 170;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_loghist", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(36), 44 * k, kInk, tr(Tx::LOG_HIST_TITLE));
    wrap_text(dl, X(60), Y(100), (W - 120) * k, 23 * k, 32 * k, kInkSoft, tr(Tx::LOG_HIST_HINT));

    const LogUploadStatus st = logupload_status();
    const bool working = st.state == LogUploadState::Working;
    float y = 200;
    if (!g.hist_n) { text_at(dl, ImVec2(X(60), Y(y + 30)), 26 * k, kGrey, tr(Tx::LOG_HIST_NONE)); y += rowh; }
    for (int i = 0; i < g.hist_n; ++i) {
        const LogHistoryEntry& e = g.hist[i];
        rough_rect(dl, ImVec2(X(40), Y(y)), ImVec2(X(W - 40), Y(y + rowh - 12)), IM_COL32(250, 248, 238, 200), IM_COL32(30, 30, 30, 120), 1.6f, 1200 + i, 1.0f);
        // when: the start (with its date), the end (with its date when it is another day), and how long
        char s0[32] = "?", s1[32] = "?", dur[48] = "", line[200];
        if (e.have_start) fmt_stamp(s0, sizeof(s0), e.start, true);
        if (e.have_end) fmt_stamp(s1, sizeof(s1), e.end, !e.have_start || e.end.year != e.start.year || e.end.month != e.start.month || e.end.day != e.start.day);
        if (e.have_start && e.have_end) {
            const long long secs = stamp_seconds(e.end) - stamp_seconds(e.start);
            if (secs < 60) _snprintf_s(dur, sizeof(dur), _TRUNCATE, "%s", tr(Tx::LOG_HIST_LT1));
            else if (secs < 3600) _snprintf_s(dur, sizeof(dur), _TRUNCATE, tr(Tx::LOG_HIST_MIN), (int)(secs / 60));
            else _snprintf_s(dur, sizeof(dur), _TRUNCATE, tr(Tx::LOG_HIST_HM), (int)(secs / 3600), (int)(secs % 3600 / 60));
        }
        _snprintf_s(line, sizeof(line), _TRUNCATE, dur[0] ? "%s  ->  %s  (%s)" : "%s  ->  %s", s0, s1, dur);
        text_at(dl, ImVec2(X(64), Y(y + 12)), 26 * k, kInk, line);
        // how it ended, its size, a crash dump
        char size[32]; format_size(size, sizeof(size), (uint32_t)(e.size > 0xFFFFFFFFull ? 0xFFFFFFFFull : e.size));
        const char* how = e.ended == 1 ? tr(Tx::LOG_HIST_CLEAN) : e.ended == 0 ? tr(Tx::LOG_HIST_BAD) : tr(Tx::LOG_HIST_UNKNOWN);
        char info[256];
        _snprintf_s(info, sizeof(info), _TRUNCATE, e.dump[0] ? "%s  \xC2\xB7  %s  \xC2\xB7  %s" : "%s  \xC2\xB7  %s", how, size, tr(Tx::LOG_HIST_DUMP));
        text_at(dl, ImVec2(X(64), Y(y + 52)), 21 * k, e.ended == 0 ? kRed : e.ended == 1 ? kGreen : kGrey, info);
        char id[16]; _snprintf_s(id, sizeof(id), _TRUNCATE, "lh_go%d", i);
        if (paper_button(id, ImVec2(X(W - 230), Y(y + 18)), ImVec2(X(W - 60), Y(y + rowh - 30)), tr(Tx::LOG_HIST_SEND), 26 * k, !working, Tone::Good)) {
            LogUploadRequest req;
            log_fill_request(req);
            wcsncpy_s(req.files[0], MAX_PATH, e.path, _TRUNCATE);
            req.nfiles = 1;
            if (e.dump[0]) { wcsncpy_s(req.files[1], MAX_PATH, e.dump, _TRUNCATE); req.nfiles = 2; }
            _snprintf_s(req.desc, sizeof(req.desc), _TRUNCATE, "[older log: %s -> %s, %s] %s", s0, s1, e.ended == 1 ? "clean exit" : e.ended == 0 ? "did NOT end cleanly" : "end unknown", g.log_desc);
            _snprintf_s(req.info, sizeof(req.info), _TRUNCATE, "proto %u, an older log (%s), role %s, room %s", (unsigned)kProtoVersion, s0,
                        signal_role()[0] ? signal_role() : "-", signal_room()[0] ? signal_room() : "-");
            log_line("MENU", "older log upload to %s:%u: %ls (%llu bytes%s, %s)", req.addr, (unsigned)req.port, e.path, (unsigned long long)e.size, e.dump[0] ? ", with its crash dump" : "",
                     e.ended == 1 ? "clean exit" : e.ended == 0 ? "did not end cleanly" : "end unknown");
            g.hist_sent = i;
            logupload_start(req);
        }
        y += rowh;
    }
    // the result of the last upload
    char msg[320]; ImU32 mcol;
    log_status_line(st, msg, sizeof(msg), mcol);
    if (msg[0]) wrap_text(dl, X(60), Y(y + 10), (W - 120) * k, 25 * k, 32 * k, mcol, msg);
    if (paper_button("lh_back", ImVec2(X(W / 2 - 160), Y(H - 96)), ImVec2(X(W / 2 + 160), Y(H - 36)), tr(Tx::BACK), 30 * k)) { g.log_hist = false; }
    ImGui::End();
}

// F2: upload this run's game log to a server that collects them, with a note about what went wrong.
void draw_log_window(const View& v) {
    ensure_inited();
    if (!g.log_inited) {
        g.log_inited = true;
        strncpy_s(g.log_addr, sizeof(g.log_addr), g.addr[0] ? g.addr : "localhost", _TRUNCATE);
    }
    if (g.log_hist) { draw_log_history(v); return; }      // 0.1.93
    const float k = v.k;
    const float o = g.log_halt ? 300.0f : 0.0f;   // the halt notice (the advice and the auto-finish note) takes this much above the upload part
    const float W = 860, H = 760 + o;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_logup", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    text_c(dl, X(W / 2), Y(36), 46 * k, g.log_halt ? kRed : kInk, tr(g.log_halt ? Tx::HALT_TITLE : (g.log_desync ? Tx::LOG_DESYNC_TITLE : Tx::LOG_TITLE)));
    if (g.log_halt) {                                                                                      // what the players can do now, and what the game does for them meanwhile
        const float ay = wrap_text(dl, X(60), Y(104), (W - 120) * k, 24 * k, 33 * k, kInk, tr(Tx::HALT_AUTOFINISH));     // wrap_text returns the y just below its last line
        wrap_text(dl, X(60), ay + 16 * k, (W - 120) * k, 26 * k, 36 * k, kRed, tr(Tx::HALT_ADVICE));
    }
    wrap_text(dl, X(60), Y(104 + o), (W - 120) * k, 24 * k, 34 * k, g.log_desync ? kAmber : kInkSoft,
              tr(g.log_desync ? Tx::LOG_DESYNC_HINT : (g.log_prev ? Tx::LOG_PREV_HINT : Tx::LOG_HINT)));

    const LogUploadStatus st = logupload_status();
    const bool working = st.state == LogUploadState::Working;

    text_at(dl, ImVec2(X(60), Y(190 + o)), 26 * k, kInk, tr(Tx::LOG_ADDR));
    paper_input("##logaddr", ImVec2(X(290), Y(178 + o)), ImVec2(X(W - 60), Y(236 + o)), g.log_addr, sizeof(g.log_addr), "localhost", 28 * k);

    if (g.log_desync) {
        wrap_text(dl, X(60), Y(270 + o), (W - 120) * k, 22 * k, 30 * k, kGrey, g.log_desync_text);     // what happened, in place of the description
    } else {
        text_at(dl, ImVec2(X(60), Y(262 + o)), 26 * k, kInk, tr(Tx::LOG_DESC));
        paper_multiline("##logdesc", ImVec2(X(60), Y(302 + o)), ImVec2(X(W - 60), Y(500 + o)), g.log_desc, sizeof(g.log_desc), tr(Tx::LOG_DESC_HINT), 26 * k);
    }

    // what will be sent
    wchar_t files[kLogUploadMaxFiles][MAX_PATH] = {};
    const int nfiles = collect_log_files(files);
    if (nfiles > 0) {
        char nm[128] = {};
        const wchar_t* slash = wcsrchr(files[0], L'\\');
        WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : files[0], -1, nm, sizeof(nm) - 1, nullptr, nullptr);
        uint32_t total = 0;
        for (int i = 0; i < nfiles; ++i) total += logupload_file_size(files[i]);
        char sizes[32]; format_size(sizes, sizeof(sizes), total);
        char line[256]; _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::LOG_FILES), nm, sizes);
        text_at(dl, ImVec2(X(60), Y(516 + o)), 22 * k, kGrey, line);
    }
    text_at(dl, ImVec2(X(60), Y(548 + o)), 20 * k, kGrey, tr(Tx::LOG_PRIVACY));

    // the result of the last upload
    char msg[320] = {};
    ImU32 mcol = kInkSoft;
    if (working) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, tr(Tx::LOG_BUSY), (unsigned)st.percent);
        const ImVec2 b0(X(60), Y(592 + o)), b1(X(W - 60), Y(612 + o));
        dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 250, 140), 2.0f);
        dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * (st.percent / 100.0f), b1.y), IM_COL32(120, 170, 120, 220), 2.0f);
        dl->AddRect(b0, b1, kInk, 2.0f, 0, 1.6f);
    } else if (st.state == LogUploadState::Done) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, tr(Tx::LOG_DONE), st.detail);
        mcol = kGreen;
    } else if (st.state == LogUploadState::Failed) {
        mcol = kRed;
        switch (st.error) {
            case LogUploadError::NoFile:    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s", tr(Tx::LOG_FAIL_NOFILE)); break;
            case LogUploadError::Connect:   _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s", tr(Tx::LOG_FAIL_CONNECT)); break;
            case LogUploadError::ServerOld: _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s", tr(Tx::LOG_FAIL_OLD)); break;
            case LogUploadError::Refused:   _snprintf_s(msg, sizeof(msg), _TRUNCATE, tr(Tx::LOG_FAIL_REFUSED), st.detail); break;
            default:                        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s", tr(Tx::LOG_FAIL_LOST)); break;
        }
    }
    if (msg[0]) wrap_text(dl, X(60), Y((working ? 624 : 596) + o), (W - 120) * k, 26 * k, 34 * k, mcol, msg);

    const float bw = (W - 120 - 2 * 16) / 3.0f;            // 0.1.93: three buttons -- upload, an older log, close
    if (paper_button("log_hist", ImVec2(X(60 + bw + 16), Y(H - 100)), ImVec2(X(60 + 2 * bw + 16), Y(H - 36)), tr(Tx::LOG_HIST_BTN), 28 * k, !working)) {
        g.log_hist = true; g.hist_at = 0; g.hist_sent = -1;
        logupload_reset();
    }
    if (paper_button("log_go", ImVec2(X(60), Y(H - 100)), ImVec2(X(60 + bw), Y(H - 36)), tr(Tx::LOG_UPLOAD), 30 * k, !working && nfiles > 0, Tone::Good)) {
        LogUploadRequest req;
        char host[64]; uint16_t port = config().signal_port;
        split_addr(g.log_addr, host, sizeof(host), port);
        strncpy_s(req.addr, sizeof(req.addr), host, _TRUNCATE);
        req.port = port;
        const char* who = g.pname[0] ? g.pname : (signal_name()[0] ? signal_name() : "player");
        strncpy_s(req.player, sizeof(req.player), who, _TRUNCATE);
        if (g.log_desync) _snprintf_s(req.desc, sizeof(req.desc), _TRUNCATE, "[battle desync] %s", g.log_desync_text);
        else strncpy_s(req.desc, sizeof(req.desc), g.log_desc, _TRUNCATE);
        _snprintf_s(req.info, sizeof(req.info), _TRUNCATE, "proto %u, role %s, room %s", (unsigned)kProtoVersion,
                    signal_role()[0] ? signal_role() : "-", signal_room()[0] ? signal_room() : "-");
        for (int i = 0; i < nfiles; ++i) wcsncpy_s(req.files[i], MAX_PATH, files[i], _TRUNCATE);
        req.nfiles = nfiles;
        if (g.log_prev) {       // the "ended badly" markers go once the server has the files
            LogUploadRequest pr;
            if (prevrun_request(pr)) { for (int i = 0; i < pr.ndelete; ++i) wcsncpy_s(req.on_ok_delete[i], MAX_PATH, pr.on_ok_delete[i], _TRUNCATE); req.ndelete = pr.ndelete; }
        }
        log_line("MENU", "log upload to %s:%u (%d file(s), description %u bytes)", host, (unsigned)port, nfiles, (unsigned)strlen(g.log_desc));
        log_flush();             // 0.1.50: the log is buffered -- what it holds goes into the file before the file is read
        logupload_start(req);
    }
    if (paper_button("log_close", ImVec2(X(60 + 2 * bw + 32), Y(H - 100)), ImVec2(X(W - 60), Y(H - 36)), tr(Tx::LOG_CLOSE), 30 * k)) {
        g.log_open = false;
        g.log_hist = false;
        g.log_desync = false;
        g.log_halt = false;
        if (g.log_prev) { prevrun_dismiss(); g.log_prev = false; }      // closing the previous run's window forgets it: it is not offered again at every start
    }
    ImGui::End();
}

// The previous run ended badly (crash, freeze, killed): its log was sent in the background at startup (prevrun_auto_start) -- say how that went -- or, when it was not (the setting is off, no server
// configured, or it failed), open the upload window on that run's files so one press sends them.
void prevrun_tick() {
    if (g.prev_stage == 0) {
        if (prevrun_auto_started()) g.prev_stage = 1;
        else {
            LogUploadRequest r;
            if (prevrun_request(r)) { g.log_prev = true; g.log_open = true; logupload_reset(); }
            g.prev_stage = 2;
        }
    } else if (g.prev_stage == 1) {
        const LogUploadStatus st = logupload_status();
        if (st.state == LogUploadState::Done) { say(tr(Tx::LOG_PREV_AUTO_DONE), st.detail); prevrun_dismiss(); logupload_reset(); g.prev_stage = 2; }
        else if (st.state == LogUploadState::Failed) { say("%s", tr(Tx::LOG_PREV_AUTO_FAIL)); g.log_prev = true; g.log_open = true; logupload_reset(); g.prev_stage = 2; }
    }
}

void room_toasts() {
    char text[512];
    while (room_take_toast(text, sizeof(text))) say("%s", text);
}

void abandon_toasts() {
    AbandonNotice nt;
    while (abandon_take_notice(nt)) {
        switch (nt.kind) {
            case AbandonNoticeKind::Denied:    say(tr(Tx::AB_DENIED), (unsigned)(nt.pos + 1)); break;
            case AbandonNoticeKind::Withdrawn: say(tr(Tx::AB_WITHDRAWN), (unsigned)(nt.pos + 1)); break;
            case AbandonNoticeKind::TimedOut:  say(tr(Tx::AB_TIMEOUT)); break;
            case AbandonNoticeKind::Busy:      say(tr(Tx::AB_BUSY)); break;
            case AbandonNoticeKind::Changed:   say(tr(Tx::AB_CHANGED)); break;
            case AbandonNoticeKind::Going:     say(tr(Tx::AB_GOING)); break;
            default: break;
        }
    }
}

void menu_toggle_log_window() {
    g.log_open = !g.log_open;
    g.log_desync = false;
    g.log_halt = false;
    if (g.log_open) logupload_reset();   // a finished result from last time is not news
}

void menu_draw() {
    g.hovering_button = false;
    poll_language();
    prevrun_tick();
    roomhist_tick();                // a room joined: remembered for a day (the list puts it first)
    saveslots_auto_tick();          // back in the warehouse after a settled run: the auto save queue takes the current saves
    {   // a battle desync (debounced or halted): offer the log upload at once, the F2 window without the description
        char text[192] = {};
        if (lockstep_take_desync_notice(text, sizeof(text)) && logupload_status().state != LogUploadState::Working) {
            g.log_open = true; g.log_prev = false; g.log_desync = true;
            strncpy_s(g.log_desync_text, sizeof(g.log_desync_text), text, _TRUNCATE);
            logupload_reset();
            log_line("MENU", "battle desync: the log upload is offered (%s)", text);
        }
        // THE BATTLE HALTED (this peer's halt or the other player's): the game cannot go on. Say so, and what to do, in the same window, once per halt.
        static bool halt_seen = false;
        if (lockstep_halted() && !halt_seen) {
            halt_seen = true;
            if (!(g.log_open && g.log_desync)) {
                g.log_open = true; g.log_prev = false; g.log_desync = true;
                if (logupload_status().state != LogUploadState::Working) logupload_reset();
            }
            g.log_halt = true;
            _snprintf_s(g.log_desync_text, sizeof(g.log_desync_text), _TRUNCATE, "halted: %s", lockstep_halt_reason());
            log_line("MENU", "battle HALTED: the players are told the game cannot go on, and offered the log upload (%s)", lockstep_halt_reason());
        } else if (!lockstep_halted()) { halt_seen = false; g.log_halt = false; }      // the halt was lifted at the end of the fight: the notice's wording goes with it
    }
    const View v = view();
    if (v.h < 100.0f) return;

    const bool title = leave_menu_screen() == MenuScreen::MainMenu;
    g.title_scene = title;

    // Follow the game's own entries. They tick only while the page is up (so ours
    // appear with them and go with them), and a modal over the menu greys them.
    {
        const ULONGLONG t = GetTickCount64();
        const bool native_up = title && g.native_ms && t - g.native_ms < 400;
        const bool modal     = native_up && g.pause_ms && t - g.pause_ms < 250;
        const float dt = ImGui::GetIO().DeltaTime;
        g.title_a   += native_up ? dt / 0.25f : -dt / 0.08f;
        g.title_a    = g.title_a < 0 ? 0 : (g.title_a > 1 ? 1 : g.title_a);
        g.title_dim += modal ? dt / 0.15f : -dt / 0.15f;
        g.title_dim  = g.title_dim < 0 ? 0 : (g.title_dim > 1 ? 1 : g.title_dim);
    }

    lan_tick();

    if (title && g.title_a > 0.001f) {
        // Above the game's own four (which start at 65% of the height, 100 px
        // apart on the 1080 stage), in the same left column and the same face.
        if (menu_entry(v, "##e_multi", tr(Tx::MENU_MULTI), 375)) {
            if (in_room()) {
                // ALREADY IN A ROOM (2026-10-04): the next thing is a save slot, not the room form again. The room panel stays one click away on its tag.
                g.win = Win::None;
                g.auto_play_until = GetTickCount64() + 10000;
                log_line("MENU", "multiplayer pressed while in a room -- going to the save screen");
            } else {
                g.win = (g.win == Win::Multi) ? Win::None : Win::Multi; g.next_list = 0;
            }
        }
        if (menu_entry(v, "##e_settings", tr(Tx::MENU_SETTINGS), 585)) g.win = (g.win == Win::Settings) ? Win::None : Win::Settings;
        if (menu_entry(v, "##e_backup", tr(Tx::MENU_BACKUP), 480)) {
            g.win = (g.win == Win::Backup) ? Win::None : Win::Backup;
            g.sync_page = false; g.sync_slot = g.sync_idx = -1;
            if (g.win == Win::Backup) refresh_backup_view();
        }
        if (g.win == Win::Multi)  draw_multi_window(v);
        if (g.win == Win::Backup) { draw_backup_window(v); draw_backup_confirm(v); draw_sync_confirm(v); if (g.acct_warn) draw_acct_warn(v); if (g.acct_list) draw_acct_list(v, false); }
        if (g.win == Win::Settings) draw_settings_window(v);
    } else {
        g.win = Win::None;
        g.dlg = Confirm::None;
    }

    // A locked room refused the password (or asked for one the list did not know about): ask again, or say why not.
    {
        const uint32_t seq = signal_error_seq();
        if (seq != g.err_seen) {
            g.err_seen = seq;
            const char* code = signal_error_code();
            if (!strcmp(code, "password") && g.pw_id[0] && g.win == Win::Multi) {
                g.pw_ask = true; g.pw_wrong = g.pw_sent; g.pw_buf[0] = 0;
            } else if (!strcmp(code, "server")) {
                say("%s", tr(Tx::PW_SERVER_OLD));
            } else if (!strcmp(code, "relay_full")) {
                int ru = 0, rm = 0; signal_relay_info(&ru, &rm);
                say(tr(Tx::TR_RELAY_FULL), (unsigned)(rm > 0 ? rm : 10), (unsigned)(rm > 0 ? rm : 10));
            } else if (strcmp(code, "password") != 0 && signal_error()[0]) {
                // Anything else the server refused (a room name that is taken, a full room ..): the player had to read the log to
                // find out why the click did nothing.
                say(tr(Tx::SERVER_SAID), signal_error());
            }
            g.pw_sent = false;
        }
        // The prompt belongs to the lobby window: leaving it, or the lobby, takes it away.
        if (g.pw_ask && (g.win != Win::Multi || signal_state() != SignalState::Connected || in_room())) {
            g.pw_ask = false; g.pw_wrong = false; g.pw_buf[0] = 0;
        }
        if (g.pw_ask) draw_pw_prompt(v);
        // ...and so does the server-relay notice
        if (g.relay_warn && (g.win != Win::Multi || signal_state() != SignalState::Connected || in_room())) g.relay_warn = false;
        if (g.relay_warn) draw_relay_warn(v);
    }

    // The first-run notice, once the title screen is up.
    if (title && g.title_a > 0.001f && config().beta_notice && !g.beta_closed) draw_beta_notice(v);
    // WHICH ACCOUNT'S SAVES (2026-10-15): several account folders and neither Steam nor the player has named one -- ask, on the title screen, after a few
    // seconds (Steam's id usually arrives by then and settles it without a question).
    if (title) { if (g.title_since <= 0) g.title_since = now(); } else g.title_since = 0;
    if (title && g.title_since > 0 && now() - g.title_since > 4.0 && !g.acct_list && !in_room() && savefile_dir_needs_choice()) draw_acct_list(v, true);

    draw_board(v);      // first: its canvas sits behind every other window of the mod
    draw_mapvote(v);    // 0.1.95: the players' picks of the next map node
    draw_mp_tag(v);
    draw_save_sync(v);
    draw_abandon(v);
    abandon_toasts();
    draw_treasure_pick(v);   // EXPERIMENTAL shared treasure chest
    draw_housecoop(v);       // EXPERIMENTAL house boss co-op
    draw_area_gate(v);       // the host: entering an area another player has not unlocked
    draw_room_notice(v);
    if (g.log_open) draw_log_window(v);
    room_toasts();
    draw_chat(v);

    // A room just opened or closed: the connect window has done its job.
    static bool prev_room = false;
    const bool room_now = in_room();
    if (room_now && !prev_room) {
        if (g.win == Win::Multi) g.win = Win::None;
        say(tr(Tx::ENTERED_ROOM), signal_room());
        g.create_pw[0] = 0; g.lan_pw[0] = 0; g.pw_buf[0] = 0; g.pw_ask = false;
        g.panel_open = true;      // show the player who is in it, once
        // ...and take them to the save screen, which is the next thing the game
        // needs from them. Bounded, in case the menu is still fading in.
        g.auto_play_until = GetTickCount64() + 10000;
    }
    if (!room_now && prev_room) { g.room_name[0] = 0; say(tr(Tx::LEFT_ROOM)); chat_reset(); draw_reset(); settings_room_left(); }
    prev_room = room_now;

    // The dial to the host gave up on every address. A player who never got through is told why and walked out of the room
    // (staying would leave them on a save screen with nobody to play with); a redial after the host's boss fight is not a
    // first connection and keeps trying quietly.
    {
        static uint32_t dial_seen = 0;
        const uint32_t sq = net_dial_seq();
        if (!in_room()) g.room_connected = false;
        else if (net_state() == NetState::Connected || net_state() == NetState::Ready) g.room_connected = true;
        if (sq != dial_seen) {
            dial_seen = sq;
            if (in_room() && !room_is_host() && !g.room_connected) {
                say(tr(Tx::DIAL_FAILED), (unsigned)config().net_port);
                signal_request_leave();
            }
        }
    }

    draw_toast(v);
    draw_cursor(v);
}

// 0.1.91: THE GAME'S CURSOR STEPS ASIDE FOR THE DRAWING TOOL. glaiel::TempHideMouseCursor (C_TempHideCursor) is what the game's own screens call
// from their update, every frame they want no cursor; the Cursor shows it again the first frame nobody does. Its fatal error path (no custom cursor)
// is never taken: the singleton and its type cookie are read off the function's own first instructions and checked exactly as it checks them --
//   40 53 / 48 81 EC 80 00 00 00 / 48 8B 0D [cursor] / 48 85 C9 / 74 1E / 48 8B 05 [cookie] / 48 39 41 F8 / 75 11 / C6 41 39 01
// Called from the map's and the combat menu's update hooks, i.e. where the game's own callers are.
namespace {
typedef void (__fastcall* fn_temphide)();
bool call_temphide(uintptr_t fn) { __try { ((fn_temphide)fn)(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
}
void menu_board_cursor_tick() {
    if (!g.board_cursor || GetTickCount64() - g.board_cursor_at > 200) return;     // a tool over the board, seen by the last frames
    static uintptr_t fn = 0, cursor_slot = 0, cookie_slot = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const uintptr_t f = addr_of_call(C_TempHideCursor);
        uint8_t b[38] = {};
        if (f && mem_read((const void*)f, b, sizeof(b)) && b[0] == 0x40 && b[1] == 0x53 && b[9] == 0x48 && b[10] == 0x8B && b[11] == 0x0D &&
            b[21] == 0x48 && b[22] == 0x8B && b[23] == 0x05 && b[34] == 0xC6 && b[35] == 0x41 && b[36] == 0x39 && b[37] == 0x01) {
            int32_t d1 = 0, d2 = 0;
            memcpy(&d1, b + 12, 4); memcpy(&d2, b + 24, 4);
            fn = f; cursor_slot = f + 16 + (intptr_t)d1; cookie_slot = f + 28 + (intptr_t)d2;
            log_line("MENU", "drawing board: the game's cursor is hidden while a tool is over the board (TempHideMouseCursor found)");
        } else log_line_lvl(LogLevel::Warn, "MENU", "!! drawing board: TempHideMouseCursor is not where this build has it -- the game's cursor stays beside the tool");
    }
    if (!fn) return;
    uintptr_t cur = 0, cookie = 0, have = 0;
    if (!mem_read((const void*)cursor_slot, &cur, sizeof(cur)) || !cur || !mem_read((const void*)cookie_slot, &cookie, sizeof(cookie)) ||
        !mem_read((const void*)(cur - 8), &have, sizeof(have)) || have != cookie) return;      // no custom cursor: the call would be a fatal error
    call_temphide(fn);
}

} // namespace mgmp
