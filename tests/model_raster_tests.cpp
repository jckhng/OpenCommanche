#include "../src/model_raster.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>

namespace {
void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
}
int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        const auto include_pixels = argc == 4 && std::string(argv[1]) == "--pixels";
        if (argc == 3 || include_pixels) {
            std::ifstream input(argv[include_pixels ? 2 : 1], std::ios::binary);
            input.exceptions(std::ios::failbit | std::ios::badbit);
            std::ofstream output(argv[include_pixels ? 3 : 2]);
            const auto integer = [&] {
                std::array<unsigned char, 4> b{};
                input.read(reinterpret_cast<char*>(b.data()), b.size());
                return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
            };
            const auto count = integer();
            if (count > 10000) { throw std::invalid_argument("Too many raster cases"); }
            for (unsigned index = 0; index < count; ++index) {
                std::array<std::uint32_t, 9> h{};
                for (auto& value : h) { value = integer(); }
                if (h[0] > 255 || h[1] > 255) { throw std::invalid_argument("Invalid raster dimensions"); }
                ModelSprite sprite{static_cast<std::uint8_t>(h[0]), static_cast<std::uint8_t>(h[1]),
                    static_cast<std::uint8_t>(h[2]), static_cast<std::int8_t>(h[3]), static_cast<std::uint8_t>(h[4]),
                    static_cast<std::uint8_t>(h[5]), h[6] != 0, h[7] != 0 ? ModelHorizon::player : ModelHorizon::terrain,
                    static_cast<std::int32_t>(h[8])};
                std::vector<std::uint8_t> pixels(h[0] * h[1]), steps(256);
                TerrainFrame frame;
                frame.depth.resize(scene_buffer_side);
                input.read(reinterpret_cast<char*>(pixels.data()), pixels.size());
                input.read(reinterpret_cast<char*>(steps.data()), steps.size());
                input.read(reinterpret_cast<char*>(frame.depth.data()), frame.depth.size());
                input.read(reinterpret_cast<char*>(frame.camera.horizon.data()), frame.camera.horizon.size());
                std::uint64_t work = 0;
                rasterize_model(frame, pixels, 0, steps, 0, 0, sprite, [&](CpuOperation operation, unsigned count) {
                    require(operation == CpuOperation::instructions, "SG raster has no string or I/O operations");
                    work += count;
                });
                output << work;
                if (include_pixels) {
                    for (std::size_t position = 0; position < frame.pixels.size(); ++position) {
                        if (frame.pixels[position] != 0) { output << ' ' << position << ':' << unsigned(frame.pixels[position]); }
                    }
                }
                output << '\n';
            }
            if (!output) { throw std::runtime_error("Cannot write raster work"); }
            return 0;
        }
        require(argc == 1, "Usage: model_raster_tests [INPUT OUTPUT]");
        TerrainFrame frame;
        frame.depth.assign(scene_buffer_side, 0);
        std::vector<std::uint8_t> pixels{3, 4, 5, 6}, steps(256, 1);
        ModelSprite sprite{2, 2, 0, 0, 100, 2, false, ModelHorizon::terrain};
        std::uint64_t work = 0;
        const CpuWorkSink sink = [&](CpuOperation, unsigned count) { work += count; };
        rasterize_model(frame, pixels, 0, steps, 0, 0, sprite, sink);
        require(work == 155 && frame.pixels[2 * 256 + 50] == 3 && frame.pixels[3 * 256 + 178] == 6,
                "SG normal columns must preserve origin, interleave and original dispatch work");
        frame.pixels.fill(0);
        sprite.mirrored = true;
        sprite.horizon = ModelHorizon::player;
        sprite.player_horizon = 255;
        work = 0;
        rasterize_model(frame, pixels, 0, steps, 0, 0, sprite, sink);
        require(work == 156 && frame.pixels[1 * 256 + 50] == 3 && frame.pixels[2 * 256 + 177] == 6,
                "Mirrored player columns must preserve wrapped flat horizon and the single RET path");
        const std::vector<std::uint8_t> edge_pixels{7, 8, 9};
        ModelSprite edge{3, 1, 0, 0, 255, 2, false, ModelHorizon::terrain};
        frame.pixels.fill(0);
        rasterize_model(frame, edge_pixels, 0, steps, 0, 0, edge);
        require(frame.pixels[2 * 256 + 255] == 7 && frame.pixels[2 * 256 + 128] == 8 &&
                frame.pixels[2 * 256 + 1] == 9 && frame.pixels[2 * 256] == 0,
                "Normal packed columns must carry from column 255 to column one");
        edge.x = 0;
        edge.mirrored = true;
        frame.pixels.fill(0);
        rasterize_model(frame, edge_pixels, 0, steps, 0, 0, edge);
        require(frame.pixels[2 * 256] == 7 && frame.pixels[2 * 256 + 127] == 8 &&
                frame.pixels[2 * 256 + 254] == 9 && frame.pixels[2 * 256 + 255] == 0,
                "Mirrored packed columns must borrow from column zero to column 254");
        sprite.origin_y = 3;
        sprite.y = 3;
        work = 0;
        frame.pixels.fill(0);
        rasterize_model(frame, pixels, 0, steps, 0, 0, sprite, sink);
        require(work == 55 && std::all_of(frame.pixels.begin(), frame.pixels.end(), [](auto x) { return x == 0; }),
                "Row-zero clipping must return before column work and use the shared two-instruction epilogue");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
