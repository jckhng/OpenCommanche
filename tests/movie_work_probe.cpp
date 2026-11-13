#include "comanchecd/assets.hpp"
#include "comanchecd/audio_scheduler.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 4 || std::filesystem::exists(argv[3])) {
            throw std::invalid_argument("movie_work_probe INSTALLATION PAYLOAD_DIRECTORY FRESH_OUTPUT");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const auto movies = installation.load_demo_cinematics();
        std::ofstream output(argv[3]);
        output << "movie,frame,stage,dispatches,strings,restarts,extra,yields,io,cycles\n";
        for (const auto& item : {std::make_pair("animation", &movies.animation),
                                std::make_pair("title", &movies.title),
                                std::make_pair("hangar", &movies.hangar)}) {
            for (std::size_t frame = 0; frame < item.second->frame_count(); ++frame) {
                for (const bool blit : {false, true}) {
                    comanchecd::AudioScheduler scheduler(48000);
                    const auto work = [&](comanchecd::CpuOperation operation, unsigned count) { scheduler.execute(operation, count); };
                    if (blit) { item.second->blit_work(work); }
                    else { item.second->frame(frame, work); }
                    const auto accounting = scheduler.accounting();
                    output << item.first << ',' << frame << ',' << (blit ? "blit" : "decode") << ','
                        << accounting.dispatches << ',' << accounting.string_iterations << ',' << accounting.repeat_restarts << ','
                        << accounting.special_stosb_iterations << ',' << accounting.terminal_charges << ','
                        << accounting.io_cycles << ',' << scheduler.cycles() << '\n';
                }
            }
        }
        return output ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
