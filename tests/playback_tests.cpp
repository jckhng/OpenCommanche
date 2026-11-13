#include "comanchecd/audio.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        std::vector<std::uint8_t> pcm;
        for (unsigned value = 0; value < 512; ++value) {
            pcm.push_back(static_cast<std::uint8_t>(value * 71));
        }
        for (const unsigned rate : {44100u, 48000u}) {
            comanchecd::Sb16Playback whole(rate), split(rate);
            const auto expected = whole.render(pcm);
            std::vector<std::int16_t> actual;
            for (const auto value : pcm) {
                const auto part = split.render({value});
                actual.insert(actual.end(), part.begin(), part.end());
                require(split.render({}).empty(), "Empty playback blocks must preserve the channel phase");
            }
            require(expected == actual, "Playback interpolation and filter history must be independent of source chunks");
            require(actual.size() == ((pcm.size() - 1) * rate + comanchecd::Sb16Playback::source_rate - 1) /
                    comanchecd::Sb16Playback::source_rate,
                    "Playback must retain the integer DSP rate and one lookahead sample");
            comanchecd::Sb16Playback silence(rate);
            const auto quiet = silence.render(std::vector<std::uint8_t>(32, 128));
            require(quiet == std::vector<std::int16_t>(quiet.size(), 0), "Unsigned DSP silence must remain signed PCM zero");
        }
        bool rejected = false;
        try {
            comanchecd::Sb16Playback invalid(0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Unsupported playback rates must reject before rendering");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
