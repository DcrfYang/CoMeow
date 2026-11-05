// mgmp_imeui.h -- THE IME'S CANDIDATES DRAWN INSIDE THE GAME (0.1.175; the player's report 2026-11-04: "typing pinyin, the game covers the IME's window -- a
// screenshot shows it"). On a laptop whose NVIDIA driver presents the game's OpenGL frames straight to the screen (layered on a DXGI flip swapchain), the
// desktop's composition is skipped while the game covers the monitor: the IME's candidate window exists -- a screenshot, taken from the composition,
// has it -- but is never on the screen. 0.1.172 (input context, not topmost, show bits) could not help: nothing ABOVE the game is shown in that mode.
//
// So while a text box of ours has the keyboard the candidate list is asked for the way games ask for it: TSF's UI-less mode. A UI element sink on the
// window's thread is told when the text service (Sogou, Microsoft Pinyin ...) wants to show its candidate or reading window; for a candidate list it is
// told NOT to show it (BeginUIElement *pbShow = FALSE) and the list is read (ITfCandidateListUIElement) and drawn by ImGui at the text box's caret, with
// the composition string (WM_IME_COMPOSITION GCS_COMPSTR). Choosing still goes through the IME (number keys, space, arrows). A text service that ignores
// UI-less mode keeps drawing its own window, as before. Grep IMEUI.
#pragma once
#include <windows.h>

namespace mgmp {

// Window thread, once (first time a text box of ours takes the keyboard): COM + the TSF thread manager + the UI element sink. False when TSF is not there.
bool imeui_init(HWND hwnd);
// Window thread: whether candidates are taken over now (a text box of ours has the keyboard).
void imeui_set_active(bool on);
// Window thread, from WM_IME_COMPOSITION / WM_IME_ENDCOMPOSITION: the composition string as the IME has it now.
void imeui_on_composition(HWND hwnd, LPARAM lp);
void imeui_end_composition();
// Whether UI-less mode has been seen working (a candidate list was taken over at least once): the composition window can then be hidden too.
bool imeui_takes_over();
// Render thread, inside the ImGui frame: the composition and the candidate list at the caret, if any.
void imeui_draw();

} // namespace mgmp
