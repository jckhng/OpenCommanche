#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace comanchecd {
// Update-only CMOS timer of the retained DOSBox-X device profile, with IRQ8 disabled.
class RtcTiming {
    static constexpr double update_period_ms = 1000;
    static constexpr double update_lead_ms = .001, minimum_delay_ms = .01;
public:
    explicit RtcTiming(long double next_ms = update_period_ms, double ended_ms = 0)
        : next_ms_(next_ms), ended_ms_(ended_ms) {
        if (!std::isfinite(next_ms) || !std::isfinite(ended_ms) || next_ms < 0 || ended_ms < 0) {
            throw std::invalid_argument("Invalid CMOS update epoch");
        }
    }

    long double next() const { return next_ms_; }
    void advance(long double milliseconds) {
        if (!std::isfinite(milliseconds) || milliseconds < next_ms_) {
            throw std::invalid_argument("CMOS update precedes its deadline");
        }
        const auto now = static_cast<double>(milliseconds);
        if (now >= ended_ms_ + update_period_ms - update_lead_ms) {
            ended_ms_ = std::max(ended_ms_, now - update_period_ms);
            ended_ms_ -= std::fmod(ended_ms_, update_period_ms);
            ended_ms_ += update_period_ms;
        }
        const auto delay = std::max(minimum_delay_ms, ended_ms_ + update_period_ms - now);
        // PIC_AddEvent retains the scheduled epoch and rounds this delay through float.
        next_ms_ += static_cast<float>(delay);
    }

private:
    long double next_ms_;
    double ended_ms_;
};
}
