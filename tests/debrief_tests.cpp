#include "comanchecd/debrief.hpp"
#include "comanchecd/pilot.hpp"

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
        using namespace comanchecd;
        GameplaySetup setup;
        setup.ammunition = 4;
        setup.range = 4000;
        setup.category_count = 1;
        setup.vehicle_parameters[2] = 8;
        FlightState player;
        player.armor = 4;
        player.range = 3000;
        player.ammunition = setup.vehicle_parameters;
        MissionScene scene;
        scene.entities[1].target = 1;
        auto result = evaluate_mission(setup, player, scene);
        require(result.survived && result.completed && result.score == 3707 && result.enemies_destroyed == 1,
                "A completed objective earns its recovered bonus, armor weighting and resource percentages");
        player.armor = -1;
        result = evaluate_mission(setup, player, scene);
        require(!result.survived && result.completed && result.score == 0 && result.message == DebriefMessage::wrecked,
                "Destroying the objectives does not award a mission score after a fatal crash");
        player.armor = 0;
        result = evaluate_mission(setup, player, scene);
        require(result.survived && result.percentages[0] == 20, "Armor zero retains the original final surviving unit");
        scene.entities[1].active = true;
        scene.entities[1].category_color = 1;
        player.range = std::uint32_t(-270);
        result = evaluate_mission(setup, player, scene);
        require(!result.completed && result.message == DebriefMessage::fuel && result.percentages[4] == 0,
                "Negative fuel reports zero remaining and the original fuel-abort message");

        setup = {};
        setup.ammunition = 4;
        setup.range = 4000;
        setup.category_count = 1;
        setup.vehicle_parameters = {500, 64, 8, 8, 8, 0};
        player = {};
        player.armor = -2;
        player.range = std::uint32_t(-270);
        player.view.height = 0;
        scene = {};
        scene.entities[1].target = 1;
        std::vector<std::pair<CpuOperation, unsigned>> work;
        const CpuWorkSink account = [&](CpuOperation operation, unsigned count) { work.emplace_back(operation, count); };
        result = evaluate_mission(setup, player, scene, account);
        require(work == std::vector<std::pair<CpuOperation, unsigned>>{
                    {CpuOperation::instructions, 499}, {CpuOperation::repeat_copy_bytes, 47},
                    {CpuOperation::instructions, 21}},
                "Fatal return must clamp fuel and armor before the result-message copy and score formatting");
        player.armor = 0;
        player.ammunition = {250, 32, 4, 4, 4, 0};
        work.clear();
        result = evaluate_mission(setup, player, scene, account);
        require(result.score == 2220 && work == std::vector<std::pair<CpuOperation, unsigned>>{
                    {CpuOperation::instructions, 511}, {CpuOperation::repeat_copy_bytes, 47},
                    {CpuOperation::instructions, 44}},
                "Surviving return must retain percentage branches and each decimal score digit");

        PilotStorage pilots{std::vector<std::uint8_t>(pilot_record_bytes)};
        pilots.select(3);
        pilots.choose_campaign(3, 0);
        pilots.choose_mission(3, 0, 1);
        CampaignMissions campaign;
        campaign.missions.resize(10);
        campaign.missions[1].restricted = true;
        result = {};
        result.survived = result.completed = true;
        result.score = 2000;
        pilots.record_result(result, campaign);
        require(pilots.selected() == 0 && pilots.completed(0, 0, 1),
                "Mission completion follows the selected pilot through the original ranking sort");
        result.survived = false;
        pilots.record_result(result, campaign);
        require(!pilots.completed(pilots.selected(), 0, 1) && result.message == DebriefMessage::setback,
                "A crash rolls back restricted progress before showing the setback message");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
