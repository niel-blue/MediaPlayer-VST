#include "FfmpegAudioDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace MediaPlayer {
namespace {

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

struct FormatContextDeleter final {
    void operator()(AVFormatContext* context) const noexcept
    {
        if (context != nullptr)
            avformat_close_input(&context);
    }
};

struct CodecContextDeleter final {
    void operator()(AVCodecContext* context) const noexcept
    {
        avcodec_free_context(&context);
    }
};

struct PacketDeleter final {
    void operator()(AVPacket* packet) const noexcept
    {
        av_packet_free(&packet);
    }
};

struct FrameDeleter final {
    void operator()(AVFrame* frame) const noexcept
    {
        av_frame_free(&frame);
    }
};

struct SwrContextDeleter final {
    void operator()(SwrContext* context) const noexcept
    {
        swr_free(&context);
    }
};

struct ChannelLayoutGuard final {
    AVChannelLayout layout {};

    ~ChannelLayoutGuard()
    {
        av_channel_layout_uninit(&layout);
    }

    void reset() noexcept
    {
        av_channel_layout_uninit(&layout);
        layout = {};
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

} // namespace

struct FfmpegAudioStream::Impl final {
    FormatContextPtr formatContext;
    CodecContextPtr codecContext;
    PacketPtr packet;
    FramePtr frame;
    SwrContextPtr resampler;
    ChannelLayoutGuard inputLayout;
    ChannelLayoutGuard outputLayout;

    int audioStreamIndex {-1};
    int sourceSampleRate {0};
    int outputSampleRate {0};
    bool inputEof {false};
    bool decoderFlushSent {false};
    bool decoderEof {false};
    bool pendingFrame {false};
    bool finished {false};
    bool failed {false};

    DecodedAudioInfo info;
    std::vector<float> converted;
    std::vector<float> left;
    std::vector<float> right;

    void reset() noexcept
    {
        formatContext.reset();
        codecContext.reset();
        packet.reset();
        frame.reset();
        resampler.reset();
        inputLayout.reset();
        outputLayout.reset();
        audioStreamIndex = -1;
        sourceSampleRate = 0;
        outputSampleRate = 0;
        inputEof = false;
        decoderFlushSent = false;
        decoderEof = false;
        pendingFrame = false;
        finished = false;
        failed = false;
        info = {};
        converted.clear();
        left.clear();
        right.clear();
    }
};

FfmpegAudioStream::FfmpegAudioStream()
    : impl_(std::make_unique<Impl>())
{
}

FfmpegAudioStream::~FfmpegAudioStream() = default;

bool FfmpegAudioStream::open(const std::filesystem::path& file,
                             int outputSampleRate,
                             DecodedAudioInfo& info,
                             std::string& error)
{
    impl_->reset();
    info = {};
    error.clear();

    if (outputSampleRate <= 0) {
        error = "Output sample rate must be positive";
        return false;
    }

    // C++20 changes filesystem::path::u8string() to std::u8string. Keep the
    // UTF-8 path explicit so the same decoder source remains usable by the
    // C++17 DAW plug-in and the C++20 standalone application.
#if defined(__cpp_char8_t)
    const auto utf8Value = file.u8string();
    const std::string utf8Path(
        reinterpret_cast<const char*>(utf8Value.data()), utf8Value.size());
#else
    const std::string utf8Path = file.u8string();
#endif
    AVFormatContext* rawFormatContext = nullptr;
    auto result = avformat_open_input(&rawFormatContext, utf8Path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "avformat_open_input: " + ffmpegError(result);
        return false;
    }
    impl_->formatContext.reset(rawFormatContext);

    result = avformat_find_stream_info(impl_->formatContext.get(), nullptr);
    if (result < 0) {
        error = "avformat_find_stream_info: " + ffmpegError(result);
        close();
        return false;
    }

    impl_->audioStreamIndex = av_find_best_stream(
        impl_->formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (impl_->audioStreamIndex < 0) {
        error = "No audio stream: " + ffmpegError(impl_->audioStreamIndex);
        close();
        return false;
    }

    const auto* stream = impl_->formatContext->streams[impl_->audioStreamIndex];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        error = "No decoder for the audio codec";
        close();
        return false;
    }

    impl_->codecContext.reset(avcodec_alloc_context3(decoder));
    if (!impl_->codecContext) {
        error = "avcodec_alloc_context3 failed";
        close();
        return false;
    }

    result = avcodec_parameters_to_context(impl_->codecContext.get(), stream->codecpar);
    if (result < 0) {
        error = "avcodec_parameters_to_context: " + ffmpegError(result);
        close();
        return false;
    }

    result = avcodec_open2(impl_->codecContext.get(), decoder, nullptr);
    if (result < 0) {
        error = "avcodec_open2: " + ffmpegError(result);
        close();
        return false;
    }

    impl_->sourceSampleRate = impl_->codecContext->sample_rate;
    const auto sourceChannels = impl_->codecContext->ch_layout.nb_channels;
    if (impl_->sourceSampleRate <= 0 || sourceChannels <= 0) {
        error = "Invalid source audio format";
        close();
        return false;
    }

    result = av_channel_layout_copy(&impl_->inputLayout.layout,
                                    &impl_->codecContext->ch_layout);
    if (result < 0) {
        error = "av_channel_layout_copy: " + ffmpegError(result);
        close();
        return false;
    }

    av_channel_layout_default(&impl_->outputLayout.layout, 2);
    impl_->outputSampleRate = outputSampleRate;

    SwrContext* rawResampler = nullptr;
    result = swr_alloc_set_opts2(&rawResampler,
                                 &impl_->outputLayout.layout,
                                 AV_SAMPLE_FMT_FLT,
                                 outputSampleRate,
                                 &impl_->inputLayout.layout,
                                 impl_->codecContext->sample_fmt,
                                 impl_->sourceSampleRate,
                                 0,
                                 nullptr);
    if (result < 0) {
        error = "swr_alloc_set_opts2: " + ffmpegError(result);
        close();
        return false;
    }
    impl_->resampler.reset(rawResampler);

    result = swr_init(impl_->resampler.get());
    if (result < 0) {
        error = "swr_init: " + ffmpegError(result);
        close();
        return false;
    }

    impl_->packet.reset(av_packet_alloc());
    impl_->frame.reset(av_frame_alloc());
    if (!impl_->packet || !impl_->frame) {
        error = "FFmpeg packet/frame allocation failed";
        close();
        return false;
    }

    impl_->info.sourceSampleRate = impl_->sourceSampleRate;
    impl_->info.sourceChannels = sourceChannels;
    impl_->info.outputSampleRate = outputSampleRate;

    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        impl_->info.durationFrames = static_cast<std::uint64_t>(av_rescale_q(
            stream->duration,
            stream->time_base,
            AVRational {1, outputSampleRate}));
    } else if (impl_->formatContext->duration != AV_NOPTS_VALUE &&
               impl_->formatContext->duration > 0) {
        impl_->info.durationFrames = static_cast<std::uint64_t>(av_rescale_q(
            impl_->formatContext->duration,
            AV_TIME_BASE_Q,
            AVRational {1, outputSampleRate}));
    }
    info = impl_->info;
    return true;
}

AudioDecodeResult FfmpegAudioStream::decode(PcmRingBuffer& output, std::string& error)
{
    error.clear();
    if (!impl_->codecContext || !impl_->resampler || !impl_->packet || !impl_->frame) {
        error = "FFmpeg decoder is not open";
        return AudioDecodeResult::error;
    }
    if (impl_->failed) {
        error = "FFmpeg decoder is in a failed state";
        return AudioDecodeResult::error;
    }
    if (impl_->finished)
        return AudioDecodeResult::finished;

    auto fail = [&](const std::string& message) {
        impl_->failed = true;
        error = message;
        return AudioDecodeResult::error;
    };

    auto convertFrame = [&]() {
        const auto estimatedSamples = av_rescale_rnd(
            swr_get_delay(impl_->resampler.get(), impl_->sourceSampleRate) +
                impl_->frame->nb_samples,
            impl_->outputSampleRate,
            impl_->sourceSampleRate,
            AV_ROUND_UP);
        if (estimatedSamples <= 0) {
            av_frame_unref(impl_->frame.get());
            impl_->pendingFrame = false;
            return AudioDecodeResult::producedAudio;
        }

        if (static_cast<std::size_t>(estimatedSamples) > output.capacityFrames())
            return fail("Decoded audio frame is larger than the PCM ring buffer");
        if (output.freeFrames() < static_cast<std::size_t>(estimatedSamples))
            return AudioDecodeResult::needOutputSpace;

        impl_->converted.resize(static_cast<std::size_t>(estimatedSamples) * 2);
        auto* outputData = reinterpret_cast<std::uint8_t*>(impl_->converted.data());
        std::uint8_t* outputPlanes[] = {outputData};
        const auto convertedSamples = swr_convert(
            impl_->resampler.get(),
            outputPlanes,
            static_cast<int>(estimatedSamples),
            const_cast<const std::uint8_t**>(impl_->frame->extended_data),
            impl_->frame->nb_samples);
        if (convertedSamples < 0)
            return fail("swr_convert: " + ffmpegError(convertedSamples));

        impl_->left.resize(static_cast<std::size_t>(convertedSamples));
        impl_->right.resize(static_cast<std::size_t>(convertedSamples));
        for (int sample = 0; sample < convertedSamples; ++sample) {
            impl_->left[static_cast<std::size_t>(sample)] =
                impl_->converted[2 * static_cast<std::size_t>(sample)];
            impl_->right[static_cast<std::size_t>(sample)] =
                impl_->converted[2 * static_cast<std::size_t>(sample) + 1];
        }

        const auto written = output.writeStereo(
            impl_->left.data(),
            impl_->right.data(),
            static_cast<std::size_t>(convertedSamples));
        if (written != static_cast<std::size_t>(convertedSamples))
            return fail("PCM ring buffer became full while decoding");

        impl_->info.outputFrames += static_cast<std::uint64_t>(convertedSamples);
        av_frame_unref(impl_->frame.get());
        impl_->pendingFrame = false;
        return convertedSamples > 0
            ? AudioDecodeResult::producedAudio
            : AudioDecodeResult::finished;
    };

    auto drainResampler = [&]() {
        const auto delay = swr_get_delay(impl_->resampler.get(), impl_->sourceSampleRate);
        if (delay <= 0) {
            impl_->finished = true;
            return AudioDecodeResult::finished;
        }

        const auto estimatedSamples = av_rescale_rnd(
            delay,
            impl_->outputSampleRate,
            impl_->sourceSampleRate,
            AV_ROUND_UP);
        if (estimatedSamples <= 0) {
            impl_->finished = true;
            return AudioDecodeResult::finished;
        }
        if (static_cast<std::size_t>(estimatedSamples) > output.capacityFrames())
            return fail("Resampler output is larger than the PCM ring buffer");
        if (output.freeFrames() < static_cast<std::size_t>(estimatedSamples))
            return AudioDecodeResult::needOutputSpace;

        impl_->converted.resize(static_cast<std::size_t>(estimatedSamples) * 2);
        auto* outputData = reinterpret_cast<std::uint8_t*>(impl_->converted.data());
        std::uint8_t* outputPlanes[] = {outputData};
        const auto convertedSamples = swr_convert(
            impl_->resampler.get(),
            outputPlanes,
            static_cast<int>(estimatedSamples),
            nullptr,
            0);
        if (convertedSamples < 0)
            return fail("swr_convert (flush): " + ffmpegError(convertedSamples));
        if (convertedSamples == 0) {
            impl_->finished = true;
            return AudioDecodeResult::finished;
        }

        impl_->left.resize(static_cast<std::size_t>(convertedSamples));
        impl_->right.resize(static_cast<std::size_t>(convertedSamples));
        for (int sample = 0; sample < convertedSamples; ++sample) {
            impl_->left[static_cast<std::size_t>(sample)] =
                impl_->converted[2 * static_cast<std::size_t>(sample)];
            impl_->right[static_cast<std::size_t>(sample)] =
                impl_->converted[2 * static_cast<std::size_t>(sample) + 1];
        }

        const auto written = output.writeStereo(
            impl_->left.data(),
            impl_->right.data(),
            static_cast<std::size_t>(convertedSamples));
        if (written != static_cast<std::size_t>(convertedSamples))
            return fail("PCM ring buffer became full while flushing the resampler");

        impl_->info.outputFrames += static_cast<std::uint64_t>(convertedSamples);
        return AudioDecodeResult::producedAudio;
    };

    while (true) {
        if (impl_->pendingFrame) {
            const auto result = convertFrame();
            if (result != AudioDecodeResult::producedAudio)
                return result;
            if (output.availableFrames() > 0)
                return result;
            continue;
        }

        if (impl_->decoderEof)
            return drainResampler();

        auto result = avcodec_receive_frame(impl_->codecContext.get(), impl_->frame.get());
        if (result == 0) {
            impl_->pendingFrame = true;
            continue;
        }
        if (result == AVERROR_EOF) {
            impl_->decoderEof = true;
            continue;
        }
        if (result != AVERROR(EAGAIN))
            return fail("avcodec_receive_frame: " + ffmpegError(result));

        if (impl_->inputEof) {
            if (impl_->decoderFlushSent)
                continue;

            result = avcodec_send_packet(impl_->codecContext.get(), nullptr);
            if (result < 0 && result != AVERROR_EOF)
                return fail("avcodec_send_packet (flush): " + ffmpegError(result));
            impl_->decoderFlushSent = true;
            continue;
        }

        while (true) {
            result = av_read_frame(impl_->formatContext.get(), impl_->packet.get());
            if (result == AVERROR_EOF) {
                impl_->inputEof = true;
                break;
            }
            if (result < 0)
                return fail("av_read_frame: " + ffmpegError(result));
            if (impl_->packet->stream_index != impl_->audioStreamIndex) {
                av_packet_unref(impl_->packet.get());
                continue;
            }

            result = avcodec_send_packet(impl_->codecContext.get(), impl_->packet.get());
            av_packet_unref(impl_->packet.get());
            if (result < 0 && result != AVERROR(EAGAIN))
                return fail("avcodec_send_packet: " + ffmpegError(result));
            break;
        }
    }
}

DecodedAudioInfo FfmpegAudioStream::decodedInfo() const noexcept
{
    return impl_->info;
}

bool FfmpegAudioStream::seekToNormalized(double normalizedPosition, std::string& error)
{
    error.clear();
    if (!impl_->formatContext || !impl_->codecContext || !impl_->resampler) {
        error = "FFmpeg decoder is not open";
        return false;
    }

    const auto position = std::clamp(normalizedPosition, 0.0, 1.0);
    auto result = 0;
    const auto* stream = impl_->formatContext->streams[impl_->audioStreamIndex];
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        const auto timestamp = static_cast<std::int64_t>(std::llround(
            position * static_cast<double>(stream->duration)));
        result = av_seek_frame(impl_->formatContext.get(),
                               impl_->audioStreamIndex,
                               timestamp,
                               AVSEEK_FLAG_BACKWARD);
    } else if (impl_->formatContext->duration != AV_NOPTS_VALUE &&
               impl_->formatContext->duration > 0) {
        const auto timestamp = static_cast<std::int64_t>(std::llround(
            position * static_cast<double>(impl_->formatContext->duration)));
        result = av_seek_frame(impl_->formatContext.get(),
                               -1,
                               timestamp,
                               AVSEEK_FLAG_BACKWARD);
    } else if (position == 0.0) {
        result = av_seek_frame(impl_->formatContext.get(),
                               impl_->audioStreamIndex,
                               0,
                               AVSEEK_FLAG_BACKWARD);
    } else {
        error = "The media duration is unavailable";
        return false;
    }

    if (result < 0) {
        error = "av_seek_frame: " + ffmpegError(result);
        return false;
    }

    avcodec_flush_buffers(impl_->codecContext.get());
    av_packet_unref(impl_->packet.get());
    av_frame_unref(impl_->frame.get());
    swr_close(impl_->resampler.get());
    result = swr_init(impl_->resampler.get());
    if (result < 0) {
        error = "swr_init after seek: " + ffmpegError(result);
        return false;
    }

    impl_->inputEof = false;
    impl_->decoderFlushSent = false;
    impl_->decoderEof = false;
    impl_->pendingFrame = false;
    impl_->finished = false;
    impl_->failed = false;
    impl_->info.outputFrames = 0;
    return true;
}

void FfmpegAudioStream::close() noexcept
{
    impl_->reset();
}

bool FfmpegAudioDecoder::decodeToQueue(const std::filesystem::path& file,
                                       int outputSampleRate,
                                       PcmRingBuffer& output,
                                       DecodedAudioInfo& info,
                                       std::string& error) const
{
    FfmpegAudioStream stream;
    if (!stream.open(file, outputSampleRate, info, error))
        return false;

    while (true) {
        const auto result = stream.decode(output, error);
        if (result == AudioDecodeResult::finished) {
            info = stream.decodedInfo();
            return true;
        }
        if (result == AudioDecodeResult::error)
            return false;
        if (result == AudioDecodeResult::needOutputSpace) {
            error = "PCM ring buffer is full while decoding";
            return false;
        }
    }
}

} // namespace MediaPlayer
