// Two real processes, Winsock loopback (proto 98): the host's board as a digest, a client's request for the full board, the answer from the receive thread, the stream switch.
#include "mgmp_net.h"
#include "mgmp_log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace mgmp;
namespace mgmp { void log_line(const char*, const char*, ...) {} void log_line_lvl(LogLevel, const char*, const char*, ...) {} }

static const uint64_t kBattle = 0x00C0FFEE12345678ull;
static const uint32_t kUnits = 12;

static void make_board(uint32_t turn, BoardMsg (&chunks)[2], uint32_t& n, BoardSumMsg& sum) {
    n = 0;
    for (uint32_t first = 0; first < kUnits; first += kBoardChunk) {
        BoardMsg& m = chunks[n++];
        m = BoardMsg{};
        m.battle = kBattle; m.turn = turn; m.cats = kUnits; m.total = kUnits; m.first = first;
        m.count = (uint8_t)((kUnits - first) < kBoardChunk ? (kUnits - first) : kBoardChunk);
        for (int k = 0; k < 4; ++k) m.rng[k] = 0x5151000000000000ull + turn * 16 + k;
        for (uint32_t k = 0; k < m.count; ++k) {
            BoardUnit& u = m.units[k];
            u.index = (uint8_t)(first + k); u.ident = 0xAB00 + u.index; u.hp = (int32_t)(100 + u.index + turn); u.maxhp = 200; u.flags = kBoardLinked;
            u.tx = (int32_t)u.index % 5; u.ty = (int32_t)u.index / 5; u.key_a = 7; u.key_b = (int32_t)u.index; u.carried = -1;
            strcpy(u.def, "Leaper");
        }
    }
    sum = BoardSumMsg{};
    sum.battle = kBattle; sum.turn = turn; sum.cats = kUnits;
    memcpy(sum.rng, chunks[0].rng, sizeof(sum.rng));
    for (uint32_t c = 0; c < n; ++c) for (uint32_t k = 0; k < chunks[c].count; ++k) sum.digest[chunks[c].units[k].index] = board_unit_digest(chunks[c].units[k]);
}

static bool wait_for(bool (*pred)(), DWORD ms) {
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < ms) {
        if (pred()) return true;
        NetMsg m{};
        while (net_poll(m)) net_msg_release(m);
        Sleep(2);
    }
    return pred();
}

static BoardAssembled g_box;
static BoardSumMsg g_sum;
static bool full0() { return net_host_board(kBattle, 0, g_box); }
static bool sum1()  { return net_host_boardsum(kBattle, 1, g_sum); }
static bool full1() { return net_host_board(kBattle, 1, g_box); }
static bool full2() { return net_host_board(kBattle, 2, g_box); }
static bool sum2()  { return net_host_boardsum(kBattle, 2, g_sum); }

static int client(unsigned port) {
    if (!net_join("127.0.0.1", (uint16_t)port)) return 20;
    auto ack = [](uint32_t turn) { HashMsg h{}; h.battle_id = kBattle; h.turn = turn; return net_send_hash(h); };
    // turn 0: the whole board, to everyone
    if (!wait_for(full0, 10000)) return 21;
    if (g_box.total != kUnits || g_box.unit[11].hp != 111 || strcmp(g_box.unit[3].def, "Leaper") != 0) return 22;
    if (!ack(0)) return 23;
    // turn 1: only the digest comes; the full board is not here until it is asked for
    if (!wait_for(sum1, 10000)) return 24;
    if (net_host_board(kBattle, 1, g_box)) return 25;
    BoardMsg chunks[2]; uint32_t n = 0; BoardSumMsg mine;
    make_board(1, chunks, n, mine);
    if (g_sum.cats != kUnits || memcmp(g_sum.digest, mine.digest, sizeof(uint32_t) * kUnits) != 0 || memcmp(g_sum.rng, mine.rng, sizeof(mine.rng)) != 0) return 26;
    BoardReqMsg q{}; q.battle = kBattle; q.turn = 1;
    if (!net_send_boardreq(q)) return 27;
    if (!wait_for(full1, 10000)) return 28;
    if (g_box.unit[11].hp != 112 || g_box.rng[0] != mine.rng[0]) return 29;
    // ask to be streamed from now on, then let the host publish turn 2
    q.flags = kBoardReqStream;
    if (!net_send_boardreq(q) || !ack(1)) return 30;
    if (!wait_for(full2, 10000) || !wait_for(sum2, 10000)) return 31;     // streamed: the full board came without asking
    if (g_box.unit[0].hp != 102) return 32;
    if (!net_board_seen_past(kBattle, 1) || net_board_seen_past(kBattle, 2)) return 33;
    if (!ack(2)) return 34;
    Sleep(300);
    net_shutdown();
    return 0;
}

static int host(unsigned port) {
    if (!net_host((uint16_t)port)) return 2;
    wchar_t exe[32768]; GetModuleFileNameW(nullptr, exe, 32768);
    wchar_t cmd[33000]; swprintf_s(cmd, L"\"%ls\" --client %u", exe, port);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &child)) return 3;
    CloseHandle(child.hThread);
    int result = 4;
    uint32_t published = 0;     // boundaries published so far
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (GetTickCount64() < deadline) {
        if (published == 0 && net_peer_count() == 2) {
            BoardMsg chunks[2]; uint32_t n = 0; BoardSumMsg sum;
            make_board(0, chunks, n, sum);
            if (net_board_publish(chunks, n, nullptr) != 1) { result = 5; break; }
            published = 1;
        }
        NetMsg m{};
        bool stop = false;
        while (net_poll(m)) {
            if (m.type == MSG_HASH && m.hash.battle_id == kBattle) {
                const uint32_t t = m.hash.turn;
                if (t == 2) { result = 0; stop = true; }
                else if (t + 1 == published) {
                    BoardMsg chunks[2]; uint32_t n = 0; BoardSumMsg sum;
                    make_board(published, chunks, n, sum);
                    if (net_board_publish(chunks, n, &sum) != 1) { result = 6; stop = true; }
                    ++published;
                }
            }
            net_msg_release(m);
        }
        if (stop) break;
        Sleep(1);
    }
    if (WaitForSingleObject(child.hProcess, 20000) != WAIT_OBJECT_0) result = 7;
    DWORD code = 1; GetExitCodeProcess(child.hProcess, &code);
    if (code && !result) result = 100 + (int)code;
    CloseHandle(child.hProcess);
    net_shutdown();
    printf("two-process board digest/request/stream: %s (code %d)\n", result ? "FAILED" : "PASSED", result);
    return result;
}

int main(int argc, char** argv) {
    if (argc > 2) return client((unsigned)atoi(argv[2]));
    return host(43000 + GetCurrentProcessId() % 20000);
}
