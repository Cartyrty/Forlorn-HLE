#pragma once
// Audio output: voices playing interleaved PCM (8 or 16 bits, mono or stereo, any rate), with volume and pitch. Windows
// plays them through XAudio2 voices, Linux mixes them in software into one SDL audio stream.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace platform {
class Voice {
public:
    virtual ~Voice() = default;
    // Replaces whatever was queued with `bytes` of PCM (which must stay valid while queued), played once plus
    // `loops` more times (-1: forever). Does not start playback.
    virtual void submit(const void *data, std::size_t bytes, int loops) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;  // pauses; start() resumes
    virtual void flush() = 0; // drops what is queued
    virtual void setVolume(float volume) = 0;
    virtual void setPitch(float ratio) = 0;
    virtual bool busy() = 0;                   // something is queued (playing or paused)
    virtual std::uint64_t framesPlayed() = 0;  // since the last submit (loops included)
};
// A voice, or nullptr when there is no audio output. onEnd (any thread) runs when the queued PCM has finished.
std::unique_ptr<Voice> createVoice(int channels, int rate, int bits, std::function<void()> onEnd = {});
bool audioAvailable();
} // namespace platform
