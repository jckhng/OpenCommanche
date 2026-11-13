#include "comanchecd/sound_device.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::invalid_argument("sound_device_probe EVENTS OUTPUT");
        }
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2], std::ios::binary);
        unsigned rate;
        std::size_t count;
        if (!(input >> rate >> count) || !output) {
            throw std::invalid_argument("Cannot read device fixture or write output");
        }
        struct Event { std::size_t frame; unsigned type, reg, value; };
        std::vector<Event> events;
        Event event;
        while (input >> event.frame >> event.type >> event.reg >> event.value) {
            if (event.frame >= count || event.type > 1 || event.reg > 255 || event.value > 255 ||
                    (!events.empty() && event.frame < events.back().frame)) {
                throw std::invalid_argument("Invalid device event");
            }
            events.push_back(event);
        }
        if (!input.eof()) {
            throw std::invalid_argument("Truncated device fixture");
        }
        comanchecd::SoundDevice device(rate);
        std::size_t cursor = 0;
        for (std::size_t frame = 0; frame < count; ++frame) {
            while (cursor < events.size() && events[cursor].frame == frame) {
                const auto& current = events[cursor++];
                if (current.type == 0) {
                    device.write_fm(static_cast<std::uint8_t>(current.reg), static_cast<std::uint8_t>(current.value));
                } else {
                    device.queue_digital(static_cast<std::uint8_t>(current.value));
                }
            }
            const auto sample = static_cast<std::uint16_t>(device.render_sample());
            output.put(static_cast<char>(sample));
            output.put(static_cast<char>(sample >> 8));
        }
        return output ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
