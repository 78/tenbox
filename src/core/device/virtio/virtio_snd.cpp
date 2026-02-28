#include "core/device/virtio/virtio_snd.h"
#include "core/vmm/types.h"
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

    if (mmio_) mmio_->NotifyUsedBuffer();
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

        std::vector<uint8_t> read_buf;
        VirtqChainElem* status_elem = nullptr;

        for (auto& elem : chain) {
            if (!elem.writable) {
                read_buf.insert(read_buf.end(), elem.addr, elem.addr + elem.len);
            } else {
                if (!status_elem) status_elem = &elem;
            }
        }

        VirtioSndPcmStatus status{};
        status.status = VIRTIO_SND_S_OK;
        status.latency_bytes = 0;

        // Write status to the buffer
        if (status_elem && status_elem->len >= sizeof(VirtioSndPcmStatus)) {
            std::memcpy(status_elem->addr, &status, sizeof(status));
        }

        // Extract PCM data and queue for later processing
        PendingTxBuffer pending{};
        pending.head = head;
        pending.status_len = sizeof(VirtioSndPcmStatus);

        if (read_buf.size() > sizeof(VirtioSndPcmXfer) &&
            pcm_format_ == VIRTIO_SND_PCM_FMT_S16) {
            const uint8_t* pcm_data = read_buf.data() + sizeof(VirtioSndPcmXfer);
            size_t pcm_size = read_buf.size() - sizeof(VirtioSndPcmXfer);
            size_t sample_count = pcm_size / sizeof(int16_t);
            pending.pcm_data.resize(sample_count);
            std::memcpy(pending.pcm_data.data(), pcm_data, pcm_size);
        }

        // Queue the buffer for later completion (timer will release at audio rate)
        {
            std::lock_guard<std::mutex> lock(tx_mutex_);
            pending_tx_buffers_.push_back(std::move(pending));
        }
    }
    // Note: We do NOT call mmio_->NotifyUsedBuffer() here.
    // The timer thread will do it when releasing buffers.
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
    period_running_ = true;
    period_thread_ = std::thread(&VirtioSndDevice::PeriodTimerThread, this);
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
            mmio_->NotifyUsedBuffer();
        }
    }
}

void VirtioSndDevice::StopPeriodTimer() {
    if (period_running_) {
        period_running_ = false;
        period_cv_.notify_all();
        if (period_thread_.joinable()) {
            period_thread_.join();
        }
    }
}

void VirtioSndDevice::PeriodTimerThread() {
    auto start_time = std::chrono::steady_clock::now();
    uint64_t bytes_processed = 0;  // Track audio position in bytes

    while (period_running_) {
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
            std::unique_lock<std::mutex> lock(period_mutex_);
            period_cv_.wait_for(lock, std::chrono::milliseconds(10),
                                [this]() { return !period_running_.load(); });
            continue;
        }

        uint32_t bytes_per_second = sample_rate * channels * 2; // S16

        // Calculate timing: how far ahead/behind are we?
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        int64_t audio_ms = static_cast<int64_t>(bytes_processed) * 1000 / bytes_per_second;
        int64_t drift_ms = audio_ms - elapsed_ms;  // positive = ahead, negative = behind

        // If we're behind, process buffers immediately
        // If we're ahead, sleep until we need to process
        if (drift_ms > 0) {
            // We're ahead of real-time, sleep a bit
            int64_t sleep_ms = (std::min)(drift_ms, (int64_t)10);
            std::unique_lock<std::mutex> lock(period_mutex_);
            period_cv_.wait_for(lock, std::chrono::milliseconds(sleep_ms),
                                [this]() { return !period_running_.load(); });
            continue;
        }

        // If we're way behind (> 200ms), reset timing
        if (drift_ms < -200) {
            start_time = std::chrono::steady_clock::now();
            bytes_processed = 0;
            continue;
        }

        // Process one buffer
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
            // No buffers available, wait briefly
            std::unique_lock<std::mutex> lock(period_mutex_);
            period_cv_.wait_for(lock, std::chrono::milliseconds(1),
                                [this]() { return !period_running_.load(); });
            continue;
        }

        // Send PCM data to manager
        size_t pcm_bytes = 0;
        if (!buf.pcm_data.empty() && audio_port_) {
            AudioChunk chunk;
            chunk.sample_rate = sample_rate;
            chunk.channels = channels;
            pcm_bytes = buf.pcm_data.size() * sizeof(int16_t);
            chunk.pcm = std::move(buf.pcm_data);
            audio_port_->SubmitPcm(chunk);
        }

        // Track audio position
        bytes_processed += (pcm_bytes > 0) ? pcm_bytes : period_bytes;

        // Return buffer to guest
        if (mmio_) {
            auto* txq = mmio_->GetQueue(VIRTIO_SND_VQ_TX);
            if (txq) {
                txq->PushUsed(buf.head, buf.status_len);
                mmio_->NotifyUsedBuffer();
            }
        }
    }
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
