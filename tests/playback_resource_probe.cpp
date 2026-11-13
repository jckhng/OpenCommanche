#include "comanchecd/assets.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!file) {
        throw std::runtime_error("Cannot write playback fixture");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            throw std::invalid_argument("Usage: playback_resource_probe INSTALLATION PAYLOAD_DIRECTORY OUTPUT");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const std::filesystem::path output(argv[3]);
        if (std::filesystem::exists(output)) {
            throw std::invalid_argument("Playback output must be fresh");
        }
        std::filesystem::create_directories(output);
        const auto movie = installation.load_overview();
        std::vector<std::uint8_t> palette;
        for (const auto& color : movie.palette()) {
            palette.insert(palette.end(), color.begin(), color.end());
        }
        write(output / "palette.bin", palette);
        for (std::size_t index = 0; index < movie.frame_count(); ++index) {
            write(output / ("frame-" + std::to_string(index) + ".bin"), movie.frame(index));
        }
        for (unsigned index = 0; index < 4; ++index) {
            const auto demo = installation.load_demo(static_cast<std::uint8_t>(index));
            std::ofstream file(output / ("demo-" + std::to_string(index) + ".txt"));
            const auto& pose = demo.pose();
            file << pose.x << ' ' << pose.y << ' ' << pose.heading << ' ' << unsigned(pose.height) << '\n';
            for (const auto& input : demo.inputs()) {
                file << unsigned(input.key) << ' ' << unsigned(input.power) << ' ' << unsigned(input.trigger) << ' '
                     << unsigned(input.target) << ' ' << input.horizontal << ' ' << input.vertical << '\n';
            }
            if (!file) {
                throw std::runtime_error("Cannot write demo fixture");
            }
        }
        std::cout << "Decoded " << movie.frame_count() << " KDV frames and four demo recordings\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
