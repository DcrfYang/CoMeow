// mgmp_chat.cpp -- see mgmp_chat.h.
#include "mgmp_chat.h"

#include <windows.h>
#include <cstring>
#include <string>

#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_signal.h"

namespace mgmp {
namespace {

ChatLine g_hist[kChatHistory];
int      g_count = 0;                      // lines kept (the newest kChatHistory)
uint64_t g_show_until = 0;
volatile LONG g_open = 0;
volatile LONG g_enter_open = 0;            // Enter pressed while open
ULONGLONG g_eat_until = 0;                 // after an Enter the chat consumed: its key-up and WM_CHAR are eaten until then

// No control characters, no leading or trailing blanks, UTF-8 left whole.
void clean(const char* in, char* out, size_t cap) {
    std::string t;
    for (const unsigned char* c = (const unsigned char*)(in ? in : ""); *c; ++c) if (*c >= 0x20 && *c != 0x7F) t.push_back((char)*c);
    const size_t a = t.find_first_not_of(' ');
    const size_t b = t.find_last_not_of(' ');
    t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    if (t.size() >= cap) {
        t.resize(cap - 1);
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();      // not a sequence cut in half
        if (!t.empty() && ((unsigned char)t.back() & 0x80)) t.pop_back();
    }
    strncpy_s(out, cap, t.c_str(), _TRUNCATE);
}

void keep(const char* name, const char* text, bool self) {
    if (g_count == kChatHistory) { memmove(&g_hist[0], &g_hist[1], sizeof(ChatLine) * (kChatHistory - 1)); --g_count; }
    ChatLine& l = g_hist[g_count++];
    l = ChatLine{};
    strncpy_s(l.name, sizeof(l.name), name, _TRUNCATE);
    strncpy_s(l.text, sizeof(l.text), text, _TRUNCATE);
    l.self = self;
    g_show_until = GetTickCount64() + kChatShowMs;
}

} // namespace

bool chat_available() { return net_active() && net_peer_count() >= 2; }

void chat_on_message(uint8_t from, const ChatMsg& m) {
    char name[kChatNameMax], text[kChatTextMax];
    clean(m.name, name, sizeof(name));
    clean(m.text, text, sizeof(text));
    if (!text[0]) return;
    if (!name[0]) _snprintf_s(name, sizeof(name), _TRUNCATE, "#%u", (unsigned)from);
    keep(name, text, false);
    log_line("CHAT", "%s: %s", name, text);
}

bool chat_submit(const char* text) {
    ChatMsg m;
    clean(text, m.text, sizeof(m.text));
    if (!m.text[0]) return false;
    char name[kChatNameMax];
    clean(signal_name(), name, sizeof(name));
    if (!name[0]) _snprintf_s(name, sizeof(name), _TRUNCATE, "#%u", (unsigned)net_self());
    strncpy_s(m.name, sizeof(m.name), name, _TRUNCATE);
    keep(m.name, m.text, true);
    log_line("CHAT", "%s (me): %s", m.name, m.text);
    if (!net_send_chat(m)) log_line_lvl(LogLevel::Warn, "CHAT", "!! the line could not be sent");
    return true;
}

int chat_count() { return g_count; }
const ChatLine& chat_line(int index) {
    static const ChatLine kNone;
    return index >= 0 && index < g_count ? g_hist[index] : kNone;
}
uint64_t chat_show_until() { return g_show_until; }
void chat_reset() { g_count = 0; g_show_until = 0; InterlockedExchange(&g_open, 0); InterlockedExchange(&g_enter_open, 0); }

bool chat_input_open() { return g_open != 0; }
void chat_set_input_open(bool open) { InterlockedExchange(&g_open, open ? 1 : 0); }

bool chat_enter_key(bool other_text_focus) {
    if (g_open) {
        InterlockedExchange(&g_enter_open, 1);
        g_eat_until = GetTickCount64() + 400;
        return true;
    }
    if (other_text_focus || !chat_available()) return false;
    InterlockedExchange(&g_open, 1);
    g_eat_until = GetTickCount64() + 400;
    return true;
}

bool chat_take_enter_while_open() { return InterlockedExchange(&g_enter_open, 0) != 0; }

bool chat_swallow_enter_remains(uint32_t msg, uintptr_t wparam) {
    if (!g_eat_until || GetTickCount64() > g_eat_until) return false;
    if ((msg == WM_KEYUP && wparam == VK_RETURN) || (msg == WM_CHAR && wparam == 13)) return true;
    return false;
}

} // namespace mgmp
