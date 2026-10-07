#ifdef _WIN32

#include "audio.h"

#include "../common/platform.h"

#include <xaudio2.h>

#include "../common/log.h"

namespace sm2m {

AudioOut::~AudioOut() {
    if (disabled_) return;
    if (voice_) {
        voice_->Stop(0);
        voice_->DestroyVoice();
        voice_ = nullptr;
    }
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (xa_) {
        xa_->Release();
        xa_ = nullptr;
    }
}

bool AudioOut::Init(const Sm64Api* api, std::string& error) {
    api_ = api;
    HMODULE dll = LoadLibraryW(L"xaudio2_9.dll");
    if (!dll) dll = LoadLibraryW(L"xaudio2_8.dll");
    if (!dll) {
        error = "XAudio2 not available";
        return false;
    }
    using CreateVerFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR, DWORD);
    using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    auto createVer = reinterpret_cast<CreateVerFn>(reinterpret_cast<void*>(GetProcAddress(dll, "XAudio2CreateWithVersionInfo")));
    auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(dll, "XAudio2Create")));
    HRESULT hr = E_FAIL;
    if (createVer) hr = createVer(&xa_, 0, XAUDIO2_DEFAULT_PROCESSOR, 0x0A000000 /* NTDDI_WIN10 */);
    if (FAILED(hr) && create) hr = create(&xa_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !xa_) {
        error = "XAudio2Create failed";
        return false;
    }
    if (FAILED(xa_->CreateMasteringVoice(&master_))) {
        error = "no audio output device";
        return false;
    }
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 32000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = 32000 * 4;
    if (FAILED(xa_->CreateSourceVoice(&voice_, &wfx))) {
        error = "CreateSourceVoice failed";
        voice_ = nullptr;
        return false;
    }
    LOGI("audio: XAudio2 ready (32 kHz stereo)");
    return true;
}

void AudioOut::Start() {
    if (!voice_ || running_ || disabled_) return;
    voice_->FlushSourceBuffers();
    XAUDIO2_VOICE_STATE st{};
    voice_->GetState(&st);
    playedBase_ = st.SamplesPlayed;
    submittedFrames_ = 0;
    voice_->Start(0);
    running_ = true;
    paused_ = false;
}

void AudioOut::Stop() {
    if (!voice_ || !running_ || disabled_) return;
    voice_->Stop(0);
    voice_->FlushSourceBuffers();
    running_ = false;
    paused_ = false;
}

void AudioOut::SetPaused(bool paused) {
    if (!voice_ || !running_ || disabled_ || paused == paused_) return;
    paused_ = paused;
    if (paused) voice_->Stop(0); // keeps the queued buffers
    else voice_->Start(0);
}

void AudioOut::SetVolume(float v) {
    if (voice_ && !disabled_) voice_->SetVolume(v);
}

void AudioOut::Tick() {
    if (!voice_ || !running_ || disabled_ || !api_ || !api_->audio_tick) return;
    XAUDIO2_VOICE_STATE st{};
    voice_->GetState(&st);
    const uint64_t played = st.SamplesPlayed - playedBase_;
    const uint32_t queued = submittedFrames_ > played ? uint32_t(submittedFrames_ - played) : 0;
    int16_t* buf = buffers_[next_];
    // Same pacing as libsm64's test harness: ask for ~1100 frames, keep < 6000 queued.
    const uint32_t samples = api_->audio_tick(queued, 1100, buf);
    const uint32_t frames = samples * 2;
    if (frames == 0 || queued >= 6000 || st.BuffersQueued >= kBuffers - 1) return;
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = frames * 4;
    xb.pAudioData = reinterpret_cast<const BYTE*>(buf);
    if (SUCCEEDED(voice_->SubmitSourceBuffer(&xb))) {
        submittedFrames_ += frames;
        next_ = (next_ + 1) % kBuffers;
    }
}

} // namespace sm2m

#endif
