#include "comanchecd/assets.hpp"
#include "comanchecd/flight.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            throw std::invalid_argument("Usage: combat_probe INSTALLATION PAYLOAD_DIRECTORY FIXTURE");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const auto tables = installation.load_terrain_tables();
        std::ifstream input(argv[3]);
        std::size_t cases;
        if (!(input >> cases) || cases > 10000) {
            throw std::invalid_argument("Invalid combat fixture count");
        }
        for (std::size_t index = 0; index < cases; ++index) {
            comanchecd::GameplaySetup setup;
            setup.x = setup.y = 0x80000000;
            setup.ammunition = 100;
            comanchecd::MissionScene scene;
            scene.range = 1000;
            scene.player_height = 100;
            auto& missile = scene.entities[140];
            auto& target = scene.entities[1];
            unsigned height, angle, age, target_height, calls;
            input >> missile.x >> missile.y >> height >> angle >> age >> missile.speed
                  >> target.x >> target.y >> target_height >> calls;
            if (!input || height > 255 || angle > 255 || age > 255 || target_height > 255 || calls > 128) {
                throw std::invalid_argument("Invalid combat fixture");
            }
            missile.active = target.active = true;
            missile.kind = comanchecd::guided_missile_kind;
            missile.height = static_cast<std::uint8_t>(height);
            missile.angle = static_cast<std::uint8_t>(angle);
            missile.random_phase = static_cast<std::uint8_t>(age);
            target.kind = 3;
            target.height = static_cast<std::uint8_t>(target_height);
            target.attribute7 = 255;
            comanchecd::SceneAssets assets;
            assets.terrain.height.assign(comanchecd::terrain_side * comanchecd::terrain_side, 0);
            comanchecd::FlightController flight(setup, scene, tables, 20);
            comanchecd::TargetingFrame targeting;
            targeting.candidate = 1;
            targeting.entities[1].x = 80;
            comanchecd::FlightControls controls;
            controls.cycle_target = true;
            flight.step(controls, assets, {}, &targeting);
            for (unsigned call = 0; call < calls; ++call) {
                flight.update_entities(assets);
                const auto& result = flight.scene().entities[140];
                std::cout << index << ' ' << call << ' ' << result.x << ' ' << result.y << ' '
                          << unsigned(result.height) << ' ' << unsigned(result.angle) << ' '
                          << unsigned(result.random_phase) << ' ' << unsigned(result.phase) << '\n';
            }
        }
        input >> std::ws;
        if (!input.eof()) {
            throw std::invalid_argument("Trailing combat fixture input");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
