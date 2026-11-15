#include "runtime/mixer.h"

#include <algorithm>
#include <cstring>

namespace nax {

int Mixer::active_voices() const {
    int count = 0;
    for (const MixVoice& v : voices_) count += v.active ? 1 : 0;
    return count;
}

int Mixer::free_voice() const {
    for (int i = 0; i < int(voices_.size()); i++)
        if (!voices_[i].active) return i;
    return -1;
}

void Mixer::start(int index, uint32_t address, uint32_t bytes, MixFormat format, int source_rate, uint32_t step) {
    MixVoice& v = voices_[index];
    const MixVoice defaults;
    v = defaults;
    v.active = true;
    v.address = address;
    v.bytes = bytes;
    v.format = format;
    // The step is given relative to the source rate; the mixer advances by output frames.
    v.step = uint32_t(uint64_t(step) * uint64_t(source_rate) / uint64_t(output_rate_));
    if (v.step == 0) v.step = 1;
}

void Mixer::continue_voice(int index, uint32_t address, uint32_t bytes) {
    MixVoice& v = voices_[index];
    v.address = address;
    v.bytes = bytes;
    v.ended = false;
    v.active = true;
}

void Mixer::stop(int index) {
    voices_[index].active = false;
    voices_[index].ended = false;
}

void Mixer::read_frame(const MixVoice& v, const uint8_t* memory, uint32_t frame, int* left, int* right) const {
    const uint8_t* p = memory + v.address;
    if (v.format.sixteen_bit) {
        int16_t s[2];
        if (v.format.stereo) {
            std::memcpy(s, p + frame * 4, 4);
            *left = s[0];
            *right = s[1];
        } else {
            std::memcpy(s, p + frame * 2, 2);
            *left = *right = s[0];
        }
    } else if (v.format.stereo) {
        *left = (int(p[frame * 2]) - 128) << 8;
        *right = (int(p[frame * 2 + 1]) - 128) << 8;
    } else {
        *left = *right = (int(p[frame]) - 128) << 8;
    }
}

void Mixer::mix(int16_t* out, int frames, const uint8_t* memory, MixListener* listener) {
    accumulator_.assign(size_t(frames) * 2, 0);
    for (int index = 0; index < int(voices_.size()); index++) {
        MixVoice& v = voices_[index];
        int ends_in_a_row = 0;
        for (int i = 0; i < frames && v.active;) {
            const uint32_t count = v.frames();
            const uint32_t frame = uint32_t(v.position >> 16);
            if (frame >= count) {
                // A buffer that holds no whole frame, or a listener that keeps handing over empty buffers,
                // would otherwise stop the mixer here for good.
                if (++ends_in_a_row > 8) {
                    v.active = false;
                    if (listener) listener->voice_finished(index);
                    break;
                }
                const uint64_t length = uint64_t(count) << 16;
                v.position = v.position >= length ? v.position - length : 0;
                if (v.loops != 0 && count > 0) {
                    if (v.loops > 0) v.loops--;
                    continue;
                }
                v.ended = true;
                if (listener) listener->buffer_finished(index);
                if (!v.active) break;
                if (v.ended) {
                    v.active = false;
                    v.ended = false;
                    if (listener) listener->voice_finished(index);
                    break;
                }
                continue;
            }
            ends_in_a_row = 0;
            // The gains are worked out per frame because a listener may change the volume between buffers.
            const int64_t gain = int64_t(std::clamp(v.volume, 0, 0x7FFF)) * std::clamp(master_, 0, 0x7FFF);
            const int pan = std::clamp(v.pan, 0, 0xFFFF);
            int64_t left_gain = gain * std::min(0x8000, 0x10000 - pan) / 0x8000;
            int64_t right_gain = gain * std::min(0x8000, pan) / 0x8000;
            if (v.left_only) right_gain = 0;
            if (v.right_only) left_gain = 0;

            int l0, r0, l1, r1;
            read_frame(v, memory, frame, &l0, &r0);
            if (frame + 1 < count) {
                read_frame(v, memory, frame + 1, &l1, &r1);
            } else {
                l1 = l0;
                r1 = r0;
            }
            const int fraction = int(v.position & 0xFFFF);
            const int left = l0 + int((int64_t(l1 - l0) * fraction) >> 16);
            const int right = r0 + int((int64_t(r1 - r0) * fraction) >> 16);
            v.last_left = left;
            v.last_right = right;
            accumulator_[size_t(i) * 2] += int32_t((left * left_gain) >> 30);
            accumulator_[size_t(i) * 2 + 1] += int32_t((right * right_gain) >> 30);
            v.position += v.step;
            i++;
        }
    }
    for (size_t i = 0; i < accumulator_.size(); i++) out[i] = int16_t(std::clamp(accumulator_[i], -32768, 32767));
}

}  // namespace nax
