#include "comanchecd/timing.hpp"

#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::uint32_t irq_random_xor = 0xa50a50a0;
constexpr std::uint32_t irq_random_add = 0x00050a05;
}

IrqClock::IrqClock(std::uint64_t phase) : phase_(phase) {
    if (phase >= period) {
        throw std::invalid_argument("Timer phase exceeds its PIT period");
    }
}

std::uint64_t IrqClock::advance(std::uint64_t samples) {
    // Split before multiplying so even a full-width sample count is safe.
    const auto remainder = (samples % period) * pit_frequency + phase_;
    const auto ticks = (samples / period) * pit_frequency + remainder / period;
    phase_ = remainder % period;
    return ticks;
}

std::uint64_t IrqClock::phase() const { return phase_; }

void IrqState::interrupt(const CpuWorkSink& shake_work) {
    const auto mixed = (random_seed ^ irq_random_xor) + irq_random_add;
    random_seed = (mixed >> 3) | (mixed << 29);
    if (shake_ticks == 0) {
        return;
    }
    --shake_ticks;
    shake_offset = shake_ticks == 0 ? 0 : random_seed & 3;
    if (shake_work) {
        shake_work(CpuOperation::instructions, shake_ticks == 0 ? 5 : 4);
        shake_work(CpuOperation::port_read_byte, 1);
        shake_work(CpuOperation::instructions, 2);
        shake_work(CpuOperation::port_write_byte, 1);
        shake_work(CpuOperation::instructions, 2);
        shake_work(CpuOperation::port_write_byte, 1);
        shake_work(CpuOperation::instructions, 1);
        shake_work(CpuOperation::port_write_byte, 1);
    }
}
} // namespace comanchecd
