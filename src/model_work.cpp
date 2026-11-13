#include "model_work.hpp"
#include <algorithm>
#include <stdexcept>

namespace comanchecd {
ModelClipResult model_clip_work(const TerrainTables& tables, const std::vector<std::uint8_t>& projection,
                               const TerrainFrame& frame, const SceneView& view, const SceneEntity& entity,
                               std::size_t slot, std::int32_t lateral, std::int32_t depth,
                               ModelProjection kind, const CpuWorkSink& work) {
    const auto instructions = [&](unsigned count) { work(CpuOperation::instructions, count); };
    const auto animated = kind == ModelProjection::animated;
    const auto abort = [&]() { instructions(animated ? 1 : 2); return ModelClipResult{}; };
    const auto magnitude = [](std::uint32_t value) { return value ^ std::uint32_t(static_cast<std::int32_t>(value) >> 31); };
    const auto distance = static_cast<std::uint32_t>(depth);
    instructions(3);
    if (distance > 0x3fffffffu) { return abort(); }
    instructions(2);
    if (distance == 0) { return abort(); }
    std::size_t level = 0, remaining = tables.levels;
    if (!animated) {
        instructions(16);
        const auto x = magnitude(view.x - entity.x), y = magnitude(view.y - entity.y);
        if (x <= y) { instructions(2); }
        instructions(3);
        if ((std::max(x, y) >> 10) != 0) { instructions(4); }
        instructions(13);
        if (distance < 0x500000) { instructions(1); }
        const auto near = distance < 0x500000 ? distance : distance - 0x500000;
        if (tables.levels < 2) { throw std::invalid_argument("Model work requires original distance levels"); }
        const auto middle = tables.levels >> 1;
        instructions(8);
        if (near > (tables.distance[4][middle - 1] >> 3)) {
            level = middle;
            remaining >>= 1;
            instructions(2);
        }
        for (;;) {
            instructions(4);
            if (near <= (tables.distance[4][level] >> 3)) { break; }
            ++level;
            instructions(3);
            if (--remaining == 0) { instructions(1); return abort(); }
        }
        instructions(4);
    } else {
        const auto middle = tables.levels >> 1;
        if (middle == 0) { throw std::invalid_argument("Model work requires original distance levels"); }
        remaining = middle;
        instructions(11);
        if (distance > (tables.distance[4][middle - 1] >> 3)) { level = middle; instructions(1); }
    }
    const auto near_level = level;
    for (;;) {
        instructions(4);
        if (distance <= (tables.distance[4][level] >> 3)) { break; }
        ++level;
        instructions(3);
        if (--remaining == 0) { instructions(1); return abort(); }
    }
    const auto width = tables.distance[0][level];
    if (width == 0) { throw std::invalid_argument("Zero original model projection width"); }
    instructions(7);
    if (0x08000000u / width > 127) { instructions(1); }
    instructions(animated ? 17 : 18);
    const auto eye = animated ? view.height : view.height & 0xfe;
    const auto relative_height = (unsigned(static_cast<std::uint8_t>(~eye)) + entity.height) >> 1;
    const auto y = projection.at(level * scene_buffer_side + relative_height);
    if (y < 1) { return abort(); }
    instructions(2);
    if (y > 254) { return abort(); }
    instructions(4);
    const auto divisor = width >> 3;
    if (divisor == 0) { return abort(); }
    const auto x = lateral / static_cast<std::int64_t>(divisor) + 128;
    instructions(4);
    if (static_cast<std::uint32_t>(x) >= 245) { return abort(); }
    instructions(2);
    if (x < 5) { return abort(); }
    const ModelClipResult result{true, y};
    if (animated) { instructions(4); return result; }
    instructions(3);
    const auto finish = [&]() { instructions(1); return result; };
    if (slot > 60) { return finish(); }
    const auto horizon = x >= 32 && x < 32 + scene_columns ? frame.camera.horizon[x - 32] : 0;
    const auto inverted = static_cast<std::uint8_t>(~static_cast<std::uint8_t>(horizon + y));
    instructions(6);
    if (inverted < 41) { return finish(); }
    instructions(2);
    if (inverted - 41 >= 200) { return finish(); }
    instructions(2);
    if (entity.category_color == 0) { return finish(); }
    instructions(4);
    const auto target_x = x - 46;
    if (target_x < 0) { return finish(); }
    instructions(2);
    if (target_x > 160) { return finish(); }
    instructions(2);
    if (target_x <= 3) { return finish(); }
    instructions(6);
    if (y < frame.depth.at(near_level * scene_buffer_side + x)) { return finish(); }
    instructions(2);
    return finish();
}
} // namespace comanchecd
