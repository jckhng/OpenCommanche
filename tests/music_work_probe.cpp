#include "comanchecd/assets.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 7 || std::filesystem::exists(argv[3])) {
            throw std::invalid_argument("music_work_probe INSTALLATION PAYLOAD_DIRECTORY FRESH_OUTPUT FANFARE_IRQS CANYON_IRQS CALIBRATION");
        }
        const auto fanfare_irqs = std::stoul(argv[4]), canyon_irqs = std::stoul(argv[5]);
        if (fanfare_irqs == 0 || fanfare_irqs > 2000 || canyon_irqs == 0 || canyon_irqs > 2000) {
            throw std::invalid_argument("Unsupported music IRQ count");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const auto movies = installation.load_demo_cinematics();
        comanchecd::AudioEntry entry;
        entry.execution = comanchecd::AudioExecution::scheduled;
        entry.music_start = comanchecd::MusicStart::deferred;
        const auto calibration = std::stoul(argv[6]);
        if (calibration == 0 || calibration > 1000000) { throw std::invalid_argument("Unsupported OPL calibration"); }
        entry.opl_delay_iterations = static_cast<unsigned>(calibration);
        comanchecd::MissionAudio audio(installation.load_audio(), {movies.fanfare, {}}, entry);
        constexpr unsigned inactive_lead_in = 10;
        while (audio.music_irq_work().size() < inactive_lead_in) { audio.render(1); }
        audio.change_music({movies.fanfare, {}});
        while (audio.music_irq_work().size() < inactive_lead_in + fanfare_irqs) { audio.render(1); }
        if (audio.music_irq_work().size() != inactive_lead_in + fanfare_irqs) { throw std::runtime_error("IRQ entry overshot transition"); }
        audio.change_music({{}, movies.canyon_music});
        while (audio.music_irq_work().size() < inactive_lead_in + fanfare_irqs + canyon_irqs) { audio.render(1); }
        std::ofstream output(argv[3]);
        output << "tick,dispatches,strings,restarts,extra,yields,io,opl\n";
        for (const auto& item : audio.music_irq_work()) {
            output << std::dec << item.tick << ',' << item.work.dispatches << ',' << item.work.string_iterations << ','
                << item.work.repeat_restarts << ',' << item.work.special_stosb_iterations << ','
                << item.work.terminal_charges << ',' << item.work.io_cycles << ',';
            for (auto index = item.first_write; index < item.last_write; ++index) {
                const auto& write = audio.writes().at(index);
                output << std::hex << std::setfill('0') << std::setw(2) << unsigned(write.reg) << std::setw(2) << unsigned(write.value);
            }
            output << '\n';
        }
        return output ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
