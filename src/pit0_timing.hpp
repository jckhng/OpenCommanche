#pragma once

#include "comanchecd/timing.hpp"
#include <cmath>
#include <stdexcept>

namespace comanchecd {
class Pit0Timing {
public:
    explicit Pit0Timing(long double origin_ms = 0, unsigned divisor = bios_pit_divisor) {
        reload(divisor, origin_ms);
    }

    void reload(unsigned divisor, long double milliseconds) {
        if (!divisor || divisor > bios_pit_divisor || !std::isfinite(milliseconds)) {
            throw std::invalid_argument("Invalid PIT0 square-wave epoch");
        }
        origin_ms_ = milliseconds;
        period_ms_ = static_cast<long double>(divisor) * 1000 / pit_frequency;
    }

    bool output_high(long double milliseconds) const {
        if (!std::isfinite(milliseconds) || milliseconds < origin_ms_) {
            throw std::invalid_argument("PIT0 output precedes its reload epoch");
        }
        return std::fmod(milliseconds - origin_ms_, period_ms_) < period_ms_ / 2;
    }

private:
    long double origin_ms_ = 0, period_ms_ = 0;
};
}
