#pragma once

#include "comanchecd/scene.hpp"
#include "comanchecd/options.hpp"
#include "comanchecd/cpu_work.hpp"

namespace comanchecd {
constexpr std::size_t scene_columns = 192;
constexpr std::size_t scene_buffer_side = 256;
constexpr std::size_t terrain_distance_capacity = 250;

enum class TerrainDetail { high, medium, low };

struct TerrainTables {
    std::array<std::int32_t, 641> sine{};
    std::uint32_t levels = 0;
    std::array<std::array<std::uint32_t, terrain_distance_capacity>, 5> distance{};
    std::vector<std::uint8_t> depth_padding;
    std::array<std::uint32_t, 8> angle_octants{};
    std::array<std::uint8_t, 1025> angle_ratio{};
    std::array<std::uint32_t, 128> sprite_offsets{};
    std::vector<std::uint8_t> sprite_steps;
    Transfer transfer{};
    std::uint8_t initial_height_gap = 0;
    std::array<std::int8_t, 156> gust{};
};

enum class SceneViewMode { cockpit, chase, fixed };

struct SceneView {
    std::uint32_t x = 0, y = 0, heading = 0;
    std::int32_t pitch = 0, roll = 0, vertical_pan = 0;
    std::uint8_t height = 0;
    SceneViewMode mode = SceneViewMode::cockpit;
    std::int32_t body_roll = 0;
};

struct SceneCamera {
    std::int32_t sine = 0, cosine = 0, pitch = 0, roll = 0;
    std::array<std::uint8_t, scene_columns> horizon{};
};

struct TargetProjection {
    std::int32_t depth = 0;
    std::uint8_t x = 0, y = 0, bearing = 0;
};

struct TargetingFrame {
    std::array<TargetProjection, scene_slots> entities{};
    std::size_t candidate = 0;
};

struct TerrainFrame {
    SceneCamera camera;
    TargetingFrame targeting;
    std::array<std::uint8_t, scene_buffer_side * scene_buffer_side> sky{}, pixels{}, write_mask{};
    std::vector<std::uint8_t> depth;
};

struct ReflectionState {
    std::uint32_t random = 0;
};

class WaterReflections {
public:
    WaterReflections(const Palette& palette, const Transfer& transfer);
    void draw(TerrainFrame& frame, ReflectionState& state, const CpuWorkSink& work = {}) const;

private:
    std::array<ColorMap, 4> colors_{};
};

class TerrainRenderer {
public:
    explicit TerrainRenderer(TerrainTables tables, std::uint32_t projection_scale = 0x03000000,
                             SceneryPolicy scenery = {});
    SceneCamera camera(const SceneView& view) const;
    SceneView target_camera(const SceneEntity& subject, std::uint8_t bearing) const;
    TerrainFrame render(const SceneAssets& assets, const SceneView& view, const CpuWorkSink& work = {}) const;
    TerrainFrame render_monitor(const SceneAssets& assets, const SceneView& view, TerrainFrame frame,
                               const CpuWorkSink& work = {}) const;
    void shade_distance(TerrainFrame& frame, const SceneAssets& assets, std::uint8_t terrain_variant,
                        const CpuWorkSink& work = {}) const;
    void draw_models(TerrainFrame& frame, const SceneAssets& assets, const MissionScene& scene,
                     const SceneView& view, const CpuWorkSink& work = {}) const;
    const std::vector<std::uint8_t>& projection() const;

private:
    TerrainTables tables_;
    SceneryPolicy scenery_;
    std::vector<std::uint8_t> projection_;
};
} // namespace comanchecd
