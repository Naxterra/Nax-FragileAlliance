// A software mixer for the sample voices of a translated program. It reads the sample data straight from
// guest memory, so the program's own buffers are played without copying, and it reports the end of every
// buffer while it mixes, so a streaming program can hand over its next buffer at the same point where its
// original sound driver asked for it.
#pragma once

#include <cstdint>
#include <vector>

namespace nax {

struct MixFormat {
    bool sixteen_bit = true;  // signed 16-bit samples; otherwise unsigned 8-bit samples
    bool stereo = false;      // interleaved left and right samples; otherwise one channel
};

struct MixVoice {
    bool active = false;
    bool ended = false;        // the current buffer ran out and no new buffer has been given yet
    uint32_t address = 0;      // guest address of the current buffer
    uint32_t bytes = 0;        // length of the current buffer
    MixFormat format;
    uint64_t position = 0;     // frames into the buffer, as a 32.16 fixed-point number
    uint32_t step = 0x10000;   // source frames per output frame, as a 16.16 fixed-point number
    int volume = 0x7FFF;       // 0 to 7FFFh
    int pan = 0x8000;          // 0 is left, 8000h the middle and FFFFh right
    bool left_only = false, right_only = false;  // a mono sample sent to one speaker
    int loops = 0;             // further passes through the buffer; -1 repeats until the voice is stopped
    uint32_t tag = 0;          // free for the caller
    int last_left = 0, last_right = 0;  // the last frame read, so a following buffer joins without a click

    uint32_t frames() const {
        const uint32_t size = (format.sixteen_bit ? 2u : 1u) * (format.stereo ? 2u : 1u);
        return bytes / size;
    }
};

// Called while mixing. buffer_finished is called when a voice reaches the end of its buffer; the listener may
// give the voice a new buffer with Mixer::continue_voice, stop it, or leave it. A voice that has no new
// buffer afterwards ends, and voice_finished is called for it.
class MixListener {
public:
    virtual ~MixListener() = default;
    virtual void buffer_finished(int voice) = 0;
    virtual void voice_finished(int voice) = 0;
};

class Mixer {
public:
    Mixer(int voices, int output_rate) : voices_(voices), output_rate_(output_rate) {}

    int output_rate() const { return output_rate_; }
    int voice_count() const { return int(voices_.size()); }
    MixVoice& voice(int index) { return voices_[index]; }
    const MixVoice& voice(int index) const { return voices_[index]; }
    int active_voices() const;
    // Returns a free voice, or -1.
    int free_voice() const;
    // Starts a voice on a buffer. step is the playback speed relative to source_rate as a 16.16 number.
    void start(int index, uint32_t address, uint32_t bytes, MixFormat format, int source_rate, uint32_t step);
    // Gives a playing voice its next buffer; the position carries over, so the two buffers join seamlessly.
    void continue_voice(int index, uint32_t address, uint32_t bytes);
    void stop(int index);
    void set_master_volume(int volume) { master_ = volume; }
    int master_volume() const { return master_; }

    // Mixes frames stereo frames of signed 16-bit samples into out. memory is the base of guest memory.
    void mix(int16_t* out, int frames, const uint8_t* memory, MixListener* listener);

private:
    void read_frame(const MixVoice& v, const uint8_t* memory, uint32_t frame, int* left, int* right) const;
    std::vector<MixVoice> voices_;
    std::vector<int32_t> accumulator_;
    int output_rate_;
    int master_ = 0x7FFF;
};

}  // namespace nax
