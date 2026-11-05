#include "mgmp_imeui.h"
#include "mgmp_menu.h"      // menu_font: the CJK face the menus draw with
#include "mgmp_log.h"

#include <windows.h>
#include <imm.h>
#include <msctf.h>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // GImGui->PlatformImeData: where the active text box's caret is

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "imm32.lib")

namespace mgmp {
namespace {

std::atomic<bool> g_active{false};       // a text box of ours has the keyboard
std::atomic<bool> g_seen{false};         // a candidate list was taken over at least once
bool g_tried = false;                    // window thread
ITfUIElementMgr* g_mgr = nullptr;        // window thread
int g_logs = 0;

SRWLOCK g_lock = SRWLOCK_INIT;           // the state below: written on the window thread, read by the frame
std::wstring g_comp;                     // the composition string (GCS_COMPSTR)
std::wstring g_reading;                  // a reading element's string
std::vector<std::wstring> g_cands;       // the candidate list's current page
int g_sel = -1;                          // the selected entry in that page
DWORD g_cand_id = 0, g_read_id = 0;      // the UI elements taken over (0 = none)

std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void read_candidates(ITfCandidateListUIElement* c) {
    UINT count = 0, sel = 0, pages = 0, cur = 0;
    if (FAILED(c->GetCount(&count))) count = 0;
    if (FAILED(c->GetSelection(&sel))) sel = 0;
    std::vector<UINT> idx;
    if (SUCCEEDED(c->GetPageIndex(nullptr, 0, &pages)) && pages > 0 && pages < 1000) {
        idx.resize(pages);
        if (FAILED(c->GetPageIndex(idx.data(), pages, &pages))) { idx.clear(); pages = 0; }
    } else pages = 0;
    if (FAILED(c->GetCurrentPage(&cur))) cur = 0;
    UINT start = 0, end = count;
    if (pages && cur < pages) { start = idx[cur]; end = cur + 1 < pages ? idx[cur + 1] : count; }
    else if (count > 9) { start = (sel / 9) * 9; end = start + 9 < count ? start + 9 : count; }       // no pages given: nine around the selection
    if (end > count) end = count;
    if (end > start + 12) end = start + 12;
    std::vector<std::wstring> list;
    for (UINT i = start; i < end; ++i) {
        BSTR s = nullptr;
        if (SUCCEEDED(c->GetString(i, &s)) && s) { list.emplace_back(s, SysStringLen(s)); SysFreeString(s); }
        else list.emplace_back();
    }
    AcquireSRWLockExclusive(&g_lock);
    g_cands.swap(list);
    g_sel = (sel >= start && sel < end) ? (int)(sel - start) : -1;
    ReleaseSRWLockExclusive(&g_lock);
}

void read_reading(ITfReadingInformationUIElement* r) {
    BSTR s = nullptr;
    std::wstring w;
    if (SUCCEEDED(r->GetString(&s)) && s) { w.assign(s, SysStringLen(s)); SysFreeString(s); }
    AcquireSRWLockExclusive(&g_lock);
    g_reading.swap(w);
    ReleaseSRWLockExclusive(&g_lock);
}

// One element: a candidate list or a reading window is taken over (read, and not shown by the text service); anything else is left to it.
bool take(DWORD id, bool begin) {
    if (!g_mgr) return false;
    ITfUIElement* el = nullptr;
    if (FAILED(g_mgr->GetUIElement(id, &el)) || !el) return false;
    bool taken = false;
    ITfCandidateListUIElement* c = nullptr;
    if (SUCCEEDED(el->QueryInterface(IID_ITfCandidateListUIElement, (void**)&c)) && c) {
        read_candidates(c); c->Release();
        g_cand_id = id; taken = true;
        if (begin && !g_seen.exchange(true)) log_line("IMEUI", "the IME's candidate list is drawn in the game from here on (TSF UI-less mode works with this IME)");
    } else {
        ITfReadingInformationUIElement* r = nullptr;
        if (SUCCEEDED(el->QueryInterface(IID_ITfReadingInformationUIElement, (void**)&r)) && r) { read_reading(r); r->Release(); g_read_id = id; taken = true; }
    }
    if (begin && g_logs < 20) {
        ++g_logs;
        BSTR d = nullptr; std::wstring desc;
        if (SUCCEEDED(el->GetDescription(&d)) && d) { desc.assign(d, SysStringLen(d)); SysFreeString(d); }
        log_line("IMEUI", "UI element %lu ('%s') %s", (unsigned long)id, utf8(desc).c_str(), taken ? "taken over -- drawn in the game" : "left to the IME");
    }
    el->Release();
    return taken;
}

class UiSink final : public ITfUIElementSink {
    LONG ref_ = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ITfUIElementSink) { *ppv = static_cast<ITfUIElementSink*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override { const LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return (ULONG)r; }
    STDMETHODIMP BeginUIElement(DWORD id, BOOL* show) override {
        if (show) *show = TRUE;
        if (!g_active.load()) return S_OK;
        if (take(id, true) && show) *show = FALSE;
        return S_OK;
    }
    STDMETHODIMP UpdateUIElement(DWORD id) override {
        if (g_active.load() && (id == g_cand_id || id == g_read_id)) take(id, false);
        return S_OK;
    }
    STDMETHODIMP EndUIElement(DWORD id) override {
        AcquireSRWLockExclusive(&g_lock);
        if (id == g_cand_id) { g_cand_id = 0; g_cands.clear(); g_sel = -1; }
        if (id == g_read_id) { g_read_id = 0; g_reading.clear(); }
        ReleaseSRWLockExclusive(&g_lock);
        return S_OK;
    }
};

} // namespace

bool imeui_init(HWND) {
    if (g_tried) return g_mgr != nullptr;
    g_tried = true;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITfThreadMgrEx* tm = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgrEx, (void**)&tm);
    if (FAILED(hr) || !tm) { log_line("IMEUI", "!! no TSF thread manager (hr %08lX, COM %08lX) -- the IME draws its own window as before", (unsigned long)hr, (unsigned long)co); return false; }
    TfClientId tid = 0;
    hr = tm->ActivateEx(&tid, 0);
    ITfSource* src = nullptr;
    if (FAILED(tm->QueryInterface(IID_ITfUIElementMgr, (void**)&g_mgr)) || !g_mgr || FAILED(tm->QueryInterface(IID_ITfSource, (void**)&src)) || !src) {
        log_line("IMEUI", "!! TSF has no UI element manager here -- the IME draws its own window as before");
        if (g_mgr) { g_mgr->Release(); g_mgr = nullptr; }
        return false;
    }
    DWORD cookie = 0;
    UiSink* sink = new UiSink();
    const HRESULT ad = src->AdviseSink(IID_ITfUIElementSink, static_cast<ITfUIElementSink*>(sink), &cookie);
    sink->Release();
    src->Release();
    if (FAILED(ad)) { log_line("IMEUI", "!! the UI element sink was refused (hr %08lX)", (unsigned long)ad); g_mgr->Release(); g_mgr = nullptr; return false; }
    log_line("IMEUI", "TSF UI element sink in place (COM %08lX, activate %08lX, client %lu): an IME that supports UI-less mode shows its candidates inside the game", (unsigned long)co, (unsigned long)hr, (unsigned long)tid);
    return true;   // tm is kept activated for the process's life (never released: the mod is never unloaded)
}

void imeui_set_active(bool on) {
    g_active.store(on);
    if (!on) {
        AcquireSRWLockExclusive(&g_lock);
        g_comp.clear(); g_reading.clear(); g_cands.clear(); g_sel = -1;
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void imeui_on_composition(HWND hwnd, LPARAM lp) {
    if (!g_active.load()) return;
    std::wstring w;
    if (lp & GCS_COMPSTR) {
        if (HIMC h = ImmGetContext(hwnd)) {
            const LONG bytes = ImmGetCompositionStringW(h, GCS_COMPSTR, nullptr, 0);
            if (bytes > 0 && bytes < 4096) { w.resize((size_t)bytes / sizeof(wchar_t)); ImmGetCompositionStringW(h, GCS_COMPSTR, &w[0], (DWORD)bytes); }
            ImmReleaseContext(hwnd, h);
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_comp.swap(w);
    ReleaseSRWLockExclusive(&g_lock);
}

void imeui_end_composition() {
    AcquireSRWLockExclusive(&g_lock);
    g_comp.clear();
    ReleaseSRWLockExclusive(&g_lock);
}

bool imeui_takes_over() { return g_seen.load(); }

void imeui_draw() {
    if (!g_active.load() || !GImGui) return;
    std::string comp, reading;
    std::vector<std::string> cands;
    int sel = -1;
    AcquireSRWLockShared(&g_lock);
    comp = utf8(g_comp); reading = utf8(g_reading); sel = g_sel;
    for (const std::wstring& c : g_cands) cands.push_back(utf8(c));
    ReleaseSRWLockShared(&g_lock);
    const std::string& top = !reading.empty() ? reading : comp;
    if (top.empty() && cands.empty()) return;

    ImFont* font = menu_font();
    if (!font) font = ImGui::GetFont();
    const ImGuiPlatformImeData& ime = GImGui->PlatformImeData;
    const ImGuiIO& io = ImGui::GetIO();
    const float line = ime.InputLineHeight > 4.0f ? ime.InputLineHeight : 24.0f;
    const float fs = line * 0.8f < 18.0f ? 18.0f : (line * 0.8f > 40.0f ? 40.0f : line * 0.8f);
    const float pad = fs * 0.45f, gap = fs * 0.8f;
    auto size = [&](const std::string& s) { return font->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str()); };

    // the panel's size: the composition on the first line, the numbered candidates on the second
    float w2 = 0.0f;
    std::vector<std::string> labels;
    for (size_t i = 0; i < cands.size(); ++i) {
        char num[8]; _snprintf_s(num, sizeof(num), _TRUNCATE, "%u ", (unsigned)(i + 1) % 10);
        labels.push_back(std::string(num) + cands[i]);
        w2 += size(labels.back()).x + (i ? gap : 0.0f);
    }
    const float w1 = top.empty() ? 0.0f : size(top).x;
    const float w = (w1 > w2 ? w1 : w2) + pad * 2.0f;
    const float h = pad * 2.0f + (top.empty() ? 0.0f : fs * 1.2f) + (cands.empty() ? 0.0f : fs * 1.25f);
    ImVec2 p(ime.InputPos.x, ime.InputPos.y + line + 4.0f);
    if (p.x + w > io.DisplaySize.x - 4.0f) p.x = io.DisplaySize.x - 4.0f - w;
    if (p.x < 4.0f) p.x = 4.0f;
    if (p.y + h > io.DisplaySize.y - 4.0f) p.y = ime.InputPos.y - h - 4.0f;      // no room below the box: above it

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(p.x + 3, p.y + 4), ImVec2(p.x + w + 3, p.y + h + 4), IM_COL32(0, 0, 0, 60), 8.0f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 250), 8.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(200, 200, 200, 255), 8.0f, 0, 1.5f);
    float y = p.y + pad;
    if (!top.empty()) { dl->AddText(font, fs, ImVec2(p.x + pad, y), IM_COL32(30, 30, 30, 255), top.c_str()); y += fs * 1.2f; }
    float x = p.x + pad;
    for (size_t i = 0; i < labels.size(); ++i) {
        const ImU32 col = (int)i == sel ? IM_COL32(235, 110, 20, 255) : IM_COL32(30, 30, 30, 255);
        dl->AddText(font, fs, ImVec2(x, y), col, labels[i].c_str());
        x += size(labels[i]).x + gap;
    }
}

} // namespace mgmp
