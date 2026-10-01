// mgmp_i18n.h -- the mod's UI text follows the GAME's language setting (2026-10-01).
//
// The game keeps its language in settings.txt beside the account's saves folder
// (`current_language zh`), and changes it at runtime from its own options screen, writing the file at
// once. The menu polls that line about once a second (menu_draw) and hands it to i18n_set_lang_code; every
// string the mod draws goes through tr(). The table covers all ten languages the game ships (the columns
// of data/text/combined.csv); a code this build does not know falls back to English, as the game does.
//
// THE TABLE IS GENERATED: scratch/i18n_src.py is the source, scratch/gen_i18n.py writes
// mgmp_i18n_table.h (ASCII only, so it compiles with or without /utf-8) and refuses a translation whose
// printf placeholders differ from the English one.
#pragma once

#include "mgmp_i18n_table.h"

namespace mgmp {

enum Lang : int { kLangEn = 0, kLangEs, kLangFr, kLangDe, kLangIt, kLangPt, kLangRu, kLangKo, kLangJa, kLangZh };

// The string in the current language. Never null.
const char* tr(Tx id);

// A class name (kClassKeys order), in the current language; "unknown" for 255.
const char* tr_class(uint8_t klass);

// `code` is the game's own value of current_language ("zh", "en", "ja", "pt-br", ...). True when the
// language changed.
bool i18n_set_lang_code(const char* code);
Lang i18n_lang();

} // namespace mgmp
