#include "comanchecd/session.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void controller_test() {
    using namespace comanchecd;
    GameplaySetup setup;
    MissionScene scene;
    scene.range = 1000;
    scene.player_height = 80;
    scene.vehicle_parameters[2] = 5;
    TerrainTables tables;
    tables.sine[128] = 0x7fffffff;
    SceneAssets assets;
    assets.terrain.height.assign(terrain_side * terrain_side, 0);
    FlightController flight(setup, scene, tables, 20);
    flight.enter_recording({0x12345678, 0x23456789, 0, 80});
    FlightControls controls;
    controls.source = ControlSource::recording;
    controls.recorded_power = 124;
    controls.horizontal = -512;
    controls.vertical = 512;
    controls.fire = true;
    flight.step(controls, assets);
    require(flight.state().control_power == 124 && flight.state().ammunition[2] == 4,
            "Recording must preserve absolute power and signed controls beyond keyboard limits");
    flight.step(controls, assets);
    require(flight.state().ammunition[2] == 3, "Adjacent recorded fire events must both launch");
    require(flight.recorded_key(0x1a, assets) == RecordedAction::stores && flight.state().weapon == 1,
            "Recorded weapon selection must be applied after the controller pass");
    require(flight.recorded_key(7, assets) == RecordedAction::view &&
            flight.recorded_key(7, assets) == RecordedAction::none, "Repeated chase selection must not redraw");
    require(flight.recorded_key(8, assets) == RecordedAction::view, "Fixed camera must capture its anchor");
    const auto first = flight.render_view(assets);
    flight.step(controls, assets);
    const auto second = flight.render_view(assets);
    require(first.mode == SceneViewMode::fixed && first.x == second.x && first.y == second.y &&
            first.height == 10 && second.pitch == 0 && second.roll == 0,
            "Fixed camera must retain position while tracking the player");
}

void open_demo(comanchecd::GameSession& session) {
    using namespace comanchecd;
    session.consume({0x1c});
    if (session.menu_state().pilot_mode == PilotMode::editor) {
        session.consume({0x20});
        session.consume({0x1c});
        session.consume({0x1c});
    }
    require(session.menu_state().mode == FrontendMode::main, "Pilot must reach operations");
    for (unsigned row = 0; row < 4; ++row) {
        session.consume({0x50});
    }
    session.consume({0x1c});
    require(session.demo_playing() && !session.flying(), "View Demo must start its title sequence");
}

void presentation_chunks(const comanchecd::Installation& installation) {
    using namespace comanchecd;
    GameSession whole(installation, [](const auto&) {}), split(installation, [](const auto&) {});
    open_demo(whole);
    open_demo(split);
    const auto image_hashes = [](const auto& images) {
        std::vector<std::uint64_t> result;
        for (const auto& image : images) {
            std::uint64_t hash = 14695981039346656037ull;
            for (const auto value : image.pixels) { hash = (hash ^ value) * 1099511628211ull; }
            for (const auto& color : image.palette) {
                for (const auto value : color) { hash = (hash ^ value) * 1099511628211ull; }
            }
            result.push_back(hash);
        }
        return result;
    };
    constexpr std::size_t samples = audio_sample_rate * 6;
    const auto expected = whole.render_audio(samples);
    const auto expected_images = image_hashes(whole.intermediate_images());
    std::vector<std::int16_t> actual;
    std::vector<std::uint64_t> actual_images;
    for (const auto size : {1u, 47u, 509u, 2048u, 731u, 65537u, 32768u, 16384u, 32768u, 32768u, 8040u, 72999u}) {
        const auto count = std::min<std::size_t>(size, samples - actual.size());
        const auto block = split.render_audio(count);
        actual.insert(actual.end(), block.begin(), block.end());
        const auto images = image_hashes(split.intermediate_images());
        actual_images.insert(actual_images.end(), images.begin(), images.end());
    }
    const auto block = split.render_audio(samples - actual.size());
    actual.insert(actual.end(), block.begin(), block.end());
    const auto images = image_hashes(split.intermediate_images());
    actual_images.insert(actual_images.end(), images.begin(), images.end());
    require(expected == actual && expected_images.size() > 1 && expected_images == actual_images &&
            whole.irq_ticks() == split.irq_ticks(),
            "Movie work, queued PCM and ordered presentations must survive arbitrary host audio chunks");
}

void integration(const comanchecd::Installation& installation) {
    using namespace comanchecd;
    presentation_chunks(installation);
    const auto frontend = installation.load_frontend();
    const auto missions = installation.load_missions(0);
    for (std::uint8_t index = 0; index < 4; ++index) {
        auto resources = frontend.mission_resources;
        for (std::size_t slot = 11; slot < resources.size(); ++slot) {
            resources[slot][0] = '-';
        }
        const auto& mission = missions.missions.at(index);
        std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
        auto recording = std::make_shared<DemoRecording>(installation.load_demo(index));
        FlightMission selected{parse_gameplay(mission, resources, frontend.gameplay_presets, {}), resources,
                               installation.load_menu_settings().record(), recording};
        TrainingFlight flight(installation, selected);
        require(flight.demo_position() == 1, "Mission entry must consume the first demo record");
        std::size_t nested = 0;
        std::uint64_t hash = 14695981039346656037ull;
        for (std::size_t tick = 0; tick < recording->inputs().size() && !flight.demo_finished() && !flight.state().ended; ++tick) {
            const auto previous = flight.demo_position();
            flight.advance({});
            const auto consumed = flight.demo_position() - previous;
            require(consumed == 1 || consumed == 3, "A view change must consume two nested records");
            nested += consumed - 1;
            for (const auto byte : flight.cockpit_image().pixels) {
                hash = (hash ^ byte) * 1099511628211ull;
            }
            flight.render_audio(training_samples_per_tick);
        }
        std::cout << "demo " << unsigned(index) << " records " << flight.demo_position() << '/' << recording->inputs().size()
                  << " nested " << nested << " ended " << flight.state().ended << " pixels " << hash << std::endl;
        require(flight.demo_finished(), "Recording must run to completion without an early mission exit");
    }

    unsigned writes = 0;
    GameSession session(installation, [&](const auto&) { ++writes; });
    open_demo(session);
    const auto before = writes;
    bool first_movie = false;
    for (unsigned step = 0; step < 600 && !first_movie; ++step) {
        session.render_audio(441);
        first_movie = !session.intermediate_images().empty() &&
            std::any_of(session.image().pixels.begin(), session.image().pixels.end(), [](auto pixel) { return pixel != 0; });
    }
    require(first_movie, "The fanfare must reach TANIM's first presentation");
    const auto& movie_image = session.image();
    require(std::all_of(movie_image.pixels.begin(), movie_image.pixels.begin() + 24 * cockpit_width,
                [](auto pixel) { return pixel == 0; }) &&
            std::all_of(movie_image.pixels.begin() + 216 * cockpit_width, movie_image.pixels.end(),
                [](auto pixel) { return pixel == 0; }),
            "TANIM must clear both pages before presenting, including the rows outside its movie rectangle");
    unsigned started = 0, finished = 0;
    std::uint64_t pcm_hash = 14695981039346656037ull;
    for (unsigned step = 0; step < 12000 && finished < 4; ++step) {
        const auto was_flying = session.flying();
        if (was_flying) {
            session.advance({}, {});
        }
        const auto pcm = session.render_audio(training_samples_per_tick);
        for (const auto sample : pcm) {
            pcm_hash = (pcm_hash ^ std::uint16_t(sample)) * 1099511628211ull;
        }
        if (!was_flying && session.flying()) {
            ++started;
        } else if (was_flying && !session.flying()) {
            ++finished;
        }
    }
    require(started == 4 && finished == 4, "Attract cycle must run all four cinematics and flights");
    session.consume({0x39});
    require(!session.demo_playing() && !session.flying() && session.menu_state().mode == FrontendMode::main && writes == before,
            "Any key must leave attract playback without writing pilot results");
    std::cout << "attract flights " << finished << " audio " << pcm_hash << std::endl;
    for (unsigned row = 0; row < 4; ++row) {
        session.consume({0x50});
    }
    session.consume({0x1c});
    for (unsigned step = 0; step < 1000 && !session.flying(); ++step) {
        session.render_audio(training_samples_per_tick);
    }
    require(session.flying(), "Restarted attract sequence must reach its first flight");
    session.consume({0x3b});
    require(!session.demo_playing() && !session.flying() && session.menu_state().mode == FrontendMode::main && writes == before,
            "Any key in a recorded flight must return to operations without debrief or pilot writes");
}
}

int main(int argc, char** argv) {
    try {
        controller_test();
        if (argc == 3) {
            integration(comanchecd::Installation(argv[1], argv[2]));
        } else {
            require(argc == 1, "demo_tests [INSTALLATION PAYLOAD_DIRECTORY]");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
