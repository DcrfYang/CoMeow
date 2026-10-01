#include "mgmp_menu.h"
#include "mgmp_i18n.h"
#include "mgmp_ui.h"

#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
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
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_saveslots.h"
#include "mgmp_page.h"
#include "mgmp_abandon.h"
#include "mgmp_room.h"
#include "mgmp_catview.h"
#include "mgmp_checkpoint.h"
#include "mgmp_catpalette.h"
#include "mgmp_savefile.h"
#include "mgmp_session.h"
#include "mgmp_signal.h"
#include "mgmp_logupload.h"

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
enum class Win { None, Multi, Backup };
enum class Confirm { None, Overwrite, Load, Delete, Undo, Import };

struct Menu {
    ImFont* font = nullptr;

    Tex paper_panel, paper_tag, cursor_def, cursor_over;
    Tex icon_multi, icon_cats, cat_head, cat_ear_l, cat_ear_r;

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
    // The password prompt for joining a locked room.
    bool     pw_ask   = false;
    bool     pw_wrong = false;        // the last try was refused: say so
    bool     pw_sent  = false;        // a join with a password is in flight
    char     pw_id[16]   = {};
    char     pw_name[48] = {};
    char     pw_buf[40]  = {};
    uint32_t err_seen = 0;            // the signal client's refusal counter as of the last frame
    // The F2 log-upload panel.
    bool     log_open   = false;
    bool     log_inited = false;
    char     log_addr[64] = {};
    char     log_desc[kLogUploadDescMax + 1] = {};
    // The first-run beta notice.
    bool     beta_closed = false;     // dismissed for this run
    bool     beta_dont   = false;     // its "do not show again" box
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

    // --- backup window ---
    char save_name[kSaveNameMax] = {};
    Confirm confirm = Confirm::None;
    int     confirm_idx = -1;
    double  confirm_until = 0;
    SaveSlotInfo current[kGameSlots];
    double  refreshed_at = 0;

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

bool overlay_window_begin(const char* id, ImVec2 pos, ImVec2 size, bool focus = true) {
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
        const char* e = s;
        const char* fit = s;
        const char* space = nullptr;   // just after the last space that still fits
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
        if (*e && *e != ' ' && space && space > s) cut = space;   // broke inside a word: back up to the last space
        if (cut == s) cut = s + 1;
        std::string ln(s, cut);
        while (!ln.empty() && ln.back() == ' ') ln.pop_back();
        text_at(dl, ImVec2(x, y), fs, col, ln.c_str());
        y += line_h;
        s = cut;
        while (*s == ' ') ++s;
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
void draw_paw(ImDrawList* dl, ImVec2 c, float w, ImU32 col) {
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
    for (const Blob& e : b)
        dl->AddEllipseFilled(ImVec2(c.x + e.x * w, c.y + e.y * w), ImVec2(e.rx * w, e.ry * w), col, 0.0f, 32);
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
    strncpy_s(g.pname, sizeof(g.pname), c.signal_name, _TRUNCATE);
}

// ---------------------------------------------------------------------------
// the multiplayer window (title screen)
// ---------------------------------------------------------------------------
void draw_room_info(ImDrawList* dl, const View& v, ImVec2 o, float w, float& y_out, bool compact);

void draw_multi_window(const View& v) {
    ensure_inited();
    const float W = 900, H = in_room() ? 860.0f : (signal_state() == SignalState::Connected ? 800.0f : 720.0f);
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
        float y = 0;
        draw_room_info(dl, v, ImVec2(X(60), Y(130)), (W - 120) * k, y, false);
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
        text_c(dl, X(W / 2), Y(130), 30 * k, kInkSoft, tr(Tx::CONNECT_FIRST));

        text_at(dl, ImVec2(X(70), Y(212)), 30 * k, kInk, tr(Tx::SERVER_ADDR));
        paper_input("##addr", ImVec2(X(290), Y(200)), ImVec2(X(W - 70), Y(262)), g.addr, sizeof(g.addr), "localhost", 30 * k);
        text_at(dl, ImVec2(X(70), Y(302)), 30 * k, kInk, tr(Tx::PLAYER_NAME));
        paper_input("##name", ImVec2(X(290), Y(290)), ImVec2(X(W - 70), Y(352)), g.pname, sizeof(g.pname), tr(Tx::NAME_HINT), 30 * k);
        text_c(dl, X(W / 2), Y(372), 22 * k, kGrey, tr(Tx::ADDR_HINT));

        const char* err = signal_error();
        if (st == SignalState::Failed && err[0]) {
            text_c(dl, X(W / 2), Y(430), 26 * k, kRed, tr(Tx::NET_FAILED));
            text_c(dl, X(W / 2), Y(468), 22 * k, kRed, err);
        } else if (st == SignalState::Closed) {
            text_c(dl, X(W / 2), Y(430), 26 * k, kAmber, tr(Tx::SERVER_LOST));
        }

        if (paper_button("connect", ImVec2(X(70), Y(H - 130)), ImVec2(X(W / 2 - 10), Y(H - 58)), tr(Tx::CONNECT), 34 * k, !busy, Tone::Good)) {
            char host[64]; uint16_t port = config().signal_port;
            split_addr(g.addr, host, sizeof(host), port);
            signal_request_connect(host, port, g.pname);
            log_line("MENU", "connect requested: %s:%u as '%s'", host, (unsigned)port, g.pname);
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
        text_c(dl, X(W / 2), Y(340), 24 * k, kGrey, signal_status());
        if (paper_button("cancel", ImVec2(X(W / 2 - 150), Y(H - 130)), ImVec2(X(W / 2 + 150), Y(H - 58)), tr(Tx::CANCEL), 34 * k))
            signal_request_disconnect();
        ImGui::End();
        return;
    }

    // --- connected, no room: the list -----------------------------------------
    if (now() >= g.next_list && !busy) { signal_request_list(); g.next_list = now() + 3.0; }

    char who[120];
    _snprintf_s(who, sizeof(who), _TRUNCATE, tr(Tx::CONNECTED_AS), signal_server(), signal_name());
    text_c(dl, X(W / 2), Y(112), 26 * k, kGreen, who);

    // search
    text_at(dl, ImVec2(X(50), Y(150)), 26 * k, kInk, tr(Tx::SEARCH));
    paper_input("##search", ImVec2(X(190), Y(138)), ImVec2(X(W - 50), Y(190)), g.search, sizeof(g.search), tr(Tx::SEARCH_HINT), 26 * k);

    const float lx0 = X(50), lx1 = X(W - 50), ly0 = Y(204), ly1 = Y(H - 270);
    rough_rect(dl, ImVec2(lx0, ly0), ImVec2(lx1, ly1), IM_COL32(255, 255, 250, 70), kInk, 2.2f, 91, 1.2f);
    const uint32_t total = signal_rooms(g.rooms, 32);
    // The rows that match the search: open rooms first, locked ones after them (the server sends them that way
    // already; sorting here as well keeps the list right whatever sent it).
    uint32_t order[32]; uint32_t n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < total && i < 32; ++i)
            if (g.rooms[i].pw == (pass == 1) &&
                (contains_ci(g.rooms[i].name, g.search) || contains_ci(g.rooms[i].host, g.search) || contains_ci(g.rooms[i].id, g.search)))
                order[n++] = i;
    if (n == 0) {
        text_c(dl, X(W / 2), Y(300), 30 * k, kInkSoft, total == 0 ? tr(Tx::NO_ROOMS) : tr(Tx::NO_MATCH));
        if (total == 0) text_c(dl, X(W / 2), Y(346), 24 * k, kGrey, tr(Tx::NO_ROOMS_HINT));
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
            float name_x = rp.x + 20 * k;
            if (r.pw) { draw_lock(cdl, ImVec2(name_x + 14 * k, rp.y + rowh * 0.5f), 30 * k, kInk); name_x += 40 * k; }
            text_at(cdl, ImVec2(name_x, rp.y + 10 * k), 28 * k, kInk, r.name[0] ? r.name : r.id);
            text_at(cdl, ImVec2(X(420), rp.y + 12 * k), 26 * k, kInkSoft, r.host);
            char cnt[16]; _snprintf_s(cnt, sizeof(cnt), _TRUNCATE, "%u/%u", (unsigned)r.players, (unsigned)r.cap);
            text_at(cdl, ImVec2(X(640), rp.y + 12 * k), 26 * k, kInkSoft, cnt);
            const bool full = r.players >= r.cap;
            if (paper_button(r.id, ImVec2(X(730), rp.y + 6 * k), ImVec2(X(W - 70), rp.y + rowh - 6 * k),
                             full ? tr(Tx::FULL) : tr(Tx::JOIN), 26 * k, !full && !busy && !g.pw_ask, Tone::Good)) {
                strncpy_s(g.room_name, sizeof(g.room_name), r.name[0] ? r.name : r.id, _TRUNCATE);
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

    // create rows: the name, and an optional password (empty = an open room)
    text_at(dl, ImVec2(X(50), Y(H - 252)), 26 * k, kInk, tr(Tx::ROOM_NAME));
    paper_input("##room", ImVec2(X(190), Y(H - 264)), ImVec2(X(W - 320), Y(H - 212)), g.room, sizeof(g.room), tr(Tx::ROOM_NAME_HINT), 26 * k);
    text_at(dl, ImVec2(X(50), Y(H - 190)), 26 * k, kInk, tr(Tx::ROOM_PW));
    paper_input("##roompw", ImVec2(X(190), Y(H - 202)), ImVec2(X(W - 320), Y(H - 150)), g.create_pw, sizeof(g.create_pw), tr(Tx::ROOM_PW_HINT), 26 * k,
                ImGuiInputTextFlags_Password);
    if (paper_button("create", ImVec2(X(W - 300), Y(H - 264)), ImVec2(X(W - 50), Y(H - 150)), tr(Tx::CREATE_ROOM), 30 * k, !busy && !g.pw_ask, Tone::Good)) {
        signal_request_create(g.room, g.create_pw);
        strncpy_s(g.room_name, sizeof(g.room_name), g.room, _TRUNCATE);
        log_line("MENU", "create requested: '%s'%s", g.room, g.create_pw[0] ? " (password)" : "");
    }

    if (paper_button("refresh", ImVec2(X(50), Y(H - 120)), ImVec2(X(50 + (W - 100 - 20) / 3), Y(H - 56)), tr(Tx::REFRESH), 30 * k, !busy)) {
        signal_request_list(); g.next_list = now() + 3.0;
    }
    if (paper_button("disc", ImVec2(X(50 + (W - 100 - 20) / 3 + 10), Y(H - 120)), ImVec2(X(50 + 2 * (W - 100 - 20) / 3 + 10), Y(H - 56)), tr(Tx::DISCONNECT_SERVER), 30 * k, !busy, Tone::Danger))
        signal_request_disconnect();
    if (paper_button("back", ImVec2(X(50 + 2 * (W - 100 - 20) / 3 + 20), Y(H - 120)), ImVec2(X(W - 50), Y(H - 56)), tr(Tx::BACK), 30 * k))
        g.win = Win::None;
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
    if (!host && signal_host_addr()[0]) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::HOST_ADDR), signal_host_addr(), (unsigned)signal_host_port());
        text_at(dl, ImVec2(o.x, y), 22 * k, kGrey, line);
        y += 34 * k;
    }

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
    if (host && n < 2 && !room_locked()) {
        text_at(dl, ImVec2(o.x, y), 22 * k, kGrey, tr(Tx::TELL_ROOM));
        y += 34 * k;
    }

    // The lock: until it is on nobody may pick a save; while it is on the room is hidden and closed.
    text_at(dl, ImVec2(o.x, y), 22 * k, room_locked() ? kGreen : kAmber,
            room_locked() ? tr(Tx::LOCKED_LINE)
                          : (host ? tr(Tx::UNLOCKED_HOST) : tr(Tx::UNLOCKED_CLIENT)));
    y += 34 * k;

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
    const NetStats ns = net_stats();
    _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::PACKETS), ns.sent, ns.received);
    text_at(dl, ImVec2(o.x, y), 22 * k, kGrey, line);
    y += 32 * k;
    if (!compact && session_status()[0]) {
        text_at(dl, ImVec2(o.x, y), 20 * k, kGrey, session_status());
        y += 28 * k;
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
        if (g_fd.save) saveslots_export(g_fd.index, g_fd.path);
        else           saveslots_import(g_fd.index, g_fd.path);
    }
    InterlockedExchange(&g_fd.state, 0);
    return true;
}

void refresh_backup_view() {
    saveslots_refresh();
    saveslots_current(g.current);
    g.refreshed_at = now();
    g.confirm = Confirm::None;
}

bool confirm_armed(Confirm kind, int idx) {
    return g.confirm == kind && g.confirm_idx == idx && now() < g.confirm_until;
}
void arm(Confirm kind, int idx) { g.confirm = kind; g.confirm_idx = idx; g.confirm_until = now() + 3.0; }

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
    paper_input("##sname", ImVec2(X(190), Y(158)), ImVec2(X(760), Y(214)), g.save_name, sizeof(g.save_name),
                tr(Tx::BACKUP_NAME_HINT), 26 * k);
    if (paper_button("undo", ImVec2(X(800), Y(158)), ImVec2(X(1090), Y(214)),
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
    }

    // List.
    const float ly0 = Y(268), ly1 = Y(H - 110);
    rough_rect(dl, ImVec2(X(50), ly0), ImVec2(X(W - 50), ly1), IM_COL32(255, 255, 250, 60), kInk, 2.2f, 17, 1.2f);
    ImGui::SetCursorScreenPos(ImVec2(X(56), ly0 + 4 * k));
    ImGui::BeginChild("##list", ImVec2((W - 112) * k, ly1 - ly0 - 8 * k), false, ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const float rowh = 80 * k;
    const float rx = X(56), rw = (W - 112) * k;
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
        const bool ow = confirm_armed(Confirm::Overwrite, i);
        if (paper_button(id, ImVec2(rp.x + 794 * k, by0), ImVec2(rp.x + 898 * k, by1),
                         ow ? tr(Tx::CONFIRM_OVERWRITE) : (b.used ? tr(Tx::OVERWRITE) : tr(Tx::SAVE)), 24 * k, !locked, b.used ? Tone::Normal : Tone::Good, ow)) {
            if (b.used && !ow) arm(Confirm::Overwrite, i);
            else {
                saveslots_save(i, g.save_name);
                refresh_backup_view();
            }
        }
        if (b.used) {
            _snprintf_s(id, sizeof(id), _TRUNCATE, "l%d", i);
            const bool ld = confirm_armed(Confirm::Load, i);
            if (paper_button(id, ImVec2(rp.x + 904 * k, by0), ImVec2(rp.x + 1008 * k, by1),
                             ld ? tr(Tx::CONFIRM_LOAD) : tr(Tx::LOAD), 24 * k, !locked, Tone::Good, ld)) {
                if (!ld) arm(Confirm::Load, i);
                else {
                    saveslots_load(i);
                    refresh_backup_view();
                }
            }
            _snprintf_s(id, sizeof(id), _TRUNCATE, "d%d", i);
            const bool dd = confirm_armed(Confirm::Delete, i);
            // One file out: the whole position (see saveslots_export).
            _snprintf_s(id, sizeof(id), _TRUNCATE, "x%d", i);
            if (paper_button(id, ImVec2(rp.x + 1014 * k, by0), ImVec2(rp.x + 1118 * k, by1), tr(Tx::EXPORT), 24 * k,
                             !dialog_open))
                file_dialog_open(true, i, b.name);
            _snprintf_s(id, sizeof(id), _TRUNCATE, "d%d", i);
            if (paper_button(id, ImVec2(rp.x + 1124 * k, by0), ImVec2(rp.x + 1228 * k, by1),
                             dd ? tr(Tx::CONFIRM_DELETE) : tr(Tx::DELETE_BTN), 24 * k, true, Tone::Danger, dd)) {
                if (!dd) arm(Confirm::Delete, i);
                else {
                    saveslots_delete(i);
                    refresh_backup_view();
                }
            }
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

// ---------------------------------------------------------------------------
// cursor + toast
// ---------------------------------------------------------------------------
void draw_cursor(const View& v) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDrawCursor) return;                 // the debug panel is drawing its own arrow
    if (!io.WantCaptureMouse) return;               // the game's cursor is showing and is right
    if (io.MousePos.x < -1e5f) return;

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
    if (show_rows)     H = 170 + (np ? np : 1) * 62 + 40 + 76;
    else if (choosing) H = 190 + (items ? items : 1) * 76 + 40 + 76;
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
        float y = 150;
        int index = -1;
        if (sv.prep) {
            if (paper_button("hs_prep", ImVec2(X(50), Y(y)), ImVec2(X(W - 50), Y(y + 64)),
                             tr(Tx::PREP_STAGE), 28 * k, true, Tone::Good))
                checkpoint_host_pick(-1);
            y += 76;
        }
        for (uint32_t i = 0; i < sv.n; ++i, ++index) {
            char when[40], label[160], id[24];
            format_filetime(sv.entry[i].stamp, when, sizeof(when));
            _snprintf_s(label, sizeof(label), _TRUNCATE, tr(Tx::SYNC_ENTRY),
                        i + 1, (unsigned long long)sv.entry[i].seq, when, i == 0 ? tr(Tx::LATEST) : "");
            _snprintf_s(id, sizeof(id), _TRUNCATE, "hs_%u", i);
            if (paper_button(id, ImVec2(X(50), Y(y)), ImVec2(X(W - 50), Y(y + 64)), label, 22 * k))
                checkpoint_host_pick((int)i);
            y += 76;
        }
        if (paper_button("hs_repick", ImVec2(X(W / 2 - 150), Y(H - 96)), ImVec2(X(W / 2 + 150), Y(H - 40)),
                         tr(Tx::REPICK), 26 * k))
            room_abort_selection();
    } else {
        text_c(dl, X(W / 2), Y(50), 44 * k, kRed, tr(Tx::COMBO_INVALID));
        text_c(dl, X(W / 2), Y(122), 26 * k, kInk, tr(Tx::COMBO_INVALID_1));
        text_c(dl, X(W / 2), Y(160), 26 * k, kInk, tr(Tx::COMBO_INVALID_2));
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
    const float W = 820, H = 330;
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

// The first-run notice: this is a test release, back your saves up. Shown over the title screen until dismissed;
// "do not show again" writes ui.beta_notice = false into mgmp.json.
void draw_beta_notice(const View& v) {
    const float k = v.k;
    const float W = 860, H = 470;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!modal_begin("##mgmp_beta")) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };
    text_c(dl, X(W / 2), Y(40), 44 * k, kInk, tr(Tx::BETA_TITLE));
    wrap_text(dl, X(70), Y(122), (W - 140) * k, 28 * k, 42 * k, kInk, tr(Tx::BETA_TEXT));

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

// "68 KB" / "1.2 MB" for the preview line.
void format_size(char* out, size_t cap, uint32_t n) {
    if (n < 1024) _snprintf_s(out, cap, _TRUNCATE, "%u B", (unsigned)n);
    else if (n < (1u << 20)) _snprintf_s(out, cap, _TRUNCATE, "%u KB", (unsigned)(n / 1024));
    else _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", n / 1048576.0);
}

// The files an upload carries: this run's trace log, and the boot log beside it when there is one.
int collect_log_files(wchar_t files[kLogUploadMaxFiles][MAX_PATH]) {
    int n = 0;
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

// F2: upload this run's game log to a server that collects them, with a note about what went wrong.
void draw_log_window(const View& v) {
    ensure_inited();
    if (!g.log_inited) {
        g.log_inited = true;
        strncpy_s(g.log_addr, sizeof(g.log_addr), g.addr[0] ? g.addr : "localhost", _TRUNCATE);
    }
    const float k = v.k;
    const float W = 860, H = 760;
    const ImVec2 a = P(v, (kStageW - W) * 0.5f, (kStageH - H) * 0.5f);
    const ImVec2 sz(W * k, H * k);
    if (!overlay_window_begin("##mgmp_logup", a, sz)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paper(dl, v, a, ImVec2(a.x + sz.x, a.y + sz.y));
    auto X = [&](float x) { return a.x + x * k; };
    auto Y = [&](float y) { return a.y + y * k; };

    text_c(dl, X(W / 2), Y(36), 46 * k, kInk, tr(Tx::LOG_TITLE));
    wrap_text(dl, X(60), Y(104), (W - 120) * k, 24 * k, 34 * k, kInkSoft, tr(Tx::LOG_HINT));

    const LogUploadStatus st = logupload_status();
    const bool working = st.state == LogUploadState::Working;

    text_at(dl, ImVec2(X(60), Y(190)), 26 * k, kInk, tr(Tx::LOG_ADDR));
    paper_input("##logaddr", ImVec2(X(290), Y(178)), ImVec2(X(W - 60), Y(236)), g.log_addr, sizeof(g.log_addr), "localhost", 28 * k);

    text_at(dl, ImVec2(X(60), Y(262)), 26 * k, kInk, tr(Tx::LOG_DESC));
    paper_multiline("##logdesc", ImVec2(X(60), Y(302)), ImVec2(X(W - 60), Y(500)), g.log_desc, sizeof(g.log_desc), tr(Tx::LOG_DESC_HINT), 26 * k);

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
        text_at(dl, ImVec2(X(60), Y(516)), 22 * k, kGrey, line);
    }
    text_at(dl, ImVec2(X(60), Y(548)), 20 * k, kGrey, tr(Tx::LOG_PRIVACY));

    // the result of the last upload
    char msg[320] = {};
    ImU32 mcol = kInkSoft;
    if (working) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, tr(Tx::LOG_BUSY), (unsigned)st.percent);
        const ImVec2 b0(X(60), Y(592)), b1(X(W - 60), Y(612));
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
    if (msg[0]) wrap_text(dl, X(60), Y(working ? 624 : 596), (W - 120) * k, 26 * k, 34 * k, mcol, msg);

    if (paper_button("log_go", ImVec2(X(60), Y(H - 100)), ImVec2(X(W / 2 - 10), Y(H - 36)), tr(Tx::LOG_UPLOAD), 30 * k, !working && nfiles > 0, Tone::Good)) {
        LogUploadRequest req;
        char host[64]; uint16_t port = config().signal_port;
        split_addr(g.log_addr, host, sizeof(host), port);
        strncpy_s(req.addr, sizeof(req.addr), host, _TRUNCATE);
        req.port = port;
        const char* who = g.pname[0] ? g.pname : (signal_name()[0] ? signal_name() : "player");
        strncpy_s(req.player, sizeof(req.player), who, _TRUNCATE);
        strncpy_s(req.desc, sizeof(req.desc), g.log_desc, _TRUNCATE);
        _snprintf_s(req.info, sizeof(req.info), _TRUNCATE, "proto %u, role %s, room %s", (unsigned)kProtoVersion,
                    signal_role()[0] ? signal_role() : "-", signal_room()[0] ? signal_room() : "-");
        for (int i = 0; i < nfiles; ++i) wcsncpy_s(req.files[i], MAX_PATH, files[i], _TRUNCATE);
        req.nfiles = nfiles;
        log_line("MENU", "log upload to %s:%u (%d file(s), description %u bytes)", host, (unsigned)port, nfiles, (unsigned)strlen(g.log_desc));
        logupload_start(req);
    }
    if (paper_button("log_close", ImVec2(X(W / 2 + 10), Y(H - 100)), ImVec2(X(W - 60), Y(H - 36)), tr(Tx::LOG_CLOSE), 30 * k))
        g.log_open = false;
    ImGui::End();
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
    if (g.log_open) logupload_reset();   // a finished result from last time is not news
}

void menu_draw() {
    g.hovering_button = false;
    poll_language();
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

    if (title && g.title_a > 0.001f) {
        // Above the game's own four (which start at 65% of the height, 100 px
        // apart on the 1080 stage), in the same left column and the same face.
        if (menu_entry(v, "##e_multi", tr(Tx::MENU_MULTI), 480)) { g.win = (g.win == Win::Multi) ? Win::None : Win::Multi; g.next_list = 0; }
        if (menu_entry(v, "##e_backup", tr(Tx::MENU_BACKUP), 585)) {
            g.win = (g.win == Win::Backup) ? Win::None : Win::Backup;
            if (g.win == Win::Backup) refresh_backup_view();
        }
        if (g.win == Win::Multi)  draw_multi_window(v);
        if (g.win == Win::Backup) draw_backup_window(v);
    } else {
        g.win = Win::None;
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
            }
            g.pw_sent = false;
        }
        // The prompt belongs to the lobby window: leaving it, or the lobby, takes it away.
        if (g.pw_ask && (g.win != Win::Multi || signal_state() != SignalState::Connected || in_room())) {
            g.pw_ask = false; g.pw_wrong = false; g.pw_buf[0] = 0;
        }
        if (g.pw_ask) draw_pw_prompt(v);
    }

    // The first-run notice, once the title screen is up.
    if (title && g.title_a > 0.001f && config().beta_notice && !g.beta_closed) draw_beta_notice(v);

    draw_mp_tag(v);
    draw_save_sync(v);
    draw_abandon(v);
    abandon_toasts();
    draw_room_notice(v);
    if (g.log_open) draw_log_window(v);
    room_toasts();

    // A room just opened or closed: the connect window has done its job.
    static bool prev_room = false;
    const bool room_now = in_room();
    if (room_now && !prev_room) {
        if (g.win == Win::Multi) g.win = Win::None;
        say(tr(Tx::ENTERED_ROOM), signal_room());
        g.create_pw[0] = 0; g.pw_buf[0] = 0; g.pw_ask = false;
        g.panel_open = true;      // show the player who is in it, once
        // ...and take them to the save screen, which is the next thing the game
        // needs from them. Bounded, in case the menu is still fading in.
        g.auto_play_until = GetTickCount64() + 10000;
    }
    if (!room_now && prev_room) { g.room_name[0] = 0; say(tr(Tx::LEFT_ROOM)); }
    prev_room = room_now;

    draw_toast(v);
    draw_cursor(v);
}

} // namespace mgmp
