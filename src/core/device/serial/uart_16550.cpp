#include "core/device/serial/uart_16550.h"
void Uart16550::PushInput(uint8_t byte) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_count_ < kFifoSize) {
        rx_buf_[rx_tail_] = byte;
        rx_tail_ = (rx_tail_ + 1) % kFifoSize;
        rx_count_++;
    }
}

bool Uart16550::HasInput() const {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    return rx_count_ > 0;
}

uint8_t Uart16550::PopRx() {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_count_ == 0) return 0;
    uint8_t byte = rx_buf_[rx_head_];
    rx_head_ = (rx_head_ + 1) % kFifoSize;
    rx_count_--;
    return byte;
}

void Uart16550::RaiseIrqIfNeeded() {
    bool need_irq = false;
    if ((ier_ & 0x02) && thre_pending_)  need_irq = true;
    if ((ier_ & 0x01) && HasInput())     need_irq = true;
    if (need_irq && irq_callback_) irq_callback_();
}

void Uart16550::PioRead(uint16_t offset, uint8_t size, uint32_t* value) {
    uint8_t val = 0;

    if (IsDlab() && offset <= 1) {
        val = (offset == 0) ? dll_ : dlh_;
        *value = val;
        return;
    }

    switch (offset) {
    case kRBR:
        val = PopRx();
        RaiseIrqIfNeeded();
        break;
    case kIER:
        val = ier_;
        break;
    case kIIR:
        // Priority: RLS > RDA > THRE > Modem Status
        if (HasInput() && (ier_ & 0x01)) {
            val = 0x04; // Received Data Available
        } else if (thre_pending_ && (ier_ & 0x02)) {
            val = 0x02; // THR Empty
            thre_pending_ = false; // reading IIR clears THRE condition
        } else {
            val = 0x01; // No interrupt pending
        }
        // Bits 7:6 = 11 when FIFO is enabled: 16550A identification signal.
        // Linux 8250 autoconfig uses (IIR & 0xC0) == 0xC0 to detect 16550A.
        if (fifo_enabled_) {
            val |= 0xC0;
        }
        break;
    case kLCR:
        val = lcr_;
        break;
    case kMCR:
        val = mcr_;
        break;
    case kLSR:
        val = kLsrThre | kLsrTemt;
        if (HasInput())
            val |= kLsrDr;
        break;
    case kMSR:
        val = 0;
        break;
    case kSCR:
        val = scr_;
        break;
    default:
        val = 0;
        break;
    }

    *value = val;
}

void Uart16550::PioWrite(uint16_t offset, uint8_t size, uint32_t value) {
    uint8_t val = static_cast<uint8_t>(value);

    if (IsDlab() && offset <= 1) {
        if (offset == 0) dll_ = val;
        else             dlh_ = val;
        return;
    }

    switch (offset) {
    case kTHR:
        if (tx_callback_) {
            tx_callback_(val);
        }
        thre_pending_ = true;
        RaiseIrqIfNeeded();
        break;
    case kIER: {
        uint8_t old_ier = ier_;
        ier_ = val;
        if ((val & 0x02) && !(old_ier & 0x02)) {
            thre_pending_ = true;
        }
        RaiseIrqIfNeeded();
        break;
    }
    case kFCR:
        // FCR bit 0: FIFO enable.  Bit 1: clear RX FIFO.  Bit 2: clear TX
        // FIFO (TX drains synchronously through tx_callback_, so nothing to
        // clear).  Bits 6:7 set the RX trigger level; we don't model the
        // threshold separately since RX is a ring consumed on demand.
        fifo_enabled_ = (val & 0x01) != 0;
        if (val & 0x02) {
            std::lock_guard<std::mutex> lock(rx_mutex_);
            rx_head_ = 0;
            rx_tail_ = 0;
            rx_count_ = 0;
        }
        break;
    case kLCR:
        lcr_ = val;
        break;
    case kMCR:
        mcr_ = val;
        break;
    case kSCR:
        scr_ = val;
        break;
    default:
        break;
    }
}
