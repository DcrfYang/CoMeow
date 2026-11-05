// test_voice.cpp -- the voice chat's jitter buffer (0.1.134) against a link that stalls: frames held back for a while, then delivered at once (what a lost packet does to
// a reliable stream). Drives the real mgmp_voice.cpp mixer; the worker and its devices are not started.
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "mgmp_voice.h"
#include "mgmp_voicecodec.h"
#include "mgmp_config.h"
#include "mgmp_i18n.h"
#include "mgmp_log.h"
#include "mgmp_proto.h"

using namespace mgmp;

static int g_checks = 0, g_fail = 0;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static std::vector<std::string> g_log;
static unsigned long long g_sim = 1000000;
static unsigned long long sim_now() { return g_sim; }
namespace mgmp {
void log_line(const char* tag, const char* fmt, ...) { char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a); g_log.push_back(std::string(tag) + " " + b); }
void log_line_lvl(LogLevel, const char* tag, const char* fmt, ...) { char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a); g_log.push_back(std::string(tag) + " " + b); }
bool session_exp_voice() { return true; }
bool net_active() { return true; }
uint8_t net_peer_count() { return 2; }
uint8_t net_self() { return 0; }
bool net_send_voice(const VoiceMsg&) { return true; }
const char* tr(Tx) { return ""; }
void room_toast(const char*) {}
const Config& config() { static Config c; return c; }
}

// one talk spurt of `frames` 40 ms frames; the link delivers frame k at arrive[k] (ms); the speakers take 20 ms every 20 ms. Returns the blocks that played silence
// in the middle of the spurt (a gap the listener hears) and the samples that never played.
struct Result { int gaps = 0; int played = 0; };
static Result run(const std::vector<int>& arrive, int frames) {
    VoiceCodec codec{};
    std::vector<VoiceMsg> msgs((size_t)frames);
    for (int k = 0; k < frames; ++k) {
        int16_t pcm[640];
        for (int i = 0; i < 640; ++i) pcm[i] = (int16_t)(8000 * std::sin((k * 640 + i) * 0.05));
        VoiceMsg& m = msgs[(size_t)k];
        m = VoiceMsg{};
        m.seq = (uint16_t)k; m.samples = 640; m.pred = pcm[0]; m.index = (uint8_t)codec.index;
        m.len = (uint16_t)voice_adpcm_encode(pcm, 640, codec, m.data);
    }
    Result r;
    int next = 0;
    bool started = false;
    const int end = arrive.back() + 3000;
    const unsigned long long base = g_sim;
    for (int t = 0; t <= end; t += 20) {
        g_sim = base + (unsigned long long)t;
        while (next < frames && arrive[(size_t)next] <= t) voice_on_message(1, msgs[(size_t)next++]);
        int16_t out[320];
        const bool any = voice_test_mix(out);
        bool loud = false;
        if (any) for (int i = 0; i < 320; ++i) if (std::abs(out[i]) > 200) { loud = true; break; }
        if (loud) { started = true; ++r.played; }
        else if (started && next < frames) ++r.gaps;           // silence while the speaker is still talking
    }
    return r;
}

int main() {
    voice_test_clock(&sim_now);
    voice_test_sound(true);
    // a steady link: no gap, and the target stays where it starts
    {
        std::vector<int> a; for (int k = 0; k < 100; ++k) a.push_back(k * 40);
        Result r = run(a, 100);
        CHECK(r.gaps == 0);
        CHECK(voice_test_target(1) == 120);
        CHECK(r.played >= 195);                                 // 100 x 40 ms = 200 blocks, nothing cut
    }
    // a link that stalls 400 ms every 2 s (and then delivers the backlog at once): the first stalls are heard, the buffer learns, the later ones are not
    {
        std::vector<int> a;
        for (int k = 0; k < 300; ++k) {
            int t = k * 40;
            const int cycle = t % 2000;
            if (cycle >= 1000 && cycle < 1400) t = t - cycle + 1400;          // held back, delivered at the end of the stall
            a.push_back(t + 3000);
        }
        Result r = run(a, 300);
        std::printf("stalling link: %d gap block(s) heard, %d block(s) played, target %d ms\n", r.gaps, r.played, voice_test_target(1));
        CHECK(voice_test_target(1) > 120 && voice_test_target(1) <= 600);
        CHECK(r.played >= 560);                                 // 300 x 40 ms = 600 blocks: next to nothing cut (the faster playback catches up instead)
        CHECK(r.gaps <= 60);                                    // only the first stalls, before the target has learned the link
    }
    // a speaker who pauses between sentences (the gate closes, no frames for 600 ms, then the frame rate again): no stall -- the target does not grow
    {
        const int before = voice_test_target(1);
        std::vector<int> a; int t = 3000;
        for (int sentence = 0; sentence < 5; ++sentence) { for (int k = 0; k < 40; ++k) { a.push_back(t); t += 40; } t += 600; }
        run(a, (int)a.size());
        CHECK(voice_test_target(1) <= before);
    }
    for (auto& l : g_log) if (getenv("VOICE_VERBOSE")) std::printf("  log: %s\n", l.c_str());
    std::printf("test_voice: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
