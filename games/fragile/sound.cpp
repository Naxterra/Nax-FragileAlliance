// Native replacements of the HMI SOS sound library in FRAGILE.EXE (original 000734CB to 0007A2D2). The
// library loaded the DOS drivers HMIDRV.386 and HMIDET.386 and played the game's samples through a Sound
// Blaster. Here the game's calls to the library's digital-sample interface drive a software mixer whose
// output goes to Windows through XAudio2. The game's own code still decides what plays: it streams music and
// speech from the WAV files in parts and hands each part over from the callback that the library called at
// the end of a buffer, and that callback is called here at the same point.
//
// The library's timer service stays original code: it only counts ticks and calls the game's events. The
// mixer runs where the game called the driver's service routine on every tick of its 120 Hz timer event.
#include "games/fragile/game.h"

#include <string>

namespace nax::native {

namespace {
// The sample description that the game hands to sosDIGIStartSample and sosDIGIContinueSample. The offsets
// were read off the library's code at 00073C82.
constexpr uint32_t kSampleData = 0x00;       // far pointer: offset, then selector
constexpr uint32_t kSampleSize = 0x08;       // bytes
constexpr uint32_t kSampleLoops = 0x0C;      // word
constexpr uint32_t kSampleChannel = 0x10;    // word: 0 left, 1 right, 2 both, 3 interleaved stereo data
constexpr uint32_t kSampleVolume = 0x14;     // word, 0 to 7FFFh
constexpr uint32_t kSampleCallback = 0x1C;   // far pointer to void cdecl callback(driver, action, sample)
constexpr uint32_t kSampleFlags = 0x28;      // word
constexpr uint32_t kSampleRangeStart = 0x30; // with flag 40h: the part of the buffer that plays
constexpr uint32_t kSampleRangeLength = 0x34;
constexpr uint32_t kSamplePitch = 0x38;      // with flag 400h: playback speed as a 16.16 number
constexpr uint32_t kSamplePan = 0x40;        // word: 0 left, 8000h middle, FFFFh right

constexpr uint16_t kFlagRange = 0x0040;
constexpr uint16_t kFlagPitch = 0x0400;
constexpr uint16_t kFlagEightBit = 0x0020;   // the game sets it for 8-bit data on the 16-bit device it was told of

constexpr uint32_t kActionProcessed = 0;     // the buffer has been played; the callback may hand over the next one
constexpr uint32_t kActionDone = 2;          // the sample has ended

// The capabilities that sosDIGIDetectGetCaps fills in; the game reads only these two.
constexpr uint32_t kCapsBits = 0x24;
constexpr uint32_t kCapsStereo = 0x28;

constexpr uint32_t kDriverHandle = 0;
constexpr int kLeadFrames = Game::kSoundRate * 60 / 1000;  // sound handed to Windows ahead of playback
constexpr int kMixFrames = Game::kSoundRate / 200;          // mixed in steps of 5 ms

uint32_t far_address(Game& g, uint32_t offset, uint16_t selector) { return g.segment_base(selector) + offset; }

uint32_t sample_struct(Game& g) { return far_address(g, g.c.b.e, uint16_t(g.c.c.e)); }

bool valid_voice(Game& g, uint32_t voice) { return voice < uint32_t(Game::kSoundVoices) && g.mixer.voice(int(voice)).active; }

// The buffer that a sample description names: the whole buffer, or the range that flag 40h selects.
void sample_buffer(Game& g, uint32_t s, uint32_t* address, uint32_t* bytes) {
    *address = far_address(g, g.rd32(s + kSampleData), g.rd16(s + kSampleData + 4));
    *bytes = g.rd32(s + kSampleSize);
    if (g.rd16(s + kSampleFlags) & kFlagRange) {
        *address += g.rd32(s + kSampleRangeStart);
        *bytes = g.rd32(s + kSampleRangeLength);
    }
}

// Calls the game's sample callback as the library did: far, with the C calling convention, from the sound
// interrupt, and with every register as it was afterwards.
void call_sample_callback(Game& g, int voice, uint32_t action) {
    const Game::SampleCallback& callback = g.sample_callbacks[voice];
    if (!callback.offset) return;
    const Cpu saved = g.c;
    g.push32(uint32_t(voice));
    g.push32(action);
    g.push32(kDriverHandle);
    g.push32(callback.selector ? callback.selector : g.c.sel[CS]);
    g.push32(Machine::kNativeReturn);
    g.c.iflag = false;
    g.c.eip = callback.offset;
    g.run_until(Machine::kNativeReturn, saved.sp.e - 12);
    const Fpu fpu = g.c.fpu;
    g.c = saved;
    g.c.fpu = fpu;
}

class SampleEvents : public MixListener {
public:
    explicit SampleEvents(Game& g) : g_(g) {}
    void buffer_finished(int voice) override {
        Game::SampleCallback& callback = g_.sample_callbacks[voice];
        if (callback.pending) {
            callback.pending = false;
            g_.mixer.continue_voice(voice, callback.pending_address, callback.pending_bytes);
            return;
        }
        call_sample_callback(g_, voice, kActionProcessed);
    }
    void voice_finished(int voice) override {
        call_sample_callback(g_, voice, kActionDone);
        g_.sample_callbacks[voice] = {};
    }

private:
    Game& g_;
};

// Keeps about 60 ms of mixed sound queued at the device.
void pump(Game& g) {
    if (!g.audio.is_open() || g.sound_mixing) return;
    g.sound_mixing = true;
    SampleEvents events(g);
    int16_t block[kMixFrames * 2];
    while (g.audio.can_submit() && g.audio.queued_frames() < kLeadFrames) {
        g.mixer.mix(block, kMixFrames, g.mem, &events);
        g.audio.submit(block, kMixFrames);
    }
    g.sound_mixing = false;
}

void stop_all(Game& g) {
    for (int i = 0; i < Game::kSoundVoices; i++) {
        g.mixer.stop(i);
        g.sample_callbacks[i] = {};
    }
}
}  // namespace

// Original 000787B6: sosDIGIInitSystem(path, flags). Nothing has to be loaded.
void sound_digital_init_system(Machine& m) {
    m.c.a.e = 0;
    m.ret();
}

// Original 0007882F: sosDIGIUnInitSystem().
void sound_digital_uninit_system(Machine& m) {
    m.c.a.e = 0;
    m.ret();
}

// Original 0007886F: sosDIGIInitDriver(driver id, far hardware settings, far init info, far handle), which
// loaded the card's driver from HMIDRV.386. The init info carries the sample rate the game asks for at +10h.
// A non-zero result makes the game print "Failed" and play without sound, which is also what happens here
// when Windows has no sound device.
void sound_digital_init_driver(Machine& m) {
    Game& g = game(m);
    const uint32_t stack = g.c.base[SS] + g.c.sp.e;
    const uint32_t info = far_address(g, g.rd32(stack + 4), g.rd16(stack + 8));
    const uint32_t handle = far_address(g, g.rd32(stack + 12), g.rd16(stack + 16));
    const uint32_t rate = g.rd32(info + 0x10);
    std::string error;
    if (!g.audio.is_open()) {
        // The game opens the driver once at start-up; the movie players open it again for their sound track
        // while the game's own samples may still play, so only the first opening sets the device up.
        g.sound_source_rate = rate >= 4000 && rate <= 48000 ? int(rate) : 22050;
        stop_all(g);
        if (!g.audio.open(Game::kSoundRate, &error)) {
            g.log("sound: %s; the game runs without sound", error.c_str());
            g.c.a.e = 1;
            g.ret(16);
            return;
        }
        g.mixer.set_master_volume(0x7FFF);
        g.log("sound: XAudio2 output at %d Hz, game samples at %d Hz", Game::kSoundRate, g.sound_source_rate);
    }
    g.sound_driver_opens++;
    g.wr32(handle, kDriverHandle);
    g.c.a.e = 0;
    g.ret(16);
}

// Original 0007912F: sosDIGIUnInitDriver(handle, free buffer, free driver). The device closes when the last
// opening is undone.
void sound_digital_uninit_driver(Machine& m) {
    Game& g = game(m);
    if (g.sound_driver_opens > 0 && --g.sound_driver_opens == 0) {
        stop_all(g);
        g.audio.close();
    }
    g.c.a.e = 0;
    g.ret();
}

// Original 000795E9: sosDIGIDetectInit(path), which loaded HMIDET.386.
void sound_detect_init(Machine& m) {
    m.c.a.e = 0;
    m.ret();
}

// Original 00079A41: sosDIGIDetectGetCaps(driver id, far capabilities). The mixer takes 16-bit stereo.
void sound_detect_caps(Machine& m) {
    Game& g = game(m);
    const uint32_t caps = sample_struct(g);
    g.wr32(caps + kCapsBits, 16);
    g.wr32(caps + kCapsStereo, 1);
    g.c.a.e = 0;
    g.ret();
}

// Original 00079733: sosDIGIDetectUnInit().
void sound_detect_uninit(Machine& m) {
    m.c.a.e = 0;
    m.ret();
}

// Original 00073C82: sosDIGIStartSample(handle, far sample). Returns the sample handle, or -1 when every
// voice is busy.
void sound_start_sample(Machine& m) {
    Game& g = game(m);
    const uint32_t s = sample_struct(g);
    const int voice = g.mixer.free_voice();
    if (voice < 0 || !g.audio.is_open()) {
        g.c.a.e = 0xFFFFFFFFu;
        g.ret();
        return;
    }
    uint32_t address, bytes;
    sample_buffer(g, s, &address, &bytes);
    const uint16_t flags = g.rd16(s + kSampleFlags);
    const uint16_t channel = g.rd16(s + kSampleChannel);
    MixFormat format;
    format.sixteen_bit = !(flags & kFlagEightBit);
    format.stereo = channel == 3;
    const uint32_t step = (flags & kFlagPitch) ? g.rd32(s + kSamplePitch) : 0x10000u;
    g.mixer.start(voice, address, bytes, format, g.sound_source_rate, step);
    MixVoice& v = g.mixer.voice(voice);
    v.volume = g.rd16(s + kSampleVolume);
    v.pan = g.rd16(s + kSamplePan);
    v.left_only = channel == 0;
    v.right_only = channel == 1;
    const int16_t loops = int16_t(g.rd16(s + kSampleLoops));
    v.loops = loops < 0 ? -1 : loops;
    Game::SampleCallback& callback = g.sample_callbacks[voice];
    callback = {};
    callback.offset = g.rd32(s + kSampleCallback);
    callback.selector = g.rd16(s + kSampleCallback + 4);
    g.c.a.e = uint32_t(voice);
    g.ret();
}

// Original 0007429B: sosDIGIStopSample(handle, sample). The game marks its own records as finished, so no
// callback is made.
void sound_stop_sample(Machine& m) {
    Game& g = game(m);
    const uint32_t voice = g.c.d.e;
    if (voice < uint32_t(Game::kSoundVoices)) {
        g.mixer.stop(int(voice));
        g.sample_callbacks[voice] = {};
    }
    g.c.a.e = 0;
    g.ret();
}

// Original 00074349: sosDIGIContinueSample(handle, sample, far sample). Hands over the next buffer of a
// playing sample. Inside the callback at the end of a buffer it plays on at once; otherwise it follows when the
// current buffer has been played.
void sound_continue_sample(Machine& m) {
    Game& g = game(m);
    const uint32_t voice = g.c.d.e;
    if (voice < uint32_t(Game::kSoundVoices)) {
        uint32_t address, bytes;
        sample_buffer(g, sample_struct(g), &address, &bytes);
        MixVoice& v = g.mixer.voice(int(voice));
        if (v.active && v.ended) {
            g.mixer.continue_voice(int(voice), address, bytes);
        } else if (v.active) {
            Game::SampleCallback& callback = g.sample_callbacks[voice];
            callback.pending = true;
            callback.pending_address = address;
            callback.pending_bytes = bytes;
        }
    }
    g.c.a.e = voice;
    g.ret();
}

// Original 00074716: sosDIGISetSampleVolume(handle, sample, volume). Returns the previous volume.
void sound_set_sample_volume(Machine& m) {
    Game& g = game(m);
    const uint32_t voice = g.c.d.e;
    uint32_t previous = 0;
    if (valid_voice(g, voice)) {
        MixVoice& v = g.mixer.voice(int(voice));
        previous = uint32_t(v.volume);
        v.volume = int(int16_t(g.c.b.x));
    }
    g.c.a.e = previous;
    g.ret();
}

// Original 000749B9: sosDIGISetPanLocation(handle, sample, position). Returns the previous position.
void sound_set_pan(Machine& m) {
    Game& g = game(m);
    const uint32_t voice = g.c.d.e;
    uint32_t previous = 0;
    if (valid_voice(g, voice)) {
        MixVoice& v = g.mixer.voice(int(voice));
        previous = uint32_t(v.pan);
        v.pan = int(g.c.b.x);
    }
    g.c.a.e = previous;
    g.ret();
}

// Original 00074D0E: sosDIGISetMasterVolume(handle, volume).
void sound_set_master_volume(Machine& m) {
    Game& g = game(m);
    g.mixer.set_master_volume(int(int16_t(g.c.d.x)));
    g.c.a.e = 0;
    g.ret();
}

// Original 0007A2D2: the number of samples that are still playing on a driver.
void sound_samples_playing(Machine& m) {
    Game& g = game(m);
    g.c.a.e = uint32_t(g.mixer.active_voices());
    g.ret();
}

// Site 0006BF28 in the game's 120 Hz timer event: the far call of the driver's service routine, which mixed
// the next part of the sound card's buffer.
void sound_service(Machine& m) { pump(game(m)); }

}  // namespace nax::native
