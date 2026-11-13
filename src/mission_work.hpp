#pragma once

#include "comanchecd/mission.hpp"

namespace comanchecd::mission_work {
void catalog(const CampaignMissions& campaign, const CpuWorkSink& work);
void briefing(const CampaignMissions& campaign, unsigned mission,
    const std::array<std::uint8_t, 256>& widths, const MissionResources& resources, const CpuWorkSink& work);
}
