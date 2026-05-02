#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tenbox::daemon {

enum class PixelFormat {
    kRgba,
    kBgra,
    kYuv420p,
    kYuv444p,
};

enum class VideoCodec {
    kH264,
    kVp8,
    kVp9,
    kAv1,
};

// Subset of H.264 profiles we negotiate over WebRTC. `kConstrainedBaseline`
// is the universal fallback (matches profile-level-id=42e0xx); `kMain`
// (profile-level-id=4Dxxxx) and `kHigh` (profile-level-id=64xxxx) are selected
// only when the browser actually advertises them in the offer.
enum class H264Profile {
    kConstrainedBaseline,
    kMain,
    kHigh,
};

// Read-only view of a planar YUV patch to be stamped onto the encoder's persistent
// input frame. Used by the slice-mode pipeline (see ApplySlice / EncodeFrame).
struct VideoSlice {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    const uint8_t* planes[3] = {};
    int strides[3] = {};
};

struct EncodedVideoFrame {
    VideoCodec codec = VideoCodec::kH264;
    std::span<const uint8_t> data;
    bool keyframe = false;
    int64_t pts_us = 0;
};

struct VideoEncoderConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitrate_bps = 4'000'000;
    uint32_t framerate = 60;
    PixelFormat input_format = PixelFormat::kYuv420p;
    VideoCodec codec = VideoCodec::kH264;
    H264Profile h264_profile = H264Profile::kConstrainedBaseline;
    std::vector<std::string> h264_encoder_candidates = {"h264_nvenc", "libx264"};
};

struct AudioChunk {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    std::span<const int16_t> samples;
    int64_t pts_us = 0;
};

struct EncodedAudioFrame {
    std::string codec = "opus";
    std::span<const uint8_t> data;
    int64_t pts_us = 0;
};

class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    virtual bool Open(uint32_t sample_rate, uint32_t channels, std::string* error) = 0;
    virtual bool Encode(const AudioChunk& chunk, EncodedAudioFrame* output, std::string* error) = 0;
    virtual void Close() = 0;
};

// Slice-mode H.264 encoder backed by FFmpeg's libavcodec encoders.
//
// Pipeline (mirrors x264's persistent input picture):
//   ApplySlice(slice)       -- stamp a planar YUV patch onto the persistent input.
//   EncodeFrame(pts, &out)  -- encode whatever has been accumulated.
// The caller is responsible for driving these in pairs: apply N slices that
// together cover at least the first full frame (HasFullSeed() turns true once
// a (0,0,W,H) slice has been applied), then call EncodeFrame.
class FfmpegH264VideoEncoder {
public:
    FfmpegH264VideoEncoder();
    ~FfmpegH264VideoEncoder();

    bool Open(const VideoEncoderConfig& config, std::string* error);
    bool ApplySlice(const VideoSlice& slice, std::string* error);
    bool EncodeFrame(int64_t pts_us, EncodedVideoFrame* output, std::string* error);
    bool HasFullSeed() const;
    std::string SelectedEncoderName() const;
    void RequestKeyframe();
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class OpusAudioEncoder final : public AudioEncoder {
public:
    OpusAudioEncoder();
    ~OpusAudioEncoder() override;

    bool Open(uint32_t sample_rate, uint32_t channels, std::string* error) override;
    bool Encode(const AudioChunk& chunk, EncodedAudioFrame* output, std::string* error) override;
    void Close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<uint8_t> encoded_;
};

}  // namespace tenbox::daemon
