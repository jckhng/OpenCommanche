#pragma once

#include "comanchecd/video_timing.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace comanchecd {
class VideoClock {
public:
    explicit VideoClock(const VideoTiming& timing = {}) : timing_(timing) {
        if (!std::isfinite(timing.line_ms) || !std::isfinite(timing.frame_ms) ||
                !std::isfinite(timing.frame_start_ms) || !std::isfinite(timing.ideal_frame_ms) ||
                !(timing.line_ms > 0 && timing.display_ms > 0 && timing.display_ms < timing.retrace_start_ms &&
                  timing.retrace_start_ms < timing.retrace_end_ms && timing.retrace_end_ms < timing.frame_ms) ||
                !timing.visible_lines || timing.lines_done > timing.visible_lines) {
            throw std::invalid_argument("Invalid VGA timing");
        }
        start_frame();
        deadlines_[line] += static_cast<long double>(float(timing_.line_ms)) * timing_.lines_done;
        if (timing_.lines_done == timing_.visible_lines) { deadlines_[line] = infinity; }
        next_ = *std::min_element(deadlines_.begin(), deadlines_.end());
    }

    long double next() const { return next_; }
    void advance(long double now_ms) {
        while (next() <= now_ms) {
            const auto index = std::size_t(std::min_element(deadlines_.begin(), deadlines_.end()) - deadlines_.begin());
            const auto time = deadlines_[index];
            deadlines_[index] = infinity;
            if (index == line) {
                if (++timing_.lines_done < timing_.visible_lines) {
                    deadlines_[line] = time + float(timing_.line_ms);
                } else {
                    timing_.ideal_frame_ms += timing_.frame_ms;
                }
            } else if (index == vertical) {
                timing_.frame_start_ms = time;
                timing_.lines_done = 0;
                start_frame();
            }
            next_ = *std::min_element(deadlines_.begin(), deadlines_.end());
        }
    }
    bool retrace(long double now_ms) const {
        const auto phase = now_ms - timing_.frame_start_ms;
        return phase >= timing_.retrace_start_ms && phase < timing_.retrace_end_ms;
    }

private:
    enum Event { line, interrupt, start, panning, vertical, event_count };
    static constexpr long double infinity = std::numeric_limits<long double>::infinity();
    void start_frame() {
        const auto time = timing_.frame_start_ms;
        const auto adjustment = std::clamp((timing_.ideal_frame_ms - double(time)) * .25, -.1, .1);
        deadlines_ = {time + float(timing_.line_ms / 4), time + float(timing_.display_ms + .005),
                      time + float(timing_.retrace_start_ms), time + float(timing_.retrace_end_ms),
                      time + std::max(1.0, float(timing_.frame_ms) + adjustment)};
    }
    VideoTiming timing_;
    std::array<long double, event_count> deadlines_{};
    long double next_ = 0;
};
}
