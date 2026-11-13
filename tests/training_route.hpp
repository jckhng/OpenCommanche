#pragma once

#include "comanchecd/training.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace training_test {
constexpr unsigned victory_tick_limit = 2500;

class Route {
public:
    explicit Route(const comanchecd::Installation& installation) {
        const auto frontend = installation.load_frontend();
        const auto mission = installation.load_missions(0).missions.at(0);
        const auto setup = comanchecd::parse_gameplay(mission, frontend.mission_resources, frontend.gameplay_presets, {});
        for (const auto& object : setup.objects) {
            if (object.target != 0) {
                targets_.push_back(object);
            }
        }
        if (targets_.empty()) {
            throw std::runtime_error("Training route requires mission objectives");
        }
    }

    comanchecd::FlightControls controls(const comanchecd::FlightState& state, unsigned tick) const {
        constexpr unsigned waypoint_ticks = 250;
        constexpr double full_turn = 6.283185307179586;
        constexpr double heading_units = 4294967296.0;
        constexpr int steering_limit = 350;
        constexpr int heading_error_scale = 0x100000;
        constexpr int yaw_damping_scale = 0x40000;
        constexpr int approach_distance = 0x10000000;
        constexpr unsigned retained_rockets = 50;
        const auto& target = targets_[(tick / waypoint_ticks) % targets_.size()];
        const auto dx = std::int32_t(target.x - state.view.x);
        const auto dy = std::int32_t(target.y - state.view.y);
        auto angle = std::atan2(double(dx), -double(dy));
        if (angle < 0) {
            angle += full_turn;
        }
        const auto error = std::int32_t(std::uint32_t(angle * heading_units / full_turn) - state.view.heading);
        comanchecd::FlightControls result;
        result.throttle = 4;
        result.horizontal = std::clamp(error / heading_error_scale - state.yaw_rate / yaw_damping_scale,
                                       -steering_limit, steering_limit);
        result.vertical = std::max(std::abs(dx), std::abs(dy)) > approach_distance ? -steering_limit : 0;
        result.cycle_target = state.target.slot == 0 && tick % 4 == 0;
        result.weapon = state.ammunition[2] != 0 ? comanchecd::WeaponSelection::hellfire :
            state.ammunition[1] > retained_rockets ? comanchecd::WeaponSelection::rockets : comanchecd::WeaponSelection::cannon;
        result.fire = state.target.slot != 0 && (result.weapon == comanchecd::WeaponSelection::cannon || tick % 8 == 0);
        return result;
    }

private:
    std::vector<comanchecd::GameplayObject> targets_;
};
} // namespace training_test
