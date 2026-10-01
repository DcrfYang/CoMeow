// Runs the lobby server embedded (the form the mod uses for LAN play) so a script can talk to it.
//   lan_embed_harness <port> <seconds>   start, stay up for <seconds>, stop, start again on the same port, stop (exit 0 = all well)
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include "mgmp_lobby_server.h"

static void sink(const char* s) { printf("[lobby] %s\n", s); fflush(stdout); }

int main(int argc, char** argv) {
    const unsigned short port = argc > 1 ? (unsigned short)atoi(argv[1]) : 27700;
    const int secs = argc > 2 ? atoi(argv[2]) : 5;
    char err[160] = {};
    for (int round = 0; round < 2; ++round) {
        if (!mgmp::lobby_server_start(port, sink, err, sizeof(err))) { printf("start failed: %s\n", err); return 1; }
        printf("running=%d port=%u\n", mgmp::lobby_server_running(), (unsigned)mgmp::lobby_server_port());
        fflush(stdout);
        Sleep(round == 0 ? secs * 1000 : 500);
        mgmp::lobby_server_stop();
        printf("stopped: running=%d port=%u\n", mgmp::lobby_server_running(), (unsigned)mgmp::lobby_server_port());
        fflush(stdout);
    }
    return 0;
}
