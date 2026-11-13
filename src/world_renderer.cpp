#include "comanchecd/world_renderer.hpp"
#include "color_metric.hpp"
#include "flight_work.hpp"
#include "model_raster.hpp"
#include "model_work.hpp"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::size_t sky_column_height = 150;
constexpr std::size_t sky_columns = 512;
constexpr std::size_t depth_column_origin = 32;

class ModelCallWork {
public:
    ModelCallWork(const CpuWorkSink& work, bool active, std::int32_t depth) : work_(work), called_(active && depth >= 0) {
        if (!work_) { return; }
        work_(CpuOperation::instructions, 4);
        if (active) { work_(CpuOperation::instructions, 2); }
        if (called_) { work_(CpuOperation::instructions, 3); }
    }
    ~ModelCallWork() {
        if (work_) { work_(CpuOperation::instructions, called_ ? 3 : 2); }
    }
    ModelCallWork(const ModelCallWork&) = delete;
    ModelCallWork& operator=(const ModelCallWork&) = delete;
private:
    const CpuWorkSink& work_;
    bool called_;
};

std::int32_t multiply_high(std::int32_t first, std::int32_t second) {
    return static_cast<std::int32_t>((std::int64_t(first) * second) >> 32);
}

std::size_t buffer_column(std::size_t column) {
    return 16 + (column >> 1) + ((column & 1) << 7);
}

std::size_t terrain_position(std::uint32_t x, std::uint32_t y) {
    return std::size_t(y >> 22) * terrain_side + (x >> 22);
}

std::uint32_t model_integer(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        throw std::invalid_argument("Truncated materialized model frame");
    }
    std::uint32_t result = 0;
    for (std::size_t byte = 0; byte < count; ++byte) {
        result |= std::uint32_t(bytes[offset + byte]) << (byte * 8);
    }
    return result;
}
}

WaterReflections::WaterReflections(const Palette& palette, const Transfer& transfer) {
    constexpr std::size_t first_water_color = 251;
    for (std::size_t water = 0; water < colors_.size(); ++water) {
        for (std::size_t sample = 0; sample < palette.size(); ++sample) {
            auto color = palette[sample];
            for (std::size_t channel = 0; channel < color.size(); ++channel) {
                color[channel] = static_cast<std::uint8_t>((color[channel] + palette[first_water_color + water][channel] + 1) >> 1);
            }
            colors_[water][sample] = color_metric::nearest(palette, transfer, color, 1, 255);
        }
    }
}

void WaterReflections::draw(TerrainFrame& frame, ReflectionState& state, const CpuWorkSink& work) const {
    constexpr std::uint8_t first_water_color = 251;
    constexpr unsigned reflection_rows = 135;
    constexpr std::uint32_t feedback = 0xbc9abb09;
    const auto instructions = [&](unsigned count) { if (work) { work(CpuOperation::instructions, count); } };
    instructions(14); // 2784c terrain guard, generator update and column setup.
    const auto transformed = static_cast<std::int32_t>(0u - state.random) >> 1;
    const auto delta = static_cast<std::uint32_t>(transformed) ^ (transformed < 0 ? feedback : 0u);
    auto random = state.random + delta;
    auto carried_color = static_cast<std::uint8_t>(delta);
    for (std::size_t column = 0; column < scene_columns; ++column) {
        instructions(9);
        const auto horizon = frame.camera.horizon[column];
        const auto start = static_cast<std::uint8_t>(horizon + reflection_rows);
        bool second_test = false;
        for (unsigned offset = 0; offset < reflection_rows; ++offset) {
            const auto row = static_cast<std::uint8_t>(start - offset);
            const auto position = std::size_t(row) * scene_buffer_side + buffer_column(column);
            const auto water = frame.pixels[position];
            instructions(2);
            if (water < first_water_color) {
                instructions(3);
                second_test = !second_test;
                if (offset + 1 == reflection_rows && !second_test) {
                    instructions(1); // The second transparent test exits through 278b9.
                }
                continue;
            }
            if (water == 255) {
                throw std::invalid_argument("Reflection color 255 requires unrecovered adjacent DOS memory");
            }
            const auto jitter = (carried_color ^ random) & 3;
            const auto source_row = static_cast<std::uint8_t>(2 * start - row + jitter);
            const auto sample = frame.pixels[std::size_t(source_row) * scene_buffer_side + buffer_column(column)];
            carried_color = colors_[water - first_water_color][sample];
            frame.pixels[position] = carried_color;
            ++random;
            second_test = false;
            instructions(15 + 3);
        }
        random = (random >> 1) | (random << 31);
        instructions(6);
    }
    state.random = random;
    instructions(1);
}

TerrainRenderer::TerrainRenderer(TerrainTables tables, std::uint32_t projection_scale, SceneryPolicy scenery)
    : tables_(std::move(tables)), scenery_(scenery) {
    if (tables_.levels == 0 || tables_.levels > terrain_distance_capacity) {
        throw std::invalid_argument("Unsupported terrain distance table");
    }
    if (!tables_.depth_padding.empty() && tables_.depth_padding.size() != tables_.levels * (scene_buffer_side - scene_columns)) {
        throw std::invalid_argument("Invalid inherited terrain depth padding");
    }
    projection_.resize(tables_.levels * scene_buffer_side);
    for (std::size_t level = 0; level < tables_.levels; ++level) {
        const auto scale = static_cast<std::int32_t>((std::uint64_t(tables_.distance[2][level]) * projection_scale) >> 32);
        for (std::size_t height = 0; height < scene_buffer_side; ++height) {
            const auto relative = static_cast<std::int32_t>((std::uint32_t(height) << 24) - 0x80000000u);
            const auto projected = std::uint32_t(multiply_high(relative, scale)) + tables_.distance[3][level];
            const auto clipped = std::clamp<std::int32_t>(static_cast<std::int32_t>(projected), 0, 0xff00);
            projection_[level * scene_buffer_side + height] = static_cast<std::uint8_t>(clipped >> 8);
        }
    }
}

const std::vector<std::uint8_t>& TerrainRenderer::projection() const {
    return projection_;
}

SceneView TerrainRenderer::target_camera(const SceneEntity& subject, std::uint8_t bearing) const {
    constexpr std::int32_t camera_distance = 0x10000000;
    SceneView view;
    view.x = subject.x - multiply_high(camera_distance, tables_.sine[bearing * 2]);
    view.y = subject.y + multiply_high(camera_distance, tables_.sine[bearing * 2 + 128]);
    view.heading = std::uint32_t(bearing) << 24;
    view.height = subject.height;
    return view;
}

SceneCamera TerrainRenderer::camera(const SceneView& view) const {
    SceneCamera result;
    const auto index = view.heading >> 23;
    const auto fraction = (view.heading << 9) >> 1;
    const auto complement = (~(view.heading << 9)) >> 1;
    const auto interpolate = [&](std::size_t offset) {
        const auto first = multiply_high(tables_.sine[index + offset], static_cast<std::int32_t>(complement));
        const auto second = multiply_high(tables_.sine[index + offset + 1], static_cast<std::int32_t>(fraction));
        return static_cast<std::int32_t>((std::uint32_t(first) + std::uint32_t(second)) << 1);
    };
    result.sine = interpolate(0);
    result.cosine = interpolate(128);
    if (view.mode != SceneViewMode::cockpit) {
        result.sine = tables_.sine[index];
        result.cosine = tables_.sine[index + 128];
    }
    result.pitch = view.pitch;
    const auto limit = 33 - ((std::uint32_t(view.pitch < 0 ? -std::int64_t(view.pitch) : view.pitch) * 64) >> 8);
    if (limit > 33) {
        throw std::invalid_argument("Unsupported scene pitch");
    }
    result.roll = std::clamp(view.roll, -static_cast<std::int32_t>(limit), static_cast<std::int32_t>(limit));
    for (std::size_t column = 0; column < scene_columns; ++column) {
        const auto offset = static_cast<std::int32_t>(column) - 96;
        const auto magnitude = offset < 0 ? -offset : offset;
        const auto slope = (std::int64_t(view.pitch) * magnitude + 128) >> 8;
        const auto horizon = std::int64_t(result.roll) - view.vertical_pan + (offset < 0 ? -slope : slope);
        result.horizon[column] = static_cast<std::uint8_t>(horizon);
    }
    return result;
}

TerrainFrame TerrainRenderer::render(const SceneAssets& assets, const SceneView& view, const CpuWorkSink& work) const {
    if (assets.terrain.height.size() != terrain_side * terrain_side ||
            assets.terrain.color.size() != terrain_side * terrain_side || assets.models.empty()) {
        throw std::invalid_argument("Terrain rendering requires materialized scene assets");
    }
    const auto& sky = assets.models.back().bytes;
    const auto pixels = sky.size() >= 16 ? std::uint32_t(sky[12]) | (std::uint32_t(sky[13]) << 8) |
        (std::uint32_t(sky[14]) << 16) | (std::uint32_t(sky[15]) << 24) : 0;
    if (pixels > sky.size() || sky.size() - pixels != sky_column_height * sky_columns) {
        throw std::invalid_argument("Unsupported materialized panoramic sky");
    }
    TerrainFrame result;
    result.camera = camera(view);
    auto sky_column = ((~view.heading - 0x30000000u) >> 23);
    const std::size_t column_step = scenery_.pixels == PixelSize::large ? 2 : 1;
    sky_column &= ~(static_cast<std::uint32_t>(column_step) - 1);
    const auto flat_color = scenery_.clouds == CloudMode::flat ?
        color_metric::nearest(assets.terrain.palette, tables_.transfer, assets.terrain.palette[255], 1, 254) : 0;
    const auto sky_step = scenery_.clouds == CloudMode::flat ? 1 : column_step;
    for (std::size_t reverse = scene_columns; reverse > 0; reverse -= sky_step) {
        const auto column = reverse - sky_step;
        if (scenery_.clouds == CloudMode::flat) {
            for (std::size_t row = 0; row < scene_buffer_side; ++row) {
                const auto position = row * scene_buffer_side + buffer_column(column);
                result.sky[position] = flat_color;
                result.write_mask[position] = 1;
            }
            continue;
        }
        for (std::size_t row = 0; row < sky_column_height; ++row) {
            const auto y = static_cast<std::uint8_t>(result.camera.horizon[column] - 1 - row);
            const auto position = std::size_t(y) * scene_buffer_side + buffer_column(column);
            result.sky[position] = sky[pixels + sky_column * sky_column_height + row];
            result.write_mask[position] = 1;
        }
        sky_column = (sky_column + column_step) % sky_columns;
    }
    result.pixels = result.sky;
    if (column_step == 1 && scenery_.clouds != CloudMode::flat) {
        flight_work::sky(view.heading, work);
    }
    result.depth.resize(tables_.levels * scene_buffer_side);
    if (!tables_.depth_padding.empty()) {
        for (std::size_t level = 0; level < tables_.levels; ++level) {
            const auto source = tables_.depth_padding.begin() + level * (scene_buffer_side - scene_columns);
            const auto destination = result.depth.begin() + level * scene_buffer_side;
            std::copy_n(source, depth_column_origin, destination);
            std::copy_n(source + depth_column_origin, depth_column_origin, destination + depth_column_origin + scene_columns);
        }
    }
    std::array<std::uint8_t, scene_columns> occlusion{};
    auto center_x = view.x;
    auto center_y = view.y;
    const auto eye_height = static_cast<std::uint8_t>(~view.height) >> 1;
    for (std::size_t level = 0; level < tables_.levels; ++level) {
        const auto forward = static_cast<std::int32_t>(tables_.distance[1][level]);
        center_x += multiply_high(forward, result.camera.sine);
        center_y -= multiply_high(forward, result.camera.cosine);
        const auto width = static_cast<std::int32_t>(tables_.distance[0][level] * column_step);
        const auto step_y = multiply_high(width, result.camera.sine);
        const auto step_x = multiply_high(width, result.camera.cosine);
        for (std::size_t column = 0; column < scene_columns; column += column_step) {
            const auto offset = std::uint32_t(column / column_step) - std::uint32_t(95 / column_step);
            const auto x = center_x + offset * std::uint32_t(step_x);
            const auto y = center_y + offset * std::uint32_t(step_y);
            const auto position = terrain_position(x, y);
            const auto relative = static_cast<std::uint8_t>(eye_height + assets.terrain.height[position]);
            const auto height = projection_[level * scene_buffer_side + relative];
            // Coarse left-hand spans use the inner column's horizon; the right half uses the outer column.
            const auto horizon_column = column + (column_step == 2 && column < scene_columns / 2 ? 1 : 0);
            for (unsigned row = occlusion[column]; row < height; ++row) {
                const auto target_y = static_cast<std::uint8_t>(result.camera.horizon[horizon_column] + row);
                const auto target = std::size_t(target_y) * scene_buffer_side + buffer_column(column);
                result.pixels[target] = assets.terrain.color[position];
                result.write_mask[target] |= 2;
            }
            occlusion[column] = std::max(occlusion[column], height);
            if (column_step == 2) {
                occlusion[column + 1] = occlusion[column];
            }
        }
        std::copy(occlusion.begin(), occlusion.end(), result.depth.begin() + level * scene_buffer_side + depth_column_origin);
    }
    if (column_step == 1) { flight_work::terrain(result, tables_.levels, work); }
    if (column_step == 2) {
        for (std::size_t row = 0; row < scene_buffer_side; ++row) {
            for (std::size_t column = 0; column < scene_columns; column += 2) {
                const auto source = row * scene_buffer_side + buffer_column(column);
                const auto destination = source + scene_buffer_side / 2;
                result.pixels[destination] = result.pixels[source];
                result.write_mask[destination] = result.write_mask[source];
            }
        }
    }
    return result;
}

TerrainFrame TerrainRenderer::render_monitor(const SceneAssets& assets, const SceneView& view, TerrainFrame frame,
                                            const CpuWorkSink& work) const {
    constexpr unsigned monitor_columns = 40;
    constexpr unsigned first_column = 44;
    constexpr unsigned first_depth_column = 88;
    constexpr unsigned monitor_distance_step = 3;
    const auto angle = view.heading >> 23;
    frame.camera.sine = tables_.sine[angle];
    frame.camera.cosine = tables_.sine[angle + 128];
    std::fill_n(frame.camera.horizon.begin() + 56, monitor_columns * 2, 0);
    if (assets.terrain.height.size() != terrain_side * terrain_side ||
            assets.terrain.color.size() != terrain_side * terrain_side || assets.models.empty() ||
            frame.depth.size() != tables_.levels * scene_buffer_side) {
        throw std::invalid_argument("Monitor rendering requires a complete terrain frame");
    }
    const auto& sky = assets.models.back().bytes;
    const auto sky_source = model_integer(sky, 12, 4);
    if (sky_source > sky.size() || sky.size() - sky_source != sky_column_height * sky_columns) {
        throw std::invalid_argument("Unsupported monitor panoramic sky");
    }
    auto sky_column = ((~view.heading - 0x14000000u) >> 23);
    const auto flat_color = scenery_.clouds == CloudMode::flat ?
        color_metric::nearest(assets.terrain.palette, tables_.transfer, assets.terrain.palette[255], 1, 254) : 0;
    for (unsigned reverse = monitor_columns; reverse > 0; --reverse) {
        const auto column = first_column + reverse - 1;
        if (scenery_.clouds == CloudMode::flat) {
            for (std::size_t row = 0; row < scene_buffer_side; ++row) {
                frame.pixels[row * scene_buffer_side + column] = flat_color;
            }
            continue;
        }
        for (unsigned row = 0; row < 60; ++row) {
            frame.pixels[(165 - row) * scene_buffer_side + column] = sky[sky_source + sky_column * sky_column_height + 90 + row];
        }
        sky_column = (sky_column + 2) % sky_columns;
    }
    std::array<std::uint8_t, monitor_columns> occlusion{};
    auto center_x = view.x;
    auto center_y = view.y;
    const auto eye_height = static_cast<std::uint8_t>(~view.height) >> 1;
    for (std::size_t level = 0; level < tables_.levels; level += monitor_distance_step) {
        // The original contiguous tables alias the next table at the capacity boundary.
        const auto following = level + 1 < terrain_distance_capacity ? tables_.distance[1][level + 1] : tables_.distance[2][0];
        const auto forward = static_cast<std::int32_t>(tables_.distance[1][level] + following);
        center_x += multiply_high(forward, frame.camera.sine);
        center_y -= multiply_high(forward, frame.camera.cosine);
        const auto width = static_cast<std::int32_t>(tables_.distance[0][level] << 1);
        const auto step_y = multiply_high(width, frame.camera.sine);
        const auto step_x = multiply_high(width, frame.camera.cosine);
        for (unsigned column = 0; column < monitor_columns; ++column) {
            const auto offset = std::uint32_t(column) - 19;
            const auto x = center_x + offset * std::uint32_t(step_x);
            const auto y = center_y + offset * std::uint32_t(step_y);
            const auto position = terrain_position(x, y);
            const auto relative = static_cast<std::uint8_t>(eye_height + assets.terrain.height[position]);
            const auto height = projection_[level * scene_buffer_side + relative];
            for (unsigned row = occlusion[column]; row < height; ++row) {
                frame.pixels[row * scene_buffer_side + first_column + column] = assets.terrain.color[position];
            }
            occlusion[column] = std::max(occlusion[column], height);
            for (unsigned depth_row = 0; depth_row < monitor_distance_step && level + depth_row < tables_.levels; ++depth_row) {
                const auto target = (level + depth_row) * scene_buffer_side + first_depth_column + column * 2;
                frame.depth[target] = frame.depth[target + 1] = occlusion[column];
            }
        }
    }
    for (unsigned row = 60; row < 180; ++row) {
        const auto source = frame.pixels.begin() + row * scene_buffer_side + first_column;
        std::copy_n(source, monitor_columns, source + 128);
    }
    flight_work::monitor(frame, tables_.levels, view.heading, scenery_.clouds, work);
    return frame;
}

void TerrainRenderer::shade_distance(TerrainFrame& frame, const SceneAssets& assets, std::uint8_t terrain_variant,
                                     const CpuWorkSink& work) const {
    if (!scenery_.haze) {
        return;
    }
    flight_work::fog(frame, tables_.levels, work);
    ColorMap quarter{}, half{};
    const auto last = terrain_variant == 0 ? 255u : 250u;
    for (std::size_t index = 0; index < quarter.size(); ++index) {
        quarter[index] = half[index] = static_cast<std::uint8_t>(index);
    }
    for (std::size_t index = 0; index < last; ++index) {
        auto first = assets.terrain.palette[index];
        auto second = first;
        for (std::size_t channel = 0; channel < first.size(); ++channel) {
            const auto fog = assets.terrain.palette[255][channel];
            first[channel] = static_cast<std::uint8_t>((first[channel] * 3 + fog + 3) >> 2);
            second[channel] = static_cast<std::uint8_t>((second[channel] + fog + 1) >> 1);
        }
        quarter[index] = color_metric::nearest(assets.terrain.palette, tables_.transfer, first, 1, last);
        half[index] = color_metric::nearest(assets.terrain.palette, tables_.transfer, second, 1, last);
    }
    const auto far_level = tables_.levels - 1;
    const auto half_level = tables_.levels - (tables_.levels >> 3);
    const auto quarter_level = tables_.levels - (tables_.levels >> 2);
    if (half_level >= tables_.levels || quarter_level >= tables_.levels) {
        throw std::invalid_argument("Distance shading requires the original multi-level terrain table");
    }
    for (std::size_t column = 0; column < scene_columns; ++column) {
        const auto logical_x = column + depth_column_origin;
        const auto horizon = frame.camera.horizon[column];
        auto y = static_cast<std::uint8_t>(frame.depth[far_level * scene_buffer_side + logical_x] + horizon);
        const auto half_y = static_cast<std::uint8_t>(frame.depth[half_level * scene_buffer_side + logical_x] + horizon);
        const auto quarter_y = static_cast<std::uint8_t>(frame.depth[quarter_level * scene_buffer_side + logical_x] + horizon);
        while (y > half_y) {
            --y;
            const auto target = std::size_t(y) * scene_buffer_side + buffer_column(column);
            frame.pixels[target] = half[frame.pixels[target]];
        }
        while (y > quarter_y) {
            --y;
            const auto target = std::size_t(y) * scene_buffer_side + buffer_column(column);
            frame.pixels[target] = quarter[frame.pixels[target]];
        }
    }
}

void TerrainRenderer::draw_models(TerrainFrame& frame, const SceneAssets& assets, const MissionScene& scene,
                                  const SceneView& view, const CpuWorkSink& work) const {
    frame.targeting = {};
    std::array<std::int32_t, scene_slots> lateral{}, depth{};
    std::array<std::size_t, scene_slots> order{};
    for (std::size_t slot = 1; slot < scene_slots; ++slot) {
        order[slot] = slot;
        const auto& entity = scene.entities[slot];
        // 266bc skips inactive slots without clearing their projection table entries.
        depth[slot] = entity.projected_depth;
        frame.targeting.entities[slot].depth = depth[slot];
        if (!entity.active) {
            continue;
        }
        const auto x = static_cast<std::int32_t>(entity.x - view.x) >> 1;
        const auto y = static_cast<std::int32_t>(entity.y - view.y) >> 1;
        lateral[slot] = static_cast<std::int32_t>(std::uint32_t(multiply_high(frame.camera.cosine, x)) +
                                                std::uint32_t(multiply_high(frame.camera.sine, y)));
        depth[slot] = static_cast<std::int32_t>(std::uint32_t(multiply_high(frame.camera.sine, x)) -
                                              std::uint32_t(multiply_high(frame.camera.cosine, y)));
        frame.targeting.entities[slot].depth = depth[slot];
    }
    flight_work::model_projection(static_cast<unsigned>(std::count_if(scene.entities.begin() + 1, scene.entities.end(),
        [](const auto& entity) { return entity.active; })), work);
    flight_work::model_sort(depth, work);
    for (std::size_t end = scene_slots - 1; end > 1; --end) {
        auto selected = end;
        for (std::size_t candidate = end - 1; candidate > 0; --candidate) {
            if (depth[order[selected]] <= depth[order[candidate]]) {
                selected = candidate;
            }
        }
        std::swap(order[end], order[selected]);
    }
    if (work) { work(CpuOperation::instructions, 2); }
    for (std::size_t reverse = scene_slots; reverse > 1; --reverse) {
        const auto slot = order[reverse - 1];
        const auto& entity = scene.entities[slot];
        const ModelCallWork call_work(work, entity.active, depth[slot]);
        const auto explosion = entity.phase == EntityPhase::explosion;
        const auto animated = explosion || entity.kind == cannon_round_kind || entity.kind == enemy_cannon_kind ||
                              entity.kind == flare_kind || entity.kind == chaff_kind;
        if (work && entity.active && depth[slot] >= 0) {
            const auto player = entity.kind == 0 && !explosion;
            if (player && view.mode == SceneViewMode::cockpit) {
                work(CpuOperation::instructions, 3);
            } else if (!explosion && entity.kind == artillery_kind) {
                work(CpuOperation::instructions, 1);
            } else if (!animated) {
                const auto chase_player = player && view.mode == SceneViewMode::chase;
                if (player) { work(CpuOperation::instructions, chase_player ? 8 : 4); }
                work(CpuOperation::instructions, 1);
                const auto clip = model_clip_work(tables_, projection_, frame, view, entity, slot, lateral[slot], depth[slot],
                                                  ModelProjection::directional, work);
                if (clip.visible) {
                    const auto counter_rotor = entity.kind == 1 || entity.kind == 6;
                    const auto layered = player || counter_rotor || entity.kind == 2 || entity.kind == 4 ||
                        entity.kind == 5 || entity.kind == 7 || entity.kind == 8 || entity.kind == 11 ||
                        entity.kind == 15 || entity.kind == 16 || entity.kind == 17;
                    const auto pitched = static_cast<std::uint8_t>(clip.y + (player ? view.body_roll : 0));
                    work(CpuOperation::instructions, counter_rotor ? (pitched > 130 ? 54 : 53) :
                        layered ? (pitched > 130 ? 36 : 35) : 13);
                }
                if (chase_player) { work(CpuOperation::instructions, 3); }
              } else {
                  model_clip_work(tables_, projection_, frame, view, entity, slot, lateral[slot], depth[slot],
                                  ModelProjection::animated, work);
            }
        }
        if (!entity.active || (!explosion && entity.kind == artillery_kind) ||
                (entity.kind == 0 && !explosion && view.mode == SceneViewMode::cockpit) || depth[slot] <= 0 || depth[slot] > 0x3fffffff) {
            continue;
        }
        const auto distance_level = [&](std::uint32_t distance) {
            const auto end = tables_.distance[4].begin() + tables_.levels;
            return std::size_t(std::lower_bound(tables_.distance[4].begin(), end, distance,
                [](auto entry, auto value) { return (entry >> 3) < value; }) - tables_.distance[4].begin());
        };
        const auto distance = static_cast<std::uint32_t>(depth[slot]);
        const auto level = distance_level(distance);
        const auto near_level = animated ? level : distance_level(distance >= 0x500000 ? distance - 0x500000 : distance);
        if (level == tables_.levels) {
            continue;
        }
        const auto width = tables_.distance[0][level];
        const auto divisor = width >> 3;
        if (width == 0 || divisor == 0) {
            continue;
        }
        const auto screen_x = lateral[slot] / static_cast<std::int64_t>(divisor) + 128;
        const auto eye_height = animated ? view.height : view.height & 0xfe;
        const auto relative_height = (unsigned(static_cast<std::uint8_t>(~eye_height)) + entity.height) >> 1;
        const auto screen_y = projection_[level * scene_buffer_side + relative_height];
        if (screen_x < 5 || screen_x >= 245 || screen_y == 0 || screen_y == 255) {
            continue;
        }
        if (!animated && entity.kind > 17 &&
                entity.kind != guided_missile_kind && entity.kind != rocket_kind && entity.kind != stinger_kind &&
                entity.kind != enemy_rocket_kind && entity.kind != climbing_missile_kind) {
            throw std::invalid_argument("Scene model draw policy has not been recovered for this entity kind");
        }
        const auto player = entity.kind == 0 && !explosion;
        if (!explosion && slot < player_slot && entity.category_color != 0) {
            const auto horizon = screen_x >= depth_column_origin && screen_x < depth_column_origin + scene_columns ?
                frame.camera.horizon[screen_x - depth_column_origin] : 0;
            const auto inverted = static_cast<std::uint8_t>(~static_cast<std::uint8_t>(horizon + screen_y));
            const auto target_x = screen_x - 46;
            if (inverted >= 41 && inverted - 41 < 200 && target_x > 3 && target_x <= 160 &&
                    screen_y >= frame.depth[near_level * scene_buffer_side + screen_x]) {
                auto& target = frame.targeting.entities[slot];
                target.x = static_cast<std::uint8_t>(target_x);
                target.y = static_cast<std::uint8_t>(inverted - 41);
                frame.targeting.candidate = slot;
            }
        }
        const auto counter_rotor = !explosion && (entity.kind == 1 || entity.kind == 6);
        const auto layered = !explosion && (player || counter_rotor || entity.kind == 2 || entity.kind == 4 ||
            entity.kind == 5 || entity.kind == 7 || entity.kind == 8 || entity.kind == 11 ||
            entity.kind == 15 || entity.kind == 16 || entity.kind == 17);
        const auto pitched_height = static_cast<std::uint8_t>(screen_y + (player ? view.body_roll : 0));
        const auto secondary_first = pitched_height > 130;
        const unsigned layers = counter_rotor ? 3 : layered ? 2 : 1;
        for (unsigned layer = 0; layer < layers; ++layer) {
            const auto reverse_rotor = counter_rotor && layer == 1;
            const auto secondary = layered && (counter_rotor ? layer != (secondary_first ? 2u : 0u) :
                ((layer == 0) == secondary_first));
            const auto model_slot = secondary ? entity.secondary_model : entity.primary_model;
            if ((secondary || entity.primary_payload == std::numeric_limits<std::size_t>::max()) && model_slot >= assets.model_payloads.size()) {
                throw std::invalid_argument("Scene model slot exceeds its resource table");
            }
            const auto payload = secondary || entity.primary_payload == std::numeric_limits<std::size_t>::max() ?
                assets.model_payloads[model_slot] : entity.primary_payload;
            const auto model = std::find_if(assets.models.begin(), assets.models.end(), [&](const auto& item) {
                return item.payload == payload;
            });
            if (model == assets.models.end() || (!animated && model->metadata.size() < 50)) {
                throw std::invalid_argument("Scene model lacks its required payload or directional metadata");
            }
            auto selected_frame = entity.animation_frame;
            if (!animated) {
                const auto difference_x = static_cast<std::int32_t>(view.x - entity.x);
                const auto difference_y = static_cast<std::int32_t>(view.y - entity.y);
                auto absolute_x = std::uint32_t(difference_x ^ (difference_x >> 31));
                auto absolute_y = std::uint32_t(difference_y ^ (difference_y >> 31));
                auto octant = unsigned(difference_y < 0) * 4 + unsigned(difference_x < 0) * 2;
                if (absolute_x <= absolute_y) {
                    ++octant;
                    std::swap(absolute_x, absolute_y);
                }
                auto angle = tables_.angle_octants[octant];
                if ((absolute_x >> 10) != 0) {
                    const auto ratio = absolute_y / (absolute_x >> 10);
                    if (ratio >= tables_.angle_ratio.size()) {
                        throw std::invalid_argument("Model bearing exceeds the original angle table");
                    }
                    angle ^= tables_.angle_ratio[ratio];
                }
                angle = (angle + 1) >> 1;
                frame.targeting.entities[slot].bearing = static_cast<std::uint8_t>(~angle);
                auto facing = static_cast<std::uint8_t>(angle + entity.angle + 4) >> 3;
                if (secondary) {
                    const auto rotor = reverse_rotor ? -int(entity.rotor_phase) + 4 :
                        entity.rotor_phase + (secondary_first ? 4 : 0);
                    facing = (facing + (static_cast<std::uint8_t>(rotor) >> 3)) & 31;
                }
                selected_frame = static_cast<std::uint8_t>(model->metadata[2 + facing] + model->metadata[34 + ((player ? pitched_height : screen_y) >> 4)]);
            }
            const auto frame_number = selected_frame & 127;
            const auto& bytes = model->bytes;
            if (frame_number == 0 || frame_number > model_integer(bytes, 2, 1)) {
                throw std::invalid_argument("Invalid SG frame " + std::to_string(frame_number) +
                    " for entity " + std::to_string(slot) + " payload " + std::to_string(payload));
            }
            const auto entry = frame_number * 8;
            const auto source_width = model_integer(bytes, entry, 1);
            const auto source_height = model_integer(bytes, entry + 1, 1);
            const auto origin_x = model_integer(bytes, entry + 2, 1);
            const auto origin_y = static_cast<std::int8_t>(model_integer(bytes, entry + 3, 1) - (reverse_rotor ? 8 : 0));
            const auto source = model_integer(bytes, entry + 4, 4);
            if (source_height == 0 || source > bytes.size() || source_width * source_height > bytes.size() - source) {
                throw std::invalid_argument("SG frame exceeds its materialized allocation");
            }
            const auto scale = 127 - std::min<std::uint32_t>(0x08000000u / width, 127);
            const auto steps = tables_.sprite_offsets[scale];
            const ModelSprite sprite{static_cast<std::uint8_t>(source_width), static_cast<std::uint8_t>(source_height),
                static_cast<std::uint8_t>(origin_x), origin_y, static_cast<std::uint8_t>(screen_x), screen_y,
                (selected_frame & 128) != 0, player && view.mode == SceneViewMode::chase ? ModelHorizon::player : ModelHorizon::terrain,
                frame.camera.roll - view.vertical_pan};
            rasterize_model(frame, bytes, source, tables_.sprite_steps, steps, near_level, sprite, work);
        }
    }
    if (work) { work(CpuOperation::instructions, 1); }
}
} // namespace comanchecd
