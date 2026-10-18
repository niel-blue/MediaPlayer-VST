#include "MediaPlayerVideoDecoder.h"

#include "MediaPlayerVideo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace MediaPlayer {
namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
constexpr int kMaxDecodedVideoWidth = 1920;
constexpr int kMaxDecodedVideoHeight = 1080;

std::int64_t durationToMicroseconds(const AVFormatContext* format,
                                    const AVStream* stream)
{
    if (format != nullptr && format->duration != AV_NOPTS_VALUE &&
        format->duration > 0)
        return format->duration;
    if (stream != nullptr && stream->duration != AV_NOPTS_VALUE &&
        stream->duration > 0)
        return av_rescale_q(stream->duration,
                            stream->time_base,
                            AVRational {1, AV_TIME_BASE});
    return 0;
}

int swsColorspaceForFrame(const AVFrame* frame, int width, int height)
{
    if (frame == nullptr)
        return (width >= 1280 || height >= 720) ? SWS_CS_ITU709
                                                 : SWS_CS_ITU601;

    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_FCC:
        return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE170M:
        return SWS_CS_SMPTE170M;
    case AVCOL_SPC_SMPTE240M:
        return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    case AVCOL_SPC_BT470BG:
        return SWS_CS_ITU601;
    default:
        // Many ordinary files omit matrix metadata. Use the conventional
        // HD/SD defaults in that case.
        return (width >= 1280 || height >= 720) ? SWS_CS_ITU709
                                                 : SWS_CS_ITU601;
    }
}

bool isFullRangeFrame(const AVFrame* frame, AVPixelFormat format)
{
    if (frame != nullptr && frame->color_range == AVCOL_RANGE_JPEG)
        return true;

    return format == AV_PIX_FMT_YUVJ420P ||
           format == AV_PIX_FMT_YUVJ422P ||
           format == AV_PIX_FMT_YUVJ444P ||
           format == AV_PIX_FMT_YUVJ440P;
}

void configureScalerColorSpace(SwsContext* scaler,
                               const AVFrame* frame,
                               AVPixelFormat format,
                               int width,
                               int height)
{
    if (scaler == nullptr)
        return;
    const auto* descriptor = av_pix_fmt_desc_get(format);
    if (descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0)
        return;

    const auto sourceSpace = swsColorspaceForFrame(frame, width, height);
    const auto sourceRange = isFullRangeFrame(frame, format) ? 1 : 0;
    sws_setColorspaceDetails(scaler,
                             sws_getCoefficients(sourceSpace),
                             sourceRange,
                             sws_getCoefficients(sourceSpace),
                             0,
                             0,
                             1 << 16,
                             1 << 16);
}

} // namespace

struct VideoDecoder::Impl final {
    explicit Impl(std::atomic<bool>* stopRequested)
        : stopRequested(stopRequested)
    {
    }

    AVFormatContext* format {nullptr};
    AVCodecContext* codec {nullptr};
    AVPacket* packet {nullptr};
    AVFrame* frame {nullptr};
    SwsContext* scaler {nullptr};
    int streamIndex {-1};
    AVRational timeBase {1, 1};
    int width {0};
    int height {0};
    int outputWidth {0};
    int outputHeight {0};
    double frameRate {30.0};
    std::int64_t durationMicroseconds {0};
    bool flushSent {false};
    std::atomic<bool>* stopRequested {nullptr};

    static int interruptCallback(void* opaque)
    {
        const auto* stop = static_cast<const std::atomic<bool>*>(opaque);
        return stop != nullptr && stop->load(std::memory_order_acquire) ? 1 : 0;
    }

    void close() noexcept
    {
        if (scaler != nullptr) {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        if (frame != nullptr)
            av_frame_free(&frame);
        if (packet != nullptr)
            av_packet_free(&packet);
        if (codec != nullptr)
            avcodec_free_context(&codec);
        if (format != nullptr)
            avformat_close_input(&format);
        streamIndex = -1;
        width = 0;
        height = 0;
        outputWidth = 0;
        outputHeight = 0;
        durationMicroseconds = 0;
        frameRate = 30.0;
        flushSent = false;
    }

    bool open(const std::filesystem::path& file, VideoInfo& info)
    {
        close();
        const auto utf8 = file.u8string();
        if (utf8.empty())
            return false;

        format = avformat_alloc_context();
        if (format == nullptr)
            return false;
        format->interrupt_callback.callback = &Impl::interruptCallback;
        format->interrupt_callback.opaque = stopRequested;
        if (avformat_open_input(&format, utf8.c_str(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(format, nullptr) < 0) {
            close();
            return false;
        }

        const auto streamResult = av_find_best_stream(
            format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamResult < 0) {
            close();
            return false;
        }
        streamIndex = streamResult;
        const auto* stream = format->streams[streamIndex];
        const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            close();
            return false;
        }

        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr ||
            avcodec_parameters_to_context(codec, stream->codecpar) < 0 ||
            avcodec_open2(codec, decoder, nullptr) < 0) {
            close();
            return false;
        }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            close();
            return false;
        }

        width = std::max(1, codec->width);
        height = std::max(1, codec->height);
        const auto outputScale = std::min({
            1.0,
            static_cast<double>(kMaxDecodedVideoWidth) / width,
            static_cast<double>(kMaxDecodedVideoHeight) / height,
        });
        outputWidth = std::max(
            1,
            static_cast<int>(std::lround(width * outputScale)));
        outputHeight = std::max(
            1,
            static_cast<int>(std::lround(height * outputScale)));
        timeBase = stream->time_base;
        const auto averageRate = av_q2d(stream->avg_frame_rate);
        const auto realRate = av_q2d(stream->r_frame_rate);
        frameRate = averageRate > 0.0 ? averageRate
                                      : (realRate > 0.0 ? realRate : 30.0);
        durationMicroseconds = durationToMicroseconds(format, stream);
        scaler = sws_getContext(width,
                                height,
                                codec->pix_fmt,
                                outputWidth,
                                outputHeight,
                                AV_PIX_FMT_BGR0,
                                SWS_BILINEAR,
                                nullptr,
                                nullptr,
                                nullptr);
        if (scaler == nullptr) {
            close();
            return false;
        }

        info.hasVideo = true;
        info.width = width;
        info.height = height;
        info.frameRate = frameRate;
        info.durationMicroseconds = durationMicroseconds;
        return true;
    }

    bool seek(double seconds)
    {
        if (format == nullptr || codec == nullptr || streamIndex < 0)
            return false;

        seconds = std::max(0.0, seconds);
        const auto targetMicroseconds = static_cast<std::int64_t>(
            std::llround(seconds * kMicrosecondsPerSecond));
        const auto timestamp = av_rescale_q(
            targetMicroseconds,
            AVRational {1, AV_TIME_BASE},
            timeBase);
        if (av_seek_frame(format, streamIndex, timestamp,
                          AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        avcodec_flush_buffers(codec);
        av_packet_unref(packet);
        av_frame_unref(frame);
        flushSent = false;
        return true;
    }

    std::shared_ptr<const VideoFrame> decodeNext()
    {
        if (codec == nullptr || packet == nullptr || frame == nullptr ||
            scaler == nullptr)
            return nullptr;

        for (;;) {
            const auto receiveResult = avcodec_receive_frame(codec, frame);
            if (receiveResult == 0) {
                const auto sourceWidth = frame->width > 0 ? frame->width : width;
                const auto sourceHeight = frame->height > 0 ? frame->height : height;
                const auto sourceFormat = static_cast<AVPixelFormat>(frame->format);
                if (sourceFormat == AV_PIX_FMT_NONE || frame->data[0] == nullptr)
                    continue;

                scaler = sws_getCachedContext(scaler,
                                              sourceWidth,
                                              sourceHeight,
                                              sourceFormat,
                                              outputWidth,
                                              outputHeight,
                                              AV_PIX_FMT_BGR0,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);
                if (scaler == nullptr)
                    continue;

                configureScalerColorSpace(scaler,
                                          frame,
                                          sourceFormat,
                                          sourceWidth,
                                          sourceHeight);

                auto output = std::make_shared<VideoFrame>();
                output->width = outputWidth;
                output->height = outputHeight;
                output->bgra.resize(static_cast<std::size_t>(outputWidth) *
                                    static_cast<std::size_t>(outputHeight) * 4);
                uint8_t* destination[] = {output->bgra.data(), nullptr,
                                           nullptr, nullptr};
                int destinationStride[] = {outputWidth * 4, 0, 0, 0};
                const auto scaledHeight = sws_scale(scaler,
                                                    frame->data,
                                                    frame->linesize,
                                                    0,
                                                    sourceHeight,
                                                    destination,
                                                    destinationStride);
                if (scaledHeight <= 0)
                    continue;

                auto timestamp = frame->best_effort_timestamp;
                if (timestamp == AV_NOPTS_VALUE)
                    timestamp = 0;
                output->presentationMicroseconds = std::max<std::int64_t>(
                    0,
                    av_rescale_q(timestamp,
                                 timeBase,
                                 AVRational {1, AV_TIME_BASE}));
                return output;
            }

            if (receiveResult == AVERROR_EOF)
                return nullptr;
            if (receiveResult != AVERROR(EAGAIN))
                return nullptr;

            if (flushSent)
                return nullptr;
            const auto readResult = av_read_frame(format, packet);
            if (readResult < 0) {
                avcodec_send_packet(codec, nullptr);
                flushSent = true;
                continue;
            }

            if (packet->stream_index == streamIndex)
                avcodec_send_packet(codec, packet);
            av_packet_unref(packet);
        }
    }
};

VideoDecoder::VideoDecoder(std::atomic<bool>* stopRequested)
    : impl_(std::make_unique<Impl>(stopRequested))
{
}

VideoDecoder::~VideoDecoder()
{
    close();
}

bool VideoDecoder::open(const std::filesystem::path& file, VideoInfo& info)
{
    return impl_->open(file, info);
}

void VideoDecoder::close() noexcept
{
    impl_->close();
}

bool VideoDecoder::seek(double seconds)
{
    return impl_->seek(seconds);
}

std::shared_ptr<const VideoFrame> VideoDecoder::decodeNext()
{
    return impl_->decodeNext();
}

bool VideoDecoder::isOpen() const noexcept
{
    return impl_->format != nullptr;
}

} // namespace MediaPlayer
