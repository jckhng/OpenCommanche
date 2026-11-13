#pragma once
#include "comanchecd/assets.hpp"

namespace comanchecd {
std::vector<GameplayPayload> load_mission_payloads(const Installation& installation, const GameplaySetup& setup,
    const MissionResources& resources, const Transfer& transfer, TerrainDetail detail, MissionAudio& audio);
}
