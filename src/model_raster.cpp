#include "model_raster.hpp"
#include <stdexcept>

namespace comanchecd {
void rasterize_model(TerrainFrame& frame, const std::vector<std::uint8_t>& pixels, std::size_t source,
                     const std::vector<std::uint8_t>& steps, std::size_t step_offset, std::size_t depth_level,
                     const ModelSprite& sprite, const CpuWorkSink& work) {
    if (sprite.width == 0 || sprite.height == 0 || source > pixels.size() ||
            std::size_t(sprite.width) * sprite.height > pixels.size() - source ||
            (depth_level + 1) * scene_buffer_side > frame.depth.size()) {
        throw std::invalid_argument("Incomplete SG raster inputs");
    }
    const auto instructions = [&](unsigned count) { if (work) { work(CpuOperation::instructions, count); } };
    // 26f80: valid SG frame, including the mirrored dispatch and origin setup.
    instructions(16 + 5);
    const auto scaled_origin = [&](unsigned origin) {
        unsigned consumed = 0;
        std::size_t count = 0;
        do {
            consumed += steps.at(step_offset + count++);
            instructions(5);
        } while (consumed <= origin);
        return static_cast<std::int32_t>(count - 1);
    };
    const auto origin_x = scaled_origin(sprite.origin_x);
    auto x = static_cast<std::uint8_t>(sprite.x + (sprite.mirrored ? origin_x : -origin_x));
    instructions(4);
    const auto negative_origin = sprite.origin_y < 0;
    instructions(negative_origin ? 2 : 1);
    const auto origin_y = scaled_origin(negative_origin ? -int(sprite.origin_y) : sprite.origin_y);
    const auto top = int(sprite.y) + (negative_origin ? origin_y : -origin_y);
    instructions(2);
    // Positive origin uses ADD/JAE before INC DH: row zero is rejected too.
    if (top > 255 || (negative_origin ? top < 0 : top <= 0)) {
        instructions(2);
        return;
    }
    instructions(1 + 12);
    const auto player = sprite.horizon == ModelHorizon::player;
    const auto direction = sprite.mirrored ? -1 : 1;
    const auto carry_column = sprite.mirrored ? 0u : scene_buffer_side - 1;
    std::size_t source_x = 0, output_x = 0;
    while (source_x < sprite.width) {
        instructions(player ? 14 : 13);
        std::size_t source_y = 0, output_y = 0;
        constexpr unsigned depth_origin = 32;
        const auto horizon = player ? static_cast<std::uint8_t>(sprite.player_horizon) :
            x >= depth_origin && x < depth_origin + scene_columns ? frame.camera.horizon[x - depth_origin] : 0;
        const auto hidden = frame.depth[depth_level * scene_buffer_side + x];
        while (source_y < sprite.height) {
            const auto y = static_cast<std::uint8_t>(top + output_y);
            instructions(2);
            if (y >= hidden) {
                const auto color = pixels[source + source_x * sprite.height + source_y];
                instructions(3);
                if (color != 0) {
                    const auto target_y = static_cast<std::uint8_t>(y + horizon);
                    const auto target = std::size_t(target_y) * scene_buffer_side + (x >> 1) + ((x & 1) << 7);
                    frame.pixels[target] = color;
                    frame.write_mask[target] |= 4;
                    instructions(1);
                }
            }
            source_y += steps.at(step_offset + output_y++);
            instructions(7);
        }
        source_x += steps.at(step_offset + output_x++);
        // ADD/ADC and SUB/SBB retain the carry across the packed VGA column byte.
        const auto column_step = 1 + unsigned(x == carry_column);
        x = static_cast<std::uint8_t>(int(x) + direction * int(column_step));
        instructions(12);
    }
    instructions(player ? 1 : 2);
}
} // namespace comanchecd
