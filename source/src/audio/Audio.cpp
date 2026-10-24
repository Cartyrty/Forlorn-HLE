// Audio output through the host's voices (platform/AudioOutput.h), with every file decoded to 16-bit PCM by
// audio/Decode.cpp:
//  * OpenAL (CocosDenshion's sound effects): buffers hold copies of the app's PCM; each source is a voice.
//  * ExtAudioFile / AudioFile: the app reads its .caf effects as linear PCM (the decoded data).
//  * AVAudioPlayer (background music, MP3): one voice per player, looping as numberOfLoops says; the delegate hears
//    audioPlayerDidFinishPlaying:successfully: on the main run loop.
//  * AVAudioSession / AudioSession: accepted.
#include "audio/Decode.h"
#include "foundation/Foundation.h"
#include "foundation/RunLoop.h"
#include "libc/Files.h"
#include "Log.h"
#include "Memory.h"
#include "platform/AudioOutput.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using hle::arg;
using hle::argFloat;
using hle::ret;
using objc::marg;
using objc::self;

namespace {
std::recursive_mutex g_lock;
bool engine() { return platform::audioAvailable(); }

// ======================================== OpenAL ========================================
struct ALBuffer { std::vector<std::uint8_t> data; int channels = 1, bits = 16, rate = 44100; };
struct ALSource {
    std::unique_ptr<platform::Voice> voice;
    int channels = 0, rate = 0, bits = 0; // the voice's format
    std::uint32_t buffer = 0;
    bool looping = false, paused = false;
    float gain = 1, pitch = 1;
    std::shared_ptr<ALBuffer> playing; // keeps the PCM alive while the voice plays it
};
std::map<std::uint32_t, std::shared_ptr<ALBuffer>> g_buffers;
std::map<std::uint32_t, ALSource> g_sources;
std::uint32_t g_nextBuffer = 1, g_nextSource = 1;
float g_listenerGain = 1;
constexpr int AL_BUFFER = 0x1009, AL_LOOPING = 0x1007, AL_GAIN = 0x100A, AL_PITCH = 0x1003, AL_SOURCE_STATE = 0x1010;
constexpr int AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013, AL_STOPPED = 0x1014;
void stopSource(ALSource &s) {
    s.paused = false;
    if (s.voice) s.voice->flush();
}
void applyVolume(ALSource &s) { if (s.voice) s.voice->setVolume(s.gain * g_listenerGain); }
void playSource(ALSource &s) {
    if (s.paused && s.voice) { s.paused = false; s.voice->start(); return; }
    auto found = g_buffers.find(s.buffer);
    if (found == g_buffers.end() || found->second->data.empty() || !engine()) return;
    const auto &b = *found->second;
    if (s.voice && (s.channels != b.channels || s.rate != b.rate || s.bits != b.bits)) s.voice.reset();
    if (!s.voice) {
        if (!(s.voice = platform::createVoice(b.channels, b.rate, b.bits))) return;
        s.channels = b.channels;
        s.rate = b.rate;
        s.bits = b.bits;
    }
    s.paused = false;
    s.playing = found->second;
    s.voice->submit(b.data.data(), b.data.size(), s.looping ? -1 : 0);
    applyVolume(s);
    s.voice->setPitch(s.pitch);
    s.voice->start();
}
int sourceState(ALSource &s) {
    if (!s.voice) return AL_INITIAL;
    if (s.paused) return AL_PAUSED;
    return s.voice->busy() ? AL_PLAYING : AL_STOPPED;
}
} // namespace

HLE(alcOpenDevice) { engine(); ret(mem::hleAlloc(16)); }
HLE(alcCloseDevice) { ret(1); }
HLE(alcCreateContext) { ret(mem::hleAlloc(16)); }
HLE(alcDestroyContext) {}
HLE(alcMakeContextCurrent) { ret(1); }
HLE(alcGetCurrentContext) { static std::uint64_t context = mem::hleAlloc(16); ret(context); }
HLE(alcGetContextsDevice) { static std::uint64_t device = mem::hleAlloc(16); ret(device); }
HLE(alcProcessContext) {}
HLE(alcSuspendContext) {}
HLE(alcGetError) { ret(0); }
HLE(alGetError) { ret(0); }
// Extensions (alBufferDataStatic, alcMacOSXMixerOutputRate...) are not offered: callers fall back to alBufferData.
HLE(alcGetProcAddress) { hlog("audio", "alcGetProcAddress(%s): not available", mem::cstring(arg(1)).c_str()); ret(0); }
HLE(alGenBuffers) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) { mem::w32(arg(1) + 4 * i, g_nextBuffer); g_buffers[g_nextBuffer++] = std::make_shared<ALBuffer>(); }
}
HLE(alDeleteBuffers) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) g_buffers.erase(mem::r32(arg(1) + 4 * i));
}
// alBufferData(buffer, format, data, size, frequency): AL_FORMAT_MONO8 0x1100, MONO16 0x1101, STEREO8 0x1102,
// STEREO16 0x1103. The data is copied.
HLE(alBufferData) {
    std::lock_guard lock(g_lock);
    auto b = std::make_shared<ALBuffer>();
    const auto format = int(arg(1));
    const auto size = std::uint32_t(arg(3));
    b->data.assign(mem::base() + arg(2), mem::base() + arg(2) + size);
    b->rate = int(arg(4));
    b->channels = format == 0x1102 || format == 0x1103 ? 2 : 1;
    b->bits = format == 0x1100 || format == 0x1102 ? 8 : 16;
    g_buffers[std::uint32_t(arg(0))] = b;
}
HLE(alGenSources) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) { mem::w32(arg(1) + 4 * i, g_nextSource); g_sources[g_nextSource++] = {}; }
}
HLE(alDeleteSources) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) {
        auto found = g_sources.find(mem::r32(arg(1) + 4 * i));
        if (found == g_sources.end()) continue;
        g_sources.erase(found);
    }
}
HLE(alSourcei) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    if (int(arg(1)) == AL_BUFFER) { stopSource(s); s.buffer = std::uint32_t(arg(2)); }
    else if (int(arg(1)) == AL_LOOPING) s.looping = arg(2) != 0;
}
// alSourcef(source, param, value): the value is a float word.
HLE(alSourcef) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    const float value = argFloat(2);
    if (int(arg(1)) == AL_GAIN) { s.gain = value; applyVolume(s); }
    else if (int(arg(1)) == AL_PITCH) { s.pitch = value; if (s.voice) s.voice->setPitch(value); }
}
HLE(alSourcefv) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    const float value = mem::rf(arg(2));
    if (int(arg(1)) == AL_GAIN) { s.gain = value; applyVolume(s); }
    else if (int(arg(1)) == AL_PITCH) s.pitch = value;
}
HLE(alGetSourcef) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    const int param = int(arg(1));
    mem::wf(arg(2), param == AL_GAIN ? s.gain : param == AL_PITCH ? s.pitch : 0.f);
}
HLE(alGetSourcefv) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    const int param = int(arg(1));
    if (param == AL_GAIN || param == AL_PITCH) mem::wf(arg(2), param == AL_GAIN ? s.gain : s.pitch);
    else for (int i = 0; i < 3; ++i) mem::wf(arg(2) + 4 * i, 0);
}
HLE(alSourcePlay) { std::lock_guard lock(g_lock); playSource(g_sources[std::uint32_t(arg(0))]); }
HLE(alSourceStop) { std::lock_guard lock(g_lock); stopSource(g_sources[std::uint32_t(arg(0))]); }
HLE(alSourceRewind) { std::lock_guard lock(g_lock); stopSource(g_sources[std::uint32_t(arg(0))]); }
HLE(alSourcePause) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    if (s.voice && sourceState(s) == AL_PLAYING) { s.voice->stop(); s.paused = true; }
}
HLE(alGetSourcei) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    int value = 0;
    if (int(arg(1)) == AL_SOURCE_STATE) value = sourceState(s);
    else if (int(arg(1)) == AL_BUFFER) value = int(s.buffer);
    else if (int(arg(1)) == AL_LOOPING) value = s.looping;
    mem::w32(arg(2), std::uint32_t(value));
}
HLE(alListenerf) {
    if (int(arg(0)) != AL_GAIN) return;
    std::lock_guard lock(g_lock);
    g_listenerGain = argFloat(1);
    for (auto &[id, s] : g_sources) applyVolume(s);
}
HLE(alGetListenerf) { mem::wf(arg(1), int(arg(0)) == AL_GAIN ? g_listenerGain : 0.f); }
HLE(alListener3f) {}
HLE(alListenerfv) {}
HLE(alSource3f) {}
HLE(alDistanceModel) {}

// ======================================== ExtAudioFile / AudioFile ========================================
// AudioStreamBasicDescription (40 bytes): mSampleRate (double) +0, mFormatID +8, mFormatFlags +12,
// mBytesPerPacket +16, mFramesPerPacket +20, mBytesPerFrame +24, mChannelsPerFrame +28, mBitsPerChannel +32.
namespace {
struct OpenFile {
    std::shared_ptr<const audio::Pcm> pcm;
    std::uint64_t position = 0; // frames (ExtAudioFile)
    int clientBits = 16, clientChannels = 0;
};
std::map<std::uint64_t, OpenFile> g_files;
std::uint64_t g_nextFile = 0x7000;
void writeAsbd(std::uint64_t p, int rate, int channels, int bits) {
    mem::write<double>(p, double(rate));
    mem::w32(p + 8, 0x6C70636D); // 'lpcm'
    mem::w32(p + 12, 0xC);       // signed integer, packed
    mem::w32(p + 16, std::uint32_t(channels * bits / 8));
    mem::w32(p + 20, 1);
    mem::w32(p + 24, std::uint32_t(channels * bits / 8));
    mem::w32(p + 28, std::uint32_t(channels));
    mem::w32(p + 32, std::uint32_t(bits));
    mem::w32(p + 36, 0);
}
std::uint64_t openUrl(std::uint64_t url) {
    const auto path = ns::text(objc::send(url, "path"));
    auto pcm = audio::decodeFile(files::hostPath(path));
    if (!pcm) {
        hlog("audio", "%s: missing or unsupported", path.c_str());
        return 0;
    }
    std::lock_guard lock(g_lock);
    const auto handle = g_nextFile++;
    g_files[handle] = {pcm, 0, 16, pcm->channels};
    return handle;
}
OpenFile *file(std::uint64_t handle) {
    auto found = g_files.find(handle);
    return found == g_files.end() ? nullptr : &found->second;
}
} // namespace
// ExtAudioFileOpenURL(CFURLRef, ExtAudioFileRef *)
HLE(ExtAudioFileOpenURL) {
    const auto handle = openUrl(arg(0));
    if (!handle) return ret(std::uint32_t(-43));
    mem::wp(arg(1), handle);
    ret(0);
}
// ExtAudioFileGetProperty(file, id, ioSize *, data): 'ffmt' file format, 'cfmt' client format, '#frm' frame count
// (SInt64).
HLE(ExtAudioFileGetProperty) {
    std::lock_guard lock(g_lock);
    auto *f = file(arg(0));
    if (!f) return ret(std::uint32_t(-50));
    switch (std::uint32_t(arg(1))) {
    case 0x66666D74: writeAsbd(arg(3), f->pcm->rate, f->pcm->channels, 16); break;      // 'ffmt'
    case 0x63666D74: writeAsbd(arg(3), f->pcm->rate, f->clientChannels, f->clientBits); break; // 'cfmt'
    case 0x2366726D: mem::write<std::uint64_t>(arg(3), f->pcm->frames()); break;          // '#frm'
    default: return ret(std::uint32_t(-50));
    }
    ret(0);
}
// The client format the app asks for: 8 or 16 bits, mono or stereo (no resampling: the files are all 44.1 kHz).
HLE(ExtAudioFileSetProperty) {
    std::lock_guard lock(g_lock);
    auto *f = file(arg(0));
    if (!f) return ret(std::uint32_t(-50));
    if (std::uint32_t(arg(1)) == 0x63666D74) {
        f->clientBits = int(mem::r32(arg(3) + 32)) == 8 ? 8 : 16;
        f->clientChannels = std::clamp(int(mem::r32(arg(3) + 28)), 1, 2);
    }
    ret(0);
}
// ExtAudioFileRead(file, UInt32 *ioNumberFrames, AudioBufferList *): {mNumberBuffers, buffers[] {channels, byteSize,
// data}} (32-bit: the first buffer at +4).
HLE(ExtAudioFileRead) {
    std::lock_guard lock(g_lock);
    auto *f = file(arg(0));
    if (!f) return ret(std::uint32_t(-50));
    const auto list = arg(2);
    const int channels = f->clientChannels ? f->clientChannels : f->pcm->channels;
    const int bytesPerFrame = channels * f->clientBits / 8;
    const auto capacity = mem::r32(list + 8) / std::uint32_t(std::max(1, bytesPerFrame));
    const auto wanted = std::min<std::uint64_t>(std::min<std::uint64_t>(mem::r32(arg(1)), capacity), f->pcm->frames() - f->position);
    const auto out = mem::rp(list + 12);
    const int source = f->pcm->channels;
    for (std::uint64_t frame = 0; frame < wanted; ++frame)
        for (int c = 0; c < channels; ++c) {
            const auto base = std::size_t((f->position + frame) * std::uint64_t(source));
            std::int32_t sample;
            if (channels == source) sample = f->pcm->samples[base + std::size_t(c)];
            else if (channels == 1) sample = (std::int32_t(f->pcm->samples[base]) + f->pcm->samples[base + 1]) / 2; // stereo -> mono
            else sample = f->pcm->samples[base];                                                                  // mono -> stereo
            const auto index = frame * std::uint64_t(channels) + std::uint64_t(c);
            if (f->clientBits == 8) mem::write<std::uint8_t>(out + index, std::uint8_t((sample >> 8) + 128));
            else mem::write<std::int16_t>(out + 2 * index, std::int16_t(sample));
        }
    f->position += wanted;
    mem::w32(arg(1), std::uint32_t(wanted));
    mem::w32(list + 8, std::uint32_t(wanted * std::uint64_t(bytesPerFrame)));
    ret(0);
}
HLE(ExtAudioFileDispose) {
    std::lock_guard lock(g_lock);
    g_files.erase(arg(0));
    ret(0);
}
// AudioFileOpenURL(url, permissions, fileTypeHint, AudioFileID *)
HLE(AudioFileOpenURL) {
    const auto handle = openUrl(arg(0));
    if (!handle) return ret(std::uint32_t(-43));
    mem::wp(arg(3), handle);
    ret(0);
}
HLE(AudioFileClose) { std::lock_guard lock(g_lock); g_files.erase(arg(0)); ret(0); }
// AudioFileGetProperty(file, id, ioSize *, data): 'dfmt' data format, 'bcnt' audio data byte count (UInt64),
// 'pcnt' packet count (UInt64), 'psze' maximum packet size.
HLE(AudioFileGetProperty) {
    std::lock_guard lock(g_lock);
    auto *f = file(arg(0));
    if (!f) return ret(std::uint32_t(-50));
    const auto &pcm = *f->pcm;
    switch (std::uint32_t(arg(1))) {
    case 0x64666D74: writeAsbd(arg(3), pcm.rate, pcm.channels, 16); break;                       // 'dfmt'
    case 0x62636E74: mem::write<std::uint64_t>(arg(3), pcm.samples.size() * 2); break;             // 'bcnt'
    case 0x70636E74: mem::write<std::uint64_t>(arg(3), pcm.frames()); break;                       // 'pcnt'
    case 0x7073697A: case 0x70737A65: mem::w32(arg(3), std::uint32_t(pcm.channels * 2)); break;    // 'psze'
    default: return ret(std::uint32_t(-50));
    }
    ret(0);
}
// AudioFileReadBytes(file, useCache, SInt64 startingByte (two words), UInt32 *ioNumBytes, buffer)
HLE(AudioFileReadBytes) {
    std::lock_guard lock(g_lock);
    auto *f = file(arg(0));
    if (!f) return ret(std::uint32_t(-50));
    const auto start = std::uint64_t(hle::arg64(2));
    const auto total = std::uint64_t(f->pcm->samples.size() * 2);
    const auto wanted = start >= total ? 0 : std::min<std::uint64_t>(mem::r32(arg(4)), total - start);
    std::memcpy(mem::base() + arg(5), reinterpret_cast<const std::uint8_t *>(f->pcm->samples.data()) + start, std::size_t(wanted));
    mem::w32(arg(4), std::uint32_t(wanted));
    ret(wanted ? 0 : std::uint32_t(-39)); // kAudioFileEndOfFileError
}
HLE(AudioServicesPlaySystemSound) {}
HLE(AudioSessionInitialize) { ret(0); }
HLE(AudioSessionSetActive) { ret(0); }
HLE(AudioSessionSetProperty) { ret(0); }
HLE(AudioSessionAddPropertyListener) { ret(0); }
// AudioSessionGetProperty(id, ioSize *, data): 'othr' (other audio playing) 0, 'chpr' sample rate 44100.
HLE(AudioSessionGetProperty) {
    const auto id = std::uint32_t(arg(0));
    if (id == 0x63687072 /*'chpr'*/) mem::write<double>(arg(2), 44100.0);
    else if (arg(2)) mem::w32(arg(2), 0);
    ret(0);
}

// ======================================== AVAudioSession ========================================
OBJC_CLASS(AVAudioSession, NSObject);
static ns::id g_session = 0;
OBJC_CLASS_METHOD(AVAudioSession, "sharedInstance", sharedInstance) {
    if (!g_session) { g_session = objc::allocInstance(self()); objc::retain(g_session); }
    ret(g_session);
}
OBJC_METHOD(AVAudioSession, "setCategory:error:", setCategory) { ret(1); }
OBJC_METHOD(AVAudioSession, "setActive:error:", setActive) { ret(1); }
OBJC_METHOD(AVAudioSession, "setDelegate:", setDelegate) {}
OBJC_METHOD(AVAudioSession, "category", category) { ret(ns::string("AVAudioSessionCategorySoloAmbient")); }
OBJC_METHOD(AVAudioSession, "otherAudioPlaying", otherAudioPlaying) { ret(0); }
static std::uint64_t sessionConstant(const char *name) {
    const auto p = mem::hleAlloc(4);
    const auto s = ns::string(name);
    objc::retain(s);
    mem::wp(p, s);
    return p;
}
static hle::DataRegistrar d_ambient("_AVAudioSessionCategoryAmbient", [] { return sessionConstant("AVAudioSessionCategoryAmbient"); });
static hle::DataRegistrar d_soloAmbient("_AVAudioSessionCategorySoloAmbient", [] { return sessionConstant("AVAudioSessionCategorySoloAmbient"); });
static hle::DataRegistrar d_playback("_AVAudioSessionCategoryPlayback", [] { return sessionConstant("AVAudioSessionCategoryPlayback"); });
static hle::DataRegistrar d_playAndRecord("_AVAudioSessionCategoryPlayAndRecord", [] { return sessionConstant("AVAudioSessionCategoryPlayAndRecord"); });

// ======================================== AVAudioPlayer ========================================
namespace {
struct Player : objc::HostObject {
    ns::id object = 0, delegate = 0, url = 0;
    std::shared_ptr<const audio::Pcm> pcm;
    std::unique_ptr<platform::Voice> voice;
    int loops = 0;
    float volume = 1;
    bool playing = false, paused = false;
    std::uint64_t startFrame = 0;             // where the submitted PCM starts (currentTime)
    std::atomic<std::uint64_t> generation{0}; // bumped when the PCM is replaced, so stale end callbacks are ignored
};
Player *player(ns::id o) { return objc::host<Player>(o); }
// The voice finished (on an audio thread): the delegate hears it on the main run loop.
void streamEnded(Player *p) {
    const auto object = p->object;
    const std::uint64_t generation = p->generation;
    runloop::post([object, generation] {
        ns::id delegate;
        {
            std::lock_guard lock(g_lock);
            auto *p = ::player(object);
            if (!p || p->generation != generation) return;
            p->playing = false;
            delegate = p->delegate;
        }
        if (delegate && objc::respondsTo(delegate, objc::sel("audioPlayerDidFinishPlaying:successfully:")))
            objc::send(delegate, "audioPlayerDidFinishPlaying:successfully:", {object, 1});
    });
}
// Submits the PCM from a frame on, looping (numberOfLoops < 0 forever, else that many extra times).
void submit(Player &p, std::uint64_t fromFrame) {
    if (!p.voice) return;
    ++p.generation;
    const auto frames = p.pcm->frames();
    fromFrame = std::min(fromFrame, frames);
    const auto bytes = std::size_t((frames - fromFrame) * std::uint64_t(p.pcm->channels) * 2);
    p.voice->submit(p.pcm->samples.data() + fromFrame * std::uint64_t(p.pcm->channels), bytes, fromFrame == 0 ? p.loops : 0);
    p.startFrame = fromFrame;
}
double playedSeconds(Player &p) {
    if (!p.voice || !p.pcm) return 0;
    const auto frames = p.pcm->frames();
    const auto at = frames ? (p.startFrame + p.voice->framesPlayed()) % frames : 0;
    return double(at) / std::max(1, p.pcm->rate);
}
} // namespace
OBJC_CLASS(AVAudioPlayer, NSObject);
// initWithContentsOfURL:error: decodes the whole file (MP3 / CAF / ...) up front.
OBJC_METHOD(AVAudioPlayer, "initWithContentsOfURL:error:", initWithContentsOfURL) {
    const auto path = ns::text(objc::send(marg(0), "path"));
    auto pcm = audio::decodeFile(files::hostPath(path));
    if (!pcm || !engine()) {
        if (marg(1)) mem::wp(marg(1), objc::send(objc::getClass("NSError"), "errorWithDomain:code:userInfo:", {ns::string("NSOSStatusErrorDomain"), 2003334207u, 0}));
        return ret(0);
    }
    auto p = std::make_unique<Player>();
    p->object = self();
    p->url = marg(0);
    objc::retain(marg(0));
    p->pcm = pcm;
    p->voice = platform::createVoice(pcm->channels, pcm->rate, 16, [raw = p.get()] { streamEnded(raw); });
    std::lock_guard lock(g_lock);
    objc::setHostObject(self(), std::move(p));
    ret(self());
}
OBJC_METHOD(AVAudioPlayer, "initWithData:error:", initWithData) {
    hlog("audio", "AVAudioPlayer initWithData: is not supported");
    ret(0);
}
OBJC_METHOD(AVAudioPlayer, "prepareToPlay", prepareToPlay) { ret(1); }
OBJC_METHOD(AVAudioPlayer, "play", play) {
    std::lock_guard lock(g_lock);
    auto *p = player(self());
    if (!p || !p->voice) return ret(0);
    if (!p->paused) submit(*p, p->startFrame);
    p->paused = false;
    p->voice->setVolume(p->volume);
    p->voice->start();
    p->playing = true;
    ret(1);
}
OBJC_METHOD(AVAudioPlayer, "pause", pause) {
    std::lock_guard lock(g_lock);
    auto *p = player(self());
    if (!p || !p->voice || !p->playing) return;
    p->voice->stop();
    p->paused = true;
    p->playing = false;
}
// -stop keeps the position (as on iOS); currentTime = 0 rewinds.
OBJC_METHOD(AVAudioPlayer, "stop", stop) {
    std::lock_guard lock(g_lock);
    auto *p = player(self());
    if (!p || !p->voice) return;
    const auto at = std::uint64_t(playedSeconds(*p) * p->pcm->rate);
    ++p->generation;
    p->voice->flush();
    p->startFrame = at;
    p->playing = false;
    p->paused = false;
}
OBJC_METHOD(AVAudioPlayer, "isPlaying", isPlaying) { std::lock_guard lock(g_lock); auto *p = player(self()); ret(p && p->playing); }
OBJC_METHOD(AVAudioPlayer, "setNumberOfLoops:", setNumberOfLoops) { std::lock_guard lock(g_lock); if (auto *p = player(self())) p->loops = std::int32_t(marg(0)); }
OBJC_METHOD(AVAudioPlayer, "numberOfLoops", numberOfLoops) { std::lock_guard lock(g_lock); auto *p = player(self()); ret(p ? std::uint32_t(p->loops) : 0); }
OBJC_METHOD(AVAudioPlayer, "setVolume:", setVolume) {
    std::lock_guard lock(g_lock);
    if (auto *p = player(self())) { p->volume = objc::margFloat(0); if (p->voice) p->voice->setVolume(p->volume); }
}
OBJC_METHOD(AVAudioPlayer, "volume", volume) { std::lock_guard lock(g_lock); auto *p = player(self()); hle::retFloat(p ? p->volume : 1); }
OBJC_METHOD(AVAudioPlayer, "setDelegate:", setDelegate) { std::lock_guard lock(g_lock); if (auto *p = player(self())) p->delegate = marg(0); }
OBJC_METHOD(AVAudioPlayer, "delegate", delegate) { std::lock_guard lock(g_lock); auto *p = player(self()); ret(p ? p->delegate : 0); }
OBJC_METHOD(AVAudioPlayer, "url", url) { std::lock_guard lock(g_lock); auto *p = player(self()); ret(p ? p->url : 0); }
OBJC_METHOD(AVAudioPlayer, "duration", duration) {
    std::lock_guard lock(g_lock);
    auto *p = player(self());
    hle::retDouble(p && p->pcm ? double(p->pcm->frames()) / std::max(1, p->pcm->rate) : 0);
}
OBJC_METHOD(AVAudioPlayer, "currentTime", currentTime) {
    std::lock_guard lock(g_lock);
    auto *p = player(self());
    hle::retDouble(p ? (p->playing || p->paused ? playedSeconds(*p) : double(p->startFrame) / std::max(1, p->pcm->rate)) : 0);
}
OBJC_METHOD(AVAudioPlayer, "setCurrentTime:", setCurrentTime) {
    std::lock_guard lock(g_lock);
    auto *p = player(self());
    if (!p || !p->pcm) return;
    const auto frame = std::uint64_t(std::max(0.0, objc::margDouble(0)) * p->pcm->rate);
    if (p->playing) { submit(*p, frame); p->voice->start(); }
    else p->startFrame = frame;
}
OBJC_METHOD(AVAudioPlayer, "numberOfChannels", numberOfChannels) { std::lock_guard lock(g_lock); auto *p = player(self()); ret(p && p->pcm ? std::uint32_t(p->pcm->channels) : 0); }
OBJC_METHOD(AVAudioPlayer, "setMeteringEnabled:", setMeteringEnabled) {}
OBJC_METHOD(AVAudioPlayer, "updateMeters", updateMeters) {}
OBJC_METHOD(AVAudioPlayer, "averagePowerForChannel:", averagePowerForChannel) { hle::retFloat(-160); }
OBJC_METHOD(AVAudioPlayer, "peakPowerForChannel:", peakPowerForChannel) { hle::retFloat(-160); }
OBJC_METHOD(AVAudioPlayer, "dealloc", dealloc) {
    std::lock_guard lock(g_lock);
    objc::setHostObject(self(), nullptr);
}
