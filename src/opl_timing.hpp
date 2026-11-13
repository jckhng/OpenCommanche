#pragma once

#include <cmath>
#include <cstdint>

namespace comanchecd {
// OPL timer/status behavior of the retained SB16 profile.
class OplTiming {
public:
    void write(std::uint8_t reg, std::uint8_t value, double milliseconds) {
        if (reg == 2 || reg == 3) {
            timers_[reg - 2].counter(value, milliseconds);
            return;
        }
        if (reg != 4) { return; }
        if (value & reset_status) {
            for (auto& timer : timers_) { timer.reset(); }
            return;
        }
        timers_[0].control(value & 1, value & 0x40, milliseconds);
        timers_[1].control(value & 2, value & 0x20, milliseconds);
    }

    std::uint8_t read(double milliseconds) {
        const auto first = timers_[0].overflow(milliseconds);
        const auto second = timers_[1].overflow(milliseconds);
        return (first || second ? reset_status : 0) | (first ? 0x40 : 0) | (second ? 0x20 : 0);
    }

private:
    static constexpr std::uint8_t reset_status = 0x80;
    class Timer {
    public:
        explicit Timer(double step_ms) : step_ms_(step_ms), period_ms_(256 * step_ms) {}
        void counter(std::uint8_t value, double milliseconds) {
            overflow(milliseconds);
            period_ms_ = (256 - value) * step_ms_;
        }
        void reset() { overflow_ = false; }
        void control(bool enabled, bool masked, double milliseconds) {
            if (enabled && !enabled_) {
                deadline_ms_ = milliseconds - std::fmod(milliseconds, step_ms_) + period_ms_;
                overflow_ = false;
            }
            enabled_ = enabled;
            masked_ = masked;
            if (masked_) { overflow_ = false; }
        }
        bool overflow(double milliseconds) {
            if (enabled_ && milliseconds >= deadline_ms_) {
                deadline_ms_ = milliseconds - std::fmod(milliseconds - deadline_ms_, period_ms_) + period_ms_;
                if (!masked_) { overflow_ = true; }
            }
            return overflow_;
        }

    private:
        double step_ms_, period_ms_, deadline_ms_ = 0;
        bool enabled_ = false, masked_ = false, overflow_ = false;
    };
    Timer timers_[2]{Timer(0.08), Timer(0.32)};
};
}
