// Voices are mixed in software into one SDL audio stream (stereo, 16-bit, 44.1 kHz): each voice steps through its PCM
// at rate * pitch / 44100 frames per output frame (linear interpolation), looping as submitted.
#include "platform/AudioOutput.h"
#include "Log.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace platform {
namespace {
constexpr int kRate = 44100;
class MixerVoice;
SDL_AudioDeviceID g_device = 0;
std::vector<MixerVoice *> g_voices; // guarded by the SDL audio device lock
void mix(void *, Uint8 *stream, int length);

bool engine() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            hlog("audio", "SDL audio unavailable (%s); audio stays silent", SDL_GetError());
            return;
        }
        SDL_AudioSpec want{}, have{};
        want.freq = kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = mix;
        g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!g_device) {
            hlog("audio", "no audio device (%s); audio stays silent", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(g_device, 0);
        hlog("audio", "SDL audio output ready (%s)", SDL_GetCurrentAudioDriver());
    });
    return g_device != 0;
}
struct Lock {
    Lock() { SDL_LockAudioDevice(g_device); }
    ~Lock() { SDL_UnlockAudioDevice(g_device); }
};

class MixerVoice final : public Voice {
public:
    MixerVoice(int channels, int rate, int bits, std::function<void()> onEnd)
        : channels_(channels), rate_(rate), bits_(bits), onEnd_(std::move(onEnd)) {
        Lock lock;
        g_voices.push_back(this);
    }
    ~MixerVoice() override {
        Lock lock;
        g_voices.erase(std::find(g_voices.begin(), g_voices.end(), this));
    }
    void submit(const void *data, std::size_t bytes, int loops) override {
        Lock lock;
        data_ = static_cast<const std::uint8_t *>(data);
        frames_ = bytes / std::size_t(channels_ * bits_ / 8);
        loops_ = loops;
        position_ = 0;
        played_ = 0;
        queued_ = frames_ != 0;
        playing_ = false;
    }
    void start() override { Lock lock; playing_ = queued_; }
    void stop() override { Lock lock; playing_ = false; }
    void flush() override { Lock lock; playing_ = queued_ = false; }
    void setVolume(float volume) override { Lock lock; volume_ = volume; }
    void setPitch(float ratio) override { Lock lock; pitch_ = std::clamp(ratio, 0.05f, 4.0f); }
    bool busy() override { Lock lock; return queued_; }
    std::uint64_t framesPlayed() override { Lock lock; return played_; }

    // Adds this voice to `out` (stereo float); returns whether it reached its end.
    bool render(float *out, int frames) {
        if (!playing_) return false;
        const double step = double(rate_) * pitch_ / kRate;
        for (int i = 0; i < frames; ++i) {
            auto index = std::size_t(position_);
            if (index >= frames_) {
                if (loops_ == 0) {
                    playing_ = queued_ = false;
                    return true;
                }
                if (loops_ > 0) --loops_;
                position_ -= double(frames_);
                index = std::size_t(position_);
            }
            const double t = position_ - double(index);
            const auto next = index + 1 < frames_ ? index + 1 : (loops_ != 0 ? 0 : index);
            for (int c = 0; c < 2; ++c) {
                const int source = channels_ == 1 ? 0 : c;
                const float a = sample(index, source), b = sample(next, source);
                out[2 * i + c] += float(a + (b - a) * t) * volume_;
            }
            position_ += step;
            const auto whole = std::uint64_t(position_);
            if (whole > index) played_ += whole - index;
        }
        return false;
    }
    std::function<void()> onEnd_;

private:
    float sample(std::size_t frame, int channel) const {
        const std::size_t at = frame * std::size_t(channels_) + std::size_t(channel);
        if (bits_ == 8) return (float(data_[at]) - 128.f) / 128.f;
        std::int16_t v;
        std::memcpy(&v, data_ + 2 * at, 2);
        return float(v) / 32768.f;
    }
    int channels_, rate_, bits_;
    const std::uint8_t *data_ = nullptr;
    std::size_t frames_ = 0;
    int loops_ = 0;
    double position_ = 0;
    std::uint64_t played_ = 0;
    float volume_ = 1, pitch_ = 1;
    bool queued_ = false, playing_ = false;
};

void mix(void *, Uint8 *stream, int length) {
    const int frames = length / 4;
    static std::vector<float> accumulator;
    accumulator.assign(std::size_t(frames) * 2, 0.f);
    std::vector<std::function<void()>> ended;
    for (auto *voice : g_voices)
        if (voice->render(accumulator.data(), frames) && voice->onEnd_) ended.push_back(voice->onEnd_);
    auto *out = reinterpret_cast<std::int16_t *>(stream);
    for (std::size_t i = 0; i < accumulator.size(); ++i)
        out[i] = std::int16_t(std::lround(std::clamp(accumulator[i], -1.f, 1.f) * 32767.f));
    for (auto &fn : ended) fn();
}
} // namespace

bool audioAvailable() { return engine(); }
std::unique_ptr<Voice> createVoice(int channels, int rate, int bits, std::function<void()> onEnd) {
    if (!engine()) return nullptr;
    return std::make_unique<MixerVoice>(channels, rate, bits, std::move(onEnd));
}
} // namespace platform
