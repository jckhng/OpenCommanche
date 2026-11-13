#include "startup_storage.hpp"
#include <algorithm>
#include <stdexcept>

namespace comanchecd {
std::vector<std::uint8_t> startup_depth_padding(const std::vector<std::uint8_t>& fanfare_score,
    const StartupMovieAssets& movie, std::uint8_t arena_phase) {
    constexpr std::size_t scene_allocation_bytes = 1024 * 1024;
    constexpr std::size_t depth_bytes = terrain_distance_capacity * scene_buffer_side;
    constexpr std::size_t resource_alignment = 4, depth_alignment = scene_buffer_side;
    constexpr std::size_t margin_bytes = (scene_buffer_side - scene_columns) / 2;
    // 5ff3..605d rewinds the arena and allocates two scene blocks, projection and depth.
    const auto projection = (arena_phase + 2 * scene_allocation_bytes + depth_alignment - 1) /
        depth_alignment * depth_alignment;
    const auto depth = projection + depth_bytes;
    std::vector<std::uint8_t> retained(depth_bytes);
    std::size_t cursor = arena_phase;
    const auto write_resource = [&](const std::vector<std::uint8_t>& encoded) {
        const auto decoded = decode_payload(encoded, PayloadKind::resource);
        cursor = (cursor + resource_alignment - 1) / resource_alignment * resource_alignment;
        const auto begin = std::max(cursor, depth);
        const auto end = std::min(cursor + decoded.bytes.size(), depth + depth_bytes);
        if (begin < end) {
            std::copy_n(decoded.bytes.begin() + (begin - cursor), end - begin, retained.begin() + (begin - depth));
        }
        cursor += decoded.bytes.size();
    };
    // The startup resource writes survive allocator reset at 5fcc. Allocation never clears RAM.
    write_resource(fanfare_score);
    write_resource(movie.score.encoded);
    write_resource(movie.sound.encoded);
    write_resource(movie.movie.encoded);
    std::vector<std::uint8_t> padding;
    padding.reserve(terrain_distance_capacity * margin_bytes * 2);
    for (std::size_t row = 0; row < terrain_distance_capacity; ++row) {
        const auto begin = retained.begin() + row * scene_buffer_side;
        padding.insert(padding.end(), begin, begin + margin_bytes);
        padding.insert(padding.end(), begin + margin_bytes + scene_columns, begin + scene_buffer_side);
    }
    return padding;
}

void apply_depth_padding(TerrainTables& tables, const std::vector<std::uint8_t>& padding) {
    constexpr auto row_bytes = scene_buffer_side - scene_columns;
    if (tables.levels == 0 || tables.levels > terrain_distance_capacity ||
            padding.size() != terrain_distance_capacity * row_bytes) {
        throw std::invalid_argument("Invalid retained depth allocation margins");
    }
    tables.depth_padding.assign(padding.begin(), padding.begin() + tables.levels * (scene_buffer_side - scene_columns));
}
}
