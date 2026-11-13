#include "comanchecd/timing.hpp"
#include "../src/video_clock.hpp"
#include "../src/rtc_timing.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        comanchecd::RtcTiming rtc;
        require(rtc.next() == 1000, "The virtual CMOS clock must start at its first update second");
        rtc.advance(1000.0006L);
        require(rtc.next() == 1999.9993896484375L,
                "CMOS rescheduling must retain the scheduled epoch and float-rounded service delay");
        rtc.advance(1999.9996L);
        require(rtc.next() == 2999.99981689453125L,
                "The CMOS early-update threshold must advance the completed-second epoch");
        comanchecd::VideoTiming video;
        video.line_ms = 1;
        video.frame_ms = 5;
        video.display_ms = 2;
        video.retrace_start_ms = 3;
        video.retrace_end_ms = 4;
        video.visible_lines = 2;
        comanchecd::VideoClock clock(video);
        require(clock.next() == 0.25L, "VGA must begin at its first quarter-line event");
        clock.advance(0.25L);
        require(clock.next() == 1.25L, "A VGA line must publish its following deadline");
        clock.advance(1.25L);
        require(clock.next() == static_cast<long double>(float(video.display_ms + .005)),
                "The last visible line must yield to the pending display event");
        clock.advance(3);
        require(clock.next() == 4 && clock.retrace(3), "Tied time advances must consume retrace entry before returning");
        clock.advance(5);
        require(clock.next() == 5.25L && !clock.retrace(5), "A new VGA frame must publish its first line event");
        video.lines_done = video.visible_lines;
        clock = comanchecd::VideoClock(video);
        require(clock.next() == static_cast<long double>(float(video.display_ms + .005)),
                "Inherited completed lines must leave the display event first");
        comanchecd::VideoClock whole_video, split_video;
        for (unsigned step = 0; step < 1000; ++step) {
            const auto time = static_cast<long double>(step) / 7;
            whole_video.advance(time);
            split_video.advance(time - .01L);
            split_video.advance(time);
            require(whole_video.next() == split_video.next() && whole_video.retrace(time) == split_video.retrace(time),
                    "VGA event deadlines and retrace must survive partitioned host advances");
        }
        comanchecd::IrqClock whole, split;
        const auto ticks = whole.advance(comanchecd::audio_sample_rate);
        std::uint64_t split_ticks = 0;
        for (unsigned sample = 0; sample < comanchecd::audio_sample_rate; ++sample) {
            split_ticks += split.advance(1);
        }
        require(ticks == 65 && ticks == split_ticks && whole.phase() == split.phase(),
                "PIT delivery must retain the exact divisor and fractional phase across chunks");
        comanchecd::IrqClock boundary;
        constexpr auto first_irq_sample = (comanchecd::IrqClock::period + comanchecd::pit_frequency - 1) /
            comanchecd::pit_frequency;
        require(boundary.advance(first_irq_sample - 1) == 0 && boundary.advance(1) == 1,
                "The first timer interrupt must occur at the rounded-up PIT threshold");
        comanchecd::IrqClock carried(comanchecd::IrqClock::period - comanchecd::pit_frequency);
        require(carried.advance(1) == 1 && carried.phase() == 0,
                "An inherited phase must deliver an interrupt at the exact threshold");
        bool rejected = false;
        try {
            comanchecd::IrqClock invalid(comanchecd::IrqClock::period);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "An out-of-period timer phase must reject");
        comanchecd::IrqClock large, partitioned;
        const auto count = std::numeric_limits<std::uint64_t>::max();
        const auto first = partitioned.advance(count / 2);
        const auto second = partitioned.advance(count - count / 2);
        require(large.advance(count) == first + second && large.phase() == partitioned.phase(),
                "Large clock advances must avoid multiplication overflow");
        comanchecd::IrqState irq{0x12345678, 2, 3};
        irq.interrupt();
        require(irq.random_seed == 0xb6e8621b && irq.shake_ticks == 1 && irq.shake_offset == 3,
                "IRQ RNG must wrap the add before rotating and choose the low two shake bits");
        irq.interrupt();
        require(irq.shake_ticks == 0 && irq.shake_offset == 0,
                "The final shake interrupt must clear the displacement");
        irq.shake_offset = 2;
        irq.interrupt();
        require(irq.shake_ticks == 0 && irq.shake_offset == 2,
                "An inactive shake must leave its carried displacement unchanged");
        comanchecd::IrqState charged{0, 2, 0};
        const auto shake_counts = [&](auto& state) {
            unsigned instructions = 0, reads = 0, writes = 0;
            state.interrupt([&](comanchecd::CpuOperation operation, unsigned count) {
                instructions += count;
                if (operation == comanchecd::CpuOperation::port_read_byte) { reads += count; }
                if (operation == comanchecd::CpuOperation::port_write_byte) { writes += count; }
            });
            return std::array<unsigned, 3>{instructions, reads, writes};
        };
        require(shake_counts(charged) == std::array<unsigned, 3>{13, 1, 3} &&
                shake_counts(charged) == std::array<unsigned, 3>{14, 1, 3} &&
                shake_counts(charged) == std::array<unsigned, 3>{0, 0, 0},
                "Shake work must include the final clear branch and exactly one status read plus three attribute writes");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
