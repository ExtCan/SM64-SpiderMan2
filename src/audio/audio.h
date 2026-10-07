// Streams libsm64's 32 kHz stereo output (Mario's voice and SFX from the
// user's ROM) through XAudio2.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>

#include "../sm64/sm64_api.h"

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace sm2m {

class AudioOut {
public:
    ~AudioOut();
    bool Init(const Sm64Api* api, std::string& error);
    // Call right after each SM64 tick (30 Hz), on the same thread.
    void Tick();
    void Start();
    void Stop();
    // Holds what is queued (the game is paused) and plays on from there.
    void SetPaused(bool paused);
    void SetVolume(float v);
    bool Ready() const { return voice_ != nullptr && !disabled_; }
    // Never touch XAudio2 again (after a failure; objects are deliberately leaked).
    void Disable() { disabled_ = true; }

private:
    static constexpr int kBuffers = 16;
    static constexpr int kFramesPerTick = 544 * 2; // libsm64 produces up to 2 x 544 stereo frames per tick
    const Sm64Api* api_ = nullptr;
    IXAudio2* xa_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    int16_t buffers_[kBuffers][kFramesPerTick * 2] = {};
    int next_ = 0;
    uint64_t submittedFrames_ = 0;
    uint64_t playedBase_ = 0;
    bool running_ = false;
    bool paused_ = false;
    bool disabled_ = false;
};

} // namespace sm2m

#endif
