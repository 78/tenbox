#pragma once

#include "common/ports.h"
#include "core/device/virtio/virtio_mmio.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// virtio-snd device ID (spec 5.14)
constexpr uint32_t VIRTIO_SND_DEVICE_ID = 25;

// Virtqueue indices
constexpr uint32_t VIRTIO_SND_VQ_CONTROL = 0;
constexpr uint32_t VIRTIO_SND_VQ_EVENT   = 1;
constexpr uint32_t VIRTIO_SND_VQ_TX      = 2;
constexpr uint32_t VIRTIO_SND_VQ_RX      = 3;
constexpr uint32_t VIRTIO_SND_VQ_MAX     = 4;

// Dataflow directions
constexpr uint8_t VIRTIO_SND_D_OUTPUT = 0;
constexpr uint8_t VIRTIO_SND_D_INPUT  = 1;

// Control request codes
constexpr uint32_t VIRTIO_SND_R_JACK_INFO      = 1;
constexpr uint32_t VIRTIO_SND_R_JACK_REMAP     = 2;
constexpr uint32_t VIRTIO_SND_R_PCM_INFO       = 0x0100;
constexpr uint32_t VIRTIO_SND_R_PCM_SET_PARAMS = 0x0101;
constexpr uint32_t VIRTIO_SND_R_PCM_PREPARE    = 0x0102;
constexpr uint32_t VIRTIO_SND_R_PCM_RELEASE    = 0x0103;
constexpr uint32_t VIRTIO_SND_R_PCM_START      = 0x0104;
constexpr uint32_t VIRTIO_SND_R_PCM_STOP       = 0x0105;
constexpr uint32_t VIRTIO_SND_R_CHMAP_INFO     = 0x0200;

// Status codes
constexpr uint32_t VIRTIO_SND_S_OK       = 0x8000;
constexpr uint32_t VIRTIO_SND_S_BAD_MSG  = 0x8001;
constexpr uint32_t VIRTIO_SND_S_NOT_SUPP = 0x8002;
constexpr uint32_t VIRTIO_SND_S_IO_ERR   = 0x8003;

// PCM events
constexpr uint32_t VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED = 0x1100;
constexpr uint32_t VIRTIO_SND_EVT_PCM_XRUN           = 0x1101;

// PCM formats
constexpr uint8_t VIRTIO_SND_PCM_FMT_S8    = 3;
constexpr uint8_t VIRTIO_SND_PCM_FMT_U8    = 4;
constexpr uint8_t VIRTIO_SND_PCM_FMT_S16   = 5;
constexpr uint8_t VIRTIO_SND_PCM_FMT_U16   = 6;
constexpr uint8_t VIRTIO_SND_PCM_FMT_S32   = 17;
constexpr uint8_t VIRTIO_SND_PCM_FMT_FLOAT = 19;

// PCM rates
constexpr uint8_t VIRTIO_SND_PCM_RATE_8000   = 1;
constexpr uint8_t VIRTIO_SND_PCM_RATE_11025  = 2;
constexpr uint8_t VIRTIO_SND_PCM_RATE_16000  = 3;
constexpr uint8_t VIRTIO_SND_PCM_RATE_22050  = 4;
constexpr uint8_t VIRTIO_SND_PCM_RATE_32000  = 5;
constexpr uint8_t VIRTIO_SND_PCM_RATE_44100  = 6;
constexpr uint8_t VIRTIO_SND_PCM_RATE_48000  = 7;
constexpr uint8_t VIRTIO_SND_PCM_RATE_96000  = 10;

// Channel map positions
constexpr uint8_t VIRTIO_SND_CHMAP_FL = 3;
constexpr uint8_t VIRTIO_SND_CHMAP_FR = 4;

constexpr uint32_t VIRTIO_SND_CHMAP_MAX_SIZE = 18;

#pragma pack(push, 1)
struct VirtioSndConfig {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
};

struct VirtioSndHdr {
    uint32_t code;
};

struct VirtioSndQueryInfo {
    VirtioSndHdr hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
};

struct VirtioSndInfo {
    uint32_t hda_fn_nid;
};

struct VirtioSndPcmInfo {
    VirtioSndInfo hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t  direction;
    uint8_t  channels_min;
    uint8_t  channels_max;
    uint8_t  padding[5];
};

struct VirtioSndPcmHdr {
    VirtioSndHdr hdr;
    uint32_t stream_id;
};

struct VirtioSndPcmSetParams {
    VirtioSndPcmHdr hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t  channels;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  padding;
};

struct VirtioSndPcmXfer {
    uint32_t stream_id;
};

struct VirtioSndPcmStatus {
    uint32_t status;
    uint32_t latency_bytes;
};

struct VirtioSndChmapInfo {
    VirtioSndInfo hdr;
    uint8_t  direction;
    uint8_t  channels;
    uint8_t  positions[VIRTIO_SND_CHMAP_MAX_SIZE];
};

struct VirtioSndEvent {
    VirtioSndHdr hdr;
    uint32_t data;
};
#pragma pack(pop)

class VirtioSndDevice : public VirtioDeviceOps {
public:
    VirtioSndDevice();
    ~VirtioSndDevice() override;

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }
    void SetMemMap(const GuestMemMap& mem) { mem_ = mem; }
    void SetAudioPort(std::shared_ptr<AudioPort> port) { audio_port_ = std::move(port); }

    uint32_t GetDeviceId() const override { return VIRTIO_SND_DEVICE_ID; }
    uint64_t GetDeviceFeatures() const override;
    uint32_t GetNumQueues() const override { return VIRTIO_SND_VQ_MAX; }
    uint32_t GetQueueMaxSize(uint32_t queue_idx) const override { return 256; }
    void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) override;
    void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) override;
    void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnStatusChange(uint32_t new_status) override;

private:
    void ProcessControlQueue(VirtQueue& vq);
    void ProcessEventQueue(VirtQueue& vq);
    void ProcessTxQueue(VirtQueue& vq);

    void HandlePcmInfo(const VirtioSndQueryInfo* query,
                       uint8_t* resp, uint32_t* resp_len);
    void HandlePcmSetParams(const VirtioSndPcmSetParams* params,
                            uint8_t* resp, uint32_t* resp_len);
    void HandlePcmStreamCmd(uint32_t code, uint32_t stream_id,
                            uint8_t* resp, uint32_t* resp_len);
    void HandleChmapInfo(const VirtioSndQueryInfo* query,
                         uint8_t* resp, uint32_t* resp_len);

    void PeriodTimerThread();
    void StartPeriodTimer();
    void StopPeriodTimer();
    void FlushPendingTxBuffers();

    static uint32_t RateEnumToHz(uint8_t rate_enum);

    VirtioMmioDevice* mmio_ = nullptr;
    GuestMemMap mem_{};
    std::shared_ptr<AudioPort> audio_port_;
    VirtioSndConfig snd_config_{};

    // Event queue: guest pre-posts writable buffers; we fill them with events.
    std::vector<uint16_t> event_buf_heads_;

    // PCM stream state
    enum class StreamState { kIdle, kPrepared, kRunning };
    StreamState stream_state_ = StreamState::kIdle;
    uint32_t pcm_sample_rate_ = 48000;
    uint8_t  pcm_channels_ = 2;
    uint8_t  pcm_format_ = VIRTIO_SND_PCM_FMT_S16;
    uint32_t pcm_buffer_bytes_ = 0;
    uint32_t pcm_period_bytes_ = 0;

    // Period timer: releases TX buffers at real audio rate to throttle guest
    std::thread period_thread_;
    std::mutex period_mutex_;
    std::condition_variable period_cv_;
    std::atomic<bool> period_running_{false};

    // Pending TX buffers waiting to be returned to guest
    struct PendingTxBuffer {
        uint16_t head;
        uint32_t status_len;
        std::vector<int16_t> pcm_data;  // Audio data to send when releasing
    };
    std::deque<PendingTxBuffer> pending_tx_buffers_;
    std::mutex tx_mutex_;
};
