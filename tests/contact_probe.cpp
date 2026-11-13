#include "comanchecd/assets.hpp"
#include "world_runtime_probe.hpp"

#include <algorithm>
#include <iostream>

namespace {
comanchecd::AudioEntry read_audio(std::istream& input) {
    comanchecd::AudioEntry entry;
    const auto read_byte = [&]() {
        unsigned value = 0;
        if (!(input >> value) || value > 255) {
            throw std::invalid_argument("Invalid audio entry byte");
        }
        return static_cast<std::uint8_t>(value);
    };
    for (auto& byte : entry.frequency_high) {
        byte = read_byte();
    }
    for (auto& byte : entry.echo) {
        byte = read_byte();
    }
    input >> entry.echo_cursor;
    entry.engine_power = read_byte();
    entry.music_volume = read_byte();
    return entry;
}

void snapshot(const std::filesystem::path& directory, const comanchecd::FlightController& flight) {
    std::filesystem::create_directories(directory);
    world_runtime_probe::flight_state(directory / "flight.txt", flight.state());
    std::ofstream state(directory / "damage.txt");
    for (const auto condition : flight.state().system_condition) {
        state << unsigned(condition) << ' ';
    }
    const auto& player = flight.state();
    state << player.armor << ' ' << player.shake_ticks << ' ' << player.crash_rotation << ' '
          << unsigned(player.cockpit_mode) << ' ' << unsigned(player.exit_countdown) << ' '
          << unsigned(player.exit_requested) << ' ' << player.ended << ' ' << player.forward_limit << '\n';
    std::ofstream scene(directory / "scene.txt");
    scene << flight.scene().random_seed << ' ' << unsigned(flight.scene().remaining_targets) << '\n';
    std::ofstream entities(directory / "entities.txt");
    std::ofstream ballistics(directory / "ballistics.txt");
    std::ofstream ai(directory / "ai.txt");
    ai << unsigned(flight.scene().preset) << ' ' << unsigned(flight.scene().wingman_alert) << ' '
       << player.enemy_tracking << ' ' << player.incoming_hit << '\n';
    for (std::size_t slot = 0; slot < comanchecd::scene_slots; ++slot) {
        const auto& entity = flight.scene().entities[slot];
        entities << slot << ' ' << entity.active << ' ' << unsigned(entity.kind) << ' ' << unsigned(entity.phase)
                 << ' ' << entity.primary_payload << ' ' << entity.x << ' ' << entity.y << ' ' << entity.speed
                 << ' ' << unsigned(entity.angle) << ' ' << unsigned(entity.height) << ' ' << unsigned(entity.random_phase)
                 << ' ' << unsigned(entity.category_color) << ' ' << unsigned(entity.attribute7) << ' '
                 << unsigned(entity.damage_stage) << ' ' << unsigned(entity.animation_frame) << ' ' << unsigned(entity.target)
                 << ' ' << entity.previous_x << ' ' << entity.previous_y << ' ' << unsigned(entity.previous_height) << '\n';
        ballistics << slot << ' ' << entity.vertical_velocity << ' ' << entity.ballistic_height << '\n';
        ai << slot << ' ' << unsigned(entity.rotor_phase) << ' ' << unsigned(entity.homing_target) << ' '
           << unsigned(entity.avoidance_turn) << ' ' << unsigned(entity.projected_x) << ' '
           << unsigned(entity.attribute9) << ' ' << entity.attribute10 << '\n';
    }
    std::ofstream(directory / "weapon.txt") << unsigned(player.weapon) << ' ' << player.firing << '\n';
    if (!state || !scene || !entities) {
        throw std::runtime_error("Cannot write contact state");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::invalid_argument("Usage: contact_probe INSTALLATION PAYLOAD_DIRECTORY INPUT OUTPUT");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        std::ifstream input(argv[3]);
        const std::filesystem::path output(argv[4]);
        if (!input || std::filesystem::exists(output)) {
            throw std::invalid_argument("Use a readable fixture and fresh output directory");
        }
        auto frontend = installation.load_frontend();
        auto resources = frontend.mission_resources;
        const auto missions = installation.load_missions(0);
        const auto& mission = missions.missions.at(0);
        for (std::size_t index = 11; index < resources.size(); ++index) {
            resources[index][0] = '-';
        }
        std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
        auto setup = comanchecd::parse_gameplay(mission, resources, frontend.gameplay_presets, {});
        const auto payloads = comanchecd::load_gameplay(setup, resources,
            [&](const auto& name, auto kind) { return installation.load_payload(name, kind); });
        auto assets = comanchecd::materialize_scene(setup, payloads, frontend.transfer);
        auto tables = installation.load_terrain_tables();
        world_output::read_depth_padding(input, tables);
        std::string schema;
        input >> schema;
        const auto ai = schema == "ai-v1";
        const auto weapons = schema == "weapons-v1" || ai;
        if (schema != "contacts-v1" && !weapons) {
            throw std::invalid_argument("Unsupported contact fixture schema");
        }
        comanchecd::MissionScene scene;
        unsigned height, gap, terrain_variant, targets;
        input >> setup.x >> setup.y >> setup.heading >> height >> scene.random_seed >> scene.range
              >> setup.ammunition >> gap >> terrain_variant >> targets;
        if (!input || std::max({height, gap, targets}) > 255 || terrain_variant > 1) {
            throw std::invalid_argument("Invalid contact entry fields");
        }
        scene.player_height = static_cast<std::uint8_t>(height);
        scene.remaining_targets = static_cast<std::uint8_t>(targets);
        tables.initial_height_gap = static_cast<std::uint8_t>(gap);
        setup.terrain_variant = static_cast<std::uint8_t>(terrain_variant);
        for (auto& condition : setup.weapons) {
            unsigned value;
            if (!(input >> value) || value > 255) {
                throw std::invalid_argument("Invalid system condition byte");
            }
            condition = static_cast<std::uint8_t>(value);
        }
        for (auto& count : scene.vehicle_parameters) {
            input >> count;
        }
        if (ai) {
            unsigned preset, alert;
            input >> preset >> scene.wingman >> alert >> scene.attack_allies >> scene.extended_enemy_range;
            scene.preset = static_cast<std::uint8_t>(preset);
            scene.wingman_alert = static_cast<std::uint8_t>(alert);
        }
        for (std::size_t slot = 0; slot < comanchecd::scene_slots; ++slot) {
            auto& entity = scene.entities[slot];
            unsigned active, kind, phase, angle, age, color, health, damage, frame, target, old_height;
            input >> active >> kind >> phase >> entity.primary_payload >> entity.x >> entity.y >> entity.speed
                  >> angle >> height >> age >> color >> health >> damage >> frame >> target
                  >> entity.previous_x >> entity.previous_y >> old_height;
            if (!input || active > 1 || phase > 1 ||
                    std::max({kind, angle, height, age, color, health, damage, frame, target, old_height}) > 255) {
                throw std::invalid_argument("Invalid contact entity fields");
            }
            entity.active = active != 0;
            entity.kind = static_cast<std::uint8_t>(kind);
            entity.phase = static_cast<comanchecd::EntityPhase>(phase);
            entity.angle = static_cast<std::uint8_t>(angle);
            entity.height = static_cast<std::uint8_t>(height);
            entity.random_phase = static_cast<std::uint8_t>(age);
            entity.category_color = static_cast<std::uint8_t>(color);
            entity.attribute7 = static_cast<std::uint8_t>(health);
            entity.damage_stage = static_cast<std::uint8_t>(damage);
            entity.animation_frame = static_cast<std::uint8_t>(frame);
            entity.target = static_cast<std::uint8_t>(target);
            entity.previous_height = static_cast<std::uint8_t>(old_height);
            if (weapons) {
                input >> entity.vertical_velocity >> entity.ballistic_height;
            }
            if (ai) {
                unsigned rotor, target_slot, avoidance, projection, mask;
                input >> rotor >> target_slot >> avoidance >> projection >> mask >> entity.attribute10;
                input >> entity.primary_model >> entity.secondary_model;
                entity.rotor_phase = static_cast<std::uint8_t>(rotor);
                entity.homing_target = static_cast<std::uint8_t>(target_slot);
                entity.avoidance_turn = static_cast<std::uint8_t>(avoidance);
                entity.projected_x = static_cast<std::uint8_t>(projection);
                entity.attribute9 = static_cast<std::uint8_t>(mask);
            }
        }
        comanchecd::MissionAudio audio(installation.load_audio(), payloads, read_audio(input));
        comanchecd::FlightController flight(setup, scene, tables, payloads.size());
        const comanchecd::TerrainRenderer renderer(tables);
        std::filesystem::create_directories(output);
        snapshot(output / "entry", flight);
        std::ofstream(output / "entry/cues.txt");
        std::ofstream(output / "entry/requests.txt");
        std::ofstream entries(output / "initial-entities.txt");
        for (const auto& entry : installation.load_scene_entry().entities) {
            entries << entry.previous_x << ' ' << entry.previous_y << ' ' << unsigned(entry.previous_height)
                    << ' ' << unsigned(entry.animation_frame) << '\n';
        }
        entries.close();
        if (!entries) {
            throw std::runtime_error("Cannot write inherited entity entries");
        }
        std::size_t calls, request_cursor = 0;
        input >> calls;
        if (!input || calls == 0 || calls > 64) {
            throw std::invalid_argument("Invalid contact call count");
        }
        for (std::size_t call = 0; call < calls; ++call) {
            std::string operation;
            unsigned throttle, fire;
            std::size_t patches;
            comanchecd::FlightControls controls;
            input >> operation >> controls.horizontal >> controls.vertical >> throttle >> fire >> patches;
            comanchecd::TargetingFrame targeting;
            if (weapons) {
                unsigned weapon, acquire;
                input >> weapon >> acquire >> targeting.candidate;
                controls.weapon = static_cast<comanchecd::WeaponSelection>(weapon);
                controls.cycle_target = acquire != 0;
                for (auto& projection : targeting.entities) {
                    unsigned x, y, bearing;
                    input >> projection.depth >> x >> y >> bearing;
                    projection.x = static_cast<std::uint8_t>(x);
                    projection.y = static_cast<std::uint8_t>(y);
                    projection.bearing = static_cast<std::uint8_t>(bearing);
                }
            }
            constexpr auto terrain_bytes = comanchecd::terrain_side * comanchecd::terrain_side;
            if (!input || patches > terrain_bytes * 2 || throttle > 15 || fire > 1) {
                throw std::invalid_argument("Invalid contact call inputs");
            }
            controls.throttle = static_cast<std::uint8_t>(throttle);
            controls.fire = fire != 0;
            for (std::size_t patch = 0; patch < patches; ++patch) {
                std::size_t index;
                unsigned value;
                input >> index >> value;
                if (!input || value > 255) {
                    throw std::invalid_argument("Invalid terrain patch");
                }
                if (index < terrain_bytes) {
                    assets.terrain.height.at(index) = static_cast<std::uint8_t>(value);
                } else {
                    assets.terrain.color.at(index - terrain_bytes) = static_cast<std::uint8_t>(value);
                }
            }
            const auto directory = output / std::to_string(call);
            std::filesystem::create_directories(directory);
            if (operation == "flight") {
                flight.step(controls, assets, {}, weapons ? &targeting : nullptr);
                if (weapons) {
                    flight.refresh_target(targeting);
                }
                audio.set_engine_power(flight.state().engine_power);
            } else if (operation == "entities") {
                flight.update_entities(assets);
            } else if (operation == "render") {
                auto frame = renderer.render(assets, flight.state().view);
                renderer.shade_distance(frame, assets, setup.terrain_variant);
                flight.update_entities(assets);
                renderer.draw_models(frame, assets, flight.scene(), flight.state().view);
                flight.observe_projection(frame.targeting);
                world_output::bytes(directory / "scene.bin", frame.pixels);
                world_output::bytes(directory / "coverage.bin", frame.write_mask);
                world_output::bytes(directory / "depth.bin", frame.depth);
            } else if (operation != "backup") {
                throw std::invalid_argument("Unknown contact operation");
            }
            snapshot(directory, flight);
            std::ofstream cues(directory / "cues.txt");
            if (operation != "backup") {
                for (const auto& cue : flight.sounds()) {
                    cues << unsigned(cue.bank) << ' ' << cue.sound << ' ' << unsigned(cue.attenuation) << '\n';
                    audio.play_cue(cue);
                }
            }
            std::ofstream requests(directory / "requests.txt");
            while (request_cursor < audio.sample_requests().size()) {
                const auto& request = audio.sample_requests()[request_cursor++];
                requests << unsigned(request.bank) << ' ' << request.sound << ' ' << unsigned(request.voice)
                         << ' ' << unsigned(request.attenuation) << ' ' << request.step << '\n';
            }
            cues.close();
            requests.close();
            if (!cues || !requests) {
                throw std::runtime_error("Cannot write contact sound events");
            }
        }
        input >> std::ws;
        if (!input.eof()) {
            throw std::invalid_argument("Trailing contact fixture input");
        }
        audio_output::write(output / "audio", audio, 1);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
