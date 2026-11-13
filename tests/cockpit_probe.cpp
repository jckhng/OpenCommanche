#include "comanchecd/assets.hpp"
#include "world_output.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
std::vector<std::uint8_t> bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::invalid_argument("Cannot read cockpit fixture bytes");
    }
    return {std::istreambuf_iterator<char>(file), {}};
}

std::uint8_t byte(std::istream& input) {
    unsigned value;
    if (!(input >> value) || value > 255) {
        throw std::invalid_argument("Invalid cockpit fixture byte");
    }
    return static_cast<std::uint8_t>(value);
}

comanchecd::FlightState player(const std::filesystem::path& path, const comanchecd::CockpitTables& tables) {
    std::ifstream input(path);
    comanchecd::FlightState result;
    result.chaff = tables.chaff;
    result.flares = tables.flares;
    input >> result.view.x >> result.view.y >> result.view.heading;
    result.view.height = byte(input);
    input >> result.view.pitch >> result.view.roll >> result.forward_speed >> result.side_speed >> result.yaw_rate >> result.range;
    result.control_power = byte(input);
    result.previous_power = byte(input);
    result.engine_power = byte(input);
    result.height_gap = byte(input);
    result.previous_height_gap = byte(input);
    for (auto& count : result.ammunition) {
        input >> count;
    }
    for (auto& condition : result.system_condition) {
        condition = byte(input);
    }
    input >> result.armor >> result.shake_ticks >> result.shake_offset >> result.crash_rotation;
    result.cockpit_mode = byte(input);
    result.firing = byte(input) != 0;
    if (!input) {
        throw std::invalid_argument("Truncated cockpit player fixture");
    }
    input >> std::ws;
    if (!input.eof()) {
        std::string marker;
        input >> marker >> result.target.slot >> result.target.projection.depth;
        result.target.projection.x = byte(input);
        result.target.projection.y = byte(input);
        result.target.projection.bearing = byte(input);
        if (marker != "target" || result.target.slot >= comanchecd::scene_slots) {
            throw std::invalid_argument("Invalid cockpit target fixture");
        }
        input >> std::ws;
        if (!input.eof()) {
            input >> marker;
            result.weapon = byte(input);
            if (marker != "weapon" || result.weapon >= result.ammunition.size()) {
                throw std::invalid_argument("Invalid cockpit weapon fixture");
            }
        }
    }
    return result;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::invalid_argument("Usage: cockpit_probe INSTALLATION PAYLOAD_DIRECTORY FIXTURE OUTPUT");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const std::filesystem::path fixture(argv[3]), output(argv[4]);
        if (std::filesystem::exists(output)) {
            throw std::invalid_argument("Use a fresh cockpit output directory");
        }
        const auto frontend = installation.load_frontend();
        auto resources = frontend.mission_resources;
        const auto missions = installation.load_missions(0);
        const auto& mission = missions.missions.at(0);
        for (std::size_t index = 11; index < resources.size(); ++index) {
            resources[index][0] = '-';
        }
        std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
        const auto setup = comanchecd::parse_gameplay(mission, resources, frontend.gameplay_presets, {});
        const auto payloads = comanchecd::load_gameplay(setup, resources,
            [&](const auto& name, auto kind) { return installation.load_payload(name, kind); });
        const auto scene_assets = comanchecd::materialize_scene(setup, payloads, frontend.transfer);
        auto terrain_tables = installation.load_terrain_tables();
        if (std::filesystem::exists(fixture / "depth-padding.txt")) {
            std::ifstream padding(fixture / "depth-padding.txt");
            world_output::read_depth_padding(padding, terrain_tables);
        }
        const comanchecd::TerrainRenderer terrain_renderer(std::move(terrain_tables));
        auto tables = installation.load_cockpit_tables();
        tables.category_count = setup.category_count;
        const auto assets = comanchecd::materialize_cockpit(payloads.back(),
            installation.load_payload("CONSOL5S.DTA", comanchecd::PayloadKind::picture), scene_assets, frontend.transfer);
        std::ifstream input(fixture / "commands.txt");
        std::string schema;
        input >> schema;
        if (schema != "cockpit-v1") {
            throw std::invalid_argument("Unsupported cockpit fixture schema");
        }
        comanchecd::CockpitEntry entry;
        input >> entry.back_page;
        entry.mode = byte(input);
        entry.previous_height = byte(input);
        entry.vertical_rate = byte(input);
        entry.flash_frame = byte(input);
        entry.nearby = byte(input);
        entry.controls.left = static_cast<comanchecd::InstrumentDisplay>(byte(input));
        entry.controls.right = static_cast<comanchecd::InstrumentDisplay>(byte(input));
        entry.previous_controls.left = static_cast<comanchecd::InstrumentDisplay>(byte(input));
        entry.previous_controls.right = static_cast<comanchecd::InstrumentDisplay>(byte(input));
        input >> tables.map_scale;
        tables.center_marker = byte(input) != 0;
        tables.heading_tape = byte(input) != 0;
        tables.flight_hud = byte(input) != 0;
        tables.chaff = byte(input);
        tables.flares = byte(input);
        tables.category_count = byte(input);
        entry.planes = bytes(fixture / "planes.bin");
        comanchecd::CockpitRenderer renderer(assets, tables, player(fixture / "entry-player.txt", tables), std::move(entry));
        std::filesystem::create_directories(output);
        world_output::bytes(output / "normal-picture.bin", assets.normal_picture);
        world_output::bytes(output / "crash-picture.bin", assets.crash_picture);
        std::string operation;
        unsigned index, tick, left, right;
        while (input >> operation) {
            if (!(input >> index >> tick >> left >> right) || index >= 256 || left > 5 || right > 5) {
                throw std::invalid_argument("Invalid cockpit fixture operation");
            }
            const auto directory = fixture / std::to_string(index);
            const auto before = player(directory / "before-player.txt", tables);
            const auto after = player(directory / "after-player.txt", tables);
            comanchecd::TerrainFrame frame;
            const auto pixels = bytes(directory / "scene.bin");
            if (pixels.size() != frame.pixels.size()) {
                throw std::invalid_argument("Incomplete cockpit scene input");
            }
            std::copy(pixels.begin(), pixels.end(), frame.pixels.begin());
            std::ifstream camera(directory / "camera.txt");
            camera >> frame.camera.sine >> frame.camera.cosine >> frame.camera.pitch >> frame.camera.roll;
            for (auto& horizon : frame.camera.horizon) {
                horizon = byte(camera);
            }
            comanchecd::MissionScene scene;
            std::ifstream entities(directory / "entities.txt");
            scene.remaining_targets = byte(entities);
            for (auto& entity : scene.entities) {
                entity.active = byte(entities) != 0;
                entity.category_color = byte(entities);
                entity.target = byte(entities);
                entities >> entity.x >> entity.y;
            }
            if (std::filesystem::exists(directory / "models.txt")) {
                std::ifstream models(directory / "models.txt");
                for (auto& entity : scene.entities) {
                    entity.kind = byte(models);
                    entity.phase = byte(models) == 0 ? comanchecd::EntityPhase::normal : comanchecd::EntityPhase::explosion;
                    models >> entity.primary_model >> entity.secondary_model >> entity.primary_payload;
                    entity.height = byte(models);
                    entity.angle = byte(models);
                    entity.animation_frame = byte(models);
                    entity.rotor_phase = byte(models);
                }
                if (!models) {
                    throw std::invalid_argument("Incomplete model fixture");
                }
            }
            const auto target = output / std::to_string(index);
            std::filesystem::create_directories(target);
            if (std::filesystem::exists(directory / "world-view.txt")) {
                std::ifstream input_view(directory / "world-view.txt");
                comanchecd::SceneView view;
                unsigned mode;
                input_view >> view.x >> view.y >> view.heading;
                view.height = byte(input_view);
                input_view >> view.pitch >> view.roll >> view.vertical_pan >> view.body_roll >> mode;
                if (!input_view || mode > 1) {
                    throw std::invalid_argument("Invalid world view fixture");
                }
                view.mode = static_cast<comanchecd::SceneViewMode>(mode);
                frame = terrain_renderer.render(scene_assets, view);
                terrain_renderer.shade_distance(frame, scene_assets, setup.terrain_variant);
                terrain_renderer.draw_models(frame, scene_assets, scene, view);
                world_output::bytes(target / "world.bin", frame.pixels);
                world_output::bytes(target / "world-coverage.bin", frame.write_mask);
                world_output::bytes(target / "world-depth.bin", frame.depth);
                std::ofstream targets(target / "targets.txt");
                targets << frame.targeting.candidate << '\n';
                for (const auto& projected : frame.targeting.entities) {
                    targets << projected.depth << ' ' << unsigned(projected.x) << ' ' << unsigned(projected.y)
                            << ' ' << unsigned(projected.bearing) << '\n';
                }
            }
            const comanchecd::CockpitControls controls{static_cast<comanchecd::InstrumentDisplay>(left),
                                                      static_cast<comanchecd::InstrumentDisplay>(right)};
            if (operation == "frame" || operation == "frame-present" || operation == "frame-present-copy" || operation == "copy") {
                renderer.begin_frame(frame, before.cockpit_mode, controls);
            }
            std::unique_ptr<comanchecd::InstrumentCamera> instrument_camera;
            if (std::filesystem::exists(directory / "monitor.txt")) {
                frame.depth = bytes(directory / "depth.bin");
                std::ifstream monitor(directory / "monitor.txt");
                comanchecd::SceneView view;
                monitor >> view.x >> view.y >> view.heading;
                view.height = byte(monitor);
                for (auto& entity : scene.entities) {
                    entity.kind = byte(monitor);
                    entity.phase = byte(monitor) == 0 ? comanchecd::EntityPhase::normal : comanchecd::EntityPhase::explosion;
                    monitor >> entity.primary_model >> entity.secondary_model >> entity.primary_payload;
                    entity.height = byte(monitor);
                    entity.angle = byte(monitor);
                    entity.animation_frame = byte(monitor);
                }
                if (!monitor) {
                    throw std::invalid_argument("Incomplete monitor fixture");
                }
                instrument_camera = std::make_unique<comanchecd::InstrumentCamera>();
                instrument_camera->view = view;
                instrument_camera->frame = terrain_renderer.render_monitor(scene_assets, view, frame);
                terrain_renderer.draw_models(instrument_camera->frame, scene_assets, scene, view);
                if (after.target.slot != 0 && controls.left == comanchecd::InstrumentDisplay::camera &&
                        controls.right == comanchecd::InstrumentDisplay::camera && (after.system_condition[6] & 128) == 0) {
                    instrument_camera->second = std::make_unique<comanchecd::InstrumentCamera>();
                    auto& second = *instrument_camera->second;
                    second.view = terrain_renderer.target_camera(scene.entities.at(after.target.slot),
                        instrument_camera->frame.targeting.entities.at(after.target.slot).bearing);
                    second.frame = terrain_renderer.render_monitor(scene_assets, second.view, instrument_camera->frame);
                    terrain_renderer.draw_models(second.frame, scene_assets, scene, second.view);
                }
                world_output::bytes(target / "monitor.bin", instrument_camera->frame.pixels);
            }
            std::vector<comanchecd::SoundCue> cues;
            if (operation == "frame" || operation == "frame-present" || operation == "frame-present-copy" || operation == "hud") {
                cues = renderer.draw_instruments(frame, before.view, after, scene, scene_assets, tick, controls, instrument_camera.get());
            } else if (operation != "present" && operation != "copy") {
                throw std::invalid_argument("Unsupported cockpit operation");
            }
            world_output::bytes(target / "planes.bin", renderer.surface().bytes());
            auto preview = renderer;
            const auto image = operation == "present" || operation == "frame-present" || operation == "frame-present-copy" ?
                renderer.present(after) : preview.present(after);
            if (operation == "frame-present-copy") {
                renderer.copy_front_page();
            }
            world_output::bytes(target / "image.bin", image.pixels);
            std::vector<std::uint8_t> palette;
            for (const auto& color : image.palette) {
                palette.insert(palette.end(), color.begin(), color.end());
            }
            world_output::bytes(target / "palette.bin", palette);
            const auto& state = renderer.state();
            std::ofstream metadata(target / "state.txt");
            metadata << state.back_page << ' ' << unsigned(state.mode) << ' ' << unsigned(state.previous_height) << ' '
                     << unsigned(state.vertical_rate) << ' ' << unsigned(state.flash_frame) << ' ' << unsigned(state.nearby) << '\n';
            std::ofstream sounds(target / "cues.txt");
            for (const auto& cue : cues) {
                sounds << unsigned(cue.bank) << ' ' << cue.sound << ' ' << unsigned(cue.attenuation) << '\n';
            }
            if (!metadata || !sounds) {
                throw std::runtime_error("Cannot write cockpit state");
            }
        }
        if (!input.eof()) {
            throw std::invalid_argument("Truncated cockpit command stream");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
