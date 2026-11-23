// test_saveplace.cpp -- where a save stands (mgmp_saveplace, 0.1.249): the decision from the save's properties and its chapter_map row, the labels,
// and the real saves kept beside the project (存档备份: the fixture of a sewers run, if present). Build and run: _buildcheck/test_saveplace.bat
#include "../src/session/mgmp_saveplace.h"
#include "../src/ui/mgmp_i18n.h"
#include "../src/core/mgmp_log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mgmp;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while(0)

namespace mgmp {
void log_line(const char*, const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
void log_line_lvl(LogLevel, const char*, const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
}

// A chapter_map row as the game's serializer (0x140228E00) writes it.
struct Node { const char* name; uint32_t col; uint8_t done; const char* level; };
static void put_u32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void put_u64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void put_str(std::vector<uint8_t>& b, const char* s) { put_u64(b, strlen(s)); b.insert(b.end(), s, s + strlen(s)); }
static std::vector<uint8_t> chapter_map(const char* file, const char* cur, uint32_t cur_col, const std::vector<Node>& nodes) {
    std::vector<uint8_t> b(32, 0x5A);
    put_str(b, file); put_u32(b, 10); put_u32(b, 1); b.push_back(0);
    put_str(b, cur); put_u32(b, cur_col);
    put_u64(b, nodes.size());
    for (const Node& n : nodes) {
        put_str(b, n.name); put_u32(b, n.col); b.push_back(n.done);
        for (int i = 0; i < 32; ++i) b.push_back((uint8_t)i);
        put_str(b, n.level);
    }
    return b;
}
static std::string text(const SavePlaceInfo& p) { char t[160]; saveplace_text(p, t, sizeof(t)); return t; }

int main() {
    // the warehouse, the gear page, the chapter page
    CHECK(saveplace_decode(true, 0, 0, nullptr).place == SavePlace::House);
    CHECK(saveplace_decode(true, -1, -1, nullptr).place == SavePlace::House);         // a save that never had the property
    CHECK(saveplace_decode(true, 1, 0, nullptr).place == SavePlace::Gear);
    const std::vector<Node> alley = { {"start",0,1,"unknown"}, {"battle",0,0,"unknown"}, {"battle",1,0,"unknown"}, {"boss",0,0,"radicalrat"} };
    auto ch = chapter_map("alley.gon", "start", 0, alley);
    SavePlaceInfo c = saveplace_decode(true, 1, 0, &ch);
    CHECK(c.place == SavePlace::Chapter && strcmp(c.map, "alley") == 0);
    CHECK(text(saveplace_decode(true, 0, 0, nullptr)) == "仓库");
    CHECK(text(saveplace_decode(true, 1, 0, nullptr)) == "选择装备");
    CHECK(text(c) == "章节选择");

    // on the map: the starting point, a finished node, a node quit inside, the map-specific ones
    SavePlaceInfo s = saveplace_decode(true, 1, 1, &ch);
    CHECK(s.place == SavePlace::Map && s.start && s.done);
    CHECK(text(s) == "小巷 · 出发点");
    auto b1 = chapter_map("sewers.gon", "battle", 1, { {"start",0,1,"unknown"}, {"battle",0,1,"unknown"}, {"battle",1,1,"unknown"} });
    SavePlaceInfo m = saveplace_decode(true, 1, 1, &b1);
    CHECK(m.place == SavePlace::Map && !m.start && m.done && strcmp(m.kind, "battle") == 0);
    CHECK(text(m) == "下水道 · 普通战斗");
    auto b2 = chapter_map("sewers.gon", "battle", 1, { {"battle",0,1,"unknown"}, {"battle",1,0,"unknown"} });
    CHECK(text(saveplace_decode(true, 1, 1, &b2)) == "下水道 · 普通战斗（未完成）");   // the pause menu's save inside a battle
    struct { const char* map; const char* kind; const char* want; } special[] = {
        { "desert.gon", "shop_water", "沙漠 · 水商店" },        { "desert.gon", "shop_cheapwater", "沙漠 · 廉价水商店" },
        { "lab.gon", "abilityneedle", "实验室 · 获取注射剂" },   { "lab.gon", "quest_event", "实验室 · 剧情事件" },
        { "lab.gon", "miniboss_event", "实验室 · 变异体事件" },  { "future.gon", "time_machine", "未来 · 时间机器" },
        { "meatworld.gon", "mw_battle1", "悸动领域 · 普通战斗" }, { "meatworld.gon", "mw_altar", "悸动领域 · 祭坛" },
        { "meatworld.gon", "mw_earlyhome", "悸动领域 · 回家" },   { "bunker.gon", "furniturebox2", "地堡 · 家具箱" },
        { "alley.gon", "hard_initial", "小巷 · 困难战斗" },       { "endoftime.gon", "c4hard", "无限 · 困难战斗" },
        { "alley.gon", "exit_desert", "小巷 · 出口" },            { "sewers.gon", "exit0", "下水道 · 出口" },
        { "caves.gon", "miniboss", "洞穴 · 小头目" },              { "caves.gon", "boss", "洞穴 · 大头目" },
        { "alley.gon", "event", "小巷 · 随机事件" },               { "alley.gon", "shop", "小巷 · 商店" },
        { "theend.gon", "treasure_radiated", "遥远未来 · 辐射宝物" }, { "crater.gon", "weather_event", "陨石坑 · 天气事件" },
    };
    for (const auto& t : special) {
        auto row = chapter_map(t.map, t.kind, 0, { {t.kind,0,1,"unknown"} });
        const std::string got = text(saveplace_decode(true, 1, 1, &row));
        if (got != t.want) printf("  %s/%s -> '%s' (want '%s')\n", t.map, t.kind, got.c_str(), t.want);
        CHECK(got == t.want);
    }

    // the fallback: unknown, with what was wrong (the log line names it)
    auto odd = chapter_map("sewers.gon", "mystery_node", 0, { {"mystery_node",0,1,"unknown"} });
    SavePlaceInfo u = saveplace_decode(true, 1, 1, &odd);
    CHECK(u.place == SavePlace::Unknown && strstr(u.why, "mystery_node") && text(u) == "下水道 · 未知");
    CHECK(saveplace_decode(false, 0, 0, nullptr).place == SavePlace::Unknown);
    CHECK(saveplace_decode(true, 1, 1, nullptr).place == SavePlace::Unknown);                  // on the map without a map
    CHECK(text(saveplace_decode(true, 1, 1, nullptr)) == "未知");
    std::vector<uint8_t> cut(b1.begin(), b1.begin() + b1.size() / 2);
    CHECK(saveplace_decode(true, 1, 1, &cut).place == SavePlace::Unknown);                     // a torn row is never read past its end
    auto lost = chapter_map("sewers.gon", "battle", 7, { {"battle",0,1,"unknown"} });
    CHECK(saveplace_decode(true, 1, 1, &lost).place == SavePlace::Unknown);                    // the current node is not in the list
    CHECK(saveplace_decode(true, 2, 0, nullptr).place == SavePlace::Unknown);

    // a real save of a sewers run (the project's fixture), when it is there
    const std::wstring fixture = L"d:\\project\\Mewgenics_Mutiplayer\\fixture-slot1.sav.bak";
    if (GetFileAttributesW(fixture.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SavePlaceInfo f = saveplace_read(fixture, false);
        printf("fixture: %s\n", text(f).c_str());
        CHECK(f.place == SavePlace::Map && strcmp(f.map, "sewers") == 0 && text(f) == "下水道 · 随机事件");
        CHECK(saveplace_read(fixture).place == SavePlace::Map && saveplace_read(fixture).place == SavePlace::Map);   // cached the second time
    }
    printf("test_saveplace: %u checks, 0 failures\n", checks);
    return 0;
}
