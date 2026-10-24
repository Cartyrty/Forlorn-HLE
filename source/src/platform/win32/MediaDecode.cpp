// Media Foundation's source reader decodes MP3 and ADTS AAC files to 16-bit stereo 44.1 kHz PCM.
#include "platform/MediaDecode.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <filesystem>
#include <mutex>

namespace platform {
bool decodeMediaFile(const std::string &hostPath, std::vector<std::int16_t> &out) {
    static std::once_flag started;
    std::call_once(started, [] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    IMFSourceReader *reader = nullptr;
    const auto wide = std::filesystem::u8path(hostPath).wstring();
    if (FAILED(MFCreateSourceReaderFromURL(wide.c_str(), nullptr, &reader))) return false;
    IMFMediaType *type = nullptr;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type);
    type->Release();
    for (;;) {
        DWORD flags = 0;
        IMFSample *sample = nullptr;
        if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample))) break;
        if (sample) {
            IMFMediaBuffer *buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                BYTE *data = nullptr;
                DWORD length = 0;
                buffer->Lock(&data, nullptr, &length);
                const auto *samples = reinterpret_cast<const std::int16_t *>(data);
                out.insert(out.end(), samples, samples + length / 2);
                buffer->Unlock();
                buffer->Release();
            }
            sample->Release();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    reader->Release();
    return !out.empty();
}
} // namespace platform
