#pragma once

#include <cmath>

namespace comanchecd {
// SB16 status-cycle behavior of the retained DOSBox-X device profile.
class DspTiming {
public:
    bool read(double milliseconds) {
        ++phase_;
        if (milliseconds >= last_read_ + phase_reset_ms) {
            phase_ = static_cast<int>(std::fmod(milliseconds / 1000 * busy_frequency, 1.0) * phase_steps);
        }
        last_read_ = milliseconds;
        return (phase_ % phase_steps) * 100 / phase_steps < busy_duty_percent;
    }

private:
    static constexpr unsigned busy_frequency = 8333333 / 6 / 16;
    static constexpr unsigned phase_steps = 16, busy_duty_percent = 50;
    static constexpr double phase_reset_ms = 0.02;
    double last_read_ = 0;
    unsigned phase_ = 0;
};
}
