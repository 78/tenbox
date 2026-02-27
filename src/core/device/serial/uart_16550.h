#pragma once

#include "core/device/device.h"
#include <mutex>
#include <array>
#include <functional>

class Uart16550 : public Device {
public:
    static constexpr uint16_t kCom1Base = 0x3F8;
    static constexpr uint16_t kRegCount = 8;

    using IrqCallback = std::function<void()>;
    void SetIrqCallback(IrqCallback cb) { irq_callback_ = std::move(cb); }
    using TxCallback = std::function<void(uint8_t)>;
    void SetTxCallback(TxCallback cb) { tx_callback_ = std::move(cb); }

    void PioRead(uint16_t offset, uint8_t size, uint32_t* value) override;
    void PioWrite(uint16_t offset, uint8_t size, uint32_t value) override;

    void PushInput(uint8_t byte);
    bool HasInput() const;
    void CheckAndRaiseIrq() { RaiseIrqIfNeeded(); }

private:
    static constexpr uint16_t kTHR = 0;
    static constexpr uint16_t kRBR = 0;
    static constexpr uint16_t kIER = 1;
    static constexpr uint16_t kIIR = 2;
    static constexpr uint16_t kFCR = 2;
    static constexpr uint16_t kLCR = 3;
    static constexpr uint16_t kMCR = 4;
    static constexpr uint16_t kLSR = 5;
    static constexpr uint16_t kMSR = 6;
    static constexpr uint16_t kSCR = 7;

    static constexpr uint8_t kLsrDr   = 0x01;
    static constexpr uint8_t kLsrThre = 0x20;
    static constexpr uint8_t kLsrTemt = 0x40;

    static constexpr size_t kFifoSize = 256;

    uint8_t ier_ = 0;
    uint8_t lcr_ = 0;
    uint8_t mcr_ = 0;
    uint8_t scr_ = 0;
    uint8_t dll_ = 0;
    uint8_t dlh_ = 0;
    bool thre_pending_ = false;

    IrqCallback irq_callback_;
    TxCallback tx_callback_;

    mutable std::mutex rx_mutex_;
    std::array<uint8_t, kFifoSize> rx_buf_{};
    size_t rx_head_ = 0;
    size_t rx_tail_ = 0;
    size_t rx_count_ = 0;

    bool IsDlab() const { return (lcr_ & 0x80) != 0; }
    uint8_t PopRx();
    void RaiseIrqIfNeeded();
};
