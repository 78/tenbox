#include "daemon/media_interfaces.h"

#include <opus/opus.h>

namespace tenbox::daemon {

struct OpusAudioEncoder::Impl {
    OpusEncoder* encoder = nullptr;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
};

OpusAudioEncoder::OpusAudioEncoder() : impl_(std::make_unique<Impl>()) {}

OpusAudioEncoder::~OpusAudioEncoder() {
    Close();
}

bool OpusAudioEncoder::Open(uint32_t sample_rate, uint32_t channels, std::string* error) {
    Close();
    if (sample_rate != 48000 || channels == 0 || channels > 2) {
        if (error) *error = "only 48kHz mono/stereo PCM is supported for Opus encoding";
        return false;
    }

    int opus_error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(
        static_cast<opus_int32>(sample_rate),
        static_cast<int>(channels),
        OPUS_APPLICATION_RESTRICTED_LOWDELAY,
        &opus_error);
    if (!encoder || opus_error != OPUS_OK) {
        if (error) *error = opus_strerror(opus_error);
        if (encoder) opus_encoder_destroy(encoder);
        return false;
    }

    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(encoder, OPUS_SET_DTX(1));

    impl_ = std::make_unique<Impl>();
    impl_->encoder = encoder;
    impl_->sample_rate = sample_rate;
    impl_->channels = channels;
    encoded_.resize(4096);
    return true;
}

bool OpusAudioEncoder::Encode(const AudioChunk& chunk, EncodedAudioFrame* output, std::string* error) {
    if (!impl_ || !impl_->encoder) {
        if (error) *error = "Opus encoder is not open";
        return false;
    }
    if (chunk.sample_rate != impl_->sample_rate || chunk.channels != impl_->channels || chunk.channels == 0) {
        if (error) *error = "Opus encoder audio format changed";
        return false;
    }
    const int frame_size = static_cast<int>(chunk.samples.size() / chunk.channels);
    if (frame_size <= 0) return false;

    const int bytes = opus_encode(
        impl_->encoder,
        reinterpret_cast<const opus_int16*>(chunk.samples.data()),
        frame_size,
        encoded_.data(),
        static_cast<opus_int32>(encoded_.size()));
    if (bytes < 0) {
        if (error) *error = opus_strerror(bytes);
        return false;
    }

    if (output) {
        output->codec = "opus";
        output->data = std::span<const uint8_t>(encoded_.data(), static_cast<size_t>(bytes));
        output->pts_us = chunk.pts_us;
    }
    return bytes > 0;
}

void OpusAudioEncoder::Close() {
    if (impl_ && impl_->encoder) {
        opus_encoder_destroy(impl_->encoder);
        impl_->encoder = nullptr;
    }
    impl_.reset();
    encoded_.clear();
}

}  // namespace tenbox::daemon
