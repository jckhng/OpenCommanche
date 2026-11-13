#include "comanchecd/training.hpp"
#include "comanchecd/keyboard.hpp"
#include "training_route.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void verify_loading_clock(const comanchecd::Installation& installation) {
    using namespace comanchecd;
    const auto frontend = installation.load_frontend();
    const auto definition = installation.load_missions(0).missions.at(0);
    auto resources = frontend.mission_resources;
    std::copy(definition.resources.begin(), definition.resources.end(), resources.begin());
    FlightMission selected{parse_gameplay(definition, resources, frontend.gameplay_presets, {}), resources};
    AudioEntry entry;
    entry.execution = AudioExecution::scheduled;
    entry.music_start = MusicStart::deferred;
    auto audio = std::make_shared<MissionAudio>(installation.load_audio(), installation.load_menu_music(),
                                               entry, AudioTracePolicy::discard);
    const auto before = audio->irq_ticks();
    {
        TrainingFlight flight(installation, selected, audio);
        require(audio->irq_ticks() > before + 1,
                "Mission loading must deliver timer interrupts before its first playable frame");
        const auto previous = flight.state();
        KeyboardInput keyboard;
        FlightInput input;
        input.take_key = [&](ExecutionClock& clock) { return keyboard.take_scan(clock); };
        FlightControls controls;
        controls.weapon = WeaponSelection::next;
        controls.chaff = controls.flare = true;
        flight.advance(controls, {}, input);
        require(flight.state().weapon == (previous.weapon + 1) % previous.ammunition.size() &&
                flight.state().chaff + 1 == previous.chaff && flight.state().flares + 1 == previous.flares,
                "A live keyboard consumer must preserve domain weapon and countermeasure commands");
        constexpr std::array<std::uint8_t, 7> unavailable_scans{0x03, 0x04, 0x05, 0x06, 0x24, 0x32, 0x37};
        constexpr std::array<FlightNotice, unavailable_scans.size()> expected_notices{
            FlightNotice::unavailable_view, FlightNotice::unavailable_view, FlightNotice::unavailable_view,
            FlightNotice::unavailable_view, FlightNotice::unavailable_calibration,
            FlightNotice::unavailable_rockets, FlightNotice::unavailable_input_mode};
        auto notice = FlightNotice::none;
        input.notice = [&](FlightNotice value) { notice = value; };
        controls = {};
        const auto mode = flight.state().cockpit_mode;
        const auto ammunition = flight.state().ammunition;
        for (std::size_t index = 0; index < unavailable_scans.size(); ++index) {
            const auto scan = unavailable_scans[index];
            input.take_key = [scan](ExecutionClock&) { return scan; };
            flight.advance(controls, {}, input);
            require(notice == expected_notices[index], "Unavailable commands must identify the missing preview behavior");
            require(flight.state().cockpit_mode == mode && flight.state().ammunition == ammunition,
                    "Unavailable preview commands must preserve the current view and stores");
        }
        input.take_key = [](ExecutionClock&) { return std::uint8_t{0x02}; };
        flight.advance(controls, {}, input);
        require(notice == FlightNotice::none && flight.state().cockpit_mode == mode,
                "Selecting the current supported view must clear an unavailable-command notice");
        input.take_key = [](ExecutionClock&) { return std::uint8_t{0x1b}; };
        flight.advance(controls, {}, input);
        require(notice == FlightNotice::none && flight.state().weapon == (previous.weapon + 2) % ammunition.size(),
                "A supported command must clear the notice and reach its normal consumer");
        constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 4> supported_views{{
            {0x07, 6}, {0x08, 7}, {0x09, 7}, {0x02, 1}}};
        for (const auto& view : supported_views) {
            input.take_key = [&](ExecutionClock&) { return view.first; };
            flight.advance(controls, {}, input);
            require(notice == FlightNotice::none && flight.state().cockpit_mode == view.second,
                    "Supported live view keys must reach their recovered drawing children");
        }
        flight.reset();
        flight.begin_settings();
        audio->execution_clock().execute(CpuOperation::instructions, ExecutionClock::cycles_per_second / 10);
        require(flight.state().shake_ticks == 0,
                "Reset must retain the replacement flight's IRQ subscription");
    }
    audio->execution_clock().execute(CpuOperation::instructions, ExecutionClock::cycles_per_second / 10);
}

std::uint64_t hash_bytes(const comanchecd::SceneImage& image) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto value : image.pixels) {
        hash = (hash ^ value) * 1099511628211ull;
    }
    return hash;
}

void verify_crash(comanchecd::TrainingFlight& flight, const comanchecd::Installation& installation) {
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
        const auto delta_x = std::int64_t(std::int32_t(object.x - flight.state().view.x));
        const auto delta_y = std::int64_t(std::int32_t(object.y - flight.state().view.y));
        const auto distance = std::uint64_t(delta_x * delta_x) + std::uint64_t(delta_y * delta_y);
        if (distance < nearest) {
            target = &object;
            nearest = distance;
        }
    }
    require(target != nullptr, "The original training mission must supply a collision target");
    constexpr double full_turn = 6.283185307179586;
    constexpr double heading_units = 4294967296.0;
    constexpr int steering_limit = 350;
    constexpr int heading_error_scale = 0x100000;
    constexpr int yaw_damping_scale = 0x40000;
    unsigned intermediate_frames = 0;
    for (unsigned tick = 0; tick < 1000 && !flight.state().ended; ++tick) {
        const auto& state = flight.state();
        const auto delta_x = std::int32_t(target->x - state.view.x);
        const auto delta_y = std::int32_t(target->y - state.view.y);
        auto angle = std::atan2(double(delta_x), -double(delta_y));
        if (angle < 0) {
            angle += full_turn;
        }
        const auto bearing = std::uint32_t(angle * heading_units / full_turn);
        const auto error = std::int32_t(bearing - state.view.heading);
        comanchecd::FlightControls controls;
        controls.throttle = 1;
        controls.vertical = -steering_limit;
        controls.horizontal = std::clamp(error / heading_error_scale - state.yaw_rate / yaw_damping_scale,
                                         -steering_limit, steering_limit);
        flight.advance(controls);
        flight.render_audio(comanchecd::training_samples_per_tick);
        intermediate_frames += static_cast<unsigned>(flight.intermediate_images().size());
        if (!flight.intermediate_images().empty()) {
            require(flight.state().cockpit_mode == 6 && flight.state().exit_countdown == 53,
                    "Live crash initialization must carry its two nested controller passes");
        }
    }
    require(flight.state().ended && flight.state().cockpit_mode == 6 && intermediate_frames == 1,
            "Live controls must reach external crash presentation and the terminal flight boundary");
    const auto final_image = flight.cockpit_image().pixels;
    flight.advance({});
    require(flight.cockpit_image().pixels == final_image, "Terminal flight must retain its last displayed image");
    flight.reset();
    require(!flight.state().ended, "Reset must recover from a completed crash");
}

void verify_guided_combat(comanchecd::TrainingFlight& flight, const comanchecd::Installation& installation) {
    const auto frontend = installation.load_frontend();
    const auto mission = installation.load_missions(0).missions.at(0);
    const auto setup = comanchecd::parse_gameplay(mission, frontend.mission_resources, frontend.gameplay_presets, {});
    const auto& destination = setup.objects.at(1);
    const auto objectives = flight.objectives_remaining();
    constexpr double full_turn = 6.283185307179586;
    constexpr double heading_units = 4294967296.0;
    constexpr int steering_limit = 350;
    constexpr int heading_error_scale = 0x100000;
    constexpr int yaw_damping_scale = 0x40000;
    constexpr int approach_distance = 0x18000000;
    bool acquired = false;
    for (unsigned tick = 0; tick < 400 && !flight.state().ended; ++tick) {
        const auto& state = flight.state();
        const auto dx = std::int32_t(destination.x - state.view.x);
        const auto dy = std::int32_t(destination.y - state.view.y);
        auto angle = std::atan2(double(dx), -double(dy));
        if (angle < 0) {
            angle += full_turn;
        }
        const auto bearing = std::uint32_t(angle * heading_units / full_turn);
        const auto error = std::int32_t(bearing - state.view.heading);
        comanchecd::FlightControls controls;
        controls.throttle = 1;
        controls.horizontal = std::clamp(error / heading_error_scale - state.yaw_rate / yaw_damping_scale,
                                         -steering_limit, steering_limit);
        controls.vertical = std::max(std::abs(dx), std::abs(dy)) > approach_distance ? -steering_limit : 0;
        controls.cycle_target = state.target.slot == 0 && tick % 4 == 0;
        controls.fire = state.target.slot != 0 && tick % 8 == 0;
        acquired |= state.target.slot != 0;
        flight.advance(controls);
        flight.render_audio(comanchecd::training_samples_per_tick);
    }
    require(acquired && flight.state().ammunition[2] < setup.vehicle_parameters[2] &&
            flight.objectives_remaining() < objectives && !flight.state().ended,
            "Live controls must acquire, guide, impact and remove training objectives");
    std::cout << "Guided flight removed " << unsigned(objectives - flight.objectives_remaining()) << " objectives\n";
    flight.reset();
}

void verify_scenery(const comanchecd::Installation& installation) {
    const auto frontend = installation.load_frontend();
    for (unsigned campaign = 0; campaign < frontend.campaign_count; ++campaign) {
        const auto missions = installation.load_missions(static_cast<std::uint8_t>(campaign));
        for (const auto& mission : missions.missions) {
            auto resources = frontend.mission_resources;
            for (std::size_t slot = 11; slot < resources.size(); ++slot) {
                resources[slot][0] = '-';
            }
            std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
            const auto setup = comanchecd::parse_gameplay(mission, resources, frontend.gameplay_presets, {});
            if (setup.terrain_variant == 0) {
                continue;
            }
            auto record = installation.load_menu_settings().record();
            record[29] = record[31] = 125;
            comanchecd::MenuSettings settings(record);
            const comanchecd::FlightMission selected{setup, resources, record};
            auto state = std::make_shared<comanchecd::ReflectionState>();
            state->random = 1;
            comanchecd::TrainingFlight flight(installation, selected, nullptr, comanchecd::training_scene_seed, state);
            const auto entry = state->random;
            require(entry != 1, "Water mission entry must consume the carried reflection state");
            settings.activate(1, 5);
            settings.activate(1, 7);
            flight.apply_settings(settings);
            flight.advance({});
            require(state->random == entry, "Reflections Off must retain RNG while large pixels render");
            settings.activate(1, 7);
            flight.apply_settings(settings);
            require(state->random == entry, "Reconfiguring terrain must not reset or consume reflection RNG");
            flight.advance({});
            require(state->random != entry, "Reflections On must resume the retained RNG");
            const auto carried = state->random;
            comanchecd::TrainingFlight next(installation, selected, nullptr, comanchecd::training_scene_seed, state);
            auto fresh = std::make_shared<comanchecd::ReflectionState>();
            fresh->random = carried;
            comanchecd::TrainingFlight reference(installation, selected, nullptr, comanchecd::training_scene_seed, fresh);
            require(state->random == fresh->random && next.image().pixels == reference.image().pixels,
                    "A subsequent mission must consume exactly the carried reflection state");
            return;
        }
    }
    throw std::runtime_error("No water mission found for scenery integration");
}

void verify_weapon_key_order(comanchecd::TrainingFlight& flight) {
    const auto ammunition = flight.state().ammunition;
    comanchecd::FlightControls controls;
    controls.fire = true;
    controls.weapon = comanchecd::WeaponSelection::previous;
    flight.advance(controls);
    require(flight.state().weapon == 1 && flight.state().ammunition[2] + 1 == ammunition[2] &&
            flight.state().ammunition[1] == ammunition[1],
            "Bracket selection must follow firing with the previously selected weapon");
    flight.advance({});
    controls.weapon = comanchecd::WeaponSelection::unchanged;
    flight.advance(controls);
    require(flight.state().ammunition[1] + 1 == ammunition[1],
            "The following trigger press must use the newly selected weapon");
    flight.reset();
}
}

int main(int argc, char** argv) {
    try {
        comanchecd::TerrainFrame frame;
        comanchecd::Palette palette{};
        palette[7] = {63, 12, 0};
        frame.pixels[255 * 256 + 16] = 7;
        frame.pixels[255 * 256 + 144] = 8;
        frame.pixels[111] = 9;
        frame.pixels[239] = 10;
        const auto image = comanchecd::scene_image(frame, palette);
        require(image.pixels[0] == 7 && image.pixels[1] == 8 && image.pixels[image.pixels.size() - 2] == 9 &&
                image.pixels.back() == 10 && image.palette == palette,
                "Scene presentation must invert rows and interleave plane pairs with the original palette");
        if (argc == 1) {
            return 0;
        }
        require(argc == 3, "Optional integration arguments are INSTALLATION PAYLOAD_DIRECTORY");
        const comanchecd::Installation installation(argv[1], argv[2]);
        verify_loading_clock(installation);
        comanchecd::TrainingFlight flight(installation);
        const auto initial = flight.image();
        const auto initial_cockpit = flight.cockpit_image();
        const auto initial_state = flight.state();
        comanchecd::FlightControls controls;
        controls.throttle = 15;
        for (unsigned tick = 0; tick < 44; ++tick) {
            flight.advance(controls);
            require(flight.render_audio(2205).size() == 2205, "A preview tick produces one audio block");
        }
        const auto before = flight.state();
        const auto before_image = flight.image();
        const auto before_cockpit = flight.cockpit_image();
        controls.horizontal = 351;
        bool rejected = false;
        try {
            flight.advance(controls);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && flight.state().range == before.range && flight.state().view.x == before.view.x &&
                flight.image().pixels == before_image.pixels && flight.cockpit_image().pixels == before_cockpit.pixels,
                "Unsupported controls must preserve the last complete tick and cockpit presentation");
        std::cout << "Training frame " << hash_bytes(flight.image()) << " height " << unsigned(flight.state().view.height) << '\n';
        flight.reset();
        require(flight.image().pixels == initial.pixels && flight.cockpit_image().pixels == initial_cockpit.pixels &&
                flight.state().range == initial_state.range &&
                flight.state().view.x == initial_state.view.x, "Reset must reconstruct the deterministic training entry");
        controls = {};
        controls.throttle = 15;
        for (unsigned tick = 0; tick < 10; ++tick) {
            flight.advance(controls);
        }
        controls.fire = true;
        flight.advance(controls);
        controls.fire = false;
        const auto launch_range = flight.state().range;
        for (unsigned tick = 0; tick < 180; ++tick) {
            flight.advance(controls);
            flight.render_audio(2205);
        }
        require(flight.state().range + 180 == launch_range, "Live flight must continue through missile impact, explosion and expiry");
        flight.reset();
        flight.advance({});
        require(flight.state().range + 1 == initial_state.range, "Reset must recover a stopped training session");
        for (unsigned selection = 0; selection < 6; ++selection) {
            flight.advance({}, {static_cast<comanchecd::InstrumentDisplay>(selection),
                                static_cast<comanchecd::InstrumentDisplay>(5 - selection)});
        }
        flight.reset();
        require(flight.cockpit_image().pixels == initial_cockpit.pixels,
                "Every instrument program must run through the training entry point and reset cleanly");
        verify_crash(flight, installation);
        verify_guided_combat(flight, installation);
        verify_weapon_key_order(flight);
        verify_scenery(installation);
        const training_test::Route route(installation);
        require(flight.objectives_remaining() == 20, "Original first training mission must contain twenty objectives");
        const auto initial_objectives = flight.objectives();
        require(std::count(initial_objectives.begin(), initial_objectives.end(), comanchecd::ObjectiveState::active) == 20,
                "Training must expose the twenty original active objective slots");
        unsigned victory_ticks = 0;
        for (; victory_ticks < training_test::victory_tick_limit && !flight.state().ended; ++victory_ticks) {
            flight.advance(route.controls(flight.state(), victory_ticks));
            flight.render_audio(comanchecd::training_samples_per_tick);
        }
        const auto result = flight.result();
        require(flight.state().ended && flight.objectives_remaining() == 0 && result.completed && result.survived &&
                result.percentages[1] == 100 && flight.state().ammunition[0] < 500 &&
                flight.state().ammunition[1] < 62 && flight.state().ammunition[2] == 0,
                "Normal controls must use all three weapons, destroy all training objectives and complete the automatic return");
        require(flight.state().shake_ticks == 1 && flight.state().shake_offset == 0 && flight.state().crash_rotation == 0,
                "Mission return must carry the DOS one-IRQ presentation reset into debrief");
        const auto completed_objectives = flight.objectives();
        require(std::count(completed_objectives.begin(), completed_objectives.end(), comanchecd::ObjectiveState::destroyed) == 20 &&
                std::count(completed_objectives.begin(), completed_objectives.end(), comanchecd::ObjectiveState::active) == 0,
                "Mission completion must retain each destroyed objective slot for debrief");
        std::cout << "Training victory after " << victory_ticks << " ticks; cannon/rocket/Hellfire ammunition "
                  << flight.state().ammunition[0] << '/' << flight.state().ammunition[1] << '/' << flight.state().ammunition[2] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
