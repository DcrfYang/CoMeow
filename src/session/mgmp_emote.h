// mgmp_emote.h -- the chat's emotes (0.1.70): The Binding of Isaac's online emotes, sent as a line of chat.
//
// No new message: a line whose WHOLE text is one token is shown as that emote -- "[:Heart:]", or a die "[:D6=4:]" / "[:D20=17:]". The sender rolls a die
// (outside every game RNG), the text carries the face, so every screen shows the same result. A game whose assets/emotes is missing (or an older mod) shows the
// token as text. The pictures are NOT in git (CoMeow/.gitignore ignores /assets/): scratch/make_emote_atlas.py builds assets/emotes/emotes.png -- a grid of
// 32x32 cells, kEmoteAtlasCols per row, in the order below -- and bubble.png from 游戏资源/以撒的结合表情资源, and packager/make_folder.py ships them.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

constexpr int kEmoteCount = 21;              // what the picker offers: 19 pictures, then the D6 and the D20
constexpr int kEmoteD6 = 19, kEmoteD20 = 20;
constexpr int kEmoteAtlasCols = 8, kEmoteAtlasCells = 72;
constexpr unsigned kEmoteRollMs = 1300;      // a die rolls this long after its line arrives, then shows its face

struct Emote {
    int pick = -1;                           // 0..kEmoteCount-1
    int roll = 0;                            // a die's face: 1..6 / 1..20; 0 for a picture
};

const char* emote_name(int pick);            // "Heart", "D6", ... (nullptr out of range)
// The whole text (no blanks around it) is one well-formed token.
bool emote_parse(const char* text, Emote& out);
// The token of `e`; false when `e` is not valid (a die without a face in range, a picture with one).
bool emote_token(const Emote& e, char* out, size_t cap);
// The atlas cell to draw `age_ms` after the line arrived: the picture; for a die its roll first, then its face.
int emote_cell(const Emote& e, uint64_t age_ms);
// The cell the picker shows for `pick`.
int emote_icon_cell(int pick);

} // namespace mgmp
