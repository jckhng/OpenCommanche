#include "comanchecd/assets.hpp"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::invalid_argument("demo_camera_probe INSTALLATION PAYLOAD_DIRECTORY INPUT OUTPUT");
        }
        const auto tables = comanchecd::Installation(argv[1], argv[2]).load_terrain_tables();
        const comanchecd::TerrainRenderer renderer(tables);
        std::ifstream input(argv[3]);
        std::ofstream output(argv[4]);
        std::uint32_t x, y, heading, target_x, target_y;
        unsigned ground;
        while (input >> x >> y >> heading >> ground >> target_x >> target_y) {
            if (ground > 255) {
                throw std::invalid_argument("Invalid camera ground height");
            }
            comanchecd::GameplaySetup setup;
            setup.x = x;
            setup.y = y;
            setup.heading = heading;
            comanchecd::MissionScene scene;
            scene.entities[comanchecd::player_slot].x = target_x;
            scene.entities[comanchecd::player_slot].y = target_y;
            comanchecd::SceneAssets assets;
            assets.terrain.height.assign(comanchecd::terrain_side * comanchecd::terrain_side, ground);
            comanchecd::FlightController flight(setup, scene, tables, 20);
            flight.recorded_key(8, assets);
            const auto view = flight.render_view(assets);
            const auto camera = renderer.camera(view);
            output << view.x << ' ' << view.y << ' ' << unsigned(view.height) << ' ' << view.heading << ' '
                   << camera.sine << ' ' << camera.cosine << ' ' << camera.pitch << ' ' << camera.roll << '\n';
        }
        if (!input.eof() || !output) {
            throw std::runtime_error("Invalid camera fixture input/output");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
