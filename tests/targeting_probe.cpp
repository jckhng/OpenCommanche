#include "comanchecd/assets.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            throw std::invalid_argument("Usage: targeting_probe INSTALLATION PAYLOAD_DIRECTORY FIXTURE");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const auto frontend = installation.load_frontend();
        auto resources = frontend.mission_resources;
        const auto mission = installation.load_missions(0).missions.at(0);
        for (std::size_t index = 11; index < resources.size(); ++index) {
            resources[index][0] = '-';
        }
        std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
        const auto setup = comanchecd::parse_gameplay(mission, resources, frontend.gameplay_presets, {});
        const auto payloads = comanchecd::load_gameplay(setup, resources,
            [&](const auto& name, auto kind) { return installation.load_payload(name, kind); });
        const auto assets = comanchecd::materialize_scene(setup, payloads, frontend.transfer);
        const auto scene = comanchecd::construct_scene(setup, assets, 1);
        const auto tables = installation.load_terrain_tables();
        const comanchecd::TerrainRenderer renderer(tables);
        std::ifstream input(argv[3]);
        unsigned count;
        input >> count;
        for (unsigned index = 0; index < count; ++index) {
            comanchecd::SceneView view;
            comanchecd::MissionScene projected;
            unsigned height, hidden, entities;
            input >> view.x >> view.y >> view.heading >> height >> view.pitch >> view.roll >> hidden >> entities;
            if (!input || height > 255 || hidden > 255 || entities > 60) {
                throw std::invalid_argument("Invalid projection fixture");
            }
            view.height = static_cast<std::uint8_t>(height);
            for (unsigned slot = 1; slot <= entities; ++slot) {
                auto& entity = projected.entities[slot];
                entity = scene.entities[60];
                unsigned color;
                input >> entity.x >> entity.y >> height >> color;
                entity.height = static_cast<std::uint8_t>(height);
                entity.category_color = static_cast<std::uint8_t>(color);
            }
            comanchecd::TerrainFrame frame;
            frame.camera = renderer.camera(view);
            frame.depth.assign(tables.levels * comanchecd::scene_buffer_side, static_cast<std::uint8_t>(hidden));
            renderer.draw_models(frame, assets, projected, view);
            std::cout << index << ' ' << frame.targeting.candidate;
            for (unsigned slot = 1; slot <= entities; ++slot) {
                const auto& target = frame.targeting.entities[slot];
                std::cout << ' ' << target.depth << ' ' << unsigned(target.x) << ' ' << unsigned(target.y)
                          << ' ' << unsigned(target.bearing);
            }
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
