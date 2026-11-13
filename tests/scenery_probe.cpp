#include "comanchecd/world_renderer.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
template<typename Bytes>
void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!file) {
        throw std::runtime_error("Cannot write scenery output");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::invalid_argument("Usage: scenery_probe PALETTE_AND_TRANSFER OUTPUT");
        }
        std::ifstream file(argv[1], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        comanchecd::SceneAssets assets;
        comanchecd::TerrainTables tables;
        if (bytes.size() != 768 + tables.transfer.size() || std::filesystem::exists(argv[2])) {
            throw std::invalid_argument("Expected palette/transfer fixture and fresh output");
        }
        for (std::size_t index = 0; index < 768; ++index) {
            assets.terrain.palette[index / 3][index % 3] = bytes[index];
        }
        std::copy(bytes.begin() + 768, bytes.end(), tables.transfer.begin());
        tables.levels = 1;
        assets.terrain.height.assign(1024 * 1024, 0);
        assets.terrain.color.assign(1024 * 1024, 0);
        comanchecd::MaterializedModel sky{};
        sky.bytes.assign(16 + 512 * 150, 0);
        sky.bytes[12] = 16;
        assets.models.push_back(std::move(sky));
        const comanchecd::TerrainRenderer renderer(tables, 0x03000000, {comanchecd::CloudMode::flat, false});
        auto frame = renderer.render(assets, {});
        const std::filesystem::path output(argv[2]);
        std::filesystem::create_directories(output);
        write(output / "sky.bin", frame.sky);
        write(output / "coverage.bin", frame.write_mask);
        frame.pixels.fill(86);
        const auto monitor = renderer.render_monitor(assets, {}, frame);
        write(output / "monitor.bin", monitor.pixels);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
