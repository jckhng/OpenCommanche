#pragma once

#include "comanchecd/world_renderer.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace world_output {
inline void read_depth_padding(std::istream& input, comanchecd::TerrainTables& tables) {
    input >> std::ws;
    if (input.peek() != 'd') {
        return;
    }
    std::string marker, encoded;
    const auto bytes = tables.levels * (comanchecd::scene_buffer_side - comanchecd::scene_columns);
    if (!(input >> marker >> encoded) || marker != "depth-padding" || encoded.size() != bytes * 2 ||
            encoded.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw std::invalid_argument("Invalid declared terrain depth padding");
    }
    tables.depth_padding.resize(bytes);
    for (std::size_t index = 0; index < bytes; ++index) {
        tables.depth_padding[index] = static_cast<std::uint8_t>(std::stoul(encoded.substr(index * 2, 2), nullptr, 16));
    }
}

template<class Bytes>
void bytes(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!output) {
        throw std::runtime_error("Cannot write terrain rendering output");
    }
}

inline void write(const std::filesystem::path& directory, const comanchecd::TerrainRenderer& renderer,
                  const comanchecd::TerrainFrame& frame) {
    std::filesystem::create_directories(directory);
    bytes(directory / "projection.bin", renderer.projection());
    bytes(directory / "sky.bin", frame.sky);
    bytes(directory / "terrain.bin", frame.pixels);
    bytes(directory / "coverage.bin", frame.write_mask);
    bytes(directory / "depth.bin", frame.depth);
    bytes(directory / "horizon.bin", frame.camera.horizon);
    std::ofstream camera(directory / "camera.txt");
    camera << frame.camera.sine << ' ' << frame.camera.cosine << ' '
           << frame.camera.pitch << ' ' << frame.camera.roll << '\n';
    if (!camera) {
        throw std::runtime_error("Cannot write scene camera output");
    }
}
}
