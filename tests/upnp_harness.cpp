// Opens a TCP listener and (optionally) asks the router to forward it, so a script can check from the internet whether the port
// is reachable.   upnp_harness <port> listen|map     -- prints state lines; a line "q" on stdin removes the mapping and exits.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "mgmp_upnp.h"

#pragma comment(lib, "ws2_32.lib")

static const char* name(mgmp::UpnpState s) {
    switch (s) {
        case mgmp::UpnpState::Idle:    return "Idle";
        case mgmp::UpnpState::Working: return "Working";
        case mgmp::UpnpState::Mapped:  return "Mapped";
        default:                       return "Failed";
    }
}

int main(int argc, char** argv) {
    const unsigned short port = argc > 1 ? (unsigned short)atoi(argv[1]) : 27601;
    const bool map = argc > 2 && !strcmp(argv[2], "map");
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    if (bind(l, (sockaddr*)&a, sizeof(a)) != 0 || listen(l, 8) != 0) { printf("LISTEN FAILED\n"); return 1; }
    printf("LISTENING %u\n", (unsigned)port); fflush(stdout);

    if (map) {
        mgmp::upnp_start(port);
        for (int i = 0; i < 200; ++i) {
            if (mgmp::upnp_state() != mgmp::UpnpState::Working) break;
            Sleep(100);
        }
        printf("UPNP %s ext=%s err=%s\n", name(mgmp::upnp_state()), mgmp::upnp_external_ip(), mgmp::upnp_error());
        fflush(stdout);
    }

    // accept (and drop) whatever comes, until told to stop
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        fd_set rd; FD_ZERO(&rd); FD_SET(l, &rd);
        timeval tv{ 0, 200000 };
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) { SOCKET c = accept(l, nullptr, nullptr); if (c != INVALID_SOCKET) closesocket(c); }
        DWORD avail = 0;
        if (PeekNamedPipe(in, nullptr, 0, nullptr, &avail, nullptr) && avail) break;
    }
    if (map) {
        mgmp::upnp_stop();
        for (int i = 0; i < 80; ++i) { Sleep(100); if (mgmp::upnp_state() == mgmp::UpnpState::Idle) break; }
        Sleep(500);
        printf("STOPPED %s\n", name(mgmp::upnp_state()));
    }
    closesocket(l);
    return 0;
}
