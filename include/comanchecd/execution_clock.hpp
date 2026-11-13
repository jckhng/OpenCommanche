#pragma once

#include "comanchecd/cpu_work.hpp"

namespace comanchecd {
// Portable CPU/device timeline for the retained 20,000-cycle/ms profile.
class ExecutionClock {
public:
    static constexpr unsigned cycles_per_second = 20000000;
    virtual ~ExecutionClock() = default;
    virtual void execute(CpuOperation operation, unsigned count) = 0;
    virtual void instruction(const std::function<void()>& action) = 0;
    virtual void read_file(unsigned bytes) = 0;
    virtual bool read_retrace() = 0;
    virtual std::uint64_t cycles() const = 0;
    virtual std::uint64_t irq_ticks() const = 0;
    virtual std::uint64_t sample_irq_ticks() = 0;
};
}
