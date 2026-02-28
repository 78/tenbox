#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;

class WasapiAudioPlayer {
public:
    WasapiAudioPlayer();
    ~WasapiAudioPlayer();

    WasapiAudioPlayer(const WasapiAudioPlayer&) = delete;
    WasapiAudioPlayer& operator=(const WasapiAudioPlayer&) = delete;

    // Input: S16 stereo 48000Hz (fixed format matching virtio-snd config)
    void SubmitPcm(uint32_t sample_rate, uint16_t channels,
                   const int16_t* data, size_t sample_count);

    void Stop();

private:
    bool EnsureInitialized();
    void ReleaseDevice();
    void RenderThread();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::thread render_thread_;

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    uint32_t buffer_frames_ = 0;

    // WASAPI device mix format (typically float32 48000Hz stereo)
    uint16_t device_channels_ = 0;
    uint16_t device_bits_ = 0;
    bool device_is_float_ = false;

    std::deque<int16_t> pcm_buffer_;
    static constexpr size_t kMaxBufferedSamples = 48000 * 2 * 2; // 2 seconds
};
