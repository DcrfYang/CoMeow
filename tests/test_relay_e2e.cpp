// The server-relay carrier end to end, minus the game: one process hosts the game port and bridges the pipes the lobby server pairs, the other dials
// "relay:<token>"; a CATDATA goes each way over the pipe. Driven by scratch/relay_e2e.py (which plays the lobby).
//   test_relay_e2e host <server ip> <server port> <game port> <host token>      reads "pipe <id>" lines on stdin
//   test_relay_e2e client <server ip> <server port> <joiner token>
#include "mgmp_net.h"
#include "mgmp_log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace mgmp;
namespace mgmp { void log_line(const char* tag, const char* f, ...) { (void)tag; (void)f; } }

static std::string g_tok;
static unsigned g_port = 0;

static DWORD WINAPI stdin_thread(LPVOID) {
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned id = 0;
        if (sscanf(line, "pipe %u", &id) == 1) net_relay_serve(g_tok.c_str(), id, (uint16_t)g_port);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 5) return 9;
    const bool host = !strcmp(argv[1], "host");
    net_relay_set_server(argv[2], (uint16_t)atoi(argv[3]));
    if (host) {
        if (argc < 6) return 9;
        g_port = (unsigned)atoi(argv[4]);
        g_tok = argv[5];
        if (!net_host((uint16_t)g_port)) return 2;
        CloseHandle(CreateThread(nullptr, 0, stdin_thread, nullptr, 0, nullptr));
    } else {
        std::string cand = std::string("relay:") + argv[4];
        if (!net_join(cand.c_str(), 0)) return 3;
    }
    bool sent = false, got = false;
    const ULONGLONG deadline = GetTickCount64() + 25000;
    int result = 4;
    ULONGLONG got_at = 0;
    while (GetTickCount64() < deadline) {
        if (!sent && net_peer_count() == 2) {
            uint8_t body[3] = { net_self(), 42, 99 };
            CatDataMsg c{}; c.id = 0x70000001ull + ((uint64_t)net_self() << 24); c.data = body; c.size = 3; c.hash = 0x1234;
            sent = net_send_catdata(c);
        }
        NetMsg m{};
        while (net_poll(m)) {
            if (m.type == MSG_CATDATA && m.from != net_self() && m.catdata.size == 3 && m.catdata.data[1] == 42 && m.catdata.data[2] == 99) { got = true; got_at = GetTickCount64(); }
            net_msg_release(m);
        }
        if (sent && got && GetTickCount64() - got_at > 600) { result = 0; break; }   // let our own frame leave first
        Sleep(2);
    }
    net_shutdown();
    printf("%s: %s (peers=%d sent=%d got=%d)\n", argv[1], result ? "FAILED" : "PASSED", (int)net_peer_count(), sent, got);
    return result;
}
