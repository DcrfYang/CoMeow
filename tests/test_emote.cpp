// test_emote.cpp -- the chat emote tokens (mgmp_emote, 0.1.70).
#include <cstdio>
#include <cstring>

#include "mgmp_emote.h"

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { ++g_fail; printf("FAIL: %s\n", what); }
}

int main() {
    char tok[64];
    // every picture and every face of both dice round-trips
    for (int i = 0; i < kEmoteCount; ++i) {
        const int faces = i == kEmoteD6 ? 6 : i == kEmoteD20 ? 20 : 0;
        for (int r = faces ? 1 : 0; r <= faces; ++r) {
            Emote e; e.pick = i; e.roll = r;
            check(emote_token(e, tok, sizeof(tok)), "a valid emote has a token");
            check(strlen(tok) < 64, "a token fits a chat line");
            Emote back;
            check(emote_parse(tok, back) && back.pick == i && back.roll == r, "a token parses back");
            const int cell = emote_cell(e, kEmoteRollMs);
            check(cell >= 0 && cell < kEmoteAtlasCells, "a settled emote has a cell");
            for (unsigned age = 0; age < kEmoteRollMs; age += 17) {
                const int c = emote_cell(e, age);
                check(c >= 0 && c < kEmoteAtlasCells, "a rolling die has a cell");
            }
        }
        const int ic = emote_icon_cell(i);
        check(ic >= 0 && ic < kEmoteAtlasCells, "the picker's icon is in the atlas");
    }
    // the faces land on the right cells (the atlas order of scratch/make_emote_atlas.py)
    { Emote e; e.pick = kEmoteD6; e.roll = 1; check(emote_cell(e, 99999) == 20, "D6 face 1"); e.roll = 6; check(emote_cell(e, 99999) == 25, "D6 face 6"); }
    { Emote e; e.pick = kEmoteD20; e.roll = 1; check(emote_cell(e, 99999) == 42, "D20 face 1"); e.roll = 20; check(emote_cell(e, 99999) == 61, "D20 face 20"); }
    { Emote e; e.pick = 8; check(emote_cell(e, 0) == 8 && strcmp(emote_name(8), "Heart") == 0, "Heart is cell 8"); }
    // invalid emotes have no token
    { Emote e; e.pick = kEmoteD6; e.roll = 7; check(!emote_token(e, tok, sizeof(tok)), "a D6 never shows 7"); }
    { Emote e; e.pick = kEmoteD6; e.roll = 0; check(!emote_token(e, tok, sizeof(tok)), "a D6 needs a face"); }
    { Emote e; e.pick = 3; e.roll = 2; check(!emote_token(e, tok, sizeof(tok)), "a picture has no face"); }
    { Emote e; e.pick = kEmoteCount; check(!emote_token(e, tok, sizeof(tok)), "out of range"); }
    // ordinary text and near-misses stay text
    const char* text[] = { "", "hello", "[:Heart:] hi", " [:Heart:]", "[:Heart:] ", "[:heart:]", "[:Heart]", "[Heart:]", "[:Heart=1:]", "[:D6:]",
                           "[:D6=0:]", "[:D6=7:]", "[:D6=03:]", "[:D20=21:]", "[:D20=-1:]", "[:D20=1x:]", "[::]", "[:=:]", "[:D6=:]", "[:Nicalis:]",
                           "[:QuestionMarkQuestionMarkQuestion:]" };
    for (const char* t : text) { Emote e; check(!emote_parse(t, e) && e.pick == -1, t); }
    { Emote e; check(!emote_parse(nullptr, e), "null is text"); }
    if (g_fail) { printf("test_emote: %d FAILED\n", g_fail); return 1; }
    printf("test_emote: all passed\n");
    return 0;
}
