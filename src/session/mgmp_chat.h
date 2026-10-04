// mgmp_chat.h -- the room's text chat (proto 81). Enter opens the input (and the whole history), Enter again sends what was typed and every screen shows the history for ten seconds, then fades it.
//
// This module is the data and the wire: the history, the lines that arrive, the input-open state and the Enter key's decision. The drawing lives in mgmp_menu (draw_chat); the key is read in the overlay's
// window procedure (mgmp_ui). Nothing here touches the game or the simulation: a line of chat is text that is shown, never an action.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

constexpr int kChatHistory = 200;
constexpr unsigned kChatShowMs = 10000;     // how long the history stays up after a line is sent or arrives
constexpr unsigned kChatFadeMs = 1200;      // and the fade after that

struct ChatLine {
    char name[kChatNameMax] = {};
    char text[kChatTextMax] = {};
    bool self = false;
};

// A session with at least one other player: there is somebody to talk to.
bool chat_available();

// A line arrived from another player.
void chat_on_message(uint8_t from, const ChatMsg& m);
// This player's own line (the UI sends it with Enter): kept in the history and sent to the others. Empty after trimming = nothing happens. Returns whether a line was sent.
bool chat_submit(const char* text);

int  chat_count();
const ChatLine& chat_line(int index);        // 0 = the oldest kept
uint64_t chat_show_until();                  // GetTickCount64 value at which the ten seconds end (0 = nothing to show)
void chat_reset();                           // a new room: the old history goes

// The input box.
bool chat_input_open();
void chat_set_input_open(bool open);
// The Enter key went down (called from the window procedure, before the game sees it). Returns true when the chat took it -- the key must not reach the game. `other_text_focus`: some text box already has the keyboard.
bool chat_enter_key(bool other_text_focus);
// The frame asks: was Enter pressed while the input was open (send what is typed and close)?
bool chat_take_enter_while_open();
// True while the chat is eating the rest of an Enter press (its key-up and character), so the game never sees half of one.
bool chat_swallow_enter_remains(uint32_t msg, uintptr_t wparam);

} // namespace mgmp
