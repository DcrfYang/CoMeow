// mgmp_voice.cpp -- see mgmp_voice.h.
#include "mgmp_voice.h"

#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mgmp_voicecodec.h"
#include "mgmp_denoise.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_room.h"
#include "mgmp_session.h"
#include "mgmp_i18n.h"
#include "mgmp_config.h"

namespace mgmp {
namespace {

constexpr int      kRate        = 16000;
constexpr int      kBlock       = 320;                    // 20 ms: the unit the devices are fed in
constexpr int      kFrameBlocks = 2;                      // 40 ms: the unit that travels
constexpr int      kFrame       = kBlock * kFrameBlocks;  // 640 samples -> 320 bytes of ADPCM
constexpr int      kBuffers     = 6;
// THE JITTER BUFFER (0.1.134, room QS4P "the voice is a bit choppy"). Every frame rides the room's RELIABLE stream (TCP, or Steam's reliable messages): a packet the link
// loses stalls everything behind it until it is sent again, and the rest then arrives at once. The old buffer started at 60 ms and threw away everything over 300 ms --
// on a link with round trips of 200..1300 ms that was a gap at every stall and a chopped-off sentence at every burst. Now each speaker has a target that LEARNS the link:
// it starts at 120 ms, grows by 60 ms at every under-run (up to 600 ms), shrinks by 20 ms after 10 s without one (down to 80 ms). A buffer over its target by more than
// 200 ms is played 6 % faster until it is back (no sound is cut out), and only a buffer over 1.2 s is cut. An under-run fades out instead of clicking.
constexpr int      kRing        = kRate * 15 / 10;        // 1.5 s of samples per speaker
constexpr int      kTargetStart = kRate * 12 / 100;       // 120 ms
constexpr int      kTargetMin   = kRate * 8 / 100;        // 80 ms
constexpr int      kTargetMax   = kRate * 6 / 10;         // 600 ms
constexpr int      kTargetUp    = kRate * 6 / 100;        // +60 ms at an under-run
constexpr int      kTargetDown  = kRate * 2 / 100;        // -20 ms after a calm spell
constexpr ULONGLONG kCalmMs     = 10000;
constexpr int      kFastOver    = kRate * 2 / 10;         // 200 ms over the target: played faster
constexpr int      kHardMax     = kRate * 12 / 10;        // 1.2 s: cut back to the target
// A STALL, told from a pause in the talk: the link held frames back (a wait of more than 150 ms between two) and then let the backlog through at once (3 or more frames
// within 30 ms of the first after the wait). A speaker who paused and goes on speaks at the frame rate -- that is no stall and does not grow the target.
constexpr ULONGLONG kStallGapMs   = 150;
constexpr ULONGLONG kBurstWithinMs = 30;
constexpr int      kBurstFrames  = 3;

// The clock (tests replace it).
ULONGLONG (*g_clock)() = &GetTickCount64;
constexpr int      kHold        = 12;                     // blocks (240 ms) the gate stays open after the voice drops under the threshold
constexpr int      kOutMax      = 16;                     // encoded frames waiting for the main thread
constexpr ULONGLONG kSpeakingMs = 450;

struct Rx {
    int16_t   ring[kRing] = {};
    int       head = 0, count = 0;
    bool      playing = false;
    ULONGLONG last_at = 0;                                // the last frame that arrived, for the indicator
    uint16_t  seq = 0;
    bool      seq_known = false;
    uint32_t  lost = 0;
    // the jitter buffer (0.1.134)
    int       target = 0;                                // 0 = not learned yet (kTargetStart); zero so the whole state stays in .bss (1.5 s rings)
    ULONGLONG calm_since = 0;
    int16_t   last = 0;
    uint32_t  skip_ctr = 0;
    ULONGLONG burst_from = 0;                              // the first frame after a long wait: the burst is counted from it
    int       burst_n = 0;
    bool      burst_counted = false;
    // statistics since the last VOICE line (voice_tick)
    uint32_t  w_frames = 0, w_underruns = 0, w_stalls = 0, w_sped = 0, w_dropped = 0, w_max_gap = 0, w_max_count = 0, w_lost = 0;
    ULONGLONG prev_at = 0;
};

struct State {
    CRITICAL_SECTION cs;
    bool        cs_ready = false;
    volatile LONG want_mic = 0, want_sound = 0;
    volatile LONG speaking = 0;                           // the gate is open (written by the worker)
    volatile LONG dn_on = 0, dn_noise_cdb = 0, dn_gain_pct = 0, dn_blocks = 0;     // (all zero: g stays in .bss) the noise suppressor's state for the VOICE line (written by the worker)
    volatile LONG thread_up = 0, stop = 0;
    HANDLE      thread = nullptr;
    char        error[160] = {};
    bool        error_new = false;
    VoiceMsg    out[kOutMax];
    int         out_n = 0;
    uint16_t    out_seq = 0;
    Rx          rx[kMaxPeers];
    bool        was_available = false;
} g;

void ensure_cs() { if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; } }
struct Lock { Lock() { ensure_cs(); EnterCriticalSection(&g.cs); } ~Lock() { LeaveCriticalSection(&g.cs); } };

void set_error(const char* what, MMRESULT r, bool input) {
    char text[120] = {};
    if (input) waveInGetErrorTextA(r, text, sizeof(text)); else waveOutGetErrorTextA(r, text, sizeof(text));
    Lock lock;
    _snprintf_s(g.error, sizeof(g.error), _TRUNCATE, "%s: %s", what, text[0] ? text : "?");
    g.error_new = true;
    log_line_lvl(LogLevel::Warn, "VOICE", "!! %s (code %u)", g.error, (unsigned)r);
}

// ---- the worker --------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Capture {
    HWAVEIN   dev = nullptr;
    WAVEHDR   hdr[kBuffers] = {};
    int16_t   data[kBuffers][kBlock] = {};
    // the gate and the frame being assembled
    int16_t   pending[kFrame] = {};
    int       pending_n = 0;
    bool      pending_active = false;
    int16_t   preroll[kFrame] = {};
    bool      have_preroll = false;
    int       hold = 0;
    double    floor_level = 300.0;
    VoiceCodec codec;
    Denoiser  dn;                       // 0.1.137: the noise suppressor, before the gate
};
struct Playback {
    HWAVEOUT  dev = nullptr;
    WAVEHDR   hdr[kBuffers] = {};
    int16_t   data[kBuffers][kBlock] = {};
    bool      queued[kBuffers] = {};
};

bool open_capture(Capture& c, HANDLE ev) {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = kRate; f.wBitsPerSample = 16;
    f.nBlockAlign = 2; f.nAvgBytesPerSec = kRate * 2;
    MMRESULT r = waveInOpen(&c.dev, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { c.dev = nullptr; set_error("the microphone cannot be opened", r, true); return false; }
    for (int i = 0; i < kBuffers; ++i) {
        c.hdr[i] = WAVEHDR{};
        c.hdr[i].lpData = (LPSTR)c.data[i]; c.hdr[i].dwBufferLength = kBlock * 2;
        if (waveInPrepareHeader(c.dev, &c.hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR || waveInAddBuffer(c.dev, &c.hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            set_error("the microphone buffers cannot be set up", MMSYSERR_ERROR, true);
            waveInReset(c.dev);
            for (int j = 0; j <= i; ++j) waveInUnprepareHeader(c.dev, &c.hdr[j], sizeof(WAVEHDR));
            waveInClose(c.dev); c.dev = nullptr; return false;
        }
    }
    r = waveInStart(c.dev);
    if (r != MMSYSERR_NOERROR) {
        set_error("the microphone cannot be started", r, true);
        waveInReset(c.dev);
        for (int j = 0; j < kBuffers; ++j) waveInUnprepareHeader(c.dev, &c.hdr[j], sizeof(WAVEHDR));
        waveInClose(c.dev); c.dev = nullptr; return false;
    }
    c.pending_n = 0; c.hold = 0; c.have_preroll = false; c.pending_active = false; c.floor_level = 300.0; c.codec = VoiceCodec{}; c.dn.reset();
    log_line("VOICE", "the microphone is open (%d Hz, mono)", kRate);
    return true;
}
void close_capture(Capture& c) {
    if (!c.dev) return;
    waveInStop(c.dev); waveInReset(c.dev);
    for (int i = 0; i < kBuffers; ++i) waveInUnprepareHeader(c.dev, &c.hdr[i], sizeof(WAVEHDR));
    waveInClose(c.dev); c.dev = nullptr;
    InterlockedExchange(&g.speaking, 0);
    log_line("VOICE", "the microphone is closed");
}

bool open_playback(Playback& p, HANDLE ev) {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = kRate; f.wBitsPerSample = 16;
    f.nBlockAlign = 2; f.nAvgBytesPerSec = kRate * 2;
    const MMRESULT r = waveOutOpen(&p.dev, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { p.dev = nullptr; set_error("the speakers cannot be opened", r, false); return false; }
    for (int i = 0; i < kBuffers; ++i) {
        p.hdr[i] = WAVEHDR{}; p.queued[i] = false;
        p.hdr[i].lpData = (LPSTR)p.data[i]; p.hdr[i].dwBufferLength = kBlock * 2;
        if (waveOutPrepareHeader(p.dev, &p.hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            set_error("the speaker buffers cannot be set up", MMSYSERR_ERROR, false);
            for (int j = 0; j < i; ++j) waveOutUnprepareHeader(p.dev, &p.hdr[j], sizeof(WAVEHDR));
            waveOutClose(p.dev); p.dev = nullptr; return false;
        }
    }
    log_line("VOICE", "the speakers are open (%d Hz, mono)", kRate);
    return true;
}
void close_playback(Playback& p) {
    if (!p.dev) return;
    waveOutReset(p.dev);
    for (int i = 0; i < kBuffers; ++i) waveOutUnprepareHeader(p.dev, &p.hdr[i], sizeof(WAVEHDR));
    waveOutClose(p.dev); p.dev = nullptr;
    log_line("VOICE", "the speakers are closed");
}

void queue_frame(const int16_t* pcm, VoiceCodec& codec) {
    VoiceMsg m;
    m.pred = (int16_t)pcm[0];
    m.index = (uint8_t)codec.index;
    m.samples = kFrame;
    m.len = (uint16_t)voice_adpcm_encode(pcm, kFrame, codec, m.data);
    Lock lock;
    m.seq = g.out_seq++;
    if (g.out_n >= kOutMax) { memmove(&g.out[0], &g.out[1], sizeof(VoiceMsg) * (kOutMax - 1)); --g.out_n; }       // the main thread is behind: the oldest goes
    g.out[g.out_n++] = m;
}

// One 20 ms block from the microphone: the gate, the frame, the encoder.
void process_block(Capture& c, const int16_t* raw) {
    // THE NOISE SUPPRESSOR (0.1.137; no setting since 0.1.138 -- always on with the voice chat): on a copy of the device's block, before the gate measures it (a quieter
    // room also opens the gate less often). It learns the room faster while the gate is closed.
    int16_t pcm[kBlock];
    memcpy(pcm, raw, sizeof(pcm));
    c.dn.process(pcm, kBlock, c.hold <= 0);
    {
        static int n = 0;
        if (++n >= 50) {                                       // once a second
            n = 0;
            InterlockedExchange(&g.dn_on, c.dn.enabled ? 1 : 0);
            InterlockedExchange(&g.dn_noise_cdb, (LONG)(c.dn.noise_db() * 10.0f));
            InterlockedExchange(&g.dn_gain_pct, (LONG)(c.dn.mean_gain_and_clear() * 100.0f + 0.5f));
            InterlockedIncrement(&g.dn_blocks);
        }
    }
    double sum = 0;
    for (int i = 0; i < kBlock; ++i) sum += std::fabs((double)pcm[i]);
    const double level = sum / kBlock;
    const double threshold = c.floor_level * 3.0 > 450.0 ? c.floor_level * 3.0 : 450.0;
    if (level > threshold) c.hold = kHold;
    else { if (c.hold > 0) --c.hold; c.floor_level += (level - c.floor_level) * 0.02; if (c.floor_level > 4000.0) c.floor_level = 4000.0; }
    const bool active = c.hold > 0;
    InterlockedExchange(&g.speaking, active ? 1 : 0);
    if (active) c.pending_active = true;
    memcpy(c.pending + c.pending_n, pcm, kBlock * 2);
    c.pending_n += kBlock;
    if (c.pending_n < kFrame) return;
    c.pending_n = 0;
    if (c.pending_active) {
        if (c.have_preroll) { queue_frame(c.preroll, c.codec); c.have_preroll = false; }             // the frame before the voice: its first syllable
        queue_frame(c.pending, c.codec);
    } else {
        memcpy(c.preroll, c.pending, sizeof(c.preroll)); c.have_preroll = true;
    }
    c.pending_active = false;
}

void service_capture(Capture& c) {
    for (int i = 0; i < kBuffers; ++i) {
        if (!(c.hdr[i].dwFlags & WHDR_DONE)) continue;
        if (c.hdr[i].dwBytesRecorded == kBlock * 2) process_block(c, c.data[i]);
        c.hdr[i].dwFlags &= ~WHDR_DONE;
        c.hdr[i].dwBytesRecorded = 0;
        waveInAddBuffer(c.dev, &c.hdr[i], sizeof(WAVEHDR));
    }
}

// THE VOLUME (0.1.133, the room panel's slider): a gain in percent on everything the speakers send, applied in the mix (the clamp below keeps a loud mix from wrapping).
volatile LONG g_gain_pct = 100;

// Mix what the speakers have said into the next 20 ms. False when nobody has anything to play.
bool mix_block(int16_t* out) {
    int acc[kBlock] = {};
    bool any = false;
    Lock lock;
    const ULONGLONG now = g_clock();
    for (int p = 0; p < kMaxPeers; ++p) {
        Rx& r = g.rx[p];
        if (!r.target) r.target = kTargetStart;
        if ((uint32_t)r.count > r.w_max_count) r.w_max_count = (uint32_t)r.count;
        if (r.count > kHardMax) {                                      // far behind (a long stall's whole backlog): back to the target at once
            const int drop = r.count - r.target;
            r.head = (r.head + drop) % kRing; r.count -= drop; r.w_dropped += (uint32_t)drop;
        }
        if (!r.playing) { if (r.count >= r.target) { r.playing = true; if (!r.calm_since) r.calm_since = now; } else continue; }
        const bool fast = r.count > r.target + kFastOver;               // behind: 6 % faster (one sample in 16 skipped) until it is back
        int i = 0;
        while (i < kBlock && r.count > 0) {
            const int16_t v = r.ring[r.head]; r.head = (r.head + 1) % kRing; --r.count;
            if (fast && (++r.skip_ctr & 15) == 0 && r.count > 0) { r.head = (r.head + 1) % kRing; --r.count; ++r.w_sped; }
            acc[i++] += v; r.last = v;
        }
        if (i < kBlock) {                                               // under-run (a stall, or the end of what was said): fade out, no click; wait for the target again
            int v = r.last;
            for (; i < kBlock; ++i) { v = v * 7 / 8; acc[i] += v; }
            r.last = 0;
            r.playing = false;
            ++r.w_underruns;
        } else if (r.calm_since && now - r.calm_since > kCalmMs && r.target > kTargetMin) {
            r.target = r.target - kTargetDown < kTargetMin ? kTargetMin : r.target - kTargetDown;
            r.calm_since = now;
        }
        any = true;
    }
    if (!any) return false;
    const int gain = (int)InterlockedCompareExchange(&g_gain_pct, 0, 0);
    if (gain != 100) for (int i = 0; i < kBlock; ++i) acc[i] = acc[i] * gain / 100;
    for (int i = 0; i < kBlock; ++i) out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    return true;
}

void service_playback(Playback& p) {
    int in_flight = 0;
    for (int i = 0; i < kBuffers; ++i) {
        if (p.queued[i] && (p.hdr[i].dwFlags & WHDR_DONE)) p.queued[i] = false;
        if (p.queued[i]) ++in_flight;
    }
    for (int i = 0; i < kBuffers && in_flight < 4; ++i) {              // at most 80 ms in the device: the delay stays short
        if (p.queued[i]) continue;
        if (!mix_block(p.data[i])) break;
        p.hdr[i].dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(p.dev, &p.hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) { p.queued[i] = true; ++in_flight; }
    }
}

DWORD WINAPI worker(LPVOID) {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Capture* cap = new Capture();
    Playback* play = new Playback();
    ULONGLONG idle_since = 0;
    while (!g.stop) {
        const bool want_mic = g.want_mic != 0, want_sound = g.want_sound != 0;
        if (want_mic && !cap->dev) { if (!open_capture(*cap, ev)) InterlockedExchange(&g.want_mic, 0); }
        if (!want_mic && cap->dev) close_capture(*cap);
        if (want_sound && !play->dev) { if (!open_playback(*play, ev)) InterlockedExchange(&g.want_sound, 0); }
        if (!want_sound && play->dev) close_playback(*play);
        if (cap->dev) service_capture(*cap);
        if (play->dev) service_playback(*play);
        if (!cap->dev && !play->dev) {
            const ULONGLONG now = GetTickCount64();
            if (!idle_since) idle_since = now;
            if (g.want_mic || g.want_sound) { idle_since = 0; continue; }
            if (now - idle_since > 1500) break;                        // nothing wanted for a while: the worker ends (the next switch starts a new one)
            Sleep(50);
        } else {
            idle_since = 0;
            WaitForSingleObject(ev, 10);
        }
    }
    close_capture(*cap); close_playback(*play);
    delete cap; delete play;
    CloseHandle(ev);
    InterlockedExchange(&g.thread_up, 0);
    return 0;
}

void ensure_worker() {
    if (g.thread_up) return;
    if (g.thread) { WaitForSingleObject(g.thread, 2000); CloseHandle(g.thread); g.thread = nullptr; }
    InterlockedExchange(&g.stop, 0);
    InterlockedExchange(&g.thread_up, 1);
    g.thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!g.thread) { InterlockedExchange(&g.thread_up, 0); log_line_lvl(LogLevel::Error, "VOICE", "!! the audio worker could not be started"); }
}

} // namespace

// tests (test_voice): the mixer without the worker and its devices
bool voice_test_mix(int16_t* out) { return mix_block(out); }
void voice_test_sound(bool on) { ensure_cs(); InterlockedExchange(&g.want_sound, on ? 1 : 0); }
int voice_test_target(uint8_t peer) { return peer < kMaxPeers ? (g.rx[peer].target ? g.rx[peer].target : kTargetStart) * 1000 / kRate : -1; }
void voice_test_clock(ULONGLONG (*clock)()) { g_clock = clock ? clock : &GetTickCount64; }

bool voice_available() { return session_exp_voice() && net_active() && net_peer_count() >= 2; }
bool voice_mic_on() { return g.want_mic != 0; }
bool voice_sound_on() { return g.want_sound != 0; }

void voice_set_mic(bool on) {
    ensure_cs();
    if (on && !voice_available()) return;
    if ((g.want_mic != 0) == on) return;
    InterlockedExchange(&g.want_mic, on ? 1 : 0);
    log_line("VOICE", "the microphone is switched %s", on ? "ON" : "off");
    if (on) ensure_worker();
}
void voice_set_sound(bool on) {
    ensure_cs();
    if (on && !voice_available()) return;
    if ((g.want_sound != 0) == on) return;
    InterlockedExchange(&g.want_sound, on ? 1 : 0);
    log_line("VOICE", "the sound is switched %s", on ? "ON" : "off");
    if (on) ensure_worker();
    if (!on) { Lock lock; for (Rx& r : g.rx) { r.count = 0; r.head = 0; r.playing = false; } }
}

bool voice_self_speaking() { return g.want_mic && g.speaking; }
bool voice_peer_speaking(uint8_t peer) {
    if (peer >= kMaxPeers) return false;
    Lock lock;
    return g.rx[peer].last_at && GetTickCount64() - g.rx[peer].last_at < kSpeakingMs;
}

void voice_on_message(uint8_t from, const VoiceMsg& m) {
    if (from >= kMaxPeers || from == net_self() || !voice_available()) return;
    ensure_cs();
    const ULONGLONG now = g_clock();
    int16_t pcm[kFrame + 64];
    const bool wanted = g.want_sound != 0;
    bool ok = false;
    if (wanted && m.samples >= 2 && m.samples <= kFrame + 64) ok = voice_adpcm_decode(m.data, m.len, m.pred, m.index, m.samples, pcm);
    Lock lock;
    Rx& r = g.rx[from];
    if (!r.target) r.target = kTargetStart;
    if (!r.last_at || now - r.last_at > 2000) { r.count = 0; r.head = 0; r.playing = false; r.seq_known = false; r.prev_at = 0; }       // a new talk spurt (the target it learned stays)
    if (r.prev_at && now - r.prev_at > r.w_max_gap) r.w_max_gap = (uint32_t)(now - r.prev_at);          // the longest wait between two frames: the link's stalls
    if (r.prev_at && now - r.prev_at > kStallGapMs) { r.burst_from = now; r.burst_n = 1; r.burst_counted = false; }  // a long wait: is a burst coming?
    else if (r.burst_from && !r.burst_counted && now - r.burst_from <= kBurstWithinMs && ++r.burst_n >= kBurstFrames) {
        r.burst_counted = true;                                    // a stall of the link: the buffer must cover it next time
        ++r.w_stalls;
        r.target = r.target + kTargetUp > kTargetMax ? kTargetMax : r.target + kTargetUp;
        r.calm_since = now;
    }
    r.prev_at = now;
    r.last_at = now;
    ++r.w_frames;
    if (r.seq_known && (uint16_t)(r.seq + 1) != m.seq) { const uint16_t gap = (uint16_t)(m.seq - r.seq - 1); r.lost += gap; r.w_lost += gap; }
    r.seq = m.seq; r.seq_known = true;
    if (!ok) return;
    for (uint32_t i = 0; i < m.samples; ++i) {
        if (r.count >= kRing) { r.head = (r.head + 1) % kRing; --r.count; }                  // full: the oldest goes
        r.ring[(r.head + r.count) % kRing] = pcm[i]; ++r.count;
    }
}

int voice_volume() { return (int)InterlockedCompareExchange(&g_gain_pct, 0, 0); }
void voice_set_volume(int pct) {
    pct = pct < 0 ? 0 : pct > 200 ? 200 : pct;
    InterlockedExchange(&g_gain_pct, pct);
}

void voice_tick() {
    InterlockedExchange(&g_gain_pct, (LONG)config().voice_volume);     // 0.1.133: ui.voice_volume, live (the panel's slider changes the live value while it is dragged)
    {   // THE VOICE STATISTICS (0.1.134), every 10 s while somebody was heard: what the link did to each speaker's frames and what the jitter buffer did about it
        static ULONGLONG s_next = 0;
        const ULONGLONG now = GetTickCount64();
        if (g.cs_ready && now >= s_next) {
            s_next = now + 10000;
            Lock lock;
            for (int p = 0; p < kMaxPeers; ++p) {
                Rx& r = g.rx[p];
                if (!r.w_frames && !r.w_underruns) continue;
                log_line("VOICE", "from peer %d, last 10 s: %u frame(s) of 40 ms (%u missing by sequence), longest wait between two %u ms, %u stall(s) of the link (a wait, then a burst) | "
                         "played: %u under-run(s) (stalls and ends of speech), %u ms played faster, %u ms cut, buffer up to %u ms, target now %d ms%s", p, r.w_frames, r.w_lost, r.w_max_gap, r.w_stalls,
                         r.w_underruns, r.w_sped * 1000u / kRate, r.w_dropped * 1000u / kRate, r.w_max_count * 1000u / kRate, r.target * 1000 / kRate, g.want_sound ? "" : " (the sound is off here)");
                r.w_frames = r.w_underruns = r.w_stalls = r.w_sped = r.w_dropped = r.w_max_gap = r.w_max_count = r.w_lost = 0;
            }
            if (g.want_mic && InterlockedExchange(&g.dn_blocks, 0))
                log_line("VOICE", "microphone: noise suppressor %s, room noise %.1f dB (16-bit power per bin), mean gain over the spectrum %ld%% in the last second (100%% = untouched)",
                         g.dn_on ? "ON" : "off", g.dn_noise_cdb / 10.0, (long)g.dn_gain_pct);
        }
    }
    ensure_cs();
    const bool avail = voice_available();
    if (!avail && (g.want_mic || g.want_sound || g.was_available)) {
        if (g.want_mic || g.want_sound) log_line("VOICE", "voice is no longer available (the host's rule is off, or the room is gone): microphone and sound off");
        voice_reset();
    }
    g.was_available = avail;
    if ((g.want_mic || g.want_sound) && !g.thread_up) ensure_worker();      // a worker that ended just as a switch was turned on
    // What the worker has encoded goes out from here: the network is the main thread's.
    VoiceMsg send[kOutMax];
    int n = 0;
    {
        Lock lock;
        n = g.out_n;
        if (n) memcpy(send, g.out, sizeof(VoiceMsg) * n);
        g.out_n = 0;
    }
    if (avail && g.want_mic) for (int i = 0; i < n; ++i) net_send_voice(send[i]);
    // A device that would not open: said once, in words.
    char err[160] = {};
    {
        Lock lock;
        if (g.error_new) { strncpy_s(err, sizeof(err), g.error, _TRUNCATE); g.error_new = false; }
    }
    if (err[0]) {
        char line[260];
        _snprintf_s(line, sizeof(line), _TRUNCATE, tr(Tx::V_DEVICE_FAIL), err);
        room_toast(line);
    }
}

void voice_reset() {
    ensure_cs();
    InterlockedExchange(&g.want_mic, 0);
    InterlockedExchange(&g.want_sound, 0);
    Lock lock;
    g.out_n = 0;
    for (Rx& r : g.rx) r = Rx{};
}

} // namespace mgmp
