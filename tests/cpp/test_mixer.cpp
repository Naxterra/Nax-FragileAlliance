// Cases for tests/test_mixer.py. Each case returns 0 when it passes and prints what went wrong otherwise.
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "runtime/mixer.h"

using namespace nax;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("failed: %s\n", what);
        failures++;
    }
}

// Guest memory for the cases: a flat buffer that sample data is written into.
struct Memory {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x10000, 0);
    uint32_t put16(uint32_t at, const std::vector<int16_t>& samples) {
        std::memcpy(bytes.data() + at, samples.data(), samples.size() * 2);
        return at;
    }
    uint32_t put8(uint32_t at, const std::vector<uint8_t>& samples) {
        std::memcpy(bytes.data() + at, samples.data(), samples.size());
        return at;
    }
};

struct Events : MixListener {
    std::function<void(int)> on_buffer;
    int buffers = 0, finished = 0;
    void buffer_finished(int voice) override {
        buffers++;
        if (on_buffer) on_buffer(voice);
    }
    void voice_finished(int) override { finished++; }
};

MixFormat mono16() { return MixFormat{true, false}; }

int eight_and_sixteen_bit_samples_play_at_the_same_level() {
    Memory memory;
    Mixer mixer(2, 22050);
    const uint32_t a = memory.put16(0x100, std::vector<int16_t>(64, 16384));
    const uint32_t b = memory.put8(0x400, std::vector<uint8_t>(64, 0xC0));  // 0C0h is +64 of 128, half scale
    mixer.start(0, a, 128, mono16(), 22050, 0x10000);
    mixer.start(1, b, 64, MixFormat{false, false}, 22050, 0x10000);
    mixer.voice(1).pan = mixer.voice(0).pan = 0x8000;
    std::vector<int16_t> out(32 * 2);
    mixer.stop(1);
    mixer.mix(out.data(), 32, memory.bytes.data(), nullptr);
    const int sixteen = out[10];
    mixer.start(1, b, 64, MixFormat{false, false}, 22050, 0x10000);
    mixer.stop(0);
    mixer.mix(out.data(), 32, memory.bytes.data(), nullptr);
    const int eight = out[10];
    check(sixteen > 16000 && sixteen < 16400, "a half-scale 16-bit sample plays at half scale");
    check(eight == sixteen, "the same level as an 8-bit sample plays at the same level");
    return failures;
}

int next_buffer_given_at_the_end_plays_without_a_gap() {
    Memory memory;
    Mixer mixer(1, 22050);
    const uint32_t first = memory.put16(0x100, std::vector<int16_t>(40, 1000));
    const uint32_t second = memory.put16(0x400, std::vector<int16_t>(40, 2000));
    mixer.start(0, first, 80, mono16(), 22050, 0x10000);
    Events events;
    bool handed_over = false;
    events.on_buffer = [&](int voice) {
        if (!handed_over) {
            mixer.continue_voice(voice, second, 80);
            handed_over = true;
        }
    };
    std::vector<int16_t> out(100 * 2);
    mixer.mix(out.data(), 100, memory.bytes.data(), &events);
    check(out[39 * 2] > 990 && out[39 * 2] < 1010, "the first buffer plays to its last frame");
    check(out[40 * 2] > 1990 && out[40 * 2] < 2010, "the second buffer follows on the very next frame");
    check(out[79 * 2] > 1990, "the second buffer plays to its end");
    check(out[80 * 2] == 0, "nothing plays after the second buffer");
    check(events.buffers == 2, "the end of each buffer is reported once");
    check(events.finished == 1, "the voice is reported as finished once, after the second buffer");
    check(!mixer.voice(0).active, "the voice is free again");
    return failures;
}

int voice_without_a_next_buffer_ends_and_is_reported_once() {
    Memory memory;
    Mixer mixer(1, 22050);
    mixer.start(0, memory.put16(0x100, std::vector<int16_t>(10, 500)), 20, mono16(), 22050, 0x10000);
    Events events;
    std::vector<int16_t> out(50 * 2);
    mixer.mix(out.data(), 50, memory.bytes.data(), &events);
    mixer.mix(out.data(), 50, memory.bytes.data(), &events);
    check(events.finished == 1, "voice_finished is called exactly once");
    check(mixer.active_voices() == 0, "no voice is left playing");
    return failures;
}

int stopping_inside_the_buffer_callback_ends_without_a_finish_report() {
    Memory memory;
    Mixer mixer(1, 22050);
    mixer.start(0, memory.put16(0x100, std::vector<int16_t>(10, 500)), 20, mono16(), 22050, 0x10000);
    Events events;
    events.on_buffer = [&](int voice) { mixer.stop(voice); };
    std::vector<int16_t> out(50 * 2);
    mixer.mix(out.data(), 50, memory.bytes.data(), &events);
    check(events.finished == 0, "a voice stopped by its owner is not reported as finished");
    check(mixer.active_voices() == 0, "the stopped voice is free");
    return failures;
}

int sample_at_half_the_output_rate_lasts_twice_as_long() {
    Memory memory;
    Mixer mixer(1, 44100);
    mixer.start(0, memory.put16(0x100, std::vector<int16_t>(100, 3000)), 200, mono16(), 22050, 0x10000);
    std::vector<int16_t> out(300 * 2);
    mixer.mix(out.data(), 300, memory.bytes.data(), nullptr);
    check(out[199 * 2] > 2990, "100 frames at 22050 Hz still play at output frame 199 of 44100 Hz");
    check(out[201 * 2] == 0, "and have ended two frames later");
    return failures;
}

int pan_to_the_left_silences_the_right_channel() {
    Memory memory;
    Mixer mixer(1, 22050);
    mixer.start(0, memory.put16(0x100, std::vector<int16_t>(10, 8000)), 20, mono16(), 22050, 0x10000);
    mixer.voice(0).pan = 0;
    std::vector<int16_t> out(5 * 2);
    mixer.mix(out.data(), 5, memory.bytes.data(), nullptr);
    check(out[0] > 7900, "the left channel plays at full level");
    check(out[1] == 0, "the right channel is silent");
    return failures;
}

int interleaved_stereo_keeps_its_channels_apart() {
    Memory memory;
    Mixer mixer(1, 22050);
    std::vector<int16_t> data;
    for (int i = 0; i < 10; i++) {
        data.push_back(6000);
        data.push_back(-6000);
    }
    mixer.start(0, memory.put16(0x100, data), uint32_t(data.size() * 2), MixFormat{true, true}, 22050, 0x10000);
    std::vector<int16_t> out(5 * 2);
    mixer.mix(out.data(), 5, memory.bytes.data(), nullptr);
    check(out[2] > 5900 && out[3] < -5900, "left and right samples go to their own channels");
    return failures;
}

int empty_buffers_handed_over_forever_do_not_stop_the_mixer() {
    Memory memory;
    Mixer mixer(1, 22050);
    mixer.start(0, memory.put16(0x100, std::vector<int16_t>(4, 100)), 8, mono16(), 22050, 0x10000);
    Events events;
    events.on_buffer = [&](int voice) { mixer.continue_voice(voice, 0x100, 0); };
    std::vector<int16_t> out(50 * 2);
    mixer.mix(out.data(), 50, memory.bytes.data(), &events);
    check(mixer.active_voices() == 0, "the voice gives up after a run of empty buffers");
    check(events.finished == 1, "and is reported as finished");
    return failures;
}

int loud_voices_together_are_clipped_not_wrapped() {
    Memory memory;
    Mixer mixer(4, 22050);
    for (int i = 0; i < 4; i++)
        mixer.start(i, memory.put16(0x100 + i * 0x100, std::vector<int16_t>(10, 30000)), 20, mono16(), 22050, 0x10000);
    std::vector<int16_t> out(5 * 2);
    mixer.mix(out.data(), 5, memory.bytes.data(), nullptr);
    check(out[0] == 32767 && out[1] == 32767, "the sum is held at the largest sample value");
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    const struct {
        const char* name;
        int (*run)();
    } cases[] = {
        {"eight_and_sixteen_bit_samples_play_at_the_same_level", eight_and_sixteen_bit_samples_play_at_the_same_level},
        {"next_buffer_given_at_the_end_plays_without_a_gap", next_buffer_given_at_the_end_plays_without_a_gap},
        {"voice_without_a_next_buffer_ends_and_is_reported_once", voice_without_a_next_buffer_ends_and_is_reported_once},
        {"stopping_inside_the_buffer_callback_ends_without_a_finish_report",
         stopping_inside_the_buffer_callback_ends_without_a_finish_report},
        {"sample_at_half_the_output_rate_lasts_twice_as_long", sample_at_half_the_output_rate_lasts_twice_as_long},
        {"pan_to_the_left_silences_the_right_channel", pan_to_the_left_silences_the_right_channel},
        {"interleaved_stereo_keeps_its_channels_apart", interleaved_stereo_keeps_its_channels_apart},
        {"empty_buffers_handed_over_forever_do_not_stop_the_mixer", empty_buffers_handed_over_forever_do_not_stop_the_mixer},
        {"loud_voices_together_are_clipped_not_wrapped", loud_voices_together_are_clipped_not_wrapped},
    };
    for (const auto& c : cases)
        if (name == c.name) return c.run();
    std::printf("unknown case %s\n", name.c_str());
    return 2;
}
