#pragma once

#include "common/ports.h"
#include <termios.h>

class PosixConsolePort final : public ConsolePort {
public:
    PosixConsolePort();
    ~PosixConsolePort() override;

    void Write(const uint8_t* data, size_t size) override;
    size_t Read(uint8_t* out, size_t size) override;

private:
    struct termios orig_termios_;
    bool raw_mode_ = false;
};
