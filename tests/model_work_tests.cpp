#include "../src/model_work.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace { void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } } }
int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        if (argc == 3) {
            std::ifstream input(argv[1], std::ios::binary);
            input.exceptions(std::ios::failbit | std::ios::badbit);
            std::ofstream output(argv[2]);
            const auto integer = [&] {
                std::array<unsigned char, 4> b{};
                input.read(reinterpret_cast<char*>(b.data()), b.size());
                return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
            };
            const auto count = integer();
            require(count < 10000, "Too many model work cases");
            for (unsigned index = 0; index < count; ++index) {
                const auto kind = integer(), slot = integer();
                SceneView view;
                view.x = integer(); view.y = integer(); view.height = static_cast<std::uint8_t>(integer());
                SceneEntity entity;
                entity.x = integer(); entity.y = integer(); entity.height = static_cast<std::uint8_t>(integer());
                entity.category_color = static_cast<std::uint8_t>(integer());
                const auto lateral = static_cast<std::int32_t>(integer()), depth = static_cast<std::int32_t>(integer());
                TerrainTables tables;
                tables.levels = integer();
                require(tables.levels > 1 && tables.levels <= terrain_distance_capacity, "Invalid model distance levels");
                for (unsigned i = 0; i < tables.levels; ++i) { tables.distance[0][i] = integer(); }
                for (unsigned i = 0; i < tables.levels; ++i) { tables.distance[4][i] = integer(); }
                std::vector<std::uint8_t> projection(tables.levels * scene_buffer_side);
                TerrainFrame frame;
                frame.depth.resize(projection.size());
                input.read(reinterpret_cast<char*>(projection.data()), projection.size());
                input.read(reinterpret_cast<char*>(frame.depth.data()), frame.depth.size());
                input.read(reinterpret_cast<char*>(frame.camera.horizon.data()), frame.camera.horizon.size());
                std::uint64_t work = 0;
                const auto result = model_clip_work(tables, projection, frame, view, entity, slot, lateral, depth,
                    kind == 0 ? ModelProjection::directional : ModelProjection::animated,
                    [&](CpuOperation operation, unsigned count) { require(operation == CpuOperation::instructions, "Model setup has no I/O or string work"); work += count; });
                output << work << ' ' << result.visible << ' ' << unsigned(result.y) << '\n';
            }
            require(bool(output), "Cannot write model work");
            return 0;
        }
        require(argc == 1, "Usage: model_work_tests [INPUT OUTPUT]");
        TerrainTables tables;
        std::vector<std::uint8_t> projection;
        TerrainFrame frame;
        SceneView view;
        SceneEntity entity;
        std::uint64_t work = 0;
        const CpuWorkSink sink = [&](CpuOperation, unsigned count) { work += count; };
        auto result = model_clip_work(tables, projection, frame, view, entity, 60, 0, 0, ModelProjection::directional, sink);
        require(!result.visible && work == 7, "A zero directional depth must pop the caller return before RET");
        work = 0;
        result = model_clip_work(tables, projection, frame, view, entity, 60, 0, 0, ModelProjection::animated, sink);
        require(!result.visible && work == 6, "Animated zero depth must return without popping another frame");
        tables.levels = 2;
        tables.distance[0].fill(8);
        tables.distance[4].fill(0x10000000);
        projection.assign(tables.levels * scene_buffer_side, 100);
        frame.depth.assign(projection.size(), 0);
        entity.kind = 2;
        entity.category_color = 0;
        work = 0;
        result = model_clip_work(tables, projection, frame, view, entity, 60, 0, 0x01000000,
                                 ModelProjection::directional, sink);
        const auto unmarked_work = work;
        entity.category_color = 6;
        work = 0;
        const auto marked = model_clip_work(tables, projection, frame, view, entity, 60, 0, 0x01000000,
                                            ModelProjection::directional, sink);
        require(result.visible && marked.visible && result.y == marked.y && work - unmarked_work == 16,
                "Model targeting work must branch on category color independently of entity kind");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
