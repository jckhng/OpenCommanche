#pragma once
#include "comanchecd/world_renderer.hpp"

namespace comanchecd {
enum class ModelHorizon { terrain, player };
struct ModelSprite {
    std::uint8_t width, height, origin_x;
    std::int8_t origin_y;
    std::uint8_t x, y;
    bool mirrored;
    ModelHorizon horizon;
    std::int32_t player_horizon = 0;
};
void rasterize_model(TerrainFrame& frame, const std::vector<std::uint8_t>& pixels, std::size_t source,
                     const std::vector<std::uint8_t>& steps, std::size_t step_offset, std::size_t depth_level,
                     const ModelSprite& sprite, const CpuWorkSink& work = {});
} // namespace comanchecd
