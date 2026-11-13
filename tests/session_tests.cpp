#include "comanchecd/session.hpp"
#include "profile.hpp"
#include "settings_profile.hpp"
#include "training_route.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void launch(comanchecd::GameSession& session) {
    using namespace comanchecd;
    if (session.menu_state().mode == FrontendMode::pilot) {
        session.consume({0x1c});
        if (session.menu_state().pilot_mode == PilotMode::editor) {
            for (const auto scan : {0x31, 0x1e, 0x14, 0x17, 0x2f, 0x12}) {
                session.consume({static_cast<std::uint8_t>(scan)});
            }
            session.consume({0x1c});
            session.consume({0x1c});
        }
    }
    require(session.menu_state().mode == FrontendMode::main, "Accepted pilot must reach operations");
    session.consume({0x1c});
    while (session.menu_state().selection != 0) {
        session.consume({0x48});
    }
    session.consume({0x1c});
    while (session.menu_state().selection != 0) {
        session.consume({0x48});
    }
    session.consume({0x1c});
    require(session.menu_state().mode == FrontendMode::briefing, "Mission selection must reach its original briefing");
    session.consume({0x1c});
    require(session.flying(), "Accepted briefing must construct and present the mission");
}

void crash(comanchecd::GameSession& session, const comanchecd::Installation& installation) {
    const auto frontend = installation.load_frontend();
    const auto mission = installation.load_missions(0).missions.at(0);
    const auto setup = comanchecd::parse_gameplay(mission, frontend.mission_resources, frontend.gameplay_presets, {});
    const comanchecd::GameplayObject* target = nullptr;
    auto nearest = std::numeric_limits<std::uint64_t>::max();
    constexpr unsigned stationary_object_kind = 3;
    for (const auto& object : setup.objects) {
        if (object.type != stationary_object_kind) {
            continue;
        }
        const auto dx = std::int64_t(std::int32_t(object.x - session.flight_state().view.x));
        const auto dy = std::int64_t(std::int32_t(object.y - session.flight_state().view.y));
        const auto distance = std::uint64_t(dx * dx) + std::uint64_t(dy * dy);
        if (distance < nearest) {
            target = &object;
            nearest = distance;
        }
    }
    require(target != nullptr, "Training must supply a collision object");
    constexpr double full_turn = 6.283185307179586;
    constexpr double heading_units = 4294967296.0;
    constexpr int steering_limit = 350;
    constexpr int heading_error_scale = 0x100000;
    constexpr int yaw_damping_scale = 0x40000;
    unsigned intermediate_frames = 0;
    for (unsigned tick = 0; tick < 1000 && session.flying(); ++tick) {
        const auto& state = session.flight_state();
        auto angle = std::atan2(double(std::int32_t(target->x - state.view.x)),
                               -double(std::int32_t(target->y - state.view.y)));
        if (angle < 0) {
            angle += full_turn;
        }
        const auto error = std::int32_t(std::uint32_t(angle * heading_units / full_turn) - state.view.heading);
        comanchecd::FlightControls controls;
        controls.throttle = 1;
        controls.vertical = -steering_limit;
        controls.horizontal = std::clamp(error / heading_error_scale - state.yaw_rate / yaw_damping_scale,
                                         -steering_limit, steering_limit);
        session.advance(controls, {});
        intermediate_frames += static_cast<unsigned>(session.intermediate_images().size());
        session.render_audio(comanchecd::training_samples_per_tick);
    }
    require(!session.flying() && session.menu_state().mode == comanchecd::FrontendMode::debrief && intermediate_frames == 1,
            "The session must preserve the nested crash presentation and reach debrief automatically");
}

void presentation_reads(const comanchecd::Installation& installation) {
    using namespace comanchecd;
    GameSession observed(installation, [](const auto&) {});
    GameSession reference(installation, [](const auto&) {});
    launch(observed);
    launch(reference);
    CockpitControls cameras;
    cameras.left = InstrumentDisplay::camera;
    cameras.right = InstrumentDisplay::camera;
    for (unsigned tick = 0; tick < 25; ++tick) {
        const auto held = observed.presentation();
        for (unsigned read = 0; read < 16; ++read) {
            const auto frame = observed.presentation();
            require(frame->cpu_cycles == held->cpu_cycles && frame->irq_ticks == held->irq_ticks &&
                    frame->random_seed == held->random_seed && frame->image.pixels == held->image.pixels,
                    "Repeated flight presentation reads must preserve clocks, RNG and cockpit pixels");
        }
        observed.advance({120, -80, 1}, cameras);
        reference.advance({120, -80, 1}, cameras);
        require(observed.render_audio(training_samples_per_tick) == reference.render_audio(training_samples_per_tick) &&
                observed.cpu_cycles() == reference.cpu_cycles() && observed.random_seed() == reference.random_seed() &&
                observed.irq_ticks() == reference.irq_ticks() && observed.image().pixels == reference.image().pixels &&
                observed.image().palette == reference.image().palette,
                "Presentation reads must preserve subsequent dual-camera execution and all PCM samples");
        require(held->cpu_cycles < observed.cpu_cycles(), "A held flight snapshot must survive later authoritative work");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            return 0;
        }
        require(argc == 3 || argc == 4, "Optional integration arguments are INSTALLATION PAYLOAD_DIRECTORY [FRESH_PROFILE_DIRECTORY]");
        presentation_reads(comanchecd::Installation(argv[1], argv[2]));
        const comanchecd::host::PilotProfile profile(argc == 4 ? argv[3] : "");
        const comanchecd::host::SettingsProfile settings_profile(argc == 4 ? argv[3] : "");
        if (argc == 4) {
            require(!std::filesystem::exists(argv[3]), "Profile integration requires a fresh directory");
        }
        std::vector<std::uint8_t> saved;
        unsigned writes = 0;
        std::vector<std::uint8_t> saved_settings;
        unsigned settings_writes = 0;
        comanchecd::GameSession session(comanchecd::Installation(argv[1], argv[2]), [&](const auto& record) {
            if (argc == 4) {
                profile.write(record);
                require(profile.read() == record, "Portable profile driver must preserve every pilot byte");
            }
            saved = record;
            ++writes;
        }, {}, {}, [&](const auto& record) {
            if (argc == 4) {
                settings_profile.write(record);
                require(settings_profile.read() == record, "Settings profile must preserve all 43 bytes");
            }
            saved_settings = record;
            ++settings_writes;
        });
        comanchecd::IrqState startup_rng{comanchecd::training_scene_seed};
        for (std::uint64_t tick = 0; tick < session.irq_ticks(); ++tick) { startup_rng.interrupt(); }
        require(session.random_seed() == startup_rng.random_seed,
                "Menu initialization IRQs must use the session RNG receiver");
        const auto before_menu_irq = session.irq_ticks();
        comanchecd::IrqState expected_menu_rng{session.random_seed()};
        const auto menu_audio = session.render_audio(comanchecd::MissionAudio::sample_rate);
        for (auto tick = before_menu_irq; tick < session.irq_ticks(); ++tick) { expected_menu_rng.interrupt(); }
        require(session.random_seed() == expected_menu_rng.random_seed,
                "Menu PIT IRQs must advance the RNG carried into flight");
        require(menu_audio.size() == comanchecd::MissionAudio::sample_rate &&
                std::any_of(menu_audio.begin(), menu_audio.end(), [](auto sample) { return sample != 0; }),
                "Operations must play its original music without requiring a mission launch");
        const auto menu_ticks = session.irq_ticks();
        launch(session);
        require(session.irq_ticks() > menu_ticks, "Mission loading must advance the inherited menu IRQ clock");
        const auto before = writes;
        const auto retained_view = session.flight_state().view;
        const auto retained_ammo = session.flight_state().ammunition;
        const auto retained_image = session.image().pixels;
        session.consume({0x01});
        require(session.flying() && session.menu_state().mode == comanchecd::FrontendMode::settings,
                "Flight Escape must open settings while retaining the mission");
        require(session.flight_state().shake_ticks <= 1 && session.flight_state().shake_offset == 0,
                "Settings drawing must allow IRQs to consume the one-tick shake reset");
        const auto settings_entry_ticks = session.irq_ticks();
        session.advance({120, -120, 1}, {});
        session.render_audio(comanchecd::MissionAudio::sample_rate);
        require(session.flight_state().view.x == retained_view.x && session.flight_state().view.y == retained_view.y &&
                session.flight_state().ammunition == retained_ammo && session.flight_state().shake_ticks == 0,
                "Settings must retain the flight without running a simulation frame");
        require(session.irq_ticks() >= settings_entry_ticks,
                "Menu drawing and audio must retain a monotonically advancing IRQ clock");
        const auto scenery_settings_ticks = session.irq_ticks();
        comanchecd::IrqState expected_settings_rng{session.random_seed()};
        session.consume({0x4d});
        for (unsigned row = 0; row < 4; ++row) {
            session.consume({0x50});
        }
        session.consume({0x1c});
        session.consume({0x1c});
        session.consume({0x50});
        session.consume({0x50});
        session.consume({0x1c});
        session.consume({0x1c});
        for (auto tick = scenery_settings_ticks; tick < session.irq_ticks(); ++tick) { expected_settings_rng.interrupt(); }
        require(session.flight_state().view.x == retained_view.x && session.random_seed() == expected_settings_rng.random_seed &&
                session.irq_ticks() > scenery_settings_ticks,
                "Settings redraw must advance IRQ/RNG work while retaining the flight pose");
        session.consume({0x01});
        require(session.flying() && session.image().pixels == retained_image,
                "Closing settings must restore the retained cockpit");
        for (unsigned tick = 0; tick < 12; ++tick) {
            session.advance({}, {});
            session.render_audio(comanchecd::training_samples_per_tick);
        }
        const auto flight_ticks = session.irq_ticks();
        const auto flight_seed = session.random_seed();
        session.consume({0x01});
        for (unsigned row = 0; row < 3; ++row) {
            session.consume({0x50});
        }
        session.consume({0x1c});
        comanchecd::IrqState expected_abort_rng{flight_seed};
        for (auto tick = flight_ticks; tick < session.irq_ticks(); ++tick) { expected_abort_rng.interrupt(); }
        require(session.irq_ticks() > flight_ticks && session.random_seed() == expected_abort_rng.random_seed,
                "Settings abort and debrief drawing must carry the flight RNG through every intervening IRQ");
        comanchecd::IrqState expected_debrief_rng{session.random_seed()};
        const auto before_debrief_irq = session.irq_ticks();
        const auto debrief_audio = session.render_audio(comanchecd::MissionAudio::sample_rate);
        for (auto tick = before_debrief_irq; tick < session.irq_ticks(); ++tick) { expected_debrief_rng.interrupt(); }
        require(session.random_seed() == expected_debrief_rng.random_seed,
                "Debrief IRQs must resume from the completed flight RNG");
        require(std::any_of(debrief_audio.begin(), debrief_audio.end(), [](auto sample) { return sample != 0; }),
                "Mission return must play the original debrief theme");
        require(!session.flying() && session.menu_state().mode == comanchecd::FrontendMode::debrief && writes == before + 1,
                "Mission abort must release flight, save once and show the original debrief");
        const auto debrief = session.image().pixels;
        session.consume({0x1c});
        require(session.image().pixels == debrief, "The original debrief waits specifically for Escape");
        session.consume({0x01});
        require(session.menu_state().mode == comanchecd::FrontendMode::main, "Debrief Escape must return to operations");
        session.consume({0x01});
        require(!session.quit_requested() && session.menu_state().mode == comanchecd::FrontendMode::settings,
                "Operations Escape must open the original settings menu");
        session.consume({0x4b});
        session.consume({0x48});
        session.consume({0x48});
        session.consume({0x1c});
        require(settings_writes == 0, "Toggling auto chaff must not implicitly save settings");
        session.consume({0x4d});
        session.consume({0x39});
        session.consume({0x39});
        require(settings_writes == 0, "Space must move the settings selection without activating it");
        session.consume({0x1c});
        require(settings_writes == 1 && saved_settings.size() == 43 && saved_settings[39] == 123,
                "Save Menu Settings must persist the changed auto-chaff marker");
        session.consume({0x1c});
        session.consume({0x01});
        require(session.menu_state().mode == comanchecd::FrontendMode::main,
                "Closing settings must resume operations");
        {
            const comanchecd::Installation installation(argv[1], argv[2]);
            unsigned saves = 0;
            comanchecd::GameSession commands(installation, [](const auto&) {}, saved, {}, [&](const auto& record) {
                require(record == installation.load_menu_settings().record(),
                        "Rejected legacy input options must preserve every saved settings byte");
                ++saves;
            });
            commands.consume({0x1c});
            commands.consume({0x01});
            commands.consume({0x4d});
            commands.consume({0x4d});
            commands.consume({0x48});
            commands.consume({0x1c});
            require(commands.menu_state().mode == comanchecd::FrontendMode::settings &&
                    commands.presentation()->flight_notice == comanchecd::FlightNotice::unavailable_calibration,
                    "Unavailable calibration must retain the settings menu");
            for (unsigned row = 0; row < 7; ++row) {
                commands.consume({0x48});
                commands.consume({0x1c});
                require(commands.menu_state().mode == comanchecd::FrontendMode::settings &&
                        commands.presentation()->flight_notice == comanchecd::FlightNotice::unavailable_input_mode,
                        "Unavailable legacy input options must retain the settings menu and identify the limit");
            }
            commands.consume({0x4b});
            require(commands.presentation()->flight_notice == comanchecd::FlightNotice::none,
                    "Supported menu navigation must clear the preview notice");
            commands.consume({0x4b});
            commands.consume({0x50});
            commands.consume({0x50});
            commands.consume({0x1c});
            require(saves == 1, "Supported settings save must still reach persistence");
        }
        for (const auto choice : {1, 5}) {
            for (int row = 0; row < choice; ++row) {
                session.consume({0x50});
            }
            session.consume({0x1c});
            require(session.menu_state().mode == (choice == 1 ? comanchecd::FrontendMode::statistics :
                                                               comanchecd::FrontendMode::credits),
                    "Information pages must open without ending the session");
            session.consume({0x39});
            require(session.menu_state().mode == comanchecd::FrontendMode::main &&
                    session.menu_state().selection == 0, "Any key must return from an information page");
        }
        const auto overview = comanchecd::Installation(argv[1], argv[2]).load_overview();
        const auto overview_ticks = session.irq_ticks();
        const auto before_overview = writes;
        for (unsigned row = 0; row < 3; ++row) {
            session.consume({0x50});
        }
        session.consume({0x1c});
        require(session.menu_state().mode == comanchecd::FrontendMode::overview &&
                    session.irq_ticks() >= overview_ticks, "Overview decoding must advance the retained music and IRQ clock");
        const auto background = session.image().pixels;
        std::size_t movie_frames = 0;
        while (movie_frames < overview.frame_count() + 1) {
            session.render_audio(comanchecd::training_samples_per_tick);
            for (const auto& image : session.intermediate_images()) {
                const auto expected = overview.frame(movie_frames % overview.frame_count());
                require(std::equal(expected.begin(), expected.end(), image.pixels.begin()) &&
                        image.palette == overview.palette(), "Overview must present every decoded frame in order and loop");
                require(std::equal(background.begin() + expected.size(), background.end(),
                                   image.pixels.begin() + expected.size()), "Overview must retain the lower hangar background");
                ++movie_frames;
            }
        }
        session.consume({0x39});
        require(session.menu_state().mode == comanchecd::FrontendMode::main && writes == before_overview &&
                session.intermediate_images().empty(), "Any key must leave overview without changing the pilot");
        launch(session);
        require(session.flight_state().ammunition[2] == 8, "A new launch must restore its mission weapon inventory");
        const auto before_crash = writes;
        crash(session, comanchecd::Installation(argv[1], argv[2]));
        require(writes == before_crash + 1, "Automatic crash return must save the pilot exactly once");
        session.consume({0x01});
        launch(session);
        const auto before_victory = writes;
        const training_test::Route route(comanchecd::Installation(argv[1], argv[2]));
        for (unsigned tick = 0; tick < training_test::victory_tick_limit && session.flying(); ++tick) {
            session.advance(route.controls(session.flight_state(), tick), {});
            session.render_audio(comanchecd::training_samples_per_tick);
        }
        const comanchecd::PilotStorage completed(saved);
        require(!session.flying() && session.menu_state().mode == comanchecd::FrontendMode::debrief &&
                writes == before_victory + 1 && completed.completed(completed.selected(), 0, 0),
                "Natural victory must reach debrief and persist training completion exactly once");
        if (argc == 4) {
            require(std::filesystem::file_size(std::filesystem::path(argv[3]) / "COMANCHE.NAM.bak") == comanchecd::pilot_record_bytes,
                    "Replacing the profile must retain a complete prior record");
            saved = profile.read();
        }
        comanchecd::GameSession resumed(comanchecd::Installation(argv[1], argv[2]), [](const auto&) {}, saved);
        require(resumed.menu_state().selected_pilot == session.menu_state().selected_pilot,
                "A new session must load the saved pilot selection");
        std::cout << "Menu, launch, abort, crash, natural victory, debrief and profile reload passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
