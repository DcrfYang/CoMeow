#include "mgmp_uitest.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "imgui.h"
#include "MinHook.h"

#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_ui.h"
#include "mgmp_leave.h"
#include "mgmp_menu.h"
#include "mgmp_checkpoint.h"

namespace mgmp {
namespace {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int          GLint;
typedef int          GLsizei;
constexpr GLenum kBack = 0x0405, kBGR = 0x80E0, kUByte = 0x1401, kPackAlign = 0x0D05,
                 kReadFbBinding = 0x8CAA, kReadFb = 0x8CA8;

struct Gl {
    void (__stdcall* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void (__stdcall* PixelStorei)(GLenum, GLint) = nullptr;
    void (__stdcall* GetIntegerv)(GLenum, GLint*) = nullptr;
    void (__stdcall* ReadBuffer)(GLenum) = nullptr;
    void (__stdcall* BindFramebuffer)(GLenum, GLuint) = nullptr;
    bool tried = false, ok = false;
};

struct Cmd { std::string op; std::string arg; };

// SDL undoes a button it was told about by message the moment GetAsyncKeyState says the
// physical button is up (WIN_CheckAsyncMouseRelease), so a posted press dies inside the
// same pump. While the harness is "holding" the button the OS is told it is down.
volatile bool g_lbutton_down = false;
SHORT (WINAPI* o_GetAsyncKeyState)(int) = nullptr;
SHORT WINAPI h_GetAsyncKeyState(int vk) {
    if (vk == VK_LBUTTON && g_lbutton_down) return (SHORT)0x8001;
    return o_GetAsyncKeyState(vk);
}
bool g_hooked = false;
void hook_async_state() {
    if (g_hooked) return;
    g_hooked = true;
    void* t = (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetAsyncKeyState");
    if (t && MH_CreateHook(t, (void*)&h_GetAsyncKeyState, (void**)&o_GetAsyncKeyState) == MH_OK &&
        MH_EnableHook(t) == MH_OK)
        log_line("UITEST", "GetAsyncKeyState hooked for injected clicks");
    else
        log_line("UITEST", "!! GetAsyncKeyState hook failed -- injected clicks will not hold");
}

struct T {
    bool on = false;
    Gl gl;
    std::wstring dir;
    std::deque<Cmd> q;
    int  wait = 0;
    int  frame = 0;
    std::string shot;      // pending screenshot name
    ImVec2 mouse = ImVec2(-1, -1);
};
T g;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ensure_dir() {
    if (!g.dir.empty()) return true;
    HMODULE mod = nullptr; wchar_t file[MAX_PATH * 2] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ensure_dir), &mod) ||
        !GetModuleFileNameW(mod, file, (DWORD)(sizeof(file) / sizeof(file[0]))))
        return false;
    std::wstring d = file;
    g.dir = d.substr(0, d.find_last_of(L"\\/"));
    return true;
}

void load_gl() {
    if (g.gl.tried) return;
    g.gl.tried = true;
    HMODULE ogl = GetModuleHandleA("opengl32.dll");
    if (!ogl) return;
    auto get = [&](const char* n) { return (void*)GetProcAddress(ogl, n); };
    g.gl.ReadPixels  = (decltype(g.gl.ReadPixels))get("glReadPixels");
    g.gl.PixelStorei = (decltype(g.gl.PixelStorei))get("glPixelStorei");
    g.gl.GetIntegerv = (decltype(g.gl.GetIntegerv))get("glGetIntegerv");
    g.gl.ReadBuffer  = (decltype(g.gl.ReadBuffer))get("glReadBuffer");
    typedef void* (__stdcall* wglGPA)(const char*);
    auto gpa = (wglGPA)get("wglGetProcAddress");
    if (gpa) g.gl.BindFramebuffer = (decltype(g.gl.BindFramebuffer))gpa("glBindFramebuffer");
    g.gl.ok = g.gl.ReadPixels && g.gl.PixelStorei && g.gl.GetIntegerv && g.gl.ReadBuffer && g.gl.BindFramebuffer;
    if (!g.gl.ok) log_line("UITEST", "!! GL entry points missing -- no screenshots");
}

void write_bmp(const std::wstring& path, int w, int h, const std::vector<unsigned char>& bgr) {
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> row((size_t)stride, 0);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { log_line("UITEST", "!! cannot write %s", narrow(path).c_str()); return; }
    unsigned char hdr[54] = { 'B', 'M' };
    const unsigned size = 54u + (unsigned)stride * (unsigned)h;
    memcpy(hdr + 2, &size, 4);
    const unsigned off = 54, dib = 40; const int planes = 1, bpp = 24;
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4);   memcpy(hdr + 22, &h, 4);      // positive height = bottom-up, as GL reads
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < h; ++y) {
        memcpy(row.data(), bgr.data() + (size_t)y * w * 3, (size_t)w * 3);
        fwrite(row.data(), 1, (size_t)stride, f);
    }
    fclose(f);
}

void take_shot(const std::string& name) {
    load_gl();
    if (!g.gl.ok || !ensure_dir()) return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const int w = (int)ds.x, h = (int)ds.y;
    if (w < 16 || h < 16) return;

    GLint prev_fb = 0, pack = 4;
    g.gl.GetIntegerv(kReadFbBinding, &prev_fb);
    g.gl.GetIntegerv(kPackAlign, &pack);
    g.gl.BindFramebuffer(kReadFb, 0);
    g.gl.ReadBuffer(kBack);
    g.gl.PixelStorei(kPackAlign, 1);
    std::vector<unsigned char> px((size_t)w * h * 3);
    g.gl.ReadPixels(0, 0, w, h, kBGR, kUByte, px.data());
    g.gl.PixelStorei(kPackAlign, pack);
    g.gl.BindFramebuffer(kReadFb, (GLuint)prev_fb);

    CreateDirectoryW((g.dir + L"\\shots").c_str(), nullptr);
    std::wstring wn(name.begin(), name.end());
    write_bmp(g.dir + L"\\shots\\" + wn + L".bmp", w, h, px);
    log_line("UITEST", "shot %s (%dx%d)", name.c_str(), w, h);
}

void poll_file() {
    if (!ensure_dir()) return;
    const std::wstring path = g.dir + L"\\mgmp_uitest.cmd";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string text;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    // An empty file is a writer that has created it and not filled it yet:
    // leave it for the next poll instead of eating the script.
    if (text.empty()) return;
    DeleteFileW(path.c_str());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t sp = line.find(' ');
        Cmd c;
        c.op = line.substr(0, sp);
        if (sp != std::string::npos) c.arg = line.substr(sp + 1);
        g.q.push_back(c);
    }
    log_line("UITEST", "%zu command(s) queued", g.q.size());
}

ImGuiKey key_of(const std::string& n) {
    if (n == "enter")     return ImGuiKey_Enter;
    if (n == "esc")       return ImGuiKey_Escape;
    if (n == "tab")       return ImGuiKey_Tab;
    if (n == "backspace") return ImGuiKey_Backspace;
    if (n == "delete")    return ImGuiKey_Delete;
    if (n == "up")        return ImGuiKey_UpArrow;
    if (n == "down")      return ImGuiKey_DownArrow;
    return ImGuiKey_None;
}

} // namespace

void uitest_before_frame() {
    if (!config().ui_test) return;
    g.on = true;
    ++g.frame;
    if (g.frame % 10 == 0 && g.q.empty()) poll_file();

    ImGuiIO& io = ImGui::GetIO();
    if (g.wait > 0) { --g.wait; return; }
    while (!g.q.empty()) {
        Cmd c = g.q.front(); g.q.pop_front();
        if (c.op == "wait") { g.wait = atoi(c.arg.c_str()); break; }
        // waitscreen main|save|house|other [frames]: hold the script until the game's
        // own scene list says this peer is on that screen (or the frame budget runs out).
        // pressbtn NAME: press the named game button the next time it ticks.
        if (c.op == "slot") { menu_request_slot(atoi(c.arg.c_str())); g.wait = 2; break; }
        // The host's handshake choice without a click: -1 = the preparation stage, 0.. = that save.
        if (c.op == "hspick") { log_line("UITEST", "hspick %s -> %s", c.arg.c_str(), checkpoint_host_pick(atoi(c.arg.c_str())) ? "ok" : "refused"); g.wait = 2; break; }
        if (c.op == "hsdismiss") { checkpoint_sync_dismiss(); break; }
        if (c.op == "pressbtn") { menu_request_press(c.arg.c_str()); g.wait = 2; break; }
        if (c.op == "waitscreen") {
            char nm[16] = {}; int budget = 60 * 240;
            sscanf_s(c.arg.c_str(), "%15s %d", nm, (unsigned)sizeof(nm), &budget);
            const MenuScreen want = !strcmp(nm, "main") ? MenuScreen::MainMenu
                                  : !strcmp(nm, "save") ? MenuScreen::SaveSelect
                                  : !strcmp(nm, "house") ? MenuScreen::House : MenuScreen::Other;
            if (leave_menu_screen() == want || budget <= 0) { g.wait = 1; break; }
            char again[48]; _snprintf_s(again, sizeof(again), _TRUNCATE, "%s %d", nm, budget - 1);
            g.q.push_front({ "waitscreen", again });
            break;
        }
        if (c.op == "move" || c.op == "click") {
            float x = 0, y = 0; sscanf_s(c.arg.c_str(), "%f %f", &x, &y);
            g.mouse = ImVec2(x, y);
            io.AddMousePosEvent(x, y);
            if (c.op == "click") {
                // Three frames, so hover is established before the press and the
                // press is a frame of its own before the release.
                g.q.push_front({ "release", "" });
                g.q.push_front({ "wait", "2" });
                g.q.push_front({ "press", "" });
                g.q.push_front({ "wait", "2" });
            }
            g.wait = 2;
            break;
        }
        // gmove / gclick / gkey drive the GAME, not the overlay: a window message is
        // posted to the game window, so SDL sees a pointer move exactly as it would
        // from a mouse -- and the real cursor is never touched. ImGui is parked far
        // off-screen so none of the mod's own widgets claim the pointer.
        if (c.op == "gmove" || c.op == "gclick") {
            float x = 0, y = 0; sscanf_s(c.arg.c_str(), "%f %f", &x, &y);
            HWND w = (HWND)ui_window();
            if (!w) { log_line("UITEST", "!! no game window yet"); continue; }
            g.mouse = ImVec2(-9999, -9999);
            io.AddMousePosEvent(-9999, -9999);
            { static bool said = false; if (!said) { said = true; RECT rc{}; GetClientRect(w, &rc);
              log_line("UITEST", "game client %ldx%ld, display %.0fx%.0f, dpi %u", rc.right, rc.bottom,
                       io.DisplaySize.x, io.DisplaySize.y, GetDpiForWindow(w)); } }
            PostMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM((int)x, (int)y));
            if (c.op == "gclick") {
                // SDL only accepts button messages for a window it believes has focus.
                PostMessageW(w, WM_ACTIVATE, WA_ACTIVE, 0);
                PostMessageW(w, WM_SETFOCUS, 0, 0);
                PostMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM((int)x, (int)y));
                g.q.push_front({ "gup", c.arg });
                g.q.push_front({ "wait", "6" });
                g.q.push_front({ "gdown", c.arg });
                g.q.push_front({ "wait", "8" });
            }
            g.wait = 3;
            break;
        }
        // realclick X Y: an OS-level click, for what posted messages cannot do. It only
        // fires when the game window really is the foreground window -- otherwise it
        // is skipped (the 2026-09-29 lesson: a synthetic click lands on whatever is on top).
        if (c.op == "realclick") {
            float x = 0, y = 0; sscanf_s(c.arg.c_str(), "%f %f", &x, &y);
            HWND w = (HWND)ui_window();
            if (!w) continue;
            SetForegroundWindow(w);
            if (GetForegroundWindow() != w) { log_line("UITEST", "!! game window is not foreground -- click skipped"); continue; }
            POINT pt{ (LONG)x, (LONG)y }, old{}; GetCursorPos(&old);
            ClientToScreen(w, &pt);
            SetCursorPos(pt.x, pt.y);
            g.q.push_front({ "realup", std::to_string(old.x) + " " + std::to_string(old.y) });
            g.q.push_front({ "wait", "6" });
            g.q.push_front({ "realdown", "" });
            g.q.push_front({ "wait", "8" });
            g.mouse = ImVec2(-9999, -9999);
            io.AddMousePosEvent(-9999, -9999);
            g.wait = 3;
            break;
        }
        if (c.op == "realdown" || c.op == "realup") {
            { POINT cp{}; GetCursorPos(&cp); log_line("UITEST", "%s: foreground=%d cursor=(%ld,%ld)", c.op.c_str(),
                             (int)(GetForegroundWindow() == (HWND)ui_window()), cp.x, cp.y); }
            if (GetForegroundWindow() == (HWND)ui_window()) {
                INPUT in{}; in.type = INPUT_MOUSE;
                in.mi.dwFlags = c.op == "realdown" ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                SendInput(1, &in, sizeof(in));
            }
            if (c.op == "realup") { int ox = 0, oy = 0; sscanf_s(c.arg.c_str(), "%d %d", &ox, &oy); SetCursorPos(ox, oy); }
            g.wait = 1;
            break;
        }
        if (c.op == "gdown" || c.op == "gup") {
            float x = 0, y = 0; sscanf_s(c.arg.c_str(), "%f %f", &x, &y);
            HWND w = (HWND)ui_window();
            log_line("UITEST", "%s posts (%d,%d) from '%s'", c.op.c_str(), (int)x, (int)y, c.arg.c_str());
            // SDL drops a button message whose extra-info carries the touch/pen signature,
            // and a posted message inherits this thread's last one.
            SetMessageExtraInfo(0);
            hook_async_state();
            g_lbutton_down = (c.op == "gdown");
            if (w) {
                PostMessageW(w, c.op == "gdown" ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                             c.op == "gdown" ? MK_LBUTTON : 0, MAKELPARAM((int)x, (int)y));
                // SDL rebuilds the button state from every mouse message's flags, so a
                // move that says "no buttons" inside the hold would cancel the press.
                if (c.op == "gdown") PostMessageW(w, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM((int)x, (int)y));
            }
            g.wait = 1;
            break;
        }
        if (c.op == "gkey") {
            HWND w = (HWND)ui_window();
            const WPARAM vk = c.arg == "esc" ? VK_ESCAPE
                            : c.arg == "enter" ? VK_RETURN
                            : c.arg == "f1" ? VK_F1 : 0;
            if (w && vk) {
                PostMessageW(w, WM_KEYDOWN, vk, 0);
                PostMessageW(w, WM_KEYUP, vk, 0xC0000001);
            }
            g.wait = 2;
            break;
        }
        if (c.op == "press")   { io.AddMouseButtonEvent(0, true);  break; }
        if (c.op == "release") { io.AddMouseButtonEvent(0, false); g.wait = 1; break; }
        if (c.op == "text")    { io.AddInputCharactersUTF8(c.arg.c_str()); g.wait = 1; break; }
        if (c.op == "key") {
            const ImGuiKey k = key_of(c.arg);
            if (k != ImGuiKey_None) { io.AddKeyEvent(k, true); g.q.push_front({ "keyup", c.arg }); }
            g.wait = 1;
            break;
        }
        if (c.op == "keyup")   { const ImGuiKey k = key_of(c.arg); if (k != ImGuiKey_None) io.AddKeyEvent(k, false); g.wait = 1; break; }
        if (c.op == "shot")    { g.shot = c.arg; break; }
        if (c.op == "log")     { log_line("UITEST", "%s", c.arg.c_str()); continue; }
        log_line("UITEST", "!! unknown command '%s'", c.op.c_str());
    }
    // Keep the injected pointer where the script put it even if the backend
    // polled the real cursor earlier this frame.
    if (g.mouse.x >= 0) io.AddMousePosEvent(g.mouse.x, g.mouse.y);
}

void uitest_after_render() {
    if (!g.on || g.shot.empty()) return;
    take_shot(g.shot);
    g.shot.clear();
}

} // namespace mgmp
