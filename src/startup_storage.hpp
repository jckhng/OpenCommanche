#pragma once

#include "comanchecd/audio.hpp"
#include "comanchecd/world_renderer.hpp"

namespace comanchecd {
// Low-byte arena origin for the qualified DOS loader layout.
constexpr std::uint8_t startup_arena_phase = 0xc0;

std::vector<std::uint8_t> startup_depth_padding(const std::vector<std::uint8_t>& fanfare_score,
    const StartupMovieAssets& movie, std::uint8_t arena_phase = startup_arena_phase);
void apply_depth_padding(TerrainTables& tables, const std::vector<std::uint8_t>& padding);
}
