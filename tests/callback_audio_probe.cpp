#include "comanchecd/callback_audio.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 4) { throw std::invalid_argument("callback_audio_probe EVENTS OUTPUT [--derive-fm]"); }
        const bool derive_fm = argc == 4 && std::string(argv[3]) == "--derive-fm";
        if (argc == 4 && !derive_fm) { throw std::invalid_argument("Unknown scheduler option"); }
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2], std::ios::binary);
        unsigned rate = 0;
        if (!(input >> rate) || !output) { throw std::invalid_argument("Cannot open callback fixture"); }
        comanchecd::CallbackAudio device(rate);
        std::array<comanchecd::AudioAccumulator, 2048> accumulator{};
        std::array<unsigned, 2> cursors{};
        bool interval_complete = false;
        char command;
        while (input >> command) {
            unsigned channel, frames;
            if (command == 'C') {
                comanchecd::AudioChannelConfig config;
                input >> channel >> config.enabled >> config.source_rate >> config.gain[0] >> config.gain[1] >> config.lowpass_alpha;
                device.configure(static_cast<comanchecd::AudioChannel>(channel), config);
            } else if (command == 'F') {
                unsigned reg, value;
                input >> reg >> value;
                if (reg > 255 || value > 255) { throw std::invalid_argument("Invalid OPL2 write"); }
                device.write_fm(static_cast<std::uint8_t>(reg), static_cast<std::uint8_t>(value));
            } else if (command == 'G') {
                input >> frames;
                if (!derive_fm) { device.generate_fm(frames); }
            } else if (command == 'D') {
                std::string hex;
                input >> hex;
                if (hex.size() % 2) { throw std::invalid_argument("Invalid digital bytes"); }
                std::vector<std::uint8_t> bytes;
                for (std::size_t i = 0; i < hex.size(); i += 2) {
                    bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
                }
                device.queue_digital(bytes);
            } else if (command == 'M') {
                input >> channel >> frames;
                auto& cursor = cursors.at(channel);
                const auto samples = derive_fm && channel == 0 ? device.service(comanchecd::AudioChannel::fm, frames) :
                    device.mix(static_cast<comanchecd::AudioChannel>(channel), frames);
                for (const auto& sample : samples) {
                    auto& sum = accumulator.at(cursor++);
                    for (unsigned c = 0; c < sum.size(); ++c) { sum[c] += sample[c]; }
                }
            } else if (command == 'E') {
                input >> channel >> frames;
                device.end_frame(static_cast<comanchecd::AudioChannel>(channel), frames);
                cursors.at(channel) = 0;
                interval_complete = true;
            } else if (command == 'O') {
                unsigned begin, end;
                std::array<std::int32_t, 2> gain{};
                input >> begin >> end >> gain[0] >> gain[1];
                for (unsigned i = begin; i < end; ++i) {
                    for (const auto sample : comanchecd::CallbackAudio::output(accumulator.at(i), gain)) {
                        const auto bits = static_cast<std::uint16_t>(sample);
                        output.put(static_cast<char>(bits));
                        output.put(static_cast<char>(bits >> 8));
                    }
                }
                if (interval_complete) { accumulator = {}; interval_complete = false; }
            } else {
                throw std::invalid_argument("Unknown callback event");
            }
            if (!input) { throw std::invalid_argument("Truncated callback event"); }
        }
        return input.eof() && output ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
