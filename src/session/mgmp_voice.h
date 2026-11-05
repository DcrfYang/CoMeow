// mgmp_voice.h -- EXPERIMENTAL VOICE CHAT (ui.exp_voice, the HOST's setting, HELLO rules bit 4, proto 107, 0.1.118).
//
// With the host's "voice chat" on, the room panel gets two buttons, a microphone and a speaker. The microphone sends what this player says to the others; the speaker plays what the
// others say. Both start OFF (nothing is recorded until the player turns the microphone on) and neither works unless the host's rule is on.
//
// Audio is 16 kHz mono 16-bit, cut into 40 ms frames and packed with IMA ADPCM (4 bits a sample: 8 KB/s while talking, nothing while silent -- a simple energy gate with a
// short hold, so the room's relay budget is not spent on breathing). A frame carries its own predictor and step index, so none depends on another. Every frame is one
// MSG_VOICE, authored by the speaker and relayed by the host to the other clients; the receiver keeps a short jitter buffer per speaker, mixes them and plays them. There is no
// echo cancelling: headphones are advised (the setting's note says so).
//
// Threads: ONE worker owns the two Windows (winmm) devices and the codec. It never touches the network: encoded frames wait in a small queue that voice_tick (main thread, every
// frame) sends, and a received frame is decoded into the speaker's buffer by voice_on_message (main thread) for the worker to mix.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

bool voice_available();                    // the host's rule is on, and this peer is in a session with somebody else
bool voice_mic_on();
bool voice_sound_on();
void voice_set_mic(bool on);               // refused (stays off) while voice_available() is false; opens the device on the worker
void voice_set_sound(bool on);
int  voice_volume();                       // percent, 0 .. 200 (ui.voice_volume): the gain the other players' voices are mixed with
void voice_set_volume(int pct);            // the live value (the worker reads it at the next block); the panel persists it
bool voice_self_speaking();                // the microphone is on and the gate is open
bool voice_peer_speaking(uint8_t peer);    // a frame of that peer arrived in the last half second (whether or not the sound is on)
void voice_on_message(uint8_t from, const VoiceMsg& m);
void voice_tick();                         // every frame, main thread: sends the worker's frames, shuts voice down when it is no longer available, says a device error once
void voice_reset();                        // a room ended: microphone and sound off, buffers dropped
bool voice_test_mix(int16_t* out);         // tests: one 20 ms block of the mix (320 samples); false when nothing plays
void voice_test_sound(bool on);            // tests: the sound switch without the worker
int  voice_test_target(uint8_t peer);      // tests: that speaker's jitter-buffer target in ms
void voice_test_clock(unsigned long long (*clock)());   // tests: the clock the buffer reads (nullptr: GetTickCount64)

} // namespace mgmp
