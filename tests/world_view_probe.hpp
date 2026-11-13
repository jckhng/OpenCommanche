#pragma once

#include "world_output.hpp"

namespace world_view_probe {
inline void run(const std::filesystem::path& movie, const std::filesystem::path& output,
                const comanchecd::GameplaySetup& setup, const comanchecd::SceneAssets& assets,
                const comanchecd::MissionScene& scene, comanchecd::TerrainTables tables) {
    std::ifstream input(movie);
    if (!input) {
        throw std::runtime_error("Cannot read controlled scene views");
    }
    world_output::read_depth_padding(input, tables);
    const comanchecd::TerrainRenderer renderer(std::move(tables));
    comanchecd::SceneView view;
    std::uint64_t heading, x, y, object_x, object_y;
    unsigned height, slot, model, object_height, angle;
    std::size_t index = 0;
    while (input >> heading) {
        if (!(input >> view.pitch >> view.roll >> x >> y >> height >> slot >> model >> object_x >> object_y >> object_height >> angle) ||
                heading > 0xffffffffu || x > 0xffffffffu || y > 0xffffffffu || height > 255 ||
                slot >= comanchecd::scene_slots || model >= assets.model_payloads.size() ||
                object_x > 0xffffffffu || object_y > 0xffffffffu || object_height > 255 || angle > 255 || index >= 64) {
            throw std::invalid_argument("Invalid controlled scene view");
        }
        view.heading = static_cast<std::uint32_t>(heading);
        view.x = static_cast<std::uint32_t>(x);
        view.y = static_cast<std::uint32_t>(y);
        view.height = static_cast<std::uint8_t>(height);
        const auto directory = output / "views" / std::to_string(index++);
        auto frame = renderer.render(assets, view);
        world_output::write(directory, renderer, frame);
        renderer.shade_distance(frame, assets, setup.terrain_variant);
        world_output::bytes(directory / "fog.bin", frame.pixels);
        auto controlled_scene = scene;
        if (slot != 0) {
            auto& entity = controlled_scene.entities[slot];
            if (!entity.active || entity.kind != 3) {
                throw std::invalid_argument("Controlled model replacement requires a stationary scene prop");
            }
            entity.primary_model = model;
            entity.x = static_cast<std::uint32_t>(object_x);
            entity.y = static_cast<std::uint32_t>(object_y);
            entity.height = static_cast<std::uint8_t>(object_height);
            entity.angle = static_cast<std::uint8_t>(angle);
        }
        renderer.draw_models(frame, assets, controlled_scene, view);
        world_output::bytes(directory / "models.bin", frame.pixels);
        world_output::bytes(directory / "model-coverage.bin", frame.write_mask);
    }
    if (!input.eof() || index == 0) {
        throw std::invalid_argument("Truncated or empty controlled scene views");
    }
}
}
