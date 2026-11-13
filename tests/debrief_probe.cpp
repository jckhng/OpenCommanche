#include "comanchecd/assets.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::invalid_argument("Usage: debrief_probe INSTALLATION PAYLOAD_DIRECTORY FIXTURE OUTPUT");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const auto assets = installation.load_frontend();
        const std::filesystem::path fixture(argv[3]), output(argv[4]);
        std::ifstream input(fixture / "input.txt");
        std::string mode;
        input >> mode;
        if (!input || std::filesystem::exists(output)) {
            throw std::invalid_argument("Use a readable fixture and fresh output directory");
        }
        comanchecd::MissionResult result;
        if (mode == "evaluate") {
            comanchecd::GameplaySetup initial;
            comanchecd::FlightState player;
            comanchecd::MissionScene scene;
            unsigned category, height, variant, wingman;
            input >> initial.ammunition >> player.armor >> initial.range >> player.range
                  >> category >> height >> variant >> wingman;
            initial.category_count = static_cast<std::uint8_t>(category);
            initial.terrain_variant = static_cast<std::uint8_t>(variant);
            player.view.height = static_cast<std::uint8_t>(height);
            scene.wingman = wingman != 0 ? 60 : 0;
            for (auto& value : initial.vehicle_parameters) {
                input >> value;
            }
            for (auto& value : player.ammunition) {
                input >> value;
            }
            for (std::size_t slot = 1; slot < comanchecd::player_slot; ++slot) {
                auto& entity = scene.entities[slot];
                unsigned active, color, target;
                input >> active >> color >> target;
                entity.active = active != 0;
                entity.category_color = static_cast<std::uint8_t>(color);
                entity.target = static_cast<std::uint8_t>(target);
            }
            result = comanchecd::evaluate_mission(initial, player, scene);
        } else if (mode == "progress") {
            unsigned survived, completed, message;
            input >> survived >> completed >> result.score >> result.enemies_destroyed >> message;
            result.survived = survived != 0;
            result.completed = completed != 0;
            result.message = static_cast<comanchecd::DebriefMessage>(message);
            std::ifstream record(fixture / "pilots.bin", std::ios::binary);
            comanchecd::PilotStorage pilots({std::istreambuf_iterator<char>(record), {}});
            comanchecd::CampaignMissions campaign;
            campaign.missions.resize(comanchecd::missions_per_campaign);
            for (auto& mission : campaign.missions) {
                unsigned restricted, alternate;
                input >> restricted >> alternate;
                mission.restricted = restricted != 0;
                mission.alternate_campaign = static_cast<std::uint8_t>(alternate);
            }
            pilots.record_result(result, campaign);
            std::filesystem::create_directories(output);
            std::ofstream out(output / "pilots.bin", std::ios::binary);
            out.write(reinterpret_cast<const char*>(pilots.record().data()), pilots.record().size());
        } else {
            throw std::invalid_argument("Unknown debrief fixture operation");
        }
        if (!input) {
            throw std::invalid_argument("Truncated debrief fixture");
        }
        std::filesystem::create_directories(output);
        std::ofstream state(output / "state.txt");
        state << result.survived << ' ' << result.completed << ' ' << result.score << ' ' << result.enemies_destroyed;
        for (const auto value : result.percentages) {
            state << ' ' << value;
        }
        state << ' ' << unsigned(result.message) << '\n';
        const auto text = comanchecd::debrief_text(assets.debrief, result);
        std::ofstream stream(output / "text.bin", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(text.data()), text.size());
        const auto picture = installation.load_picture(comanchecd::FrontendScene::debrief);
        comanchecd::PlanarSurface surface(std::vector<std::uint8_t>(comanchecd::plane_count * comanchecd::plane_bytes));
        surface.restore(0, picture.background);
        surface.restore(comanchecd::page_displacement, picture.background);
        comanchecd::MenuRenderer renderer(std::move(surface), comanchecd::Font(assets.font),
            comanchecd::build_theme(picture.palette, assets.transfer), comanchecd::build_box_remap(picture.palette, assets.transfer),
            picture.background, 0, 0);
        renderer.draw_debrief(text);
        std::ofstream planes(output / "planes.bin", std::ios::binary);
        const auto& bytes = renderer.surface().bytes();
        planes.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
