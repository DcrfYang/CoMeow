// mgmp_treasure.h -- EXPERIMENTAL (ui.experimental, the HOST's setting): the shared treasure chest.
//
// The game's treasure node is a shop (data/shops/combat_shops.gon, TreasureEasy & co.: `treasure_room true`, one free item) whose chest, once
// clicked, runs Shop::initShop's stock half (0x140796070) from its animation's "initShop" event: the stock is rolled and EVERY item in it is taken
// at once (Shop::ObtainItem). Each player got their own item. With the experimental features on, in a room:
//
//   - the stock half is held back (T_ShopStock). This peer rolls ONE CANDIDATE PER PLAYER with the game's own roller (sub_140795BB0), each from a
//     stream derived from the node seed and its slot number -- the same on every peer, whenever its chest is opened -- and, on a client, with the
//     host's unlock answers. So every screen shows the same items;
//   - a panel lists them; the player clicks one and asks the HOST, which keeps the ledger: a slot goes to the first claim, and a player takes one;
//   - the item granted to this player is moved into the shop's (empty) stock and the held-back stock half runs: the game shows it and takes it, and
//     the room closes itself as it always does. "Skip" runs it with the stock marked as given (Shop+0x7C), so nothing is taken.
//
// The items still go to each player's own inventory (per-player stash): the ledger only decides who may take which.
#pragma once

#include <cstdint>
#include <cstring>
#include "mgmp_proto.h"   // TreasureMsg, kTreasureMaxSlots, kTreasureNobody

namespace mgmp {

// THE ITEM'S DESCRIPTION AS PLAIN LINES (0.1.87, "show the item's effect under it when the mouse is on it"). In: the text the game's own builder
// returns (UTF-8), with the markup its Flash text understands. Out: lines for the panel under the hovered item --
//   - zero-width spaces (U+200B, the line-break hints of the Chinese and Japanese text) go; &nbsp; and U+00A0 become spaces;
//   - [img:x] (a stat's icon) becomes the word `img_word` gives for x (null: dropped);
//   - [s:...] ... [/s] is the game's small print: a line that starts inside it is marked with a leading '\x01';
//   - any other [tag] and a {placeholder} the builder left are dropped; a '[' or '{' that closes nowhere near is kept.
// Pure (tests/test_treasure.cpp). Returns the length written (out is always terminated when cap > 0).
inline size_t treasure_desc_plain(const char* in, char* out, size_t cap, const char* (*img_word)(const char* name, size_t len)) {
    if (!out || !cap) return 0;
    size_t w = 0;
    bool fine_print = false, line_text = false;
    auto put = [&](const char* s, size_t n) {
        for (size_t i = 0; i < n && w + 1 < cap; ++i) {
            if (s[i] == '\n') { out[w++] = '\n'; line_text = false; continue; }
            if (!line_text) {
                line_text = true;
                if (fine_print && w + 2 < cap) out[w++] = '\x01';
            }
            out[w++] = s[i];
        }
    };
    for (const char* p = in ? in : ""; *p;) {
        const unsigned char c = (unsigned char)*p;
        if (c == '\r') { ++p; continue; }
        if (c == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x8B) { p += 3; continue; }      // U+200B
        if (c == 0xC2 && (unsigned char)p[1] == 0xA0) { put(" ", 1); p += 2; continue; }                          // U+00A0
        if (!strncmp(p, "&nbsp;", 6)) { put(" ", 1); p += 6; continue; }
        if (c == '[' || c == '{') {
            const char close = c == '[' ? ']' : '}';
            const char* e = p + 1;
            while (*e && *e != close && *e != '\n' && e - p < 64) ++e;
            if (*e == close) {
                const char* t = p + 1;
                const size_t n = (size_t)(e - t);
                if (c == '[' && n > 4 && !strncmp(t, "img:", 4)) {
                    const char* word = img_word ? img_word(t + 4, n - 4) : nullptr;
                    if (word) put(word, strlen(word));
                } else if (c == '[' && n == 4 && !strncmp(t, "nbsp", 4)) put(" ", 1);         // the builder's own non-breaking space (0.1.87 live)
                else if (c == '[' && n >= 2 && !strncmp(t, "s:", 2)) fine_print = true;
                else if (c == '[' && n == 2 && !strncmp(t, "/s", 2)) fine_print = false;
                p = e + 1;
                continue;
            }
        }
        put(p, 1);
        ++p;
    }
    while (w && (out[w - 1] == '\n' || out[w - 1] == ' ')) --w;               // no empty last line
    out[w] = 0;
    return w;
}

// The host's ledger of one node. Pure (tests/test_treasure.cpp).
struct TreasureLedger {
    uint64_t node = 0;
    uint8_t  taker[kTreasureMaxSlots];
    TreasureLedger() { reset(0); }
    void reset(uint64_t n) { node = n; for (uint8_t& t : taker) t = kTreasureNobody; }
    enum Result { Granted, AlreadyMine, SlotTaken, HoldsAnother, Bad };
    Result claim(uint64_t n, uint8_t peer, uint8_t slot) {
        if (!n || n != node || slot >= kTreasureMaxSlots || peer == kTreasureNobody) return Bad;
        if (taker[slot] == peer) return AlreadyMine;
        if (taker[slot] != kTreasureNobody) return SlotTaken;
        for (uint8_t t : taker) if (t == peer) return HoldsAnother;
        taker[slot] = peer;
        return Granted;
    }
};

// T_ShopStock: true when the call was taken over (the original must NOT run now); `original` is the trampoline, kept to run the stock half later.
bool treasure_on_stock(void* shop, void (*original)(void*));
void treasure_on_message(uint8_t from, const TreasureMsg& m);
// HOST, as it enters a treasure node (before the node message): its party luck goes to every player (proto 101; the chest is rolled with it).
void treasure_publish_luck(uint64_t node);
// Every frame (FrameBegin): runs the granted take / the skip, and drops a picker whose shop is gone.
void treasure_tick();

// What the panel shows. False when no picker is up.
struct TreasureView {
    uint8_t n = 0;
    char    name[kTreasureMaxSlots][96] = {};
    char    taker[kTreasureMaxSlots][48] = {};   // "" = free
    bool    mine[kTreasureMaxSlots] = {};
    int     pending = -1;                        // the slot asked for, waiting for the host
    bool    busy = false;                        // a take or a skip is under way
    bool    mismatch = false;                    // the host's items differ from this screen's
    // THE ROOM'S OWN ROW (the items side by side in the treasure room): the items still in it (`shown`; a taken one is not), their centres on
    // the 1920x1080 stage and half the hit area's size. `building`: the row is being made (nothing to draw yet); neither: the panel.
    bool    native = false, building = false;
    bool    shown[kTreasureMaxSlots] = {};
    float   cx[kTreasureMaxSlots] = {}, cy[kTreasureMaxSlots] = {}, half = 0;
    // 0.1.87: each item's description, the game's own text as plain lines (treasure_desc_plain; "" = none); shown under the hovered item
    char    desc[kTreasureMaxSlots][1024] = {};
};
bool treasure_view(TreasureView& out);
void treasure_take(uint8_t slot);   // from the panel
void treasure_skip();
void treasure_hover(int slot);   // the item under the mouse (-1: none): lifted, and its name in the room's banner

} // namespace mgmp
