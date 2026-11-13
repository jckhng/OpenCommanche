#pragma once

#include "comanchecd/scene.hpp"

#include <filesystem>
#include <fstream>

namespace scene_output {
inline void binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Cannot write scene resource");
    }
}

inline void write(const std::filesystem::path& directory, const comanchecd::SceneAssets& assets,
                  const comanchecd::MissionScene& scene) {
    std::filesystem::create_directories(directory / "models");
    binary(directory / "height.bin", assets.terrain.height);
    binary(directory / "color.bin", assets.terrain.color);
    std::vector<std::uint8_t> palette;
    for (const auto& color : assets.terrain.palette) {
        palette.insert(palette.end(), color.begin(), color.end());
    }
    binary(directory / "palette.bin", palette);
    for (const auto& model : assets.models) {
        binary(directory / "models" / (std::to_string(model.payload) + ".bin"), model.bytes);
    }
    std::ofstream entities(directory / "entities.txt");
    for (std::size_t slot = 0; slot < scene.entities.size(); ++slot) {
        const auto& entity = scene.entities[slot];
        if (!entity.active) {
            continue;
        }
        entities << slot << ' ' << unsigned(entity.kind) << ' ' << entity.primary_model << ' ' << entity.secondary_model
                 << ' ' << entity.x << ' ' << entity.y << ' ' << entity.speed << ' ' << entity.attribute10
                 << ' ' << unsigned(entity.angle) << ' ' << unsigned(entity.height) << ' ' << unsigned(entity.target)
                 << ' ' << unsigned(entity.attribute7) << ' ' << unsigned(entity.attribute9)
                 << ' ' << unsigned(entity.category_color) << ' ' << unsigned(entity.random_phase) << '\n';
    }
    std::ofstream state(directory / "state.txt");
    state << scene.random_seed << ' ' << scene.wingman << ' ' << unsigned(scene.player_height);
    for (const auto value : scene.vehicle_parameters) {
        state << ' ' << value;
    }
    state << ' ' << scene.range << '\n';
    if (!state || !entities) {
        throw std::runtime_error("Cannot write constructed scene state");
    }
}
}
