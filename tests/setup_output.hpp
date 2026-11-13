#pragma once

#include "comanchecd/gameplay.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace setup_output {
using Bytes = std::vector<std::uint8_t>;
using Snapshot = std::map<std::uint32_t, Bytes>;

inline void append(Bytes& output, std::uint32_t value, std::size_t width = 4) {
    for (std::size_t byte = 0; byte < width; ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
    }
}

inline Snapshot snapshot(const comanchecd::GameplaySetup& setup) {
    Snapshot result;
    const auto field = [&](std::uint32_t address, std::uint32_t value, std::size_t width = 4) {
        append(result[address], value, width);
    };
    field(0x236af, setup.alternate_start, 1);
    field(0x2dc81, setup.preset, 1);
    field(0x2e290, setup.preset_reference);
    for (const auto value : {setup.x, setup.y, setup.x, setup.y}) {
        field(0x1a970, value);
    }
    field(0x1a9a0, setup.heading);
    result[0x1aa29] = {setup.map_position.begin(), setup.map_position.end()};
    for (const auto value : setup.vehicle_parameters) {
        field(0x1a9e0, value);
    }
    field(0x1a9e0, setup.range);
    result[0x1aac8] = {setup.weapons.begin(), setup.weapons.end()};
    field(0x1aac8, setup.ammunition);
    field(0x1aac8, setup.ammunition);
    field(0x2b687, setup.target_count, 1);
    field(0x2b689, setup.target_count, 1);
    field(0x1a938, setup.lights.empty() ? 0x1a910 : 0x28105);
    field(0x1a938, setup.lights.empty() ? 0x1a910 : 0x278fe);
    result[0x27556] = {setup.terrain_variant, setup.last_light_type};
    field(0x29690, setup.terrain_variant ? 103 : 100, 1);
    result[0x27a7c] = {setup.light_color.begin(), setup.light_color.end()};
    result[0x27a7c].insert(result[0x27a7c].end(), setup.secondary_color.begin(), setup.secondary_color.end());
    field(0x1aa00, static_cast<std::uint32_t>(setup.objects.size()));
    field(0x27a78, static_cast<std::uint32_t>(setup.lights.size()));
    field(0x27fa8, static_cast<std::uint32_t>(setup.waypoints.size()));
    field(0x2dba0, static_cast<std::uint32_t>(setup.overlays.size()));
    field(0x2b69c, setup.category_count, 1);
    result[0x2dc1c] = {setup.overlay_fields.begin(), setup.overlay_fields.end()};
    for (const auto address : {0x234af, 0x23435, 0x23472, 0x234ec, 0x23529, 0x2311c,
                               0x23210, 0x233f8, 0x23566, 0x235a3, 0x235e0, 0x23304,
                               0x27fac, 0x27fd4, 0x27ffc, 0x28024, 0x27a84, 0x27aac, 0x27ad4, 0x2dba4}) {
        result[address] = {};
    }
    for (const auto& object : setup.objects) {
        field(0x234af, object.target, 1);
        field(0x23435, object.type, 1);
        field(0x23472, object.category, 1);
        field(0x234ec, object.attribute2, 1);
        field(0x23529, object.attribute3, 1);
        field(0x2311c, object.x);
        field(0x23210, object.y);
        field(0x233f8, object.angle, 1);
        field(0x23566, object.attribute7, 1);
        field(0x235a3, object.attribute8, 1);
        field(0x235e0, object.attribute9, 1);
        field(0x23304, object.attribute10);
    }
    for (const auto& point : setup.waypoints) {
        field(0x27fac, point.x);
        field(0x27fd4, point.y);
        field(0x27ffc, point.altitude);
        field(0x28024, point.speed);
    }
    for (const auto& light : setup.lights) {
        field(0x27a84, light.x);
        field(0x27aac, light.y);
        field(0x27ad4, light.type, 1);
    }
    for (const auto& name : setup.overlays) {
        result[0x2dba4].insert(result[0x2dba4].end(), name.begin(), name.end());
    }
    return result;
}

inline void write(const std::filesystem::path& directory, const comanchecd::GameplaySetup& setup) {
    std::filesystem::create_directories(directory);
    for (const auto& entry : snapshot(setup)) {
        std::ostringstream name;
        name << std::hex << entry.first << ".bin";
        std::ofstream file(directory / name.str(), std::ios::binary);
        file.write(reinterpret_cast<const char*>(entry.second.data()), static_cast<std::streamsize>(entry.second.size()));
        if (!file) {
            throw std::runtime_error("Cannot write gameplay setup snapshot");
        }
    }
}
}
