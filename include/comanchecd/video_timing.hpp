#pragma once

namespace comanchecd {
struct VideoTiming {
    static constexpr double line_period_ms = 800000.0 / 25175000;
    double line_ms = line_period_ms;
    double frame_ms = line_period_ms * 525;
    double display_ms = line_period_ms * 480;
    double retrace_start_ms = line_period_ms * 490;
    double retrace_end_ms = line_period_ms * 492;
    unsigned visible_lines = 480;
    long double frame_start_ms = 0;
    double ideal_frame_ms = 0;
    unsigned lines_done = 0;
};
}
