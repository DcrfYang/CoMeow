#include "mgmp_logupload.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "json.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace mgmp {
namespace {

using nlohmann::json;

constexpr size_t kChunk = 2400;   // raw bytes per chunk line (the server accepts up to 2600)

CRITICAL_SECTION g_cs;
LONG             g_cs_once = 0;
LogUploadStatus  g_status;
HANDLE           g_thread  = nullptr;
LogUploadRequest g_req;

void cs_ensure() {
    // InitializeCriticalSection exactly once, without needing a static initialiser in a DLL.
    if (InterlockedCompareExchange(&g_cs_once, 1, 0) == 0) { InitializeCriticalSection(&g_cs); InterlockedExchange(&g_cs_once, 2); }
    while (g_cs_once != 2) Sleep(0);
}

void set_status(LogUploadState st, LogUploadError err, uint32_t pct, const char* detail) {
    EnterCriticalSection(&g_cs);
    g_status.state = st; g_status.error = err; g_status.percent = pct;
    strncpy_s(g_status.detail, sizeof(g_status.detail), detail ? detail : "", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
}

void set_percent(uint32_t pct) {
    EnterCriticalSection(&g_cs);
    g_status.percent = pct;
    LeaveCriticalSection(&g_cs);
}

std::string b64(const unsigned char* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t)p[i] << 16) | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o.push_back(t[(v >> 18) & 63]); o.push_back(t[(v >> 12) & 63]);
        o.push_back(i + 1 < n ? t[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? t[v & 63] : '=');
    }
    return o;
}

// The file's bytes, or its last kLogUploadFileMax of them. Opened with every share flag: the game still has its log
// open for writing. Empty on failure.
std::string read_tail(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER size{};
    std::string data;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
        long long take = size.QuadPart;
        if (take > (long long)kLogUploadFileMax) take = (long long)kLogUploadFileMax;
        LARGE_INTEGER off; off.QuadPart = size.QuadPart - take;
        if (SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
            data.resize((size_t)take);
            size_t got = 0;
            while (got < data.size()) {
                DWORD n = 0;
                if (!ReadFile(h, &data[got], (DWORD)(data.size() - got), &n, nullptr) || n == 0) break;
                got += n;
            }
            data.resize(got);   // the log grows while we read: what was there is what we send
        }
    }
    CloseHandle(h);
    return data;
}

std::string base_name(const wchar_t* path) {
    const wchar_t* s = wcsrchr(path, L'\\');
    const wchar_t* n = s ? s + 1 : path;
    char out[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, n, -1, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
}

// --- sockets ------------------------------------------------------------------

SOCKET dial(const char* host, uint16_t port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return INVALID_SOCKET;
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    char ps[8]; _snprintf_s(ps, sizeof(ps), _TRUNCATE, "%u", (unsigned)port);
    addrinfo* res = nullptr;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        SOCKET c = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c == INVALID_SOCKET) continue;
        u_long nb = 1; ioctlsocket(c, FIONBIO, &nb);
        connect(c, a->ai_addr, (int)a->ai_addrlen);
        fd_set wr; FD_ZERO(&wr); FD_SET(c, &wr);
        fd_set ex; FD_ZERO(&ex); FD_SET(c, &ex);
        timeval tv{}; tv.tv_sec = 8;
        const int rc = select(0, nullptr, &wr, &ex, &tv);
        if (rc > 0 && FD_ISSET(c, &wr)) {
            int err = 0; int len = sizeof(err);
            getsockopt(c, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            if (err == 0) { nb = 0; ioctlsocket(c, FIONBIO, &nb); s = c; continue; }
        }
        closesocket(c);
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        DWORD rcv = 15000, snd = 30000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv, sizeof(rcv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&snd, sizeof(snd));
    }
    return s;
}

bool send_all(SOCKET s, const std::string& line) {
    size_t off = 0;
    while (off < line.size()) {
        const int n = send(s, line.data() + off, (int)(line.size() - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

bool send_json(SOCKET s, const json& o) { return send_all(s, o.dump(-1, ' ', false, json::error_handler_t::replace) + "\n"); }

// One complete line from the server (buffered across calls). False on timeout/close.
bool recv_line(SOCKET s, std::string& buf, std::string& line) {
    for (;;) {
        const size_t nl = buf.find('\n');
        if (nl != std::string::npos) { line = buf.substr(0, nl); buf.erase(0, nl + 1); return true; }
        char tmp[2048];
        const int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        if (buf.size() > 65536) return false;
    }
}

// Reads lines until one of type `want` (returned) or an error line (returned as such, `err_msg` filled). Lines of any
// other type are skipped (a welcome carries the room list, for one).
enum class Got { Wanted, Error, Lost };
Got await(SOCKET s, std::string& buf, const char* want, json& out, std::string& err_msg) {
    std::string line;
    for (int i = 0; i < 50; ++i) {
        if (!recv_line(s, buf, line)) return Got::Lost;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        auto t = j.find("t");
        if (t == j.end() || !t->is_string()) continue;
        const std::string ty = t->get<std::string>();
        if (ty == want) { out = std::move(j); return Got::Wanted; }
        if (ty == "error") {
            auto m = j.find("msg");
            err_msg = (m != j.end() && m->is_string()) ? m->get<std::string>() : "refused";
            return Got::Error;
        }
    }
    return Got::Lost;
}

DWORD WINAPI worker(LPVOID) {
    const LogUploadRequest req = g_req;

    struct Item { std::string name, data; };
    std::vector<Item> items;
    for (int i = 0; i < req.nfiles && i < kLogUploadMaxFiles; ++i) {
        std::string d = read_tail(req.files[i]);
        if (!d.empty()) items.push_back({base_name(req.files[i]), std::move(d)});
    }
    if (items.empty()) { set_status(LogUploadState::Failed, LogUploadError::NoFile, 0, ""); return 0; }

    uint64_t total = 0;
    for (const Item& it : items) total += it.data.size();

    SOCKET s = dial(req.addr[0] ? req.addr : "localhost", req.port);
    if (s == INVALID_SOCKET) { set_status(LogUploadState::Failed, LogUploadError::Connect, 0, ""); return 0; }

    std::string buf, err;
    json reply;
    auto fail = [&](LogUploadError e, const std::string& detail) {
        set_status(LogUploadState::Failed, e, 0, detail.c_str());
        closesocket(s);
    };

    json hello = json::object();
    hello["t"] = "hello"; hello["name"] = req.player[0] ? req.player : "player";
    if (!send_json(s, hello)) { fail(LogUploadError::Lost, ""); return 0; }
    Got g = await(s, buf, "welcome", reply, err);
    if (g != Got::Wanted) { fail(g == Got::Error ? LogUploadError::Refused : LogUploadError::Lost, err); return 0; }
    {
        auto lg = reply.find("logs");
        if (lg == reply.end() || !lg->is_boolean() || !lg->get<bool>()) { fail(LogUploadError::ServerOld, ""); return 0; }
    }

    json begin = json::object();
    begin["t"] = "logbegin";
    begin["desc"] = req.desc;
    begin["info"] = req.info;
    json files = json::array();
    for (const Item& it : items) { json f = json::object(); f["name"] = it.name; f["size"] = (long long)it.data.size(); files.push_back(f); }
    begin["files"] = files;
    if (!send_json(s, begin)) { fail(LogUploadError::Lost, ""); return 0; }
    g = await(s, buf, "logready", reply, err);
    if (g != Got::Wanted) { fail(g == Got::Error ? LogUploadError::Refused : LogUploadError::Lost, err); return 0; }

    uint64_t sent = 0;
    set_status(LogUploadState::Working, LogUploadError::None, 0, "");
    for (const Item& it : items) {
        for (size_t off = 0; off < it.data.size(); off += kChunk) {
            const size_t n = it.data.size() - off < kChunk ? it.data.size() - off : kChunk;
            json c = json::object();
            c["t"] = "logchunk";
            c["d"] = b64((const unsigned char*)it.data.data() + off, n);
            if (!send_json(s, c)) { fail(LogUploadError::Lost, ""); return 0; }
            sent += n;
            // Every so often look (without waiting) for the server cutting us off.
            if (((off / kChunk) & 31) == 31) {
                fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
                timeval tv{};
                if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
                    g = await(s, buf, "logsaved", reply, err);   // only an error (or a drop) can arrive here
                    fail(g == Got::Error ? LogUploadError::Refused : LogUploadError::Lost, err);
                    return 0;
                }
                set_percent((uint32_t)(sent * 100 / total));
            }
        }
    }

    json end = json::object();
    end["t"] = "logend";
    if (!send_json(s, end)) { fail(LogUploadError::Lost, ""); return 0; }
    g = await(s, buf, "logsaved", reply, err);
    if (g != Got::Wanted) { fail(g == Got::Error ? LogUploadError::Refused : LogUploadError::Lost, err); return 0; }
    std::string id;
    { auto i = reply.find("id"); if (i != reply.end() && i->is_string()) id = i->get<std::string>(); }
    set_status(LogUploadState::Done, LogUploadError::None, 100, id.c_str());
    closesocket(s);
    return 0;
}

} // namespace

bool logupload_start(const LogUploadRequest& req) {
    cs_ensure();
    EnterCriticalSection(&g_cs);
    if (g_status.state == LogUploadState::Working) { LeaveCriticalSection(&g_cs); return false; }
    // The previous worker (if any) has finished: its state is not Working. Collect its handle.
    if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
    g_req = req;
    g_status = LogUploadStatus{};
    g_status.state = LogUploadState::Working;
    LeaveCriticalSection(&g_cs);

    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g_thread) { set_status(LogUploadState::Failed, LogUploadError::Lost, 0, ""); return false; }
    return true;
}

LogUploadStatus logupload_status() {
    cs_ensure();
    EnterCriticalSection(&g_cs);
    const LogUploadStatus s = g_status;
    LeaveCriticalSection(&g_cs);
    return s;
}

void logupload_reset() {
    cs_ensure();
    EnterCriticalSection(&g_cs);
    if (g_status.state != LogUploadState::Working) g_status = LogUploadStatus{};
    LeaveCriticalSection(&g_cs);
}

void logupload_wait(uint32_t timeout_ms) {
    cs_ensure();
    HANDLE h = g_thread;
    if (h) WaitForSingleObject(h, timeout_ms);
}

uint32_t logupload_file_size(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return 0;
    return d.nFileSizeHigh ? 0xFFFFFFFFu : d.nFileSizeLow;
}

} // namespace mgmp
