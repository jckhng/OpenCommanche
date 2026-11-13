#include "comanchecd/assets.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
}

int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        require(argc == 1 || argc == 5, "Usage: cold_audio_tests [INSTALLATION PAYLOAD_DIRECTORY OUTPUT CALIBRATION]");
        AudioTables tables;
        for (auto& envelope : tables.envelopes) { envelope = {0x80}; }
        std::vector<std::uint8_t> score(50);
        score[0] = 'M';
        score[1] = 'S';
        score[48] = 0xff;
        score[49] = 0x2f;
        MusicPlaylist playlist{score, {}};
        if (argc == 5) {
            const Installation installation(argv[1], argv[2]);
            tables = installation.load_audio();
            playlist = installation.load_menu_music();
        }
        AudioEntry entry;
        entry.execution = AudioExecution::scheduled;
        entry.startup = AudioStartup::cold;
        entry.music_start = MusicStart::deferred;
        entry.opl_delay_iterations = argc == 5 ? static_cast<unsigned>(std::stoul(argv[4])) : 239;
        {
            bool starting = false, header_seen = false;
            unsigned prefix = 0;
            auto external = entry;
            external.cpu_observer = [&](CpuOperation operation, std::uint64_t) {
                if (!starting || header_seen) { return; }
                if (operation == CpuOperation::repeat_copy_bytes) { header_seen = true; }
                else if (operation == CpuOperation::instructions) { ++prefix; }
            };
            MissionAudio music(tables, playlist, external);
            starting = true;
            music.change_music(playlist);
            constexpr unsigned external_music_prefix = 10;
            require(header_seen && prefix == external_music_prefix,
                    "External music starts must include four entry checks before the six-instruction restart prefix");
        }
        for (const auto shake : {0u, 2u}) {
            IrqState state{0x12345678, shake, 0};
            ExecutionClock* timeline = nullptr;
            std::uint64_t store_cycle = 0;
            AudioEntry timed;
            timed.execution = AudioExecution::scheduled;
            timed.startup = AudioStartup::cold;
            timed.music_start = MusicStart::deferred;
            timed.opl_delay_iterations = entry.opl_delay_iterations;
            timed.before_music_device = [&](ExecutionClock& clock) { timeline = &clock; };
            timed.interrupt = [&](const CpuWorkSink& work) {
                state.interrupt(work);
                if (!store_cycle) { store_cycle = timeline->cycles(); }
            };
            MissionAudio positioned(tables, playlist, timed);
            constexpr std::uint64_t first_rng_store_cycle = 307735;
            require(store_cycle == first_rng_store_cycle,
                    "The IRQ receiver must commit RNG state in the source store instruction before reporting shake work");
        }
        std::uint64_t dispatches = 0;
        entry.cpu_observer = [&](CpuOperation, std::uint64_t) { ++dispatches; };
        MissionAudio audio(tables, std::move(playlist), entry);
        const auto& writes = audio.writes();
        const std::array<std::pair<std::uint8_t, std::uint8_t>, 6> detection{{
            {4, 0x60}, {4, 0x80}, {2, 0xff}, {4, 0x21}, {4, 0x60}, {4, 0x80}}};
        require(writes.size() >= detection.size(), "Cold FM detection must produce its timer writes");
        for (std::size_t index = 0; index < detection.size(); ++index) {
            require(writes[index].reg == detection[index].first && writes[index].value == detection[index].second,
                    "Cold FM initialization must detect the timer before resetting voices");
        }
        require(writes.size() == 124, "Cold initialization must preserve detection, reset, program and silence writes");
        require(writes[9].reg == 0x43 && writes[10].reg == 0xb0 &&
                writes[11].reg == 0x44 && writes[12].reg == 0xb1 &&
                writes[26].reg == 0xb8 && writes[27].reg == 0xb0 && writes[27].value == 0x1f,
                "All physical FM voices must be reset before logical-channel programming");
        require(!audio.music_irq_work().empty() && audio.music_irq_work().front().work.dispatches == 1,
                "Cold timer interrupts must retain the RET callback until installation");
        constexpr unsigned ret_callback_irq_dispatches = 32;
        constexpr std::uint64_t reference_dispatches = 369219;
        constexpr unsigned dispatches_per_calibration_step = 1526;
        const auto intrinsic = dispatches - audio.irq_ticks() * ret_callback_irq_dispatches;
        require(intrinsic == reference_dispatches +
                (std::int64_t(entry.opl_delay_iterations) - 239) * dispatches_per_calibration_step,
                "Cold FM detection/initialization work must match the original instruction counts");
        if (argc == 5) {
            require(!std::filesystem::exists(argv[3]), "Output must be fresh");
            std::ofstream output(argv[3]);
            output << "cycles " << audio.execution_clock().cycles() << "\nirqs " << audio.irq_ticks()
                << "\ndispatches " << intrinsic << '\n';
            for (const auto& write : writes) { output << unsigned(write.reg) << ' ' << unsigned(write.value) << '\n'; }
            require(bool(output), "Cannot write cold FM probe output");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
