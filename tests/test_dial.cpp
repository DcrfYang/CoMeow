// net_join: the address list, the timeout, the fallback and the clean shutdown while dialling.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include "mgmp_net.h"

#pragma comment(lib, "ws2_32.lib")
using namespace mgmp;

static int fails = 0;
static void check(bool c, const char* msg) { printf("%s %s\n", c ? "PASS" : "FAIL", msg); if (!c) ++fails; }

static bool wait_state(NetState want, DWORD ms) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        const NetState s = net_state();
        if (s == want) return true;
        if (s == NetState::Failed && want != NetState::Failed) return false;
        Sleep(20);
    }
    return net_state() == want;
}

int main() {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, (sockaddr*)&a, sizeof(a)); listen(l, 4);
    int len = sizeof(a); getsockname(l, (sockaddr*)&a, &len);
    const uint16_t port = ntohs(a.sin_port);

    // a closed port to be refused by: bind, read the port, close
    SOCKET d = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in b{}; b.sin_family = AF_INET; b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(d, (sockaddr*)&b, sizeof(b)); len = sizeof(b); getsockname(d, (sockaddr*)&b, &len); closesocket(d);
    const uint16_t dead_port = ntohs(b.sin_port);

    // 1. returns at once, then connects
    DWORD t0 = GetTickCount();
    check(net_join("127.0.0.1", port), "net_join accepts a good address");
    check(GetTickCount() - t0 < 200, "net_join returns at once (the dial is off-thread)");
    check(wait_state(NetState::Connected, 3000), "it connects");
    net_shutdown();
    check(net_state() == NetState::Idle, "shutdown after a connection");

    // 2. the first address is a black hole (times out, 5 s), the second answers
    uint32_t seq0 = net_dial_seq();
    t0 = GetTickCount();
    check(net_join("10.255.255.1,127.0.0.1", port), "a list is accepted");
    check(wait_state(NetState::Connected, 12000), "the second address answers after the first timed out");
    printf("   (took %lu ms)\n", GetTickCount() - t0);
    check(net_dial_seq() == seq0, "that is not a failure");
    net_shutdown();

    // 3. a refused address: fails fast, the error is 10061, and the sequence goes up
    t0 = GetTickCount();
    check(net_join("127.0.0.1", dead_port), "dialling a closed port starts");
    check(wait_state(NetState::Failed, 6000), "it fails");
    check(GetTickCount() - t0 < 3000, "a refusal does not wait out the timeout");
    check(net_dial_error() == WSAECONNREFUSED, "the error is 'refused'");
    check(net_dial_seq() == seq0 + 1, "the failure is counted");
    net_shutdown();

    // 4. garbage in the list is skipped; nothing valid at all is refused outright
    check(net_join("not-an-address,,127.0.0.1,127.0.0.1", port), "junk and duplicates are skipped");
    check(wait_state(NetState::Connected, 3000), "and the good one connects");
    net_shutdown();
    check(!net_join("nonsense", port), "no valid address: refused at once");
    net_shutdown();

    // 5. shutting down while a dial is waiting on a black hole returns promptly and is not a failure
    seq0 = net_dial_seq();
    check(net_join("10.255.255.1", port), "dial to a black hole");
    Sleep(300);
    t0 = GetTickCount();
    net_shutdown();
    check(GetTickCount() - t0 < 1500, "shutdown does not wait out the timeout");
    check(net_dial_seq() == seq0, "an aborted dial is not a failure");

    printf(fails ? "FAILED (%d)\n" : "all good\n", fails);
    return fails ? 1 : 0;
}

namespace mgmp { void log_line(const char* tag, const char* fmt, ...) { (void)tag; (void)fmt; } }
