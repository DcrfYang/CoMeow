// test_logupload.cpp -- drives the mod's log-upload client (src/net/mgmp_logupload.cpp) against a real server.
//
// Usage: test_logupload <port> <description | @file-with-it> <file> [file...]
// Prints one line, "STATE <state> <error> <percent> <detail>", and exits 0 only on Done. tools\srv_logs_test.py starts the
// server, runs this, and checks what landed in the server's log folder.
//     cl /nologo /EHsc /std:c++17 /utf-8 /I..\src\net /I..\third_party test_logupload.cpp ..\src\net\mgmp_logupload.cpp
#include "mgmp_logupload.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace mgmp;

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: test_logupload <port> <description> <file>...\n"); return 2; }
    LogUploadRequest req;
    strcpy_s(req.addr, "localhost");
    req.port = (uint16_t)atoi(argv[1]);
    strcpy_s(req.player, "tester");
    if (argv[2][0] == '@') {   // the description from a UTF-8 file (argv is not UTF-8 on Windows)
        FILE* f = nullptr;
        if (fopen_s(&f, argv[2] + 1, "rb") == 0 && f) { fread(req.desc, 1, sizeof(req.desc) - 1, f); fclose(f); }
    } else {
        strcpy_s(req.desc, argv[2]);
    }
    strcpy_s(req.info, "proto test, role client, room -");
    for (int i = 3; i < argc && req.nfiles < kLogUploadMaxFiles; ++i)
        MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, req.files[req.nfiles++], MAX_PATH);

    if (!logupload_start(req)) { printf("could not start\n"); return 2; }
    if (logupload_start(req)) { printf("FAIL a second upload started while one was running\n"); return 2; }
    logupload_wait(60000);
    const LogUploadStatus st = logupload_status();
    printf("STATE %d %d %u %s\n", (int)st.state, (int)st.error, (unsigned)st.percent, st.detail);
    return st.state == LogUploadState::Done ? 0 : 1;
}
