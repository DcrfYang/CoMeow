// mgmp_gametext.h -- THE GAME'S OWN TEXT, in the language and the language pack the player chose (0.1.117).
//
// The mod keeps its own table of words (ui/mgmp_i18n) for what it says itself. The NAMES of things that exist in the game -- the cat classes first -- are the game's
// words, not the mod's: the mod's fixed Chinese ones ("异能者") were its translator's, while the game's own table says "灵能士" and a language pack the player installed
// says what the pack says. So the mod asks the game: Texts::get(key) is the function the game itself localizes a key with (Character's name setup calls it for a cat's
// type), and it answers in whatever the player has loaded.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// The text of a key such as "CAT_CLASS_PSYCHIC_NAME" as UTF-8 (zero-width characters the game puts into its words as line-break hints are taken out). False when the
// table or the call is not available, or the key has no text (the game answers with the key itself).
bool gametext_lookup(const char* key, char* out, size_t cap);

// A cat class by the index of net/mgmp_catbrief.h's kClassKeys (0 Fighter .. 13 Jester) in the game's words, or nullptr when the game cannot say (the caller falls back to
// its own table). Cached; read again every few seconds, so a language switched in the game's settings follows.
const char* gametext_class_name(uint8_t klass);

} // namespace mgmp
