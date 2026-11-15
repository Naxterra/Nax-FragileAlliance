// Sound output through XAudio2: a stream of signed 16-bit stereo frames that the game thread keeps topped up.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace nax {

class AudioOutput {
public:
    ~AudioOutput() { close(); }
    // Opens the default sound device. Returns false, with a reason in error, when there is none.
    bool open(int rate, std::string* error);
    void close();
    bool is_open() const { return source_ != nullptr; }
    int rate() const { return rate_; }
    // Frames handed over that the device has not played yet.
    int64_t queued_frames() const;
    // Whether another buffer can be handed over now.
    bool can_submit() const;
    // Hands over frames stereo frames; they are copied.
    void submit(const int16_t* samples, int frames);

    static constexpr int kSlots = 64;
    static constexpr int kSlotFrames = 4096;

private:
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    std::vector<int16_t> slots_;
    int next_slot_ = 0;
    int64_t submitted_ = 0;
    int rate_ = 0;
    unsigned long com_thread_ = 0;  // the thread that initialised COM for the device
};

}  // namespace nax
