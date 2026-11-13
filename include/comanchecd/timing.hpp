#pragma once

#include <cstdint>
#include "comanchecd/cpu_work.hpp"

namespace comanchecd {
constexpr unsigned audio_sample_rate = 48000;
constexpr unsigned pit_frequency = 1193182;
constexpr unsigned mission_pit_divisor = 18356;
constexpr unsigned bios_pit_divisor = 65536;
enum class PitStart { periodic, rising_edge, inherited_square_wave };

// Phase is measured in PIT input cycles times output samples per second.
class IrqClock {
public:
    static constexpr std::uint64_t period = std::uint64_t(audio_sample_rate) * mission_pit_divisor;
    explicit IrqClock(std::uint64_t phase = 0);
    std::uint64_t advance(std::uint64_t samples);
    std::uint64_t phase() const;

private:
    std::uint64_t phase_;
};

struct IrqState {
    std::uint32_t random_seed = 0;
    std::uint32_t shake_ticks = 0;
    std::uint32_t shake_offset = 0;
    void interrupt(const CpuWorkSink& shake_work = {});
};
} // namespace comanchecd
