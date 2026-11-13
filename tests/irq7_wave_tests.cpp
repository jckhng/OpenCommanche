#include "comanchecd/audio_scheduler.hpp"
#include "../src/startup_resource.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace comanchecd;
constexpr unsigned rate = 48000, budget = 20000;

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

class Clock final : public ExecutionClock {
public:
    explicit Clock(AudioScheduler& scheduler) : scheduler_(scheduler) {}
    void execute(CpuOperation operation, unsigned count) override { scheduler_.execute(operation, count); }
    void instruction(const std::function<void()>& action) override { scheduler_.instruction(action); }
    void read_file(unsigned bytes) override { scheduler_.read_file(bytes); }
    bool read_retrace() override { return scheduler_.read_retrace(); }
    std::uint64_t cycles() const override { return scheduler_.cycles(); }
    std::uint64_t irq_ticks() const override { return 0; }
    std::uint64_t sample_irq_ticks() override { return 0; }
private:
    AudioScheduler& scheduler_;
};

void raw_refills() {
    constexpr unsigned buffer_bytes = 8192;
    for (unsigned size : {buffer_bytes - 1, buffer_bytes, buffer_bytes + 5, 2 * buffer_bytes}) {
        StartupResource resource;
        resource.encoded = {'K', 'R', 'L', '0'};
        for (unsigned shift = 0; shift < 32; shift += 8) {
            resource.encoded.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        resource.encoded.resize(8 + size, 128);
        AudioScheduler scheduler(rate);
        Clock clock(scheduler);
        read_startup_resource(scheduler, clock, resource, 9);
        const auto work = scheduler.accounting();
        const auto intrinsic = work.dispatches - work.repeat_restarts - 13 * work.idle_calls;
        require(intrinsic == 440 + 74 * (size / buffer_bytes),
                "Every full raw-resource refill must retain its next CALL, including the zero-byte EOF read");
    }
}

void masked_dma() {
    AudioScheduler scheduler(rate);
    scheduler.set_interrupt_delay(838);
    scheduler.enable_interrupts(false);
    scheduler.configure(AudioChannel::digital, {true, 11111, {audio_gain_one, audio_gain_one}, 0});
    scheduler.arm_dma({128});
    unsigned entries = 0;
    scheduler.set_dma_handler([&] { ++entries; });
    scheduler.run_cycles(budget);
    scheduler.execute(CpuOperation::instructions, 20);
    require(scheduler.cycles() == budget + 21 && scheduler.accounting().terminal_charges == 1 && entries == 0,
            "DMA completion must queue a PIC event and terminate the CPU slice even while IF is clear");
    scheduler.execute(CpuOperation::enable_interrupts, 1);
    require(entries == 0, "IRQ7 must retain the STI shadow");
    scheduler.execute(CpuOperation::instructions, 1);
    require(entries == 1, "IRQ7 must deliver after the instruction following STI");
}

void dma_bridge() {
    AudioScheduler scheduler(rate);
    scheduler.configure(AudioChannel::digital, {true, 11111, {audio_gain_one, audio_gain_one}, 0});
    scheduler.arm_dma({128});
    unsigned entries = 0;
    std::uint64_t entry_credits = 0;
    bool returned = false, return_seen = false;
    scheduler.set_cpu_observer([&](CpuOperation operation, std::uint64_t) {
        if (returned && !return_seen) {
            require(operation == CpuOperation::disable_interrupts, "IRQ7's extender return must begin with CLI");
            return_seen = true;
        }
    });
    scheduler.set_dma_handler([&] {
        ++entries;
        entry_credits = scheduler.accounting().stack_segment_credits;
        returned = true;
    });
    scheduler.read_file(50000);
    require(entries == 1 && entry_credits == 3 && scheduler.accounting().stack_segment_credits == 6 && return_seen,
            "IRQ7 during a DOS read must execute the real-mode bridge and its three SS credits");
}

void dma_slice() {
    AudioScheduler scheduler(rate);
    scheduler.set_interrupt_delay(838);
    scheduler.enable_interrupts(false);
    scheduler.configure(AudioChannel::digital, {true, 11111, {audio_gain_one, audio_gain_one}, 0});
    scheduler.arm_dma({128});
    scheduler.run_cycles(budget / 2);
    scheduler.instruction([&] { scheduler.flush(); });
    require(scheduler.cycles() == budget / 2 + 2 && scheduler.accounting().terminal_charges == 1,
            "A DMA PIC event inserted before the active slice must return its unused CPU budget immediately");
}

void fm_enable() {
    AudioScheduler early(rate), late(rate);
    const AudioChannelConfig config{false, rate, {audio_gain_one / 2, audio_gain_one}, 0};
    early.configure(AudioChannel::fm, config);
    late.configure(AudioChannel::fm, config);
    constexpr unsigned silent_ms = 20, note_ms = 100;
    late.run_cycles(silent_ms * budget);
    late.take_output();
    for (auto* scheduler : {&early, &late}) {
        for (const auto& write : std::vector<std::array<std::uint8_t, 2>>{
                 {0x20, 0x01}, {0x23, 0x01}, {0x40, 0x10}, {0x43, 0},
                 {0x60, 0xf0}, {0x63, 0xf0}, {0x80, 0x77}, {0x83, 0x77},
                 {0xa0, 0x98}, {0xb0, 0x31}}) {
            scheduler->write_fm_timed(write[0], write[1], 239);
        }
        scheduler->run_cycles(note_ms * budget);
    }
    const auto expected = early.take_output(), actual = late.take_output();
    require(std::any_of(actual.begin(), actual.end(), [](const auto& sample) { return sample[1] != 0; }),
            "The first OPL address-port write must enable the configured mixer channel");
    require(expected == actual, "Disabled FM time must not advance the oscillator or LFO epoch");
    require(std::any_of(actual.begin(), actual.end(), [](const auto& sample) { return sample[0] != sample[1]; }),
            "Enabling FM must preserve the caller's configured gain");
}
}

int main(int argc, char** argv) {
    try {
        const std::string test = argc > 1 ? argv[1] : "all";
        if (test == "all" || test == "refill") { raw_refills(); }
        if (test == "all" || test == "masked") { masked_dma(); }
        if (test == "all" || test == "bridge") { dma_bridge(); }
        if (test == "all" || test == "slice") { dma_slice(); }
        if (test == "all" || test == "fm") { fm_enable(); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
