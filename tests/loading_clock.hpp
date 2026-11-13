#pragma once

#include "comanchecd/audio_scheduler.hpp"
#include "comanchecd/execution_clock.hpp"
#include "comanchecd/timing.hpp"

class LoadingClock final : public comanchecd::ExecutionClock {
public:
    LoadingClock() : scheduler_(comanchecd::audio_sample_rate) {}
    void execute(comanchecd::CpuOperation operation, unsigned count) override { scheduler_.execute(operation, count); }
    void instruction(const std::function<void()>& action) override { scheduler_.instruction(action); }
    void read_file(unsigned bytes) override { scheduler_.read_file(bytes); }
    bool read_retrace() override { return scheduler_.read_retrace(); }
    std::uint64_t cycles() const override { return scheduler_.cycles(); }
    std::uint64_t irq_ticks() const override { return 0; }
    std::uint64_t sample_irq_ticks() override {
        scheduler_.instruction([] {});
        return 0;
    }
    comanchecd::CpuAccounting accounting() const { return scheduler_.accounting(); }
private:
    comanchecd::AudioScheduler scheduler_;
};
