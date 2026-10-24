// FFmpeg decodes MP3 and ADTS AAC files: libavformat demuxes, libavcodec decodes, libswresample converts to 16-bit
// stereo 44.1 kHz.
#include "platform/MediaDecode.h"
#include "Log.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace platform {
namespace {
constexpr int kRate = 44100;
// Converts a decoded frame (or flushes the resampler when frame is null) and appends the output.
bool convert(SwrContext *swr, const AVFrame *frame, std::vector<std::int16_t> &out) {
    const int inSamples = frame ? frame->nb_samples : 0;
    const int capacity = swr_get_out_samples(swr, inSamples);
    if (capacity <= 0) return true;
    const auto start = out.size();
    out.resize(start + std::size_t(capacity) * 2);
    auto *target = reinterpret_cast<std::uint8_t *>(out.data() + start);
    const int written = swr_convert(swr, &target, capacity, frame ? const_cast<const std::uint8_t **>(frame->extended_data) : nullptr, inSamples);
    if (written < 0) { out.resize(start); return false; }
    out.resize(start + std::size_t(written) * 2);
    return true;
}
} // namespace

bool decodeMediaFile(const std::string &hostPath, std::vector<std::int16_t> &out) {
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, hostPath.c_str(), nullptr, nullptr) < 0) return false;
    AVCodecContext *codec = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = false;
    const AVCodec *decoder = nullptr;
    const int stream = avformat_find_stream_info(format, nullptr) >= 0 ? av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0) : -1;
    if (stream >= 0 && decoder && (codec = avcodec_alloc_context3(decoder)) &&
        avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) >= 0 && avcodec_open2(codec, decoder, nullptr) >= 0) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kRate, &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr) >= 0 &&
            swr_init(swr) >= 0) {
            ok = true;
            const auto receive = [&] {
                while (avcodec_receive_frame(codec, frame) >= 0) {
                    if (!convert(swr, frame, out)) ok = false;
                    av_frame_unref(frame);
                }
            };
            while (ok && av_read_frame(format, packet) >= 0) {
                if (packet->stream_index == stream && avcodec_send_packet(codec, packet) >= 0) receive();
                av_packet_unref(packet);
            }
            avcodec_send_packet(codec, nullptr); // drain
            receive();
            convert(swr, nullptr, out);
        }
    }
    if (!ok) hlog("audio", "FFmpeg could not decode %s", hostPath.c_str());
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return ok && !out.empty();
}
} // namespace platform
