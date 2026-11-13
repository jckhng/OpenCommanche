#include "comanchecd/audio.hpp"
#include "../src/menu_entry_timing.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace comanchecd;
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

PilotMenuAssets fixture(const std::vector<std::uint8_t>& score) {
    PilotMenuAssets assets;
    for (auto& resource : assets.scores) { resource.encoded = score; }
    auto& picture = assets.picture.encoded;
    picture.resize(128, 0);
    constexpr unsigned width_minus_one = line_stride * plane_count - 1;
    picture[8] = width_minus_one & 255;
    picture[9] = width_minus_one >> 8;
    for (unsigned left = width_minus_one + 1; left;) {
        const auto count = std::min(left, 63u);
        picture.push_back(192 | count);
        picture.push_back(0);
        left -= count;
    }
    picture.push_back(12);
    picture.resize(picture.size() + 768, 0);
    return assets;
}

void menu_work(const PilotMenuAssets& assets) {
    std::array<std::uint64_t, 2> reset_work{};
    for (unsigned changed : {0u, 1u}) {
        AudioScheduler scheduler(audio_sample_rate);
        Clock clock(scheduler);
        unsigned resets = 0, music = 0;
        prepare_pilot_menu(scheduler, clock, assets, false,
            [](DigitalCommand) { throw std::runtime_error("An inactive driver must not issue a stop command"); },
            [&] { return resets++ == 0 ? changed : 0; },
            [&] {
                if (resets != 2) { throw std::runtime_error("Both silence resets must precede menu music loading"); }
                ++music;
            }, [&](MenuEntryBoundary boundary) {
                if (boundary != MenuEntryBoundary::reset) { return; }
                const auto work = scheduler.accounting();
                reset_work[changed] = work.dispatches - work.repeat_restarts;
            });
        if (resets != 2 || music != 1) { throw std::runtime_error("Menu entry must retain both resets and one music start"); }
    }
    if (reset_work[1] != reset_work[0] + 6) {
        throw std::runtime_error("A changed cached voice must execute all six silence-reload instructions");
    }
    AudioScheduler scheduler(audio_sample_rate);
    Clock clock(scheduler);
    auto invalid = assets;
    invalid.scores[0].encoded.clear();
    bool rejected = false;
    try { prepare_pilot_menu(scheduler, clock, invalid, false, {}, {}, [] {}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected || scheduler.cycles() != 0) { throw std::runtime_error("Invalid menu scores must reject before CPU work"); }
}

void music_publication(AudioTables tables, const std::vector<std::uint8_t>& terminal) {
    auto old_score = terminal;
    old_score.resize(48);
    old_score.insert(old_score.end(), {0x90, 60, 100, 1, 0x80, 1, 0xff, 0x2f});
    tables.volume.fill(128);
    tables.frequencies[120] = 0x0d57;
    for (unsigned margin : {32u, 64u, 96u}) {
        AudioEntry entry;
        entry.execution = AudioExecution::scheduled;
        MissionAudio audio(tables, MusicPlaylist{{}, old_score}, entry);
        audio.render(audio_sample_rate);
        auto& clock = audio.execution_clock();
        const auto numerator = (audio.irq_ticks() + 1) * ExecutionClock::cycles_per_second * mission_pit_divisor;
        const auto before_irq = (numerator + pit_frequency - 1) / pit_frequency - margin;
        if (before_irq <= clock.cycles()) { throw std::runtime_error("Music publication fixture must precede its IRQ"); }
        while (clock.cycles() < before_irq) { clock.execute(CpuOperation::instructions, 1); }
        const auto before = audio.irq_ticks();
        audio.change_music({terminal, {}});
        if (audio.irq_ticks() == before) { throw std::runtime_error("Music publication fixture must interrupt the foreground start"); }
    }
}
}

int main() {
    try {
        using namespace comanchecd;
        AudioTables tables;
        for (auto& envelope : tables.envelopes) { envelope = {128}; }
        std::vector<std::uint8_t> score(50, 0);
        score[0] = 'M'; score[1] = 'S'; score[48] = 255; score[49] = 47;
        std::vector<CpuOperation> operations;
        AudioEntry entry;
        entry.execution = AudioExecution::scheduled;
        entry.cpu_observer = [&](CpuOperation operation, std::uint64_t) { operations.push_back(operation); };
        MissionAudio audio(tables, MusicPlaylist{score, {}}, entry);
        audio.execution_clock().execute(CpuOperation::disable_interrupts, 1);
        for (unsigned change = 0; change < 2; ++change) {
            operations.clear();
            audio.change_music({score, {}});
            if (std::find(operations.begin(), operations.end(), CpuOperation::repeat_copy_bytes) == operations.end()) {
                throw std::runtime_error("Every foreground music start must execute the MS header copy on the shared clock");
            }
        }
        menu_work(fixture(score));
        music_publication(tables, score);
        const auto before = audio.execution_clock().cycles();
        bool rejected = false;
        try { audio.start_cinematic_loop(); } catch (const std::logic_error&) { rejected = true; }
        if (!rejected || audio.execution_clock().cycles() != before) {
            throw std::runtime_error("A cinematic loop requires a pending bank transition");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
