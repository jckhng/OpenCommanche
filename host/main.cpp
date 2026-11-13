#include "sdl_host.hpp"

#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    try {
        constexpr const char* usage = "training_viewer INSTALLATION PAYLOAD_DIRECTORY [--ticks N]";
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << usage << '\n';
            return 0;
        }
        if (argc != 3 && argc != 5) {
            throw std::invalid_argument(usage);
        }
        std::size_t tick_limit = 0;
        if (argc == 5) {
            const std::string count(argv[4]);
            if (std::string(argv[3]) != "--ticks" || count.empty() || count.find_first_not_of("0123456789") != std::string::npos) {
                throw std::invalid_argument(usage);
            }
            const auto value = std::stoull(count);
            if (value == 0 || value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("Tick limit must be a positive supported integer");
            }
            tick_limit = static_cast<std::size_t>(value);
        }
        comanchecd::TrainingFlight flight(comanchecd::Installation(argv[1], argv[2]));
        comanchecd::host::SdlFlightHost host;
        std::cout << "Arrows steer | E/D throttle | Space fires | F1-F6 left display | F7-F12 right display | P pauses | R resets | Esc quits\n";
        const auto ticks = comanchecd::host::run_viewer(flight, host, tick_limit);
        std::cout << "Completed " << ticks << " training preview ticks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
