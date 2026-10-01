// mgmp_i18n.cpp -- see mgmp_i18n.h.
#include "mgmp_i18n.h"

#include <cstring>

namespace mgmp {
namespace {

// Until the game's setting has been read, Chinese: the language this mod was written in and the only one
// its players used before the table existed.
Lang g_lang = kLangZh;

bool starts(const char* s, const char* p) { return _strnicmp(s, p, strlen(p)) == 0; }

} // namespace

const char* tr(Tx id) {
    const int i = (int)id;
    if (i < 0 || i >= (int)Tx::COUNT) return "";
    const char* s = kTxTable[i][g_lang];
    return (s && s[0]) ? s : kTxTable[i][kLangEn];
}

const char* tr_class(uint8_t klass) {
    if (klass > 13) return tr(Tx::UNKNOWN);
    return tr((Tx)((int)Tx::CLASS_0 + klass));
}

bool i18n_set_lang_code(const char* code) {
    if (!code) return false;
    Lang l = kLangEn;
    // The codes the game writes are the combined.csv column names, with Chinese as "zh" (seen) or
    // "zh-cn" (the font table's name); Spanish is the csv's "sp". Anything unrecognised is English.
    if      (starts(code, "zh")) l = kLangZh;
    else if (starts(code, "ja")) l = kLangJa;
    else if (starts(code, "ko")) l = kLangKo;
    else if (starts(code, "ru")) l = kLangRu;
    else if (starts(code, "pt")) l = kLangPt;
    else if (starts(code, "it")) l = kLangIt;
    else if (starts(code, "de")) l = kLangDe;
    else if (starts(code, "fr")) l = kLangFr;
    else if (starts(code, "sp") || starts(code, "es")) l = kLangEs;
    if (l == g_lang) return false;
    g_lang = l;
    return true;
}

Lang i18n_lang() { return g_lang; }

} // namespace mgmp
