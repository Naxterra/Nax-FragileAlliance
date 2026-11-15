#include "runtime/audio.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nax {

namespace {
std::string failure(const char* what, HRESULT hr) {
    char text[128];
    std::snprintf(text, sizeof text, "%s (error %08lX)", what, static_cast<unsigned long>(hr));
    return text;
}
}  // namespace

bool AudioOutput::open(int rate, std::string* error) {
    close();
    // The mastering voice finds the sound device through COM, which needs COM on the calling thread.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) com_thread_ = GetCurrentThreadId();
    HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        if (error) *error = failure("XAudio2 could not be started", hr);
        close();
        return false;
    }
    hr = engine_->CreateMasteringVoice(&master_);
    if (FAILED(hr)) {
        if (error) *error = failure("no sound device was found", hr);
        close();
        return false;
    }
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = DWORD(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = DWORD(rate) * 4;
    hr = engine_->CreateSourceVoice(&source_, &format);
    if (FAILED(hr)) {
        if (error) *error = failure("the sound device does not take 16-bit stereo", hr);
        close();
        return false;
    }
    slots_.assign(size_t(kSlots) * kSlotFrames * 2, 0);
    next_slot_ = 0;
    submitted_ = 0;
    rate_ = rate;
    source_->Start(0);
    return true;
}

void AudioOutput::close() {
    if (source_) {
        source_->Stop(0);
        source_->DestroyVoice();
        source_ = nullptr;
    }
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (engine_) {
        engine_->Release();
        engine_ = nullptr;
    }
    // COM is released only on the thread that started it; at the end of the program the process ends anyway.
    if (com_thread_ && com_thread_ == GetCurrentThreadId()) {
        CoUninitialize();
        com_thread_ = 0;
    }
}

int64_t AudioOutput::queued_frames() const {
    if (!source_) return 0;
    XAUDIO2_VOICE_STATE state;
    source_->GetState(&state, 0);
    return submitted_ - int64_t(state.SamplesPlayed);
}

bool AudioOutput::can_submit() const {
    if (!source_) return false;
    XAUDIO2_VOICE_STATE state;
    source_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued < UINT32(kSlots - 1);
}

void AudioOutput::submit(const int16_t* samples, int frames) {
    if (!source_ || frames <= 0) return;
    frames = std::min(frames, kSlotFrames);
    int16_t* slot = slots_.data() + size_t(next_slot_) * kSlotFrames * 2;
    std::memcpy(slot, samples, size_t(frames) * 4);
    next_slot_ = (next_slot_ + 1) % kSlots;
    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = UINT32(frames) * 4;
    buffer.pAudioData = reinterpret_cast<const BYTE*>(slot);
    if (SUCCEEDED(source_->SubmitSourceBuffer(&buffer))) submitted_ += frames;
}

}  // namespace nax
