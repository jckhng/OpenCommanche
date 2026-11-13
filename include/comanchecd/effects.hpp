#pragma once

#include "comanchecd/world_renderer.hpp"

namespace comanchecd {
class ExecutionClock;
enum class EffectLayer { background, foreground };

struct EffectParticle {
    std::uint8_t x = 0, y = 0, speed = 0;
    std::int32_t depth_level = -1;
};

struct EffectsState {
    EffectsState();
    std::array<EffectParticle, 288> particles{};
    std::uint32_t gust_phase = 0;
    std::array<std::uint8_t, 17> drift{};
};

class ScriptedEffects {
public:
    ScriptedEffects(const GameplaySetup& setup, const Palette& palette, const TerrainTables& tables,
                    EffectsState entry = {});
    void draw(TerrainFrame& frame, const SceneView& view, std::uint32_t tick, std::int32_t yaw,
              std::int32_t side_speed, std::uint32_t& random, EffectLayer layer, ExecutionClock* clock = nullptr);
    void wind(std::uint32_t tick, std::uint32_t heading, std::uint32_t camera_heading,
              std::int32_t& forward, std::int32_t& side, std::uint32_t& random, ExecutionClock* clock = nullptr);
    const EffectsState& state() const;

private:
    GameplaySetup setup_;
    TerrainTables tables_;
    EffectsState state_;
    std::array<ColorMap, 2> colors_{};
    std::uint8_t solid_color_ = 0, type_ = 0;
    bool active_ = false;
    void particles(TerrainFrame& frame, const SceneView& view, std::int32_t motion,
                   std::uint32_t& random, std::size_t group, ExecutionClock* clock);
};
}
