#pragma once

#include "comanchecd/flight.hpp"

namespace comanchecd {
enum class DebriefMessage : std::uint8_t { success, aborted, wrecked, defeated, wingman, fuel, setback, sunk };

struct MissionResult {
    bool survived = false, completed = false;
    std::uint32_t score = 0, enemies_destroyed = 0;
    std::array<std::uint32_t, 5> percentages{};
    DebriefMessage message = DebriefMessage::aborted;
};

struct DebriefAssets {
    std::vector<std::uint8_t> text;
    std::array<std::vector<std::uint8_t>, 8> messages;
};

MissionResult evaluate_mission(const GameplaySetup& initial, const FlightState& player, const MissionScene& scene,
                               const CpuWorkSink& work = {});
std::vector<std::uint8_t> debrief_text(const DebriefAssets& assets, const MissionResult& result);
} // namespace comanchecd
