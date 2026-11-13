#pragma once

#include "comanchecd/audio.hpp"

#include <filesystem>
#include <fstream>

namespace audio_output {
inline void integer(std::ostream& file, std::uint32_t value, std::size_t width) {
    for (std::size_t byte = 0; byte < width; ++byte) {
        file.put(static_cast<char>(value >> (byte * 8)));
    }
}

inline void write_samples(const std::filesystem::path& directory, const comanchecd::MissionAudio& audio,
                          const std::vector<std::int16_t>& samples) {
    std::filesystem::create_directories(directory);
    std::ofstream wave(directory / "mission.wav", std::ios::binary);
    const auto size = static_cast<std::uint32_t>(samples.size() * 2);
    wave.write("RIFF", 4);
    integer(wave, size + 36, 4);
    wave.write("WAVEfmt ", 8);
    integer(wave, 16, 4);
    integer(wave, 1, 2);
    integer(wave, 1, 2);
    integer(wave, comanchecd::MissionAudio::sample_rate, 4);
    integer(wave, comanchecd::MissionAudio::sample_rate * 2, 4);
    integer(wave, 2, 2);
    integer(wave, 16, 2);
    wave.write("data", 4);
    integer(wave, size, 4);
    for (const auto sample : samples) {
        integer(wave, static_cast<std::uint16_t>(sample), 2);
    }
    std::ofstream pcm(directory / "digital.bin", std::ios::binary);
    const auto& digital = audio.digital_pcm();
    pcm.write(reinterpret_cast<const char*>(digital.data()), static_cast<std::streamsize>(digital.size()));
    std::ofstream events(directory / "opl.txt");
    for (const auto& write : audio.writes()) {
        events << write.tick << ' ' << unsigned(write.reg) << ' ' << unsigned(write.value) << '\n';
    }
    if (!wave || !pcm || !events) {
        throw std::runtime_error("Cannot write mission audio output");
    }
}

inline void write(const std::filesystem::path& directory, comanchecd::MissionAudio& audio, unsigned seconds) {
    const auto samples = audio.render(std::size_t(seconds) * comanchecd::MissionAudio::sample_rate);
    write_samples(directory, audio, samples);
}
}
