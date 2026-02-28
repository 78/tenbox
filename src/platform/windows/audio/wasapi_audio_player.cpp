#include "platform/windows/audio/wasapi_audio_player.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstring>

static const CLSID kCLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID kIID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
static const IID kIID_IAudioClient = __uuidof(IAudioClient);
static const IID kIID_IAudioRenderClient = __uuidof(IAudioRenderClient);

WasapiAudioPlayer::WasapiAudioPlayer() = default;

WasapiAudioPlayer::~WasapiAudioPlayer() {
    Stop();
}

void WasapiAudioPlayer::Stop() {
    if (running_) {
        running_ = false;
        cv_.notify_all();
        if (render_thread_.joinable()) {
            render_thread_.join();
        }
    }
    ReleaseDevice();
}

void WasapiAudioPlayer::SubmitPcm(uint32_t sample_rate, uint16_t channels,
                                    const int16_t* data, size_t sample_count) {
    if (!data || sample_count == 0 || channels == 0 || sample_rate == 0)
        return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < sample_count; ++i) {
            pcm_buffer_.push_back(data[i]);
        }
        while (pcm_buffer_.size() > kMaxBufferedSamples) {
            pcm_buffer_.pop_front();
        }
    }
    cv_.notify_one();

    if (!running_) {
        running_ = true;
        render_thread_ = std::thread(&WasapiAudioPlayer::RenderThread, this);
    }
}

void WasapiAudioPlayer::ReleaseDevice() {
    if (audio_client_) {
        audio_client_->Stop();
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (render_client_) {
        render_client_->Release();
        render_client_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
    buffer_frames_ = 0;
    device_channels_ = 0;
    device_bits_ = 0;
    device_is_float_ = false;
}

bool WasapiAudioPlayer::EnsureInitialized() {
    if (audio_client_ && render_client_)
        return true;

    ReleaseDevice();

    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr,
                                  CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                  reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) return false;

    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) return false;

    hr = device_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr)) return false;

    // In shared mode we must use the system mix format
    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr)) { ReleaseDevice(); return false; }

    device_channels_ = mix_format->nChannels;
    device_bits_ = mix_format->wBitsPerSample;
    if (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        device_is_float_ = true;
    } else if (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format);
        device_is_float_ = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    REFERENCE_TIME buffer_duration = 2000000; // 200ms
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    0, buffer_duration, 0, mix_format, nullptr);
    CoTaskMemFree(mix_format);

    if (FAILED(hr)) { ReleaseDevice(); return false; }

    hr = audio_client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) { ReleaseDevice(); return false; }

    hr = audio_client_->GetService(kIID_IAudioRenderClient,
                                    reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr)) { ReleaseDevice(); return false; }

    hr = audio_client_->Start();
    if (FAILED(hr)) { ReleaseDevice(); return false; }

    return true;
}

void WasapiAudioPlayer::RenderThread() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool need_uninit = SUCCEEDED(hr);

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                return !running_ || !pcm_buffer_.empty();
            });
            if (!running_) break;
            if (pcm_buffer_.empty()) continue;
        }

        if (!EnsureInitialized()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        UINT32 padding = 0;
        hr = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) { ReleaseDevice(); continue; }

        UINT32 available_frames = buffer_frames_ - padding;
        if (available_frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        // Input is always stereo; each frame = 2 samples
        UINT32 src_frames = static_cast<UINT32>(pcm_buffer_.size() / 2);
        UINT32 out_frames = (std::min)(available_frames, src_frames);
        if (out_frames == 0) continue;

        BYTE* buffer_data = nullptr;
        hr = render_client_->GetBuffer(out_frames, &buffer_data);
        if (FAILED(hr)) continue;

        // Input is always S16 stereo (2ch); read L/R per frame
        for (UINT32 f = 0; f < out_frames; ++f) {
            int16_t left  = pcm_buffer_.front(); pcm_buffer_.pop_front();
            int16_t right = pcm_buffer_.front(); pcm_buffer_.pop_front();
            const int16_t src[2] = { left, right };

            if (device_is_float_ && device_bits_ == 32) {
                auto* out = reinterpret_cast<float*>(buffer_data);
                for (uint16_t c = 0; c < device_channels_; ++c) {
                    out[f * device_channels_ + c] = src[c < 2 ? c : 1] / 32768.0f;
                }
            } else {
                auto* out = reinterpret_cast<int16_t*>(buffer_data);
                for (uint16_t c = 0; c < device_channels_; ++c) {
                    out[f * device_channels_ + c] = src[c < 2 ? c : 1];
                }
            }
        }

        render_client_->ReleaseBuffer(out_frames, 0);
    }

    ReleaseDevice();
    if (need_uninit) CoUninitialize();
}
