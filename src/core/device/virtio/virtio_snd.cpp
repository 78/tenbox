#include "core/device/virtio/virtio_snd.h"
#include "core/vmm/types.h"
#include "core/vmm/vm_io_loop.h"
#include <algorithm>
#include <chrono>
#include <cstring>


#ifndef VIRTIO_F_VERSION_1_DEFINED
#define VIRTIO_F_VERSION_1_DEFINED
constexpr uint64_t VIRTIO_SND_VER1 = 1ULL << 32;
#else
static constexpr uint64_t VIRTIO_SND_VER1 = 1ULL << 32;
#endif

VirtioSndDevice::VirtioSndDevice() {
    snd_config_.jacks = 0;
    snd_config_.streams = 1;
    snd_config_.chmaps = 1;
}

VirtioSndDevice::~VirtioSndDevice() {
    StopPeriodTimer();
}

uint64_t VirtioSndDevice::GetDeviceFeatures() const {
    return VIRTIO_SND_VER1;
}

void VirtioSndDevice::ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) {
    const auto* cfg = reinterpret_cast<const uint8_t*>(&snd_config_);
    if (offset + size > sizeof(snd_config_)) {
        *value = 0;
        return;
    }
    *value = 0;
    std::memcpy(value, cfg + offset, size);
}

void VirtioSndDevice::WriteConfig(uint32_t offset, uint8_t size, uint32_t value) {
    // Config space is read-only for virtio-snd
}

void VirtioSndDevice::OnStatusChange(uint32_t new_status) {
    if (new_status == 0) {
        StopPeriodTimer();
        stream_state_ = StreamState::kIdle;
        pcm_sample_rate_ = 48000;
        pcm_channels_ = 2;
        pcm_format_ = VIRTIO_SND_PCM_FMT_S16;
        event_buf_heads_.clear();
        {
            std::lock_guard<std::mutex> lock(tx_mutex_);
            pending_tx_buffers_.clear();
        }
    }
}

void VirtioSndDevice::OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) {
    switch (queue_idx) {
    case VIRTIO_SND_VQ_CONTROL:
        ProcessControlQueue(vq);
        break;
    case VIRTIO_SND_VQ_EVENT:
        ProcessEventQueue(vq);
        break;
    case VIRTIO_SND_VQ_TX:
        ProcessTxQueue(vq);
        break;
    case VIRTIO_SND_VQ_RX:
        break;
    }
}

void VirtioSndDevice::ProcessControlQueue(VirtQueue& vq) {
    uint16_t head;
    while (vq.PopAvail(&head)) {
        std::vector<VirtqChainElem> chain;
        if (!vq.WalkChain(head, &chain)) {
            vq.PushUsed(head, 0);
            continue;
        }

        std::vector<uint8_t> req_buf;
        std::vector<VirtqChainElem> resp_elems;
        for (auto& elem : chain) {
            if (!elem.writable) {
                req_buf.insert(req_buf.end(), elem.addr, elem.addr + elem.len);
            } else {
                resp_elems.push_back(elem);
            }
        }

        if (req_buf.size() < sizeof(VirtioSndHdr)) {
            vq.PushUsed(head, 0);
            continue;
        }

        std::vector<uint8_t> resp(4096, 0);
        uint32_t resp_len = 0;

        auto* hdr = reinterpret_cast<const VirtioSndHdr*>(req_buf.data());

        switch (hdr->code) {
        case VIRTIO_SND_R_JACK_INFO: {
            // No jacks
            auto* status = reinterpret_cast<VirtioSndHdr*>(resp.data());
            status->code = VIRTIO_SND_S_OK;
            resp_len = sizeof(VirtioSndHdr);
            break;
        }
        case VIRTIO_SND_R_PCM_INFO: {
            if (req_buf.size() >= sizeof(VirtioSndQueryInfo)) {
                auto* query = reinterpret_cast<const VirtioSndQueryInfo*>(req_buf.data());
                HandlePcmInfo(query, resp.data(), &resp_len);
            } else {
                auto* status = reinterpret_cast<VirtioSndHdr*>(resp.data());
                status->code = VIRTIO_SND_S_BAD_MSG;
                resp_len = sizeof(VirtioSndHdr);
            }
            break;
        }
        case VIRTIO_SND_R_PCM_SET_PARAMS: {
            if (req_buf.size() >= sizeof(VirtioSndPcmSetParams)) {
                auto* params = reinterpret_cast<const VirtioSndPcmSetParams*>(req_buf.data());
                HandlePcmSetParams(params, resp.data(), &resp_len);
            } else {
                auto* status = reinterpret_cast<VirtioSndHdr*>(resp.data());
                status->code = VIRTIO_SND_S_BAD_MSG;
                resp_len = sizeof(VirtioSndHdr);
            }
            break;
        }
        case VIRTIO_SND_R_PCM_PREPARE:
        case VIRTIO_SND_R_PCM_START:
        case VIRTIO_SND_R_PCM_STOP:
        case VIRTIO_SND_R_PCM_RELEASE: {
            uint32_t stream_id = 0;
            if (req_buf.size() >= sizeof(VirtioSndPcmHdr)) {
                auto* pcm_hdr = reinterpret_cast<const VirtioSndPcmHdr*>(req_buf.data());
                stream_id = pcm_hdr->stream_id;
            }
            HandlePcmStreamCmd(hdr->code, stream_id, resp.data(), &resp_len);
            break;
        }
        case VIRTIO_SND_R_CHMAP_INFO: {
            if (req_buf.size() >= sizeof(VirtioSndQueryInfo)) {
                auto* query = reinterpret_cast<const VirtioSndQueryInfo*>(req_buf.data());
                HandleChmapInfo(query, resp.data(), &resp_len);
            } else {
                auto* status = reinterpret_cast<VirtioSndHdr*>(resp.data());
                status->code = VIRTIO_SND_S_BAD_MSG;
                resp_len = sizeof(VirtioSndHdr);
            }
            break;
        }
        default: {
            auto* status = reinterpret_cast<VirtioSndHdr*>(resp.data());
            status->code = VIRTIO_SND_S_NOT_SUPP;
            resp_len = sizeof(VirtioSndHdr);
            break;
        }
        }

        uint32_t written = 0;
        for (auto& elem : resp_elems) {
            if (written >= resp_len) break;
            uint32_t to_copy = (std::min)(elem.len, resp_len - written);
            std::memcpy(elem.addr, resp.data() + written, to_copy);
            written += to_copy;
        }

        vq.PushUsed(head, written);
    }

    if (mmio_) mmio_->NotifyUsedBuffer(VIRTIO_SND_VQ_CONTROL);
}

void VirtioSndDevice::ProcessEventQueue(VirtQueue& vq) {
    // Guest pre-posts writable buffers for us to fill with events.
    // We collect them but currently don't use them - period elapsed is signaled
    // by returning TX buffers (see PeriodTimerThread), not via event queue.
    uint16_t head;
    while (vq.PopAvail(&head)) {
        event_buf_heads_.push_back(head);
    }
}

void VirtioSndDevice::ProcessTxQueue(VirtQueue& vq) {
    uint16_t head;
    while (vq.PopAvail(&head)) {
        std::vector<VirtqChainElem> chain;
        if (!vq.WalkChain(head, &chain)) {
            vq.PushUsed(head, 0);
            continue;
        }

        VirtqChainElem* status_elem = nullptr;

        // Measure total readable bytes and find the status element
        size_t total_readable = 0;
        for (auto& elem : chain) {
            if (!elem.writable) {
                total_readable += elem.len;
            } else {
                if (!status_elem) status_elem = &elem;
            }
        }

        VirtioSndPcmStatus status{};
        status.status = VIRTIO_SND_S_OK;
        status.latency_bytes = 0;

        if (status_elem && status_elem->len >= sizeof(VirtioSndPcmStatus)) {
            std::memcpy(status_elem->addr, &status, sizeof(status));
        }

        PendingTxBuffer pending{};
        pending.head = head;
        pending.status_len = sizeof(VirtioSndPcmStatus);

        // Copy PCM data directly from descriptor chain, skipping the xfer header
        if (total_readable > sizeof(VirtioSndPcmXfer) &&
            pcm_format_ == VIRTIO_SND_PCM_FMT_S16) {
            size_t pcm_bytes = total_readable - sizeof(VirtioSndPcmXfer);
            pending.pcm_data.resize(pcm_bytes / sizeof(int16_t));
            auto* dst = reinterpret_cast<uint8_t*>(pending.pcm_data.data());
            size_t skip = sizeof(VirtioSndPcmXfer);
            for (auto& elem : chain) {
                if (elem.writable) continue;
                if (skip >= elem.len) {
                    skip -= elem.len;
                    continue;
                }
                size_t usable = elem.len - skip;
                std::memcpy(dst, elem.addr + skip, usable);
                dst += usable;
                skip = 0;
            }
        }

        {
            std::lock_guard<std::mutex> lock(tx_mutex_);
            pending_tx_buffers_.push_back(std::move(pending));
        }
    }
}

void VirtioSndDevice::HandlePcmInfo(const VirtioSndQueryInfo* query,
                                     uint8_t* resp, uint32_t* resp_len) {
    auto* status = reinterpret_cast<VirtioSndHdr*>(resp);
    status->code = VIRTIO_SND_S_OK;

    if (query->start_id >= snd_config_.streams || query->count == 0) {
        *resp_len = sizeof(VirtioSndHdr);
        return;
    }

    uint32_t count = (std::min)(query->count,
                                snd_config_.streams - query->start_id);
    uint8_t* info_ptr = resp + sizeof(VirtioSndHdr);

    for (uint32_t i = 0; i < count; ++i) {
        auto* info = reinterpret_cast<VirtioSndPcmInfo*>(info_ptr);
        std::memset(info, 0, sizeof(*info));
        info->hdr.hda_fn_nid = 0;
        info->features = 0;
        // Only S16 format - matches typical audio processing
        info->formats = (1ULL << VIRTIO_SND_PCM_FMT_S16);
        // Only 48000 Hz - matches typical WASAPI mix format, avoids resampling
        info->rates = (1ULL << VIRTIO_SND_PCM_RATE_48000);
        info->direction = VIRTIO_SND_D_OUTPUT;
        info->channels_min = 2;
        info->channels_max = 2;
        info_ptr += sizeof(VirtioSndPcmInfo);
    }

    *resp_len = sizeof(VirtioSndHdr) + count * sizeof(VirtioSndPcmInfo);
}

void VirtioSndDevice::HandlePcmSetParams(const VirtioSndPcmSetParams* params,
                                           uint8_t* resp, uint32_t* resp_len) {
    auto* status = reinterpret_cast<VirtioSndHdr*>(resp);

    if (params->hdr.stream_id >= snd_config_.streams) {
        status->code = VIRTIO_SND_S_BAD_MSG;
        *resp_len = sizeof(VirtioSndHdr);
        return;
    }

    pcm_channels_ = params->channels;
    pcm_format_ = params->format;
    pcm_buffer_bytes_ = params->buffer_bytes;
    pcm_period_bytes_ = params->period_bytes;
    pcm_sample_rate_ = RateEnumToHz(params->rate);

    LOG_INFO("virtio-snd SET_PARAMS: rate=%uHz ch=%u fmt=%u buf=%u period=%u",
             pcm_sample_rate_, pcm_channels_, pcm_format_,
             pcm_buffer_bytes_, pcm_period_bytes_);

    status->code = VIRTIO_SND_S_OK;
    *resp_len = sizeof(VirtioSndHdr);
}

void VirtioSndDevice::HandlePcmStreamCmd(uint32_t code, uint32_t stream_id,
                                           uint8_t* resp, uint32_t* resp_len) {
    auto* status = reinterpret_cast<VirtioSndHdr*>(resp);

    if (stream_id >= snd_config_.streams) {
        status->code = VIRTIO_SND_S_BAD_MSG;
        *resp_len = sizeof(VirtioSndHdr);
        return;
    }

    switch (code) {
    case VIRTIO_SND_R_PCM_PREPARE:
        StopPeriodTimer();
        stream_state_ = StreamState::kPrepared;
        break;
    case VIRTIO_SND_R_PCM_START:
        stream_state_ = StreamState::kRunning;
        StartPeriodTimer();
        break;
    case VIRTIO_SND_R_PCM_STOP:
        StopPeriodTimer();
        FlushPendingTxBuffers();
        stream_state_ = StreamState::kPrepared;
        break;
    case VIRTIO_SND_R_PCM_RELEASE:
        StopPeriodTimer();
        FlushPendingTxBuffers();
        stream_state_ = StreamState::kIdle;
        break;
    }

    status->code = VIRTIO_SND_S_OK;
    *resp_len = sizeof(VirtioSndHdr);
}

void VirtioSndDevice::HandleChmapInfo(const VirtioSndQueryInfo* query,
                                       uint8_t* resp, uint32_t* resp_len) {
    auto* status = reinterpret_cast<VirtioSndHdr*>(resp);
    status->code = VIRTIO_SND_S_OK;

    if (query->start_id >= snd_config_.chmaps || query->count == 0) {
        *resp_len = sizeof(VirtioSndHdr);
        return;
    }

    uint32_t count = (std::min)(query->count,
                                snd_config_.chmaps - query->start_id);
    uint8_t* info_ptr = resp + sizeof(VirtioSndHdr);

    for (uint32_t i = 0; i < count; ++i) {
        auto* info = reinterpret_cast<VirtioSndChmapInfo*>(info_ptr);
        std::memset(info, 0, sizeof(*info));
        info->hdr.hda_fn_nid = 0;
        info->direction = VIRTIO_SND_D_OUTPUT;
        info->channels = 2;
        info->positions[0] = VIRTIO_SND_CHMAP_FL;
        info->positions[1] = VIRTIO_SND_CHMAP_FR;
        info_ptr += sizeof(VirtioSndChmapInfo);
    }

    *resp_len = sizeof(VirtioSndHdr) + count * sizeof(VirtioSndChmapInfo);
}

void VirtioSndDevice::StartPeriodTimer() {
    StopPeriodTimer();
    if (!io_loop_) return;  // no loop => no pacing (dev effectively silent)
    period_start_time_ = std::chrono::steady_clock::now();
    period_bytes_processed_ = 0;
    period_running_.store(true);
    period_timer_id_ = io_loop_->AddTimer(0, [this]() -> uint64_t {
        if (!period_running_.load()) return 0;  // self-destruct
        uint64_t next_ms = PeriodTick();
        if (!period_running_.load()) return 0;
        return next_ms ? next_ms : 1;  // never return 0 while running
    });
}

void VirtioSndDevice::FlushPendingTxBuffers() {
    std::deque<PendingTxBuffer> buffers;
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        buffers = std::move(pending_tx_buffers_);
        pending_tx_buffers_.clear();
    }
    
    if (buffers.empty()) return;
    
    if (mmio_) {
        auto* txq = mmio_->GetQueue(VIRTIO_SND_VQ_TX);
        if (txq) {
            for (auto& buf : buffers) {
                txq->PushUsed(buf.head, buf.status_len);
            }
            mmio_->NotifyUsedBuffer(VIRTIO_SND_VQ_TX);
        }
    }
}

void VirtioSndDevice::StopPeriodTimer() {
    if (!period_running_.exchange(false)) return;
    if (io_loop_ && period_timer_id_) {
        io_loop_->RemoveTimer(period_timer_id_);
        period_timer_id_ = 0;
    }
}

uint64_t VirtioSndDevice::PeriodTick() {
    // Get current stream parameters
    uint32_t sample_rate, period_bytes;
    uint8_t channels;
    {
        std::lock_guard<std::mutex> lock(period_mutex_);
        sample_rate = pcm_sample_rate_;
        period_bytes = pcm_period_bytes_;
        channels = pcm_channels_;
    }

    if (sample_rate == 0 || period_bytes == 0 || channels == 0) {
        return 10;  // stream not yet set up; retry later
    }

    uint32_t bytes_per_second = sample_rate * channels * 2;  // S16

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - period_start_time_).count();
    int64_t audio_ms = static_cast<int64_t>(period_bytes_processed_) * 1000 /
                       bytes_per_second;
    int64_t drift_ms = audio_ms - elapsed_ms;  // +ahead / -behind

    if (drift_ms > 0) {
        // Ahead of real time — wait until we need more samples.
        return static_cast<uint64_t>((std::min)(drift_ms, (int64_t)10));
    }

    if (drift_ms < -200) {
        // Way behind (suspend/resume?); resync the clock instead of
        // burning through every queued buffer.
        period_start_time_ = std::chrono::steady_clock::now();
        period_bytes_processed_ = 0;
        return 1;
    }

    PendingTxBuffer buf{};
    bool have_buf = false;
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        if (!pending_tx_buffers_.empty()) {
            buf = std::move(pending_tx_buffers_.front());
            pending_tx_buffers_.pop_front();
            have_buf = true;
        }
    }

    if (!have_buf) {
        return 1;  // spin gently until the guest queues more data
    }

    size_t pcm_bytes = 0;
    if (!buf.pcm_data.empty() && audio_port_) {
        AudioChunk chunk;
        chunk.sample_rate = sample_rate;
        chunk.channels = channels;
        pcm_bytes = buf.pcm_data.size() * sizeof(int16_t);
        chunk.pcm = std::move(buf.pcm_data);
        audio_port_->SubmitPcm(std::move(chunk));
    }

    period_bytes_processed_ += (pcm_bytes > 0) ? pcm_bytes : period_bytes;

    if (mmio_) {
        auto* txq = mmio_->GetQueue(VIRTIO_SND_VQ_TX);
        if (txq) {
            txq->PushUsed(buf.head, buf.status_len);
            mmio_->NotifyUsedBuffer(VIRTIO_SND_VQ_TX);
        }
    }
    return 1;  // immediately try the next buffer; drift calc paces us
}

uint32_t VirtioSndDevice::RateEnumToHz(uint8_t rate_enum) {
    switch (rate_enum) {
    case VIRTIO_SND_PCM_RATE_8000:   return 8000;
    case VIRTIO_SND_PCM_RATE_11025:  return 11025;
    case VIRTIO_SND_PCM_RATE_16000:  return 16000;
    case VIRTIO_SND_PCM_RATE_22050:  return 22050;
    case VIRTIO_SND_PCM_RATE_32000:  return 32000;
    case VIRTIO_SND_PCM_RATE_44100:  return 44100;
    case VIRTIO_SND_PCM_RATE_48000:  return 48000;
    case VIRTIO_SND_PCM_RATE_96000:  return 96000;
    default:                          return 48000;
    }
}
