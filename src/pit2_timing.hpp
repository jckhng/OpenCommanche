#pragma once

#include "comanchecd/timing.hpp"
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace comanchecd {
// The binary's calibration uses PIT2 mode 3 and latched low/high reads only.
class Pit2Timing {
public:
    void control(std::uint8_t value, long double milliseconds) {
        if (value == latch_command) {
            latch_ = counter(milliseconds);
            read_high_ = false;
            return;
        }
        if (value != mode_command) { throw std::invalid_argument("Unsupported PIT2 control word"); }
        write_high_ = false;
    }
    void write(std::uint8_t value, long double milliseconds) {
        if (!write_high_) {
            reload_ = value;
            write_high_ = true;
            return;
        }
        reload_ |= unsigned(value) << 8;
        divisor_ = reload_ == 0 ? 65536 : reload_;
        origin_ms_ = milliseconds;
        write_high_ = false;
    }
    std::uint8_t read() {
        const auto value = read_high_ ? latch_ >> 8 : latch_ & 0xff;
        read_high_ = !read_high_;
        return static_cast<std::uint8_t>(value);
    }
    void speaker(std::uint8_t value, long double milliseconds) {
        const bool gate = value & 1;
        if (gate && !(speaker_ & 1)) { origin_ms_ = milliseconds; }
        speaker_ = value;
    }
    std::uint8_t speaker() const { return speaker_; }

private:
    static constexpr std::uint8_t latch_command = 0x86, mode_command = 0xb6;
    std::uint16_t counter(long double milliseconds) const {
        const auto half_ms = static_cast<long double>(divisor_) * 500 / pit_frequency;
        const auto phase = std::fmod(milliseconds - origin_ms_, half_ms);
        const auto remaining = static_cast<unsigned>(divisor_ - phase * pit_frequency / 500);
        return static_cast<std::uint16_t>(remaining & 0xfffe);
    }
    unsigned divisor_ = 65536, reload_ = 0;
    long double origin_ms_ = 0;
    std::uint16_t latch_ = 0;
    std::uint8_t speaker_ = 0;
    bool write_high_ = false, read_high_ = false;
};
}
