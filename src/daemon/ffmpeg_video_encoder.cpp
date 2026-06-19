#include "daemon/media_interfaces.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tenbox::daemon {

namespace {

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::vector<std::string> H264EncoderCandidates(const VideoEncoderConfig& config) {
    if (!config.h264_encoder_candidates.empty()) return config.h264_encoder_candidates;
    return {"h264_nvenc", "libx264"};
}

const char* H264ProfileName(H264Profile profile) {
    switch (profile) {
    case H264Profile::kHigh:
        return "high";
    case H264Profile::kMain:
        return "main";
    case H264Profile::kConstrainedBaseline:
    default:
        return "baseline";
    }
}

const char* H264Libx264ProfileName(const VideoEncoderConfig& config) {
    if (config.input_format == PixelFormat::kYuv444p) return "high444";
    return H264ProfileName(config.h264_profile);
}

const char* H264NvencProfileName(const VideoEncoderConfig& config) {
    if (config.input_format == PixelFormat::kYuv444p) return "high444p";
    return H264ProfileName(config.h264_profile);
}

AVPixelFormat AvPixelFormatFor(PixelFormat format) {
    switch (format) {
    case PixelFormat::kYuv420p:
        return AV_PIX_FMT_YUV420P;
    case PixelFormat::kYuv444p:
        return AV_PIX_FMT_YUV444P;
    case PixelFormat::kRgba:
    case PixelFormat::kBgra:
    default:
        return AV_PIX_FMT_NONE;
    }
}

void ConfigureCommonH264Context(AVCodecContext* codec, const VideoEncoderConfig& config) {
    codec->width = static_cast<int>(config.width);
    codec->height = static_cast<int>(config.height);
    codec->time_base = AVRational{1, static_cast<int>(std::max<uint32_t>(config.framerate, 1))};
    codec->framerate = AVRational{static_cast<int>(std::max<uint32_t>(config.framerate, 1)), 1};
    codec->bit_rate = static_cast<int64_t>(config.bitrate_bps);
    codec->rc_max_rate = static_cast<int64_t>(config.bitrate_bps);
    codec->rc_buffer_size = static_cast<int>(std::max<uint32_t>(config.bitrate_bps / 2, 1));
    codec->gop_size = static_cast<int>(std::max<uint32_t>(config.framerate * 240, 1));
    codec->max_b_frames = 0;
    // Single-threaded encoding minimizes per-frame latency for remote desktop.
    // Multi-thread parallelism adds a frame pipeline delay with no throughput
    // benefit at the low resolutions / framerates used here.
    // Override with AGENTSPHERE_ENCODER_THREADS=0 to let FFmpeg choose automatically.
    int thread_count = 1;
    if (const char* v = std::getenv("AGENTSPHERE_ENCODER_THREADS"); v && v[0] != '\0') {
        const auto result = std::from_chars(v, v + std::strlen(v), thread_count);
        if (result.ec != std::errc{}) thread_count = 1;
    }
    codec->thread_count = thread_count;
    codec->pix_fmt = AvPixelFormatFor(config.input_format);
    codec->color_primaries = AVCOL_PRI_BT709;
    codec->color_trc = AVCOL_TRC_IEC61966_2_1;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_range = AVCOL_RANGE_MPEG;
}

void SetOptionalOption(AVCodecContext* codec, const char* key, const char* value) {
    if (!codec->priv_data) return;
    av_opt_set(codec->priv_data, key, value, 0);
}

bool ConfigureLibx264(AVCodecContext* codec, const VideoEncoderConfig& config, std::string* error) {
    if (!codec->priv_data) return true;

    // Match libx264's "ultrafast" preset for all profiles (mostly disables
    // analysis); "zerolatency" turns off lookahead/B-frames so EncodeFrame
    // returns a packet immediately.
    av_opt_set(codec->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codec->priv_data, "tune", "zerolatency", 0);
    av_opt_set(codec->priv_data, "profile", H264Libx264ProfileName(config), 0);
    if (config.h264_profile != H264Profile::kConstrainedBaseline) {
        // libx264's ultrafast preset disables CABAC; Main/High are selected to
        // trade a little encoder work for better quality at the same bitrate.
        av_opt_set(codec->priv_data, "coder", "cabac", 0);
    }
    if (config.h264_profile == H264Profile::kHigh) {
        // 8x8 transform is a High-profile tool, so don't enable it for Main.
        av_opt_set(codec->priv_data, "8x8dct", "1", 0);
    }

    // Emit AVCC (length-prefixed) NALUs directly so the RTP packetizer
    // (NalUnit::Separator::Length) can consume packets without a start-code
    // rewrite. FFmpeg's libx264 wrapper routes this through x264-params.
    const int rc = av_opt_set(codec->priv_data, "x264-params", "annexb=0", 0);
    if (rc < 0) {
        if (error) *error = "failed to set x264-params=annexb=0: " + AvError(rc);
        return false;
    }

    av_opt_set(codec->priv_data, "keyint", "7200", 0);
    av_opt_set(codec->priv_data, "min-keyint", "7200", 0);
    av_opt_set(codec->priv_data, "scenecut", "0", 0);
    const auto maxrate = std::to_string(config.bitrate_bps);
    const auto bufsize = std::to_string(config.bitrate_bps / 2);
    av_opt_set(codec->priv_data, "maxrate", maxrate.c_str(), 0);
    av_opt_set(codec->priv_data, "bufsize", bufsize.c_str(), 0);
    return true;
}

bool ConfigureH264Nvenc(AVCodecContext* codec, const VideoEncoderConfig& config, std::string* /*error*/) {
    if (!codec->priv_data) return true;

    // Use capped VBR for remote desktop: static/low-motion frames can shrink,
    // while maxrate/bufsize on the codec context still bound network bursts.
    SetOptionalOption(codec, "preset", "p1");
    SetOptionalOption(codec, "tune", "ull");
    SetOptionalOption(codec, "rc", "vbr");
    SetOptionalOption(codec, "cq", "24");
    SetOptionalOption(codec, "rc-lookahead", "0");
    SetOptionalOption(codec, "spatial-aq", "1");
    SetOptionalOption(codec, "temporal-aq", "1");
    SetOptionalOption(codec, "delay", "0");
    SetOptionalOption(codec, "zerolatency", "1");
    SetOptionalOption(codec, "forced-idr", "1");
    SetOptionalOption(codec, "profile", H264NvencProfileName(config));
    return true;
}

bool ConfigureEncoderOptions(AVCodecContext* codec,
                             const std::string& encoder_name,
                             const VideoEncoderConfig& config,
                             std::string* error) {
    if (encoder_name == "libx264") {
        return ConfigureLibx264(codec, config, error);
    }
    if (encoder_name == "h264_nvenc") {
        return ConfigureH264Nvenc(codec, config, error);
    }
    if (error) *error = "unsupported FFmpeg H.264 encoder '" + encoder_name + "'";
    return false;
}

void AppendAttempt(std::string* attempts, const std::string& encoder_name, const std::string& reason) {
    if (!attempts) return;
    if (!attempts->empty()) attempts->append("; ");
    attempts->append(encoder_name).append(": ").append(reason);
}

size_t StartCodeLength(const uint8_t* data, size_t size, size_t offset) {
    if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
        return 3;
    }
    if (offset + 4 <= size &&
        data[offset] == 0 &&
        data[offset + 1] == 0 &&
        data[offset + 2] == 0 &&
        data[offset + 3] == 1) {
        return 4;
    }
    return 0;
}

size_t FindStartCode(const uint8_t* data, size_t size, size_t offset) {
    for (size_t i = offset; i + 3 <= size; ++i) {
        if (StartCodeLength(data, size, i) != 0) return i;
    }
    return size;
}

bool AnnexBToLengthPrefixed(std::span<const uint8_t> input, std::vector<uint8_t>* output) {
    if (!output) return false;
    output->clear();
    const uint8_t* data = input.data();
    const size_t size = input.size();
    size_t start = FindStartCode(data, size, 0);
    while (start < size) {
        const size_t prefix = StartCodeLength(data, size, start);
        size_t nalu_start = start + prefix;
        const size_t next = FindStartCode(data, size, nalu_start);
        size_t nalu_end = next;
        while (nalu_end > nalu_start && data[nalu_end - 1] == 0) --nalu_end;
        const size_t nalu_size = nalu_end - nalu_start;
        if (nalu_size > 0xffffffffu) return false;
        if (nalu_size > 0) {
            output->push_back(static_cast<uint8_t>((nalu_size >> 24) & 0xff));
            output->push_back(static_cast<uint8_t>((nalu_size >> 16) & 0xff));
            output->push_back(static_cast<uint8_t>((nalu_size >> 8) & 0xff));
            output->push_back(static_cast<uint8_t>(nalu_size & 0xff));
            output->insert(output->end(), data + nalu_start, data + nalu_end);
        }
        start = next;
    }
    return !output->empty();
}

}  // namespace

struct FfmpegH264VideoEncoder::Impl {
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    VideoEncoderConfig config;
    std::string selected_encoder_name;
    std::vector<uint8_t> encoded_;
    int64_t next_pts = 0;
    bool output_annexb = false;
    bool force_keyframe = true;
    // True once a (0,0,W,H) slice has been applied to `frame`. The slice
    // pipeline refuses to encode before that, since partial slices on a zeroed
    // input would produce a frame with stale pixels in the unwritten regions.
    bool frame_initialized = false;
};

FfmpegH264VideoEncoder::FfmpegH264VideoEncoder() : impl_(std::make_unique<Impl>()) {}

FfmpegH264VideoEncoder::~FfmpegH264VideoEncoder() {
    Close();
}

bool FfmpegH264VideoEncoder::Open(const VideoEncoderConfig& config, std::string* error) {
    Close();
    const AVPixelFormat frame_format = AvPixelFormatFor(config.input_format);
    if (frame_format == AV_PIX_FMT_NONE) {
        if (error) *error = "unsupported FFmpeg input pixel format";
        return false;
    }
    auto state = std::make_unique<Impl>();
    state->config = config;

    std::string attempts;
    for (const auto& encoder_name : H264EncoderCandidates(config)) {
        if (encoder_name.empty()) continue;
        const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name.c_str());
        if (!codec) {
            AppendAttempt(&attempts, encoder_name, "encoder is not available in FFmpeg");
            continue;
        }

        AVCodecContext* candidate = avcodec_alloc_context3(codec);
        if (!candidate) {
            AppendAttempt(&attempts, encoder_name, "failed to allocate codec context");
            continue;
        }

        ConfigureCommonH264Context(candidate, config);
        std::string option_error;
        if (!ConfigureEncoderOptions(candidate, encoder_name, config, &option_error)) {
            AppendAttempt(&attempts, encoder_name, option_error);
            avcodec_free_context(&candidate);
            continue;
        }

        const int rc = avcodec_open2(candidate, codec, nullptr);
        if (rc < 0) {
            AppendAttempt(&attempts, encoder_name, "failed to open: " + AvError(rc));
            avcodec_free_context(&candidate);
            continue;
        }

        state->codec = candidate;
        state->selected_encoder_name = encoder_name;
        state->output_annexb = encoder_name == "h264_nvenc";
        break;
    }

    if (!state->codec) {
        if (error) {
            *error = "failed to open any FFmpeg H.264 encoder";
            if (!attempts.empty()) *error += " (" + attempts + ")";
        }
        return false;
    }

    if (!attempts.empty()) {
        std::fprintf(stdout,
                     "[INFO]  ffmpeg_video_encoder: selected encoder=%s after fallback (%s)\r\n",
                     state->selected_encoder_name.c_str(),
                     attempts.c_str());
        std::fflush(stdout);
    }

    state->frame = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (!state->frame || !state->packet) {
        if (error) *error = "failed to allocate FFmpeg encoder state";
        impl_ = std::move(state);
        Close();
        return false;
    }

    state->frame->format = frame_format;
    state->frame->width = state->codec->width;
    state->frame->height = state->codec->height;
    state->frame->color_primaries = state->codec->color_primaries;
    state->frame->color_trc = state->codec->color_trc;
    state->frame->colorspace = state->codec->colorspace;
    state->frame->color_range = state->codec->color_range;
    const int rc = av_frame_get_buffer(state->frame, 32);
    if (rc < 0) {
        if (error) *error = "failed to allocate FFmpeg frame buffer: " + AvError(rc);
        impl_ = std::move(state);
        Close();
        return false;
    }

    impl_ = std::move(state);
    return true;
}

bool FfmpegH264VideoEncoder::ApplySlice(const VideoSlice& slice, std::string* error) {
    if (!impl_ || !impl_->frame) {
        if (error) *error = "FFmpeg encoder is not open";
        return false;
    }
    const int frame_w = impl_->codec ? impl_->codec->width : impl_->frame->width;
    const int frame_h = impl_->codec ? impl_->codec->height : impl_->frame->height;
    if (frame_w <= 0 || frame_h <= 0) {
        if (error) *error = "FFmpeg encoder dimensions are invalid";
        return false;
    }

    const int x = static_cast<int>(slice.x);
    const int y = static_cast<int>(slice.y);
    int w = static_cast<int>(slice.width);
    int h = static_cast<int>(slice.height);
    if (x < 0 || y < 0 || w <= 0 || h <= 0) {
        if (error) *error = "FFmpeg slice has invalid geometry";
        return false;
    }
    const bool yuv444 = impl_->config.input_format == PixelFormat::kYuv444p;
    // I420 chroma sampling requires even-aligned offsets and dimensions.
    if (!yuv444 && ((x & 1) || (y & 1))) {
        if (error) *error = "FFmpeg slice origin must be even-aligned";
        return false;
    }
    if (x + w > frame_w) w = frame_w - x;
    if (y + h > frame_h) h = frame_h - y;
    if (w <= 0 || h <= 0) return true;
    if (!slice.planes[0] || !slice.planes[1] || !slice.planes[2]) {
        if (error) *error = "FFmpeg slice has null plane pointer";
        return false;
    }

    const int rc = av_frame_make_writable(impl_->frame);
    if (rc < 0) {
        if (error) *error = "failed to make FFmpeg frame writable: " + AvError(rc);
        return false;
    }

    // Y plane (full resolution).
    for (int row = 0; row < h; ++row) {
        std::memcpy(impl_->frame->data[0] + (y + row) * impl_->frame->linesize[0] + x,
                    slice.planes[0] + row * slice.strides[0],
                    w);
    }
    // U / V planes: half resolution for 4:2:0, full resolution for 4:4:4.
    const int uv_w = yuv444 ? w : (w + 1) / 2;
    const int uv_h = yuv444 ? h : (h + 1) / 2;
    const int uv_x = yuv444 ? x : x / 2;
    const int uv_y = yuv444 ? y : y / 2;
    for (int row = 0; row < uv_h; ++row) {
        std::memcpy(impl_->frame->data[1] + (uv_y + row) * impl_->frame->linesize[1] + uv_x,
                    slice.planes[1] + row * slice.strides[1],
                    uv_w);
        std::memcpy(impl_->frame->data[2] + (uv_y + row) * impl_->frame->linesize[2] + uv_x,
                    slice.planes[2] + row * slice.strides[2],
                    uv_w);
    }

    // A slice covering (0,0,W,H) seeds the encoder's persistent input frame.
    if (x == 0 && y == 0 && w == frame_w && h == frame_h) {
        impl_->frame_initialized = true;
    }
    return true;
}

bool FfmpegH264VideoEncoder::EncodeFrame(int64_t pts_us, EncodedVideoFrame* output, std::string* error) {
    if (!impl_ || !impl_->codec || !impl_->frame || !impl_->packet) {
        if (error) *error = "FFmpeg encoder is not open";
        return false;
    }
    if (!impl_->frame_initialized) {
        if (error) *error = "FFmpeg encoder has not received a full-frame seed yet";
        return false;
    }

    // Release the previous packet (if any). The caller's span returned from
    // the prior EncodeFrame call is expected to have been consumed by now;
    // keeping the packet alive across calls saves a memcpy of the encoded
    // bitstream into a separate `encoded_` buffer.
    av_packet_unref(impl_->packet);

    impl_->frame->pts = impl_->next_pts++;
    impl_->frame->pict_type = impl_->force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    impl_->force_keyframe = false;

    int rc = avcodec_send_frame(impl_->codec, impl_->frame);
    if (rc < 0) {
        if (error) *error = "failed to send frame to FFmpeg encoder: " + AvError(rc);
        return false;
    }

    rc = avcodec_receive_packet(impl_->codec, impl_->packet);
    if (rc == AVERROR(EAGAIN)) return false;
    if (rc < 0) {
        if (error) *error = "failed to receive FFmpeg encoded packet: " + AvError(rc);
        return false;
    }
    if (impl_->packet->size <= 0 || !impl_->packet->data) {
        return false;
    }

    const bool keyframe = (impl_->packet->flags & AV_PKT_FLAG_KEY) != 0;
    if (output) {
        std::span<const uint8_t> payload(impl_->packet->data,
                                         static_cast<size_t>(impl_->packet->size));
        if (impl_->output_annexb) {
            if (!AnnexBToLengthPrefixed(payload, &impl_->encoded_)) {
                if (error) *error = "failed to convert FFmpeg Annex B packet to AVCC";
                return false;
            }
            payload = std::span<const uint8_t>(impl_->encoded_.data(), impl_->encoded_.size());
        }
        output->codec = VideoCodec::kH264;
        output->data = payload;
        output->keyframe = keyframe;
        output->pts_us = pts_us;
    }
    return true;
}

bool FfmpegH264VideoEncoder::HasFullSeed() const {
    return impl_ && impl_->frame_initialized;
}

std::string FfmpegH264VideoEncoder::SelectedEncoderName() const {
    return impl_ ? impl_->selected_encoder_name : std::string();
}

void FfmpegH264VideoEncoder::RequestKeyframe() {
    if (impl_) impl_->force_keyframe = true;
}

void FfmpegH264VideoEncoder::Close() {
    if (!impl_) return;
    if (impl_->packet) {
        av_packet_unref(impl_->packet);
        av_packet_free(&impl_->packet);
    }
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->codec) avcodec_free_context(&impl_->codec);
    impl_.reset();
}

}  // namespace tenbox::daemon
