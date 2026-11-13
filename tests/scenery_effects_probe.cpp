#include "world_output.hpp"

#include <iostream>
#include <iterator>

namespace {
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot read scenery fixture");
    }
    return {(std::istreambuf_iterator<char>(file)), {}};
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::filesystem::exists(argv[2])) {
            throw std::invalid_argument("Usage: scenery_effects_probe FIXTURE_DIRECTORY FRESH_OUTPUT");
        }
        const std::filesystem::path input(argv[1]), output(argv[2]);
        comanchecd::SceneAssets assets;
        assets.terrain.height = read(input / "height.bin");
        assets.terrain.color = read(input / "color.bin");
        const auto palette = read(input / "palette.bin");
        if (palette.size() != 768) {
            throw std::invalid_argument("Expected six-bit palette");
        }
        for (std::size_t index = 0; index < palette.size(); ++index) {
            assets.terrain.palette[index / 3][index % 3] = palette[index];
        }
        assets.models.push_back({0, read(input / "sky.bin"), {}});
        comanchecd::TerrainTables tables;
        std::ifstream parameters(input / "tables.txt");
        parameters >> tables.levels;
        for (auto& value : tables.sine) {
            parameters >> value;
        }
        for (auto& row : tables.distance) {
            for (auto& value : row) {
                parameters >> value;
            }
        }
        for (auto& value : tables.transfer) {
            unsigned channel = 0;
            parameters >> channel;
            value = static_cast<std::uint8_t>(channel);
        }
        if (!parameters) {
            throw std::invalid_argument("Truncated terrain tables");
        }
        std::ifstream views(input / "views.txt");
        comanchecd::SceneView view;
        unsigned height, small_pixels, clouds;
        std::size_t index = 0;
        while (views >> view.heading) {
            if (!(views >> view.pitch >> view.roll >> view.x >> view.y >> height >> view.vertical_pan >> small_pixels >> clouds)) {
                throw std::invalid_argument("Truncated scenery view");
            }
            view.height = static_cast<std::uint8_t>(height);
            auto record = std::vector<std::uint8_t>(comanchecd::MenuSettings::record_bytes, 125);
            record[28] = clouds ? 125 : 123;
            record[29] = small_pixels ? 125 : 123;
            const comanchecd::TerrainRenderer renderer(tables, 0x03000000, comanchecd::MenuSettings(record).scenery_policy());
            world_output::write(output / std::to_string(index++), renderer, renderer.render(assets, view));
        }
        if (!views.eof() || index == 0) {
            throw std::invalid_argument("Empty scenery views");
        }
        std::ifstream reflections(input / "reflections.txt");
        if (reflections) {
            comanchecd::ReflectionState state;
            unsigned frames = 0;
            reflections >> state.random >> frames;
            const auto pixels = read(input / "reflection-frame.bin");
            const auto horizon = read(input / "reflection-horizon.bin");
            comanchecd::TerrainFrame frame;
            if (!reflections || frames == 0 || frames > 64 || pixels.size() != frame.pixels.size() ||
                    horizon.size() != frame.camera.horizon.size()) {
                throw std::invalid_argument("Invalid reflection fixture");
            }
            std::copy(horizon.begin(), horizon.end(), frame.camera.horizon.begin());
            const comanchecd::WaterReflections renderer(assets.terrain.palette, tables.transfer);
            std::ofstream states(output / "reflection-states.txt");
            for (unsigned index = 0; index < frames; ++index) {
                std::copy(pixels.begin(), pixels.end(), frame.pixels.begin());
                renderer.draw(frame, state);
                world_output::bytes(output / ("reflection-" + std::to_string(index) + ".bin"), frame.pixels);
                states << state.random << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
