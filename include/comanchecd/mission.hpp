#pragma once

#include "comanchecd/menu_renderer.hpp"

namespace comanchecd {

constexpr std::size_t missions_per_campaign = 10;
constexpr std::size_t mission_resource_count = 26;
constexpr std::size_t mission_filename_bytes = 12;
constexpr std::size_t mission_menu_header_bytes = 20;
constexpr std::size_t mission_menu_row_bytes = 32;
using MissionFilename = std::array<std::uint8_t, mission_filename_bytes>;
using MissionResources = std::array<MissionFilename, mission_resource_count>;

struct MissionDefinition {
    std::vector<std::uint8_t> title;
    std::vector<std::uint8_t> description;
    std::vector<MissionFilename> resources;
    std::vector<std::uint8_t> setup_text;
    bool restricted = false;
    std::uint8_t alternate_campaign = 0;
    std::size_t source_offset = 0;
};

struct CampaignMissions {
    std::vector<MissionDefinition> missions;
    std::vector<std::uint8_t> source;
};

enum class CampaignLoad { catalog, briefing };

struct MissionPresentation {
    std::vector<std::uint8_t> menu;
    std::vector<std::uint8_t> briefing;
    MissionResources resources;
    std::array<std::uint8_t, missions_per_campaign> restricted{};
    std::array<std::uint8_t, missions_per_campaign> colors{};
    std::array<std::uint8_t, missions_per_campaign> alternate{};
};

CampaignMissions parse_campaign(const std::vector<std::uint8_t>& decoded);
void populate_missions(MissionPresentation& presentation, const CampaignMissions& campaign);
std::vector<std::uint8_t> wrap_briefing(const std::vector<std::uint8_t>& menu, std::uint8_t mission,
                                      const MissionDefinition& definition,
                                      const std::array<std::uint8_t, 256>& widths);

} // namespace comanchecd
