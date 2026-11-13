#include "../src/startup_timing.hpp"
#include "comanchecd/audio.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        AudioScheduler clock(audio_sample_rate);
        std::vector<std::pair<unsigned, unsigned>> measurements;
        const auto calibration = calibrate_opl_delay(clock, [&](unsigned iterations, unsigned count) {
            measurements.emplace_back(iterations, count);
        });
        if (calibration != 239) { throw std::runtime_error("PIT2 calibration must converge to the original delay range"); }
        if (measurements.size() != 206 || measurements.front().first != 35 ||
                measurements.back().second <= 0x398 || measurements.back().second >= 0x3fc ||
                clock.accounting().dispatches != 913590 || clock.accounting().io_cycles != 37005) {
            throw std::runtime_error("Calibration must retain the original coarse estimate, refinement and CPU/I/O work");
        }
        if (argc == 2) {
            std::ofstream output(argv[1]);
            const auto work = clock.accounting();
            output << calibration << ' ' << clock.cycles() << ' ' << work.dispatches << ' '
                << work.io_cycles << ' ' << work.terminal_charges << '\n';
            for (const auto& measurement : measurements) { output << measurement.first << ' ' << measurement.second << '\n'; }
            if (!output) { throw std::runtime_error("Cannot write startup timing results"); }
        } else if (argc != 1) {
            throw std::invalid_argument("Usage: startup_timing_tests [OUTPUT]");
        }
        StartupFiles files;
        files.cdrom = std::vector<std::uint8_t>(16, 0);
        files.configuration = {'0', '1', '3', '2', '7', '1', '0', '0', '0', '0'};
        AudioScheduler startup(audio_sample_rate);
        std::vector<StartupBoundary> boundaries;
        pre_fm_startup(startup, files, [&](StartupBoundary boundary) { boundaries.push_back(boundary); });
        if (boundaries != std::vector<StartupBoundary>{StartupBoundary::find_cdrom, StartupBoundary::read_cdrom,
                StartupBoundary::read_configuration, StartupBoundary::detect_digital,
                StartupBoundary::clear_digital, StartupBoundary::detect_fm} || startup.dma_consumed() != 0) {
            throw std::runtime_error("Cold startup must retain the file/device order and invalid-selector return without DMA");
        }
        files.cdrom.reset();
        files.configuration[1] = '0';
        AudioScheduler disabled(audio_sample_rate);
        boundaries.clear();
        pre_fm_startup(disabled, files, [&](StartupBoundary boundary) { boundaries.push_back(boundary); });
        if (boundaries != std::vector<StartupBoundary>{StartupBoundary::find_cdrom,
                StartupBoundary::read_configuration, StartupBoundary::detect_digital, StartupBoundary::detect_fm} ||
                disabled.accounting().io_cycles != 0) {
            throw std::runtime_error("Missing CDR and disabled digital audio must skip their read and DSP setup");
        }
        files.configuration[2] = '0';
        AudioScheduler rejected(audio_sample_rate);
        bool unsupported = false;
        try { pre_fm_startup(rejected, files); } catch (const std::invalid_argument&) { unsupported = true; }
        if (!unsupported || rejected.cycles() != 0) {
            throw std::runtime_error("An unqualified cold sound profile must reject before advancing the clock");
        }
        files.campaigns.emplace();
        StartupCampaign campaign;
        campaign.decoded = {'{', 'A', '}', 0, 0, 0, 0, 0};
        campaign.encoded = campaign.decoded;
        constexpr std::array<std::uint8_t, 8> campaign_key{0xa3, 0xa6, 0xa2, 0x3a, 0x8a, 0x3d, 0xaa, 0x7e};
        for (std::size_t i = 0; i < campaign.encoded.size(); ++i) { campaign.encoded[i] ^= campaign_key[i]; }
        files.campaigns->push_back(campaign);
        files.campaigns->emplace_back();
        files.campaigns->push_back(campaign);
        AudioScheduler campaigns(audio_sample_rate);
        unsigned finds = 0, reads = 0, complete = 0;
        prepare_startup_campaigns(campaigns, files, [&](CampaignBoundary boundary, unsigned index) {
            if (boundary == CampaignBoundary::find) { ++finds; }
            if (boundary == CampaignBoundary::read) { ++reads; }
            if (boundary == CampaignBoundary::complete) { complete = index; }
        });
        if (finds != 10 || reads != 2 || complete != 10) {
            throw std::runtime_error("Startup must scan all ten numbered campaign slots across missing files");
        }
        StartupFiles loading_files;
        loading_files.configuration = {'0', '1', '3', '2', '7', '1'};
        loading_files.loading.emplace();
        auto& loading = *loading_files.loading;
        loading.active_settings.assign(MenuSettings::record_bytes, 125);
        loading.active_settings[24] = '1';
        loading.active_settings[25] = '2';
        loading.active_settings[26] = '0';
        loading.active_settings[27] = '4';
        loading.detail.assign(5004, 0);
        for (auto& menu : loading.menus) { menu = {' ', 255}; }
        IrqState startup_state{0x12345678, 9, 3};
        AudioScheduler loading_clock(audio_sample_rate);
        std::vector<LoadingBoundary> loaded;
        prepare_startup_loading(loading_clock, loading_files, &startup_state,
            [&](MusicLevel level) {
                if (level != MusicLevel::loud) { throw std::runtime_error("Wrong cold music setting"); }
            }, [&](LoadingBoundary boundary) { loaded.push_back(boundary); });
        if (startup_state.random_seed != 0x12345678 || startup_state.shake_ticks != 1 || startup_state.shake_offset != 0 ||
                loaded.back() != LoadingBoundary::complete ||
                std::find(loaded.begin(), loaded.end(), LoadingBoundary::read_pilots) != loaded.end() ||
                std::find(loaded.begin(), loaded.end(), LoadingBoundary::read_settings) != loaded.end()) {
            throw std::runtime_error("Cold loading must skip absent records and reset canonical shake state without changing RNG");
        }
        AudioTables tables;
        for (auto& envelope : tables.envelopes) { envelope = {0x80}; }
        std::vector<std::uint8_t> score(50);
        score[0] = 'M';
        score[1] = 'S';
        score[48] = 0xff;
        score[49] = 0x2f;
        AudioEntry entry;
        entry.execution = AudioExecution::scheduled;
        entry.startup = AudioStartup::cold;
        entry.calibration = AudioCalibration::pit2;
        entry.music_start = MusicStart::deferred;
        std::uint64_t dispatches = 0;
        IrqState whole_irq{0x12345678};
        entry.cpu_observer = [&](CpuOperation, std::uint64_t) { ++dispatches; };
        entry.interrupt = [&](const CpuWorkSink& work) { whole_irq.interrupt(work); };
        MissionAudio whole(tables, MusicPlaylist{score, {}}, entry);
        constexpr unsigned calibration_dispatches = 913590, installer_dispatches = 36;
        constexpr unsigned fm_dispatches = 369219, stub_irq_dispatches = 32;
        if (whole.irq_ticks() != 1 || whole_irq.random_seed != 3068682779u || whole.writes().size() != 124 ||
                dispatches != calibration_dispatches + installer_dispatches + fm_dispatches + stub_irq_dispatches ||
                whole.music_irq_work().front().work.dispatches != 1) {
            throw std::runtime_error("Cold calibration must precede PIT installation and carry the original FM/IRQ/RNG work");
        }
        IrqState split_irq{0x12345678};
        entry.cpu_observer = {};
        entry.interrupt = [&](const CpuWorkSink& work) { split_irq.interrupt(work); };
        MissionAudio split(tables, MusicPlaylist{score, {}}, entry);
        whole.stop_music();
        split.stop_music();
        if (whole.writes().size() != 133 || whole.writes()[124].reg != 0xb0 || whole.writes()[132].reg != 0xb8) {
            throw std::runtime_error("Cold loading must retain the shared nine-voice stop sequence");
        }
        const auto expected = whole.render(audio_sample_rate / 4);
        std::vector<std::int16_t> actual;
        constexpr unsigned fragment_samples = 733;
        while (actual.size() < expected.size()) {
            const auto block = split.render(std::min<std::size_t>(fragment_samples, expected.size() - actual.size()));
            actual.insert(actual.end(), block.begin(), block.end());
        }
        if (actual != expected || split_irq.random_seed != whole_irq.random_seed || split.irq_ticks() != whole.irq_ticks() ||
                split.execution_clock().cycles() != whole.execution_clock().cycles()) {
            throw std::runtime_error("Cold CPU time, IRQ/RNG and PCM must share one partition-independent clock");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
