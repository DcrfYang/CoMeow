// server/mgmp_server.cpp -- the mgmp signaling server.
//
// WHAT IT IS FOR.
//
// The mod's transport is already peer to peer: whoever hosts binds TCP and
// everyone else dials them. What was missing was a way to find each other --
// until this server, that meant one peer knowing the other's IP by hand and
// mgmp.json saying host or client before the game started.
//
// This server does introductions and nothing else. It never sees a game
// message, never relays one, and never learns anything about the run: it hands
// each joiner the host's address and port and then gets out of the way. The
// game traffic goes directly between the two peers on the host's own port.
//
// WHAT IT IS NOT.
//
//   - Not a relay. If the two players cannot reach each other directly this
//     server cannot help them; that needs a relay or NAT punch, which is a
//     different (much larger) piece of work.
//   - Not authoritative about anything in the game. The room is a lobby, and
//     the lobby ends the moment the game session starts.
//   - Not secure. No accounts, no authentication, no encryption. Anyone who can
//     reach the port can create or join a room. Run it on a LAN or a private
//     network, or put it behind something that does the securing.
//
// PROTOCOL. Newline-delimited JSON: one UTF-8 object per line, '\n' terminated,
// '\r' tolerated. The mod's side is src/net/mgmp_signal.cpp and the two are
// meant to be read together.
//
//   client -> server
//     {"t":"hello","name":"alice","port":27600}   must be first; `port` is
//                                                  what this peer would listen
//                                                  on if it becomes the host
//     {"t":"list"}                                ask for the room list
//     {"t":"create","name":"garden"}              create a room and join it as
//                                                  the host (optional "addr"
//                                                  overrides the address the
//                                                  server hands out; optional "pw" is
//                                                  the room password's DIGEST -- the
//                                                  mod sends a SHA-256 hex string, the
//                                                  server never sees the password)
//     {"t":"join","room":"AB12","pw":"<digest>"}  join an existing room; a room with a password
//                                                  refuses a missing or wrong digest with
//                                                  {"t":"error","code":"password"}
//     {"t":"leave"}                               leave the room, stay connected
//     {"t":"lock","on":true}                      room host only: a locked room is left
//                                                  out of every list and refuses joins
//     {"t":"away","on":true}                      this member is playing on its own for a
//                                                  while (a house boss fight); shown to the room
//     {"t":"ping"}
//     {"t":"logbegin","desc":"...","info":"...","files":[{"name":"a.log","size":123}]}
//                                                  a player uploads game logs (the F2 panel in the mod):
//                                                  up to 4 files, 16 MB each. The server answers
//                                                  {"t":"logready"} or an error, then expects the bytes as
//     {"t":"logchunk","d":"<base64>"}              (<= 2400 raw bytes each, files in the declared order, a chunk never
//                                                  spans two files), then
//     {"t":"logend"}                               and answers {"t":"logsaved","id":"<folder>"}. Everything lands in
//                                                  <logdir>\<time>_<player>_<ip>[_n]\ with an info.txt beside the files.
//
//   server -> client
//     {"t":"welcome","you":"alice","proto":1,"pw":true,"logs":true,"rooms":[...]}   "pw": knows room passwords; "logs": collects logs
//     {"t":"rooms","rooms":[{"id","name","host","players","cap","pw"}]}  open rooms first, password rooms after
//     {"t":"joined","id","name","role","addr","port","cap","you"}
//     {"t":"room","id","name","addr","port","cap","locked",
//      "players":[{name,role,away}],
//      "event":"create"|"join"|"leave"|"lock"|"unlock"|"away"|"back","who":"bob"}
//     {"t":"closed","msg":"the host left the room"}
//     {"t":"error","msg":"..."}
//     {"t":"pong"}
//
// ONE THREAD, select(). A lobby is a handful of idle sockets exchanging a few
// hundred bytes, and a single-threaded loop means there is no locking to get
// wrong and no interleaving to reason about -- every state change happens in
// the one place that touches the state.
//
// BUILD: Windows  cmake --build build --config Release -> build\Release\mgmp_server.exe
//        Linux    server/linux/build.sh  (or the CMakeLists beside this file) -> mgmp_server
// RUN:   mgmp_server [--port 27700] [--logdir <path>] [--quiet]
// The same source builds on both; see the "platform" block below the includes.

#include "json.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <string>
#include <vector>

// ONE SOURCE, TWO PLATFORMS. Everything that differs between Windows and Linux lives in the "platform" block just
// below the includes (sockets, clock, folders); the rest of the file -- protocol, rooms, passwords, log collection --
// is shared, so a protocol change can never reach one build and miss the other.
#ifdef _WIN32
// winsock2.h BEFORE windows.h, always: windows.h pulls in winsock.h otherwise,
// and the two headers collide over every type they both declare.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
typedef int SOCKET;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int    SOCKET_ERROR   = -1;
#endif

using nlohmann::json;

namespace {

// --- platform ---------------------------------------------------------------

#ifdef _WIN32
constexpr char kSep = '\\';
const char*    kEol = "\r\n";      // info.txt is opened with Notepad on Windows
constexpr int  kSendFlags = 0;
#else
constexpr char kSep = '/';
const char*    kEol = "\n";
constexpr int  kSendFlags = MSG_NOSIGNAL;   // a peer that vanished must not kill the process with SIGPIPE
#endif

volatile std::sig_atomic_t g_stop = 0;     // set by SIGINT/SIGTERM: leave the loop, clean up, exit 0

uint64_t tick_ms() {
#ifdef _WIN32
    return GetTickCount64();
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
#endif
}

struct LocalTime { unsigned year, month, day, hour, minute, second; };

LocalTime local_now() {
#ifdef _WIN32
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond };
#else
    const time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    return { (unsigned)lt.tm_year + 1900u, (unsigned)lt.tm_mon + 1u, (unsigned)lt.tm_mday,
             (unsigned)lt.tm_hour, (unsigned)lt.tm_min, (unsigned)lt.tm_sec };
#endif
}

unsigned process_id() {
#ifdef _WIN32
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}

void close_socket(SOCKET s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

int sock_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// A blocking call that a signal cut short: not a failure, just look again.
bool interrupted() {
#ifdef _WIN32
    return false;
#else
    return errno == EINTR;
#endif
}

void set_send_timeout(SOCKET s, int ms) {
#ifdef _WIN32
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
#else
    timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool net_startup() {
#ifdef _WIN32
    WSADATA wsa{};
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

void net_shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

bool is_dir(const std::string& p) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool make_dir(const std::string& p) {   // one level; false if it already exists or cannot be made
#ifdef _WIN32
    return CreateDirectoryA(p.c_str(), nullptr) != 0;
#else
    return mkdir(p.c_str(), 0750) == 0;   // the logs hold player names and addresses: not world-readable
#endif
}

void remove_file(const std::string& p) {
#ifdef _WIN32
    DeleteFileA(p.c_str());
#else
    unlink(p.c_str());
#endif
}

void remove_empty_dir(const std::string& p) {
#ifdef _WIN32
    RemoveDirectoryA(p.c_str());
#else
    rmdir(p.c_str());
#endif
}

// f(name, is_directory) for every entry of `dir` except "." and "..".
template <class F> void each_entry(const std::string& dir, F f) {
#ifdef _WIN32
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        f(std::string(fd.cFileName), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        f(std::string(e->d_name), is_dir(dir + "/" + e->d_name));
    }
    closedir(d);
#endif
}

std::string exe_dir() {
    std::string d;
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    d = exe;
#else
    char exe[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) d.assign(exe, (size_t)n);
#endif
    const size_t cut = d.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : d.substr(0, cut);
}

FILE* open_for_write(const std::string& path) {
#ifdef _WIN32
    FILE* f = nullptr;
    return fopen_s(&f, path.c_str(), "wb") == 0 ? f : nullptr;
#else
    return fopen(path.c_str(), "wb");
#endif
}

// --- limits -----------------------------------------------------------------
//
// kMaxPeers in the mod's mgmp_proto.h is 4, and the host is one of them, so a
// room holds 4. The server refuses the fifth rather than letting the game's
// handshake discover it: by then the peer has already loaded the run.
constexpr int      kRoomCap    = 4;
constexpr int      kMaxClients = 64;
constexpr int      kMaxRooms   = 64;
constexpr uint32_t kLineMax    = 8192;    // a log chunk line is ~3.3 KB; the buffer must hold a partial one plus a read
constexpr int      kUpMaxFiles = 4;
constexpr uint32_t kUpFileMax  = 16u << 20;   // per file
constexpr uint32_t kUpTotalMax = 32u << 20;   // per upload
constexpr size_t   kUpChunkMax = 2600;        // raw bytes in one chunk
constexpr size_t   kUpTextMax  = 1500;        // the description's cap, UTF-8 bytes
constexpr int      kUpPerHour  = 12;          // uploads one address may make per hour
constexpr int      kUpStoreMax = 3000;        // stored uploads before the server refuses more
constexpr uint64_t kUpIdleMs  = 60000;       // a stalled upload is dropped (and its partial files deleted)
constexpr size_t   kPwMax      = 64;      // the longest password digest accepted
constexpr int      kPwTries    = 6;       // wrong passwords before a connection is dropped

bool g_quiet = false;

// --- small helpers ----------------------------------------------------------

void stamp(char* out, size_t cap) {
    const LocalTime lt = local_now();
    snprintf(out, cap, "%02u:%02u:%02u", lt.hour, lt.minute, lt.second);
}

void logf(const char* fmt, ...) {
    if (g_quiet) return;
    char ts[16];
    stamp(ts, sizeof(ts));
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

bool same(const char* a, const char* b) { return lower(a) == lower(b); }

// Copies at most cap-1 bytes and never leaves half of a multi-byte UTF-8 character at the end.
void copy_str(char* dst, size_t cap, const std::string& src) {
    const size_t take = src.size() < cap - 1 ? src.size() : cap - 1;
    memcpy(dst, src.data(), take);
    dst[take] = 0;
    size_t n = strlen(dst);
    if (n == src.size() || n == 0) return;   // not truncated
    size_t i = n;
    while (i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) --i;   // back to the lead byte
    if (i > 0) {
        const unsigned char lead = (unsigned char)dst[i - 1];
        const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (i - 1 + need > n) dst[i - 1] = 0;   // the character was cut: drop it whole
    }
}

std::string json_str(const json& o, const char* key, const char* fallback = "") {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

// --- state ------------------------------------------------------------------

struct Member {
    int  slot  = -1;      // index into g_clients
    bool host  = false;
};

struct Room {
    bool        used = false;
    char        id[8]     = {};
    char        name[48]  = {};
    char        addr[64]  = {};      // the address joiners are told to dial
    uint16_t    port      = 27600;
    int         members[kRoomCap] = {};
    int         count     = 0;
    bool        locked    = false;   // hidden from the lists, refuses joins
    bool        has_pw    = false;   // a password was set at creation (room list shows a padlock)
    char        pw[72]    = {};      // the digest the joiner must present
};

// An upload in progress: the files are written as the chunks arrive and the folder is deleted if it never completes.
struct Upload {
    std::string dir;                   // the folder, full path
    std::string folder;                // its name (what the player is told)
    std::string player, ip, desc, info;
    struct F { std::string name; uint32_t size = 0, got = 0; } f[kUpMaxFiles];
    int      nfiles = 0;
    int      cur    = 0;               // the file being filled
    FILE*    fp     = nullptr;
    uint32_t total  = 0;
    uint64_t last  = 0;               // GetTickCount64 of the last chunk
};

struct Client {
    bool     used = false;
    Upload*  up   = nullptr;
    SOCKET   sock = INVALID_SOCKET;
    char     name[32] = {};
    char     ip[64]   = {};
    uint16_t listen_port = 27600;
    int      room    = -1;          // index into g_rooms, -1 = in the lobby
    bool     helloed = false;
    bool     away    = false;       // playing on its own for a while; see "away"
    int      pw_fails = 0;          // wrong passwords tried on this connection
    char     rbuf[kLineMax] = {};
    uint32_t rlen    = 0;
};

Client g_clients[kMaxClients];
Room   g_rooms[kMaxRooms];
SOCKET g_listen = INVALID_SOCKET;

int find_room_by_id(const char* id) {
    for (int i = 0; i < kMaxRooms; ++i)
        if (g_rooms[i].used && same(g_rooms[i].id, id)) return i;
    return -1;
}

int find_room_by_name(const char* name) {
    for (int i = 0; i < kMaxRooms; ++i)
        if (g_rooms[i].used && same(g_rooms[i].name, name)) return i;
    return -1;
}

// A room id is what a player reads aloud to a friend: no 0/O, no 1/I, and
// uppercase so it survives being typed in lowercase.
const char* kRoomAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

void make_room_id(char* out, size_t cap) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        char id[8];
        for (int i = 0; i < 4; ++i)
            id[i] = kRoomAlphabet[rand() % 32];
        id[4] = 0;
        if (find_room_by_id(id) < 0) { copy_str(out, cap, id); return; }
    }
    copy_str(out, cap, "FULL");
}

json room_summary(const Room& r) {
    json o = json::object();
    o["id"]      = r.id;
    o["name"]    = r.name;
    o["players"] = r.count;
    o["cap"]     = kRoomCap;
    o["host"]    = r.count ? g_clients[r.members[0]].name : "";
    o["pw"]      = r.has_pw;
    return o;
}

json rooms_array() {
    json a = json::array();
    // A locked room is invisible: its players have started, and nobody else may walk in. Open rooms come first and
    // password rooms after them (each group in creation order).
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < kMaxRooms; ++i)
            if (g_rooms[i].used && !g_rooms[i].locked && g_rooms[i].has_pw == (pass == 1)) a.push_back(room_summary(g_rooms[i]));
    return a;
}

bool send_json(SOCKET s, const json& o) {
    // A string cut in the middle of a multi-byte character (a long player name) must not throw out of dump() and
    // take the whole server down: bad bytes are replaced.
    std::string line = o.dump(-1, ' ', false, json::error_handler_t::replace);
    line.push_back('\n');
    size_t off = 0;
    while (off < line.size()) {
        const int n = (int)send(s, line.c_str() + off, (int)(line.size() - off), kSendFlags);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

void send_error(SOCKET s, const char* msg, const char* code = nullptr) {
    json o = json::object();
    o["t"]   = "error";
    o["msg"] = msg;
    if (code) o["code"] = code;
    send_json(s, o);
    logf("  !! %s", msg);
}

// Equal, in time that does not depend on where the strings first differ.
bool digest_equal(const char* a, const char* b) {
    const size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; ++i) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

void send_rooms(SOCKET s) {
    json o = json::object();
    o["t"]     = "rooms";
    o["rooms"] = rooms_array();
    send_json(s, o);
}

// The membership message. `event`/`who` are what the lobby shows as a line of
// text; everything else is what it draws in the table. Both in one message
// because they change together and a peer that got one without the other would
// have to guess.
void broadcast_room(int ri, const char* event, const char* who) {
    Room& r = g_rooms[ri];
    json players = json::array();
    for (int i = 0; i < r.count; ++i) {
        const Client& c = g_clients[r.members[i]];
        json p = json::object();
        p["name"] = c.name;
        p["role"] = (i == 0) ? "host" : "client";
        p["away"] = c.away;
        players.push_back(p);
    }

    for (int i = 0; i < r.count; ++i) {
        json o = json::object();
        o["t"]       = "room";
        o["id"]      = r.id;
        o["name"]    = r.name;
        o["addr"]    = r.addr;
        o["port"]    = (int)r.port;
        o["cap"]     = kRoomCap;
        o["locked"]  = r.locked;
        o["pw"]      = r.has_pw;
        o["players"] = players;
        if (event) o["event"] = event;
        if (who)   o["who"]   = who;
        send_json(g_clients[r.members[i]].sock, o);
    }
}

// Drop a client out of its room. The HOST leaving destroys the room, because a
// room whose host is gone has nobody to introduce anybody to: the joiners are
// told so by name rather than left waiting for an address that will never come.
void leave_room(int slot, const char* why) {
    Client& c = g_clients[slot];
    if (c.room < 0) return;
    const int ri = c.room;
    Room& r = g_rooms[ri];

    const bool was_host = (r.count > 0 && r.members[0] == slot);
    char who[64];
    copy_str(who, sizeof(who), c.name);

    int keep[kRoomCap];
    int kept = 0;
    for (int i = 0; i < r.count; ++i)
        if (r.members[i] != slot) keep[kept++] = r.members[i];
    r.count = 0;
    for (int i = 0; i < kept; ++i) r.members[r.count++] = keep[i];

    c.room = -1;
    c.away = false;

    if (was_host) {
        logf("room %s destroyed -- host '%s' left", r.id, who);
        for (int i = 0; i < r.count; ++i) {
            Client& m = g_clients[r.members[i]];
            m.room = -1;
            json o = json::object();
            o["t"]   = "closed";
            o["msg"] = "the host left the room";
            send_json(m.sock, o);
        }
        r.count = 0;
        r.used  = false;
        return;
    }

    logf("room %s: '%s' left (%d remain)", r.id, who, r.count);
    if (r.count) broadcast_room(ri, "leave", who);
    else { r.used = false; logf("room %s destroyed -- empty", r.id); }
}

// --- log collection ---------------------------------------------------------

std::string g_logdir;                  // where uploads are stored (--logdir; default: beside the exe)
int         g_stored = 0;              // uploads already in it (counted at startup)
struct Recent { std::string ip; std::vector<uint64_t> at; };
std::vector<Recent> g_recent;          // upload times per address, for the hourly limit

// Letters, digits, '.', '_' and '-' only; anything else becomes '_'. Never empty, never starts with a dot.
std::string sanitize(const std::string& s, size_t cap) {
    std::string r;
    for (unsigned char ch : s) {
        if (r.size() >= cap) break;
        r.push_back((isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? (char)ch : '_');
    }
    while (!r.empty() && r[0] == '.') r.erase(r.begin());
    return r.empty() ? "x" : r;
}

bool dir_exists(const std::string& p) { return is_dir(p); }

void ensure_dir(const std::string& p) {
    // One level at a time, so --logdir may name a folder inside one that does not exist yet.
    for (size_t i = 1; i < p.size(); ++i)
        if (p[i] == '\\' || p[i] == '/') make_dir(p.substr(0, i));
    make_dir(p);
}

void count_stored() {
    g_stored = 0;
    each_entry(g_logdir, [](const std::string&, bool dir) { if (dir) ++g_stored; });
}

void remove_tree(const std::string& dir) {
    // Our own folder only: the files directly inside, then the folder (we never nest).
    each_entry(dir, [&](const std::string& name, bool is_d) { if (!is_d) remove_file(dir + kSep + name); });
    remove_empty_dir(dir);
}

void up_abort(Client& c, const char* why) {
    if (!c.up) return;
    if (c.up->fp) { fclose(c.up->fp); c.up->fp = nullptr; }
    logf("upload from '%s' (%s) abandoned: %s -- partial files deleted", c.up->player.c_str(), c.up->ip.c_str(), why);
    remove_tree(c.up->dir);
    delete c.up;
    c.up = nullptr;
}

int b64_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

// Strict: the length must be a multiple of four, '=' only at the end.
bool b64_decode(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty() || in.size() % 4) return false;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4]; int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const unsigned char ch = (unsigned char)in[i + k];
            if (ch == '=') { if (i + 4 != in.size() || k < 2) return false; v[k] = 0; ++pad; }
            else { if (pad) return false; v[k] = b64_value(ch); if (v[k] < 0) return false; }
        }
        const uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        out.push_back((char)(w >> 16));
        if (pad < 2) out.push_back((char)(w >> 8));
        if (pad < 1) out.push_back((char)w);
    }
    return true;
}

// Opens file `up.cur`, skipping any that are declared empty is not allowed (sizes are >= 1), so this just opens it.
bool up_open_current(Upload& up) {
    const std::string path = up.dir + kSep + up.f[up.cur].name;
    up.fp = open_for_write(path);
    return up.fp != nullptr;
}

void handle_logbegin(Client& c, const json& j) {
    if (c.up) { send_error(c.sock, "an upload is already in progress"); return; }
    if (g_logdir.empty()) { send_error(c.sock, "this server does not collect logs", "logs"); return; }

    // the hourly limit per address
    const uint64_t now = tick_ms();
    Recent* rec = nullptr;
    for (auto& r : g_recent) if (r.ip == c.ip) rec = &r;
    if (!rec) { g_recent.push_back(Recent{c.ip, {}}); rec = &g_recent.back(); }
    for (size_t i = 0; i < rec->at.size();) {
        if (now - rec->at[i] > 3600000ull) rec->at.erase(rec->at.begin() + (long)i); else ++i;
    }
    if ((int)rec->at.size() >= kUpPerHour) { send_error(c.sock, "too many uploads from this address -- try again later", "limit"); return; }
    if (g_stored >= kUpStoreMax) { send_error(c.sock, "the server's log storage is full", "full"); return; }

    auto fit = j.find("files");
    if (fit == j.end() || !fit->is_array() || fit->empty() || fit->size() > (size_t)kUpMaxFiles) {
        send_error(c.sock, "an upload carries 1 to 4 files"); return;
    }
    Upload* up = new Upload();
    uint64_t total = 0;
    for (const json& f : *fit) {
        if (!f.is_object()) { delete up; send_error(c.sock, "bad file list"); return; }
        auto sz = f.find("size");
        const std::string nm = json_str(f, "name");
        if (sz == f.end() || !sz->is_number_integer() || nm.empty()) { delete up; send_error(c.sock, "bad file entry"); return; }
        const long long size = sz->get<long long>();
        if (size < 1 || size > (long long)kUpFileMax) { delete up; send_error(c.sock, "a file is empty or over 16 MB", "size"); return; }
        total += (uint64_t)size;
        char idx[8]; snprintf(idx, sizeof(idx), "f%d_", up->nfiles);
        up->f[up->nfiles].name = idx + sanitize(nm, 60);
        up->f[up->nfiles].size = (uint32_t)size;
        ++up->nfiles;
    }
    if (total > kUpTotalMax) { delete up; send_error(c.sock, "the upload is over 32 MB", "size"); return; }

    up->player = c.name;
    up->ip     = c.ip;
    up->desc   = json_str(j, "desc");
    if (up->desc.size() > kUpTextMax) up->desc.resize(kUpTextMax);
    up->info   = json_str(j, "info");
    if (up->info.size() > 300) up->info.resize(300);

    // <time>_<player>_<ip>, with _2, _3 ... if the same second is taken
    const LocalTime lt = local_now();
    char tm[32];
    snprintf(tm, sizeof(tm), "%04u%02u%02u-%02u%02u%02u", lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second);
    const std::string base = std::string(tm) + "_" + sanitize(c.name, 24) + "_" + sanitize(c.ip, 40);
    ensure_dir(g_logdir);
    std::string folder = base;
    for (int n = 2; n < 100 && dir_exists(g_logdir + kSep + folder); ++n) folder = base + "_" + std::to_string(n);
    up->folder = folder;
    up->dir    = g_logdir + kSep + folder;
    if (!make_dir(up->dir)) { delete up; send_error(c.sock, "the server cannot store logs right now", "full"); return; }
    if (!up_open_current(*up)) { remove_tree(up->dir); delete up; send_error(c.sock, "the server cannot store logs right now", "full"); return; }

    up->last = now;
    rec->at.push_back(now);
    c.up = up;
    logf("upload from '%s' (%s): %d file(s), %llu bytes -> %s", c.name, c.ip, up->nfiles, (unsigned long long)total, up->folder.c_str());
    json o = json::object();
    o["t"] = "logready";
    send_json(c.sock, o);
}

void handle_logchunk(Client& c, const json& j) {
    if (!c.up) { send_error(c.sock, "no upload in progress"); return; }
    Upload& up = *c.up;
    std::string raw;
    if (!b64_decode(json_str(j, "d"), raw) || raw.empty() || raw.size() > kUpChunkMax) {
        send_error(c.sock, "bad log chunk"); up_abort(c, "bad chunk"); return;
    }
    if (up.cur >= up.nfiles) { send_error(c.sock, "more data than declared"); up_abort(c, "extra data"); return; }
    auto& f = up.f[up.cur];
    if (raw.size() > f.size - f.got) { send_error(c.sock, "a chunk runs past the end of its file"); up_abort(c, "chunk past the end of its file"); return; }
    if (fwrite(raw.data(), 1, raw.size(), up.fp) != raw.size()) { send_error(c.sock, "the server cannot store logs right now", "full"); up_abort(c, "write failed"); return; }
    f.got += (uint32_t)raw.size();
    up.total += (uint32_t)raw.size();
    up.last = tick_ms();
    if (f.got == f.size) {
        fclose(up.fp); up.fp = nullptr;
        if (++up.cur < up.nfiles && !up_open_current(up)) { send_error(c.sock, "the server cannot store logs right now", "full"); up_abort(c, "open failed"); return; }
    }
}

void handle_logend(Client& c) {
    if (!c.up) { send_error(c.sock, "no upload in progress"); return; }
    Upload& up = *c.up;
    if (up.cur != up.nfiles) { send_error(c.sock, "the upload is incomplete"); up_abort(c, "ended early"); return; }

    FILE* fp = open_for_write(up.dir + kSep + "info.txt");
    if (fp) {
        const LocalTime lt = local_now();
        fprintf(fp, "received : %04u-%02u-%02u %02u:%02u:%02u (server local time)%s", lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second, kEol);
        fprintf(fp, "player   : %s%saddress  : %s%sinfo     : %s%s", up.player.c_str(), kEol, up.ip.c_str(), kEol, up.info.c_str(), kEol);
        for (int i = 0; i < up.nfiles; ++i) fprintf(fp, "file     : %s (%u bytes)%s", up.f[i].name.c_str(), up.f[i].size, kEol);
        fprintf(fp, "%sthe player's description:%s%s%s", kEol, kEol, up.desc.c_str(), kEol);
        fclose(fp);
    }
    logf("upload from '%s' (%s) saved: %s (%u bytes)", up.player.c_str(), up.ip.c_str(), up.folder.c_str(), up.total);
    ++g_stored;
    json o = json::object();
    o["t"]  = "logsaved";
    o["id"] = up.folder;
    delete c.up;
    c.up = nullptr;
    send_json(c.sock, o);
}

void close_client(int slot) {
    Client& c = g_clients[slot];
    if (!c.used) return;
    up_abort(c, "the peer disconnected");
    leave_room(slot, "disconnected");
    if (c.sock != INVALID_SOCKET) { close_socket(c.sock); c.sock = INVALID_SOCKET; }
    logf("peer '%s' (%s) disconnected", c.name[0] ? c.name : "?", c.ip);
    c = Client{};
}

// --- the commands -----------------------------------------------------------

void handle_line(int slot, const std::string& line) {
    Client& c = g_clients[slot];

    json j = json::parse(line, nullptr, /*allow_exceptions=*/false,
                         /*ignore_comments=*/false);
    if (j.is_discarded() || !j.is_object()) {
        send_error(c.sock, "that line is not a JSON object");
        return;
    }

    const std::string t = json_str(j, "t");

    if (t == "hello") {
        if (c.helloed) { send_error(c.sock, "hello was already sent"); return; }
        std::string name = json_str(j, "name");
        if (name.empty()) name = "player";
        copy_str(c.name, sizeof(c.name), name);
        auto it = j.find("port");
        if (it != j.end() && it->is_number_integer()) {
            const int p = it->get<int>();
            if (p > 0 && p <= 65535) c.listen_port = (uint16_t)p;
        }
        c.helloed = true;
        logf("peer '%s' (%s) said hello, would listen on %u",
             c.name, c.ip, (unsigned)c.listen_port);

        json o = json::object();
        o["t"]     = "welcome";
        o["you"]   = c.name;
        o["proto"] = 1;
        o["pw"]    = true;          // this server understands room passwords
        o["logs"]  = !g_logdir.empty();   // ...and collects game logs
        o["rooms"] = rooms_array();
        send_json(c.sock, o);
        return;
    }

    // Everything else needs a name, and a name only comes from hello. A peer
    // that skipped it is refused here rather than showing up in the lobby as
    // blank and unfindable.
    if (!c.helloed) { send_error(c.sock, "send hello first"); return; }

    if (t == "ping") {
        json o = json::object();
        o["t"] = "pong";
        send_json(c.sock, o);
        return;
    }

    if (t == "list") { send_rooms(c.sock); return; }

    if (t == "logbegin") { handle_logbegin(c, j); return; }
    if (t == "logchunk") { handle_logchunk(c, j); return; }
    if (t == "logend")   { handle_logend(c); return; }

    if (t == "create") {
        if (c.room >= 0) { send_error(c.sock, "you are already in a room"); return; }
        std::string name = json_str(j, "name");
        if (name.empty()) name = c.name;
        if (find_room_by_name(name.c_str()) >= 0) {
            send_error(c.sock, "a room with that name already exists");
            return;
        }

        int ri = -1;
        for (int i = 0; i < kMaxRooms; ++i) if (!g_rooms[i].used) { ri = i; break; }
        if (ri < 0) { send_error(c.sock, "too many rooms"); return; }

        Room& r = g_rooms[ri];
        r = Room{};
        r.used = true;
        make_room_id(r.id, sizeof(r.id));
        copy_str(r.name, sizeof(r.name), name);
        {
            const std::string pw = json_str(j, "pw");
            if (pw.size() > kPwMax) { r = Room{}; send_error(c.sock, "that password is too long"); return; }
            if (!pw.empty()) { r.has_pw = true; copy_str(r.pw, sizeof(r.pw), pw); }
        }

        // The address joiners are told to dial. The server's own view of the
        // peer address is right in every ordinary case; `addr` exists for the
        // one where it is not -- a host behind a forwarded port who wants to
        // advertise a public name instead.
        const std::string override_addr = json_str(j, "addr");
        copy_str(r.addr, sizeof(r.addr),
                 override_addr.empty() ? std::string(c.ip) : override_addr);
        r.port    = c.listen_port;
        r.members[0] = slot;
        r.count      = 1;
        c.room       = ri;

        logf("room %s '%s' created by '%s'%s -- joiners will dial %s:%u",
             r.id, r.name, c.name, r.has_pw ? " (password)" : "", r.addr, (unsigned)r.port);

        json o = json::object();
        o["t"]      = "joined";
        o["id"]     = r.id;
        o["name"]   = r.name;
        o["role"]   = "host";
        o["addr"]   = r.addr;
        o["port"]   = (int)r.port;
        o["cap"]    = kRoomCap;
        o["you"]    = c.name;
        o["players"] = json::array();   // filled by the broadcast below
        o["pw"]      = r.has_pw;
        send_json(c.sock, o);
        broadcast_room(ri, "create", c.name);
        return;
    }

    if (t == "join") {
        if (c.room >= 0) { send_error(c.sock, "you are already in a room"); return; }
        const std::string id = json_str(j, "room");
        const int ri = find_room_by_id(id.c_str());
        if (ri < 0) { send_error(c.sock, "no such room"); return; }
        Room& r = g_rooms[ri];
        if (r.count >= kRoomCap) {
            send_error(c.sock, "that room is full");
            return;
        }
        if (r.locked) {
            send_error(c.sock, "that room is locked");
            return;
        }
        if (r.has_pw) {
            const std::string pw = json_str(j, "pw");
            if (pw.size() > kPwMax || !digest_equal(pw.c_str(), r.pw)) {
                ++c.pw_fails;
                logf("room %s: '%s' gave a %s password (%d/%d)", r.id, c.name, pw.empty() ? "missing" : "wrong", c.pw_fails, kPwTries);
                send_error(c.sock, pw.empty() ? "that room needs a password" : "wrong password", "password");
                if (c.pw_fails >= kPwTries) { send_error(c.sock, "too many wrong passwords -- dropping the connection"); close_client(slot); }
                return;
            }
        }

        c.room = ri;
        r.members[r.count++] = slot;

        logf("room %s: '%s' joined (%d/%d)", r.id, c.name, r.count, kRoomCap);

        json o = json::object();
        o["t"]    = "joined";
        o["id"]   = r.id;
        o["name"] = r.name;
        o["role"] = "client";
        o["addr"] = r.addr;
        o["port"] = (int)r.port;
        o["cap"]  = kRoomCap;
        o["you"]  = c.name;
        o["pw"]   = r.has_pw;
        send_json(c.sock, o);
        broadcast_room(ri, "join", c.name);
        return;
    }

    if (t == "lock") {
        if (c.room < 0) { send_error(c.sock, "you are not in a room"); return; }
        Room& r = g_rooms[c.room];
        if (!r.count || r.members[0] != slot) { send_error(c.sock, "only the room's host can lock it"); return; }
        auto it = j.find("on");
        const bool on = it != j.end() && it->is_boolean() && it->get<bool>();
        if (r.locked != on) {
            r.locked = on;
            logf("room %s %s by '%s'", r.id, on ? "LOCKED" : "unlocked", c.name);
        }
        broadcast_room(c.room, on ? "lock" : "unlock", c.name);
        return;
    }

    if (t == "away") {
        auto it = j.find("on");
        const bool on = it != j.end() && it->is_boolean() && it->get<bool>();
        c.away = on;
        if (c.room >= 0) {
            logf("room %s: '%s' is %s", g_rooms[c.room].id, c.name, on ? "away (playing alone)" : "back");
            broadcast_room(c.room, on ? "away" : "back", c.name);
        }
        return;
    }

    if (t == "leave") {
        if (c.room < 0) { send_error(c.sock, "you are not in a room"); return; }
        leave_room(slot, "asked to leave");
        json o = json::object();
        o["t"] = "rooms";
        o["rooms"] = rooms_array();
        send_json(c.sock, o);
        return;
    }

    send_error(c.sock, "unknown message type");
}

void on_readable(int slot) {
    Client& c = g_clients[slot];
    char buf[2048];
    const int n = (int)recv(c.sock, buf, sizeof(buf), 0);
    if (n < 0 && interrupted()) return;          // a signal, not a dead peer: the next select brings it back
    if (n <= 0) { close_client(slot); return; }

    if (c.rlen + (uint32_t)n > kLineMax) {
        send_error(c.sock, "a line longer than 8192 bytes -- dropping the peer");
        close_client(slot);
        return;
    }
    memcpy(c.rbuf + c.rlen, buf, (size_t)n);
    c.rlen += (uint32_t)n;

    // Split out every complete line. A partial one stays in rbuf for the next
    // read, which is the whole reason this is buffered rather than assumed to
    // arrive one message per recv.
    uint32_t start = 0;
    for (uint32_t i = 0; i < c.rlen; ++i) {
        if (c.rbuf[i] != '\n') continue;
        std::string line(c.rbuf + start, i - start);
        start = i + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty()) handle_line(slot, line);
        // handle_line may have dropped the peer; the indices it wrote are gone.
        if (!g_clients[slot].used) return;
    }
    if (start) {
        memmove(c.rbuf, c.rbuf + start, c.rlen - start);
        c.rlen -= start;
    }
}

int accept_one() {
    sockaddr_in a{};
    socklen_t alen = sizeof(a);
    SOCKET s = accept(g_listen, (sockaddr*)&a, &alen);
    if (s == INVALID_SOCKET) return -1;

    int slot = -1;
    for (int i = 0; i < kMaxClients; ++i) if (!g_clients[i].used) { slot = i; break; }
    if (slot < 0) {
        json o = json::object();
        o["t"]   = "error";
        o["msg"] = "the server is full";
        send_json(s, o);
        close_socket(s);
        return -1;
    }

    Client& c = g_clients[slot];
    c = Client{};
    c.used = true;
    c.sock = s;
    c.room = -1;

    char ip[64] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    copy_str(c.ip, sizeof(c.ip), ip);

    // A slow or wedged peer must not be able to stall the loop for everyone
    // else: every send here is small, and five seconds is already far past the
    // point where the peer is simply gone.
    set_send_timeout(s, 5000);

    logf("connection from %s (slot %d)", c.ip, slot);
    return slot;
}

void usage() {
    printf("mgmp_server -- signaling for the mgmp co-op mod\n"
           "\n"
           "  mgmp_server [--port <n>] [--logdir <path>] [--quiet]\n"
           "\n"
           "  --port <n>       TCP port to listen on (default 27700)\n"
           "  --logdir <path>  where game logs uploaded from the mod's F2 panel are stored\n"
           "                   (default: a mgmp_logs folder beside this exe)\n"
           "  --quiet          no per-event output\n"
           "\n"
           "It introduces peers and nothing else: a room creator is the game's\n"
           "host, and every joiner is told that host's address and port. Set\n"
           "\"signal\": { \"addr\": \"<this machine>\", \"port\": <n> } in the\n"
           "mod's mgmp.json and use the lobby panel in game.\n");
}

} // namespace

int main(int argc, char** argv) {
    uint16_t port = 27700;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) g_quiet = true;
        else if (!strcmp(argv[i], "--logdir") && i + 1 < argc) g_logdir = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { printf("unknown argument '%s'\n\n", argv[i]); usage(); return 2; }
    }

    if (!net_startup()) {
        printf("network start-up failed\n");
        return 1;
    }
    srand((unsigned)time(nullptr) ^ process_id());

    if (g_logdir.empty()) {
        g_logdir = exe_dir() + kSep + "mgmp_logs";
    }
    while (g_logdir.size() > 3 && (g_logdir.back() == '\\' || g_logdir.back() == '/')) g_logdir.pop_back();
    ensure_dir(g_logdir);
    count_stored();
    logf("game logs from players are stored in %s (%d already there)", g_logdir.c_str(), g_stored);

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { printf("socket failed\n"); return 1; }

    int reuse = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = htons(port);
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        printf("bind %u failed (error %d) -- is another server already running?\n",
               (unsigned)port, sock_error());
        return 1;
    }
    if (listen(g_listen, 16) == SOCKET_ERROR) { printf("listen failed\n"); return 1; }

    logf("mgmp signaling server listening on 0.0.0.0:%u", (unsigned)port);
    logf("rooms hold up to %d players; it does not relay game traffic", kRoomCap);

    signal(SIGINT, [](int) { g_stop = 1; });
    signal(SIGTERM, [](int) { g_stop = 1; });
    logf("stop with Ctrl+C (or SIGTERM)");

    while (!g_stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_listen, &rd);
        SOCKET highest = g_listen;
        for (int i = 0; i < kMaxClients; ++i) {
            if (!g_clients[i].used) continue;
            FD_SET(g_clients[i].sock, &rd);
            if (g_clients[i].sock > highest) highest = g_clients[i].sock;
        }

        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 100000;   // 100 ms: nothing here is latency-critical
        const int rc = select((int)highest + 1, &rd, nullptr, nullptr, &tv);
        if (rc == SOCKET_ERROR) {
            if (interrupted()) continue;     // SIGINT/SIGTERM land here; the loop condition sees g_stop
            logf("!! select failed (error %d)", sock_error());
            break;
        }

        // A stalled upload is dropped, and its partial files go with it.
        {
            const uint64_t now = tick_ms();
            for (int i = 0; i < kMaxClients; ++i)
                if (g_clients[i].used && g_clients[i].up && now - g_clients[i].up->last > kUpIdleMs) {
                    send_error(g_clients[i].sock, "the upload stalled -- dropping it");
                    close_client(i);
                }
        }
        if (rc == 0) continue;

        if (FD_ISSET(g_listen, &rd)) accept_one();

        for (int i = 0; i < kMaxClients; ++i) {
            if (!g_clients[i].used) continue;
            if (!FD_ISSET(g_clients[i].sock, &rd)) continue;
            on_readable(i);
        }
    }

    for (int i = 0; i < kMaxClients; ++i) if (g_clients[i].used) close_client(i);
    close_socket(g_listen);
    net_shutdown();
    return 0;
}
