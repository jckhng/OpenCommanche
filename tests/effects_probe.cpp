#include "comanchecd/effects.hpp"
#include "world_output.hpp"

#include <iostream>
#include <iterator>

namespace {
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { throw std::runtime_error("Cannot open effects fixture"); }
    return {(std::istreambuf_iterator<char>(stream)), {}};
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::filesystem::exists(argv[2])) {
            throw std::invalid_argument("Usage: effects_probe FIXTURE FRESH_OUTPUT");
        }
        const std::filesystem::path input(argv[1]), output(argv[2]);
        std::filesystem::create_directories(output);
        const auto colors = read(input / "palette.bin");
        comanchecd::Palette palette{};
        if (colors.size() != 768) { throw std::invalid_argument("Invalid palette"); }
        for (std::size_t i = 0; i < colors.size(); ++i) { palette[i / 3][i % 3] = colors[i]; }
        comanchecd::TerrainTables tables;
        std::ifstream parameters(input / "tables.txt");
        parameters >> tables.levels;
        for (auto& value : tables.sine) { parameters >> value; }
        for (auto& row : tables.distance) { for (auto& value : row) { parameters >> value; } }
        for (auto& value : tables.transfer) { unsigned v; parameters >> v; value = static_cast<std::uint8_t>(v); }
        const auto gust = read(input / "gust.bin");
        if (!parameters || gust.size() != tables.gust.size()) { throw std::invalid_argument("Invalid effect tables"); }
        std::copy(gust.begin(), gust.end(), tables.gust.begin());
        comanchecd::GameplaySetup setup;
        std::ifstream config(input / "setup.txt");
        unsigned variant, type, count;
        config >> variant >> type;
        setup.terrain_variant = static_cast<std::uint8_t>(variant);
        setup.last_light_type = static_cast<std::uint8_t>(type);
        for (auto* tint : {&setup.light_color, &setup.secondary_color}) {
            for (unsigned i = 0; i < 3; ++i) { unsigned v; config >> v; (*tint)[i] = static_cast<std::uint8_t>(v); }
        }
        config >> count;
        for (unsigned i = 0; i < count; ++i) {
            comanchecd::GameplayLight interval{};
            config >> interval.x >> interval.y >> type;
            interval.type = static_cast<std::uint8_t>(type);
            setup.lights.push_back(interval);
        }
        config >> count;
        for (unsigned i = 0; i < count; ++i) {
            comanchecd::GameplayWaypoint interval{};
            config >> interval.x >> interval.y >> interval.altitude >> interval.speed;
            setup.waypoints.push_back(interval);
        }
        std::uint32_t random;
        config >> random;
        if (!config) { throw std::invalid_argument("Invalid effects configuration"); }
        comanchecd::ScriptedEffects effects(setup, palette, tables);
        comanchecd::TerrainFrame frame;
        const auto pixels = read(input / "pixels.bin");
        if (pixels.size() != frame.pixels.size()) { throw std::invalid_argument("Invalid canvas"); }
        std::copy(pixels.begin(), pixels.end(), frame.pixels.begin());
        frame.depth = read(input / "depth.bin");
        std::ifstream calls(input / "calls.txt");
        unsigned kind, tick;
        comanchecd::SceneView view;
        std::int32_t yaw, side, forward;
        std::uint32_t heading;
        unsigned index = 0;
        while (calls >> kind >> tick >> heading >> view.heading >> view.pitch >> view.roll >> view.vertical_pan >> yaw >> side >> forward) {
            frame.camera.pitch = view.pitch;
            frame.camera.roll = view.roll;
            if (kind == 2) { effects.wind(tick, heading, view.heading, forward, side, random); }
            else { effects.draw(frame, view, tick, yaw, side, random, kind == 0 ? comanchecd::EffectLayer::background : comanchecd::EffectLayer::foreground); }
            const auto directory = output / std::to_string(index++);
            std::filesystem::create_directory(directory);
            world_output::bytes(directory / "pixels.bin", frame.pixels);
            const auto& state = effects.state();
            std::ofstream result(directory / "state.txt");
            result << random << ' ' << forward << ' ' << side << ' ' << state.gust_phase << '\n';
            for (auto v : state.drift) { result << unsigned(v) << ' '; }
            result << '\n';
            for (const auto& particle : state.particles) {
                result << unsigned(particle.x) << ' ' << unsigned(particle.y) << ' ' << unsigned(particle.speed) << ' ' << particle.depth_level << '\n';
            }
        }
        if (!calls.eof() || index == 0) { throw std::invalid_argument("Invalid effect calls"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
