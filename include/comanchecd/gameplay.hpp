#pragma once

#include "comanchecd/mission.hpp"

#include <string>

namespace comanchecd {

struct GameplayObject {
    std::uint8_t target;
    std::uint8_t type;
    std::uint8_t category;
    std::uint8_t attribute2;
    std::uint8_t attribute3;
    std::uint32_t x;
    std::uint32_t y;
    std::uint8_t angle;
    std::uint8_t attribute7;
    std::uint8_t attribute8;
    std::uint8_t attribute9;
    std::uint32_t attribute10;
};

struct GameplayWaypoint {
    std::uint32_t x, y, altitude, speed;
};

struct GameplayLight {
    std::uint32_t x, y;
    std::uint8_t type;
};

struct GameplaySetup {
    std::uint8_t alternate_start = 0;
    std::uint8_t preset = 0;
    std::uint32_t preset_reference = 0;
    std::uint32_t x = 0, y = 0, heading = 0;
    std::array<std::uint8_t, 2> map_position{}; // Original 1aa29/1aa2a: chaff and flare stores.
    std::array<std::uint32_t, 6> vehicle_parameters{};
    std::uint32_t range = 0;
    std::array<std::uint8_t, 8> weapons{};
    std::uint32_t ammunition = 0;
    std::uint8_t target_count = 0, category_count = 0;
    std::uint8_t terrain_variant = 0, last_light_type = 0;
    std::array<std::uint8_t, 4> light_color{63, 63, 63, 0};
    std::array<std::uint8_t, 4> secondary_color{16, 32, 48, 0};
    std::vector<GameplayObject> objects;
    std::vector<GameplayWaypoint> waypoints;
    std::vector<GameplayLight> lights;
    std::vector<MissionFilename> overlays;
    std::array<std::uint8_t, 90> overlay_fields{};
};

GameplaySetup parse_gameplay(const MissionDefinition& definition, const MissionResources& resources,
                             const std::array<std::uint32_t, 6>& presets, const GameplaySetup& previous);

enum class PayloadKind { picture, resource };
enum class PayloadPolicy { original_loader, strict_frontend };

struct GameplayPayload {
    std::string name;
    PayloadKind kind;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> palette;
    // SG metadata copies can read into the allocation's alignment gap.
    std::vector<std::uint8_t> allocation_suffix;
};

using PayloadReader = std::function<GameplayPayload(const std::string&, PayloadKind)>;
GameplayPayload decode_payload(const std::vector<std::uint8_t>& file, PayloadKind kind,
                                PayloadPolicy policy = PayloadPolicy::original_loader);
std::vector<GameplayPayload> load_gameplay(const GameplaySetup& setup, const MissionResources& resources,
                                         const PayloadReader& reader);

} // namespace comanchecd
