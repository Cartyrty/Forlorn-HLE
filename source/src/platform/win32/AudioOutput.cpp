// Voices are XAudio2 source voices on one mastering voice.
#include "platform/AudioOutput.h"
#include "Log.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <mutex>

namespace platform {
namespace {
IXAudio2 *g_xaudio = nullptr;
IXAudio2MasteringVoice *g_master = nullptr;
bool engine() {
    static std::once_flag once;
    std::call_once(once, [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(g_xaudio->CreateMasteringVoice(&g_master))) {
            hlog("audio", "XAudio2 unavailable; audio stays silent");
            g_xaudio = nullptr;
            return;
        }
        hlog("audio", "XAudio2 output ready");
    });
    return g_xaudio != nullptr;
}

class XAudio2Voice final : public Voice, IXAudio2VoiceCallback {
public:
    XAudio2Voice(std::function<void()> onEnd) : onEnd_(std::move(onEnd)) {}
    bool create(int channels, int rate, int bits) {
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = WORD(channels);
        f.nSamplesPerSec = DWORD(rate);
        f.wBitsPerSample = WORD(bits);
        f.nBlockAlign = WORD(channels * bits / 8);
        f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
        return SUCCEEDED(g_xaudio->CreateSourceVoice(&voice_, &f, 0, 4.0f, this));
    }
    ~XAudio2Voice() override { if (voice_) voice_->DestroyVoice(); }
    void submit(const void *data, std::size_t bytes, int loops) override {
        voice_->Stop();
        voice_->FlushSourceBuffers();
        XAUDIO2_BUFFER x{};
        x.AudioBytes = UINT32(bytes);
        x.pAudioData = static_cast<const BYTE *>(data);
        x.Flags = XAUDIO2_END_OF_STREAM;
        x.LoopCount = loops < 0 ? XAUDIO2_LOOP_INFINITE : UINT32(std::min(loops, 254));
        voice_->SubmitSourceBuffer(&x);
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state);
        base_ = state.SamplesPlayed;
    }
    void start() override { voice_->Start(); }
    void stop() override { voice_->Stop(); }
    void flush() override { voice_->Stop(); voice_->FlushSourceBuffers(); }
    void setVolume(float volume) override { voice_->SetVolume(volume); }
    void setPitch(float ratio) override { voice_->SetFrequencyRatio(std::clamp(ratio, 0.05f, 4.0f)); }
    bool busy() override {
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued != 0;
    }
    std::uint64_t framesPlayed() override {
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state);
        return state.SamplesPlayed >= base_ ? state.SamplesPlayed - base_ : state.SamplesPlayed;
    }

private:
    void STDMETHODCALLTYPE OnStreamEnd() override { if (onEnd_) onEnd_(); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void *) override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}

    IXAudio2SourceVoice *voice_ = nullptr;
    std::function<void()> onEnd_;
    std::uint64_t base_ = 0;
};
} // namespace

bool audioAvailable() { return engine(); }
std::unique_ptr<Voice> createVoice(int channels, int rate, int bits, std::function<void()> onEnd) {
    if (!engine()) return nullptr;
    auto voice = std::make_unique<XAudio2Voice>(std::move(onEnd));
    if (!voice->create(channels, rate, bits)) return nullptr;
    return voice;
}
} // namespace platform
