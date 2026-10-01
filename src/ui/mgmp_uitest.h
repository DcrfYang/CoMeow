// mgmp_uitest.h -- a scripted-input and screenshot harness for the menus. DEV ONLY.
//
// WHY IT EXISTS
//
// The menus are pixels drawn over a game, and "does it look right / does the
// click do the thing" cannot be answered from a log. The obvious way to look --
// a desktop screenshot plus a synthetic OS click -- is wrong on a machine
// somebody is using: it photographs whatever window is on top and clicks into it.
// (It did exactly that on 2026-09-29, into an unrelated chat window.)
//
// This does both INSIDE the process instead. Mouse and key events go straight
// into ImGui's input queue -- the OS never hears about them -- and a screenshot
// is a glReadPixels of the game's own back buffer, so it contains the game and
// the menus and nothing else.
//
// HOW TO USE
//
//   set "ui": { "test_harness": true } in mgmp.json (off by default; nothing
//   below runs otherwise), then write commands to mgmp_uitest.cmd beside the DLL.
//   The mod reads the file, deletes it, and runs the commands one frame at a
//   time. One command per line:
//
//     wait N            hold for N frames
//     move X Y          mouse to client pixel (X,Y)
//     click X Y         move, press, release (three frames)
//     press | release   the left button on its own
//     text ...          type the rest of the line (UTF-8)
//     key NAME          enter | esc | tab | backspace | delete | up | down
//     shot NAME         write shots\NAME.bmp (the full back buffer, BGR)
//     log ...           put a line in the mod log, to mark a step
//
// It never blocks the game and never touches anything the simulation reads.
#pragma once

namespace mgmp {

// Between ImGui_ImplWin32_NewFrame and ImGui::NewFrame, so an injected mouse
// position is the LAST one queued and wins over the backend's own poll.
void uitest_before_frame();

// After ImGui_ImplOpenGL3_RenderDrawData, while the frame is still in the back
// buffer.
void uitest_after_render();

} // namespace mgmp
