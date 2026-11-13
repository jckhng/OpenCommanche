#include "comanchecd/audio.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            throw std::invalid_argument("Usage: playback_probe INPUT_PCM OUTPUT_S16 OUTPUT_RATE");
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot read DSP PCM");
        }
        const auto rate = std::stoul(argv[3]);
        if (rate != 44100 && rate != 48000) {
            throw std::invalid_argument("Unsupported playback rate");
        }
        comanchecd::Sb16Playback playback(static_cast<unsigned>(rate));
        const std::vector<std::uint8_t> pcm{std::istreambuf_iterator<char>(input), {}};
        const auto samples = playback.render(pcm);
        std::ofstream output(argv[2], std::ios::binary);
        for (const auto sample : samples) {
            const auto bits = static_cast<std::uint16_t>(sample);
            output.put(static_cast<char>(bits));
            output.put(static_cast<char>(bits >> 8));
        }
        if (!output) {
            throw std::runtime_error("Cannot write playback samples");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
