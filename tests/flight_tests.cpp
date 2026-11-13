#include "comanchecd/flight.hpp"
#include "loading_clock.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        comanchecd::GameplaySetup setup;
        setup.x = 100u << 22;
        setup.y = 400u << 22;
        comanchecd::MissionScene scene;
        scene.range = 100;
        scene.player_height = 5;
        scene.vehicle_parameters[2] = 2;
        comanchecd::TerrainTables tables;
        tables.sine[128] = 0x7fffffff;
        comanchecd::SceneAssets assets;
        assets.terrain.height.assign(1024 * 1024, 0);
        auto countermeasure_scene = scene;
        auto& chaff = countermeasure_scene.entities[120];
        chaff.active = true;
        chaff.kind = comanchecd::chaff_kind;
        chaff.height = chaff.random_phase = 2;
        chaff.animation_frame = 1;
        comanchecd::FlightController countermeasure(setup, countermeasure_scene, tables, 20);
        LoadingClock countermeasure_clock;
        countermeasure.update_entities(assets, &countermeasure_clock);
        require(countermeasure_clock.accounting().dispatches == 585 &&
                countermeasure.scene().entities[120].animation_frame == 2,
                "Chaff update must include its eight-instruction descent, age and animation callback");
        auto artillery_wait_scene = countermeasure_scene;
        artillery_wait_scene.entities[120].kind = comanchecd::artillery_kind;
        comanchecd::FlightController artillery_wait(setup, artillery_wait_scene, tables, 20);
        LoadingClock artillery_wait_clock;
        artillery_wait.update_entities(assets, &artillery_wait_clock);
        require(artillery_wait_clock.accounting().dispatches == 586,
                "Waiting artillery must include its nine-instruction age and sound gates");
        auto aircraft_scene = scene;
        auto& aircraft = aircraft_scene.entities[55];
        aircraft.active = true;
        aircraft.kind = 1;
        aircraft.damage_stage = 2;
        aircraft.height = 69;
        comanchecd::FlightController falling_aircraft(setup, aircraft_scene, tables, 20);
        LoadingClock aircraft_clock;
        falling_aircraft.update_entities(assets, &aircraft_clock);
        require(aircraft_clock.accounting().dispatches == 609 &&
                falling_aircraft.scene().entities[55].height == 65 &&
                falling_aircraft.scene().entities[55].damage_stage == 2,
                "Damaged aircraft descent must include the recovered crash callback work");
        auto wingman_scene = scene;
        constexpr unsigned wingman_slot = 60, east_heading = 64;
        wingman_scene.wingman = wingman_slot;
        wingman_scene.player_height = 100;
        auto& wingman = wingman_scene.entities[wingman_slot];
        wingman.active = true;
        wingman.kind = 4;
        wingman.x = wingman.y = 0x80000000;
        wingman.height = 100;
        wingman.angle = east_heading;
        wingman.random_phase = 1;
        wingman.speed = 0x01800000;
        auto wingman_setup = setup;
        wingman_setup.x = 0xc0000000;
        wingman_setup.y = wingman.y;
        auto wingman_tables = tables;
        wingman_tables.angle_octants[2] = 2 * (255 - east_heading) - 1;
        auto cold_wingman_scene = wingman_scene;
        cold_wingman_scene.entities[wingman_slot].y = 0xb0000000;
        cold_wingman_scene.entities[wingman_slot].angle = 128;
        cold_wingman_scene.entities[wingman_slot].random_phase = 7;
        auto cold_wingman_setup = wingman_setup;
        cold_wingman_setup.x = wingman.x;
        cold_wingman_setup.y = 0xc0000000;
        cold_wingman_setup.heading = 0x80000000;
        auto cold_wingman_tables = wingman_tables;
        cold_wingman_tables.sine[384] = static_cast<std::int32_t>(0x80000000);
        cold_wingman_tables.angle_octants[5] = 253;
        comanchecd::FlightController cold_wingman(cold_wingman_setup, cold_wingman_scene, cold_wingman_tables, 20);
        LoadingClock cold_wingman_clock;
        cold_wingman.update_entities(assets, &cold_wingman_clock);
        require(cold_wingman_clock.accounting().dispatches == 659,
                "Initial wingman formation must include its recovered callback CPU work");
        require(cold_wingman.scene().entities[wingman_slot].angle == 128 &&
                cold_wingman.scene().entities[wingman_slot].random_phase == 6,
                "Initial wingman formation must use the cleared movement basis before the first controller call");
        auto support_scene = scene;
        auto& support = support_scene.entities[wingman_slot];
        support.active = true;
        support.kind = 14;
        support.x = support.y = 0x80000000;
        support.height = 5;
        support.attribute7 = 2;
        auto support_setup = setup;
        support_setup.x = 0;
        support_setup.y = support.y;
        comanchecd::FlightController support_vehicle(support_setup, support_scene, tables, 20);
        LoadingClock support_clock;
        support_vehicle.update_entities(assets, &support_clock);
        require(support_clock.accounting().dispatches == 649,
                "Support vehicles must include shared ground movement and their attack wrapper CPU work");
        comanchecd::FlightController acknowledgement(wingman_setup, wingman_scene, wingman_tables, 20);
        comanchecd::TargetingFrame wingman_target;
        wingman_target.candidate = wingman_slot;
        wingman_target.entities[wingman_slot].x = 80;
        comanchecd::FlightControls acquire_wingman;
        acquire_wingman.cycle_target = true;
        acknowledgement.step(acquire_wingman, assets, {}, &wingman_target);
        acknowledgement.update_entities(assets);
        require(acknowledgement.scene().entities[wingman_slot].x == wingman.x &&
                acknowledgement.scene().entities[wingman_slot].random_phase == 0 &&
                acknowledgement.sounds().size() == 1 && acknowledgement.sounds()[0].sound == 12,
                "Wingman acknowledgement must preserve the original speech-induced movement hold");
        auto crash_scene = scene;
        constexpr auto explosion_slot = comanchecd::scene_slots - 1;
        auto& artillery = crash_scene.entities[explosion_slot - 1];
        artillery.active = true;
        artillery.kind = comanchecd::artillery_kind;
        artillery.random_phase = 119;
        artillery.x = setup.x;
        artillery.y = setup.y;
        artillery.height = crash_scene.player_height;
        crash_scene.entities[comanchecd::player_slot].x = setup.x;
        crash_scene.entities[comanchecd::player_slot].y = setup.y;
        const auto crash_step = [&](std::uint32_t seed) {
            crash_scene.random_seed = seed;
            comanchecd::FlightController crashed(setup, crash_scene, tables, 20);
            crashed.update_entities(assets);
            LoadingClock clock;
            crashed.step({}, assets, {}, nullptr, comanchecd::ThreatProximity::clear,
                [&](comanchecd::CpuOperation operation, unsigned count) { clock.execute(operation, count); }, &clock);
            require(crashed.scene().entities[explosion_slot].active == (seed == 0), "Crash explosion must follow the RNG gate");
            return clock.accounting().dispatches;
        };
        constexpr auto crash_explosion_dispatches = 46u;
        require(crash_step(0) == crash_step(7) + crash_explosion_dispatches,
                "Crash explosion must clock its caller and shared allocator including attenuation");
        auto radar_scene = crash_scene;
        radar_scene.player_height = 149;
        radar_scene.entities[explosion_slot - 1].height = 149;
        auto radar_setup = setup;
        radar_setup.map_position[1] = 5;
        auto radar_assets = assets;
        radar_assets.terrain.height.assign(1024 * 1024, 70);
        comanchecd::FlightController crash_radar(radar_setup, radar_scene, tables, 20);
        auto defense = comanchecd::FlightPolicy{};
        defense.flares = comanchecd::DefenseMode::automatic;
        crash_radar.set_policy(defense);
        crash_radar.update_entities(radar_assets);
        constexpr unsigned crash_view_updates = 45;
        bool changed_view = false;
        for (unsigned frame = 0; frame < crash_view_updates; ++frame) {
            crash_radar.update_entities(radar_assets);
            const auto flares_before = crash_radar.state().flares;
            crash_radar.step({}, radar_assets, [&](auto&) { changed_view = true; }, nullptr,
                frame + 1 == crash_view_updates ? comanchecd::ThreatProximity::nearby : comanchecd::ThreatProximity::clear);
            if (changed_view) {
                require(crash_radar.state().flares == flares_before && crash_radar.state().active_flares == 0,
                        "Crash view must clear stale radar proximity before automatic defense");
            }
        }
        require(changed_view, "Crash radar regression must reach the view transition");
        comanchecd::FlightController controller(setup, scene, tables, 20);
        for (const auto weapon : {comanchecd::WeaponSelection::hellfire, comanchecd::WeaponSelection::stinger}) {
            auto stinger_scene = scene;
            stinger_scene.vehicle_parameters[static_cast<unsigned>(weapon)] = 1;
            comanchecd::FlightController stinger(setup, stinger_scene, tables, 20);
            comanchecd::FlightControls stinger_controls;
            stinger_controls.weapon = weapon;
            stinger_controls.fire = true;
            std::uint64_t stinger_work = 0, stores_boundary = 0;
            unsigned stores_updates = 0, launch_sounds = 0;
            stinger.step(stinger_controls, assets, {}, nullptr, comanchecd::ThreatProximity::clear,
                [&](comanchecd::CpuOperation, unsigned count) { stinger_work += count; }, nullptr,
                [&](const comanchecd::SoundCue&) {
                    require(stores_updates == 1 && stinger_work == stores_boundary + 5,
                            "Guided launch sound must follow the clocked stores update and its five caller instructions");
                    ++launch_sounds;
                },
                {[&](const comanchecd::FlightState& player) {
                    require(player.ammunition[static_cast<unsigned>(weapon)] == 0, "Stores must see the decremented guided ammunition");
                    stores_boundary = stinger_work;
                    ++stores_updates;
                }, {}});
            require(stores_updates == 1 && launch_sounds == 1 &&
                    stinger.scene().entities[comanchecd::scene_slots - 1].kind == (weapon == comanchecd::WeaponSelection::stinger ? comanchecd::stinger_kind : comanchecd::guided_missile_kind),
                    "A guided launch must emit its stores update and sound once");
        }
        auto wing_launch_scene = wingman_scene;
        wing_launch_scene.vehicle_parameters[5] = 1;
        comanchecd::FlightController wing_launcher(setup, wing_launch_scene, tables, 20);
        comanchecd::FlightControls wing_launch;
        wing_launch.weapon = comanchecd::WeaponSelection::wingman;
        wing_launch.fire = true;
        std::uint64_t wing_launch_work = 0, wing_stores_boundary = 0;
        unsigned wing_stores_updates = 0, wing_launch_sounds = 0;
        wing_launcher.step(wing_launch, assets, {}, nullptr, comanchecd::ThreatProximity::clear,
            [&](comanchecd::CpuOperation, unsigned count) { wing_launch_work += count; }, nullptr,
            [&](const comanchecd::SoundCue&) {
                require(wing_stores_updates == 1 && wing_launch_work == wing_stores_boundary + 5,
                        "Wingman launch sound must follow its stores update and five caller instructions");
                ++wing_launch_sounds;
            },
            {[&](const comanchecd::FlightState& player) {
                require(player.ammunition[5] == 0, "Wingman stores must see the decremented ammunition");
                wing_stores_boundary = wing_launch_work;
                ++wing_stores_updates;
            }, {}});
        require(wing_stores_updates == 1 && wing_launch_sounds == 1 &&
                wing_launcher.scene().entities.back().kind == comanchecd::guided_missile_kind,
                "A wingman launch must emit one stores update and one missile sound");
        auto empty_wing_scene = wingman_scene;
        empty_wing_scene.vehicle_parameters[0] = 500;
        auto damaged_gun_setup = setup;
        damaged_gun_setup.weapons[5] = 128;
        comanchecd::FlightController empty_wing(damaged_gun_setup, empty_wing_scene, tables, 20);
        unsigned empty_stores_updates = 0;
        empty_wing.step(wing_launch, assets, {}, nullptr, comanchecd::ThreatProximity::clear, {}, nullptr, {},
            {[&](const comanchecd::FlightState& player) {
                require(player.ammunition[0] == 0 && player.ammunition[5] == 0,
                        "An empty wingman request must apply damaged-weapon normalization at the stores boundary");
                ++empty_stores_updates;
            }, {}});
        require(empty_stores_updates == 1 && empty_wing.sounds().empty(),
                "An empty wingman request must redraw stores without a launch sound");
        auto artillery_scene = scene;
        artillery_scene.player_height = 100;
        artillery_scene.vehicle_parameters[4] = 1;
        artillery_scene.entities[1].active = true;
        artillery_scene.entities[1].x = setup.x;
        artillery_scene.entities[1].y = setup.y;
        comanchecd::FlightController artillery_launcher(setup, artillery_scene, tables, 20);
        comanchecd::TargetingFrame artillery_target;
        artillery_target.candidate = 1;
        artillery_target.entities[1].x = 80;
        comanchecd::FlightControls artillery_acquire;
        artillery_acquire.cycle_target = true;
        artillery_launcher.step(artillery_acquire, assets, {}, &artillery_target);
        comanchecd::FlightControls artillery_launch;
        artillery_launch.weapon = comanchecd::WeaponSelection::artillery;
        artillery_launch.fire = true;
        std::uint64_t artillery_work = 0, announcement_boundary = 0;
        unsigned artillery_announcements = 0, artillery_stores_updates = 0;
        artillery_launcher.step(artillery_launch, assets, {}, &artillery_target, comanchecd::ThreatProximity::clear,
            [&](comanchecd::CpuOperation, unsigned count) { artillery_work += count; }, nullptr,
            [&](const comanchecd::SoundCue& cue) {
                require(cue.bank == comanchecd::SampleBank::announcements && cue.sound == 1 && artillery_stores_updates == 0,
                        "Artillery must request its announcement before redrawing stores");
                announcement_boundary = artillery_work;
                ++artillery_announcements;
            },
            {[&](const comanchecd::FlightState& player) {
                require(artillery_work == announcement_boundary + 2 && player.ammunition[4] == 0,
                        "Artillery stores must follow the announcement and two caller instructions");
                ++artillery_stores_updates;
            }, {}});
        require(artillery_announcements == 1 && artillery_stores_updates == 1 &&
                artillery_launcher.scene().entities.back().kind == comanchecd::artillery_kind,
                "Artillery must emit its ordered announcement and stores update once");
        for (const auto weapon : {comanchecd::WeaponSelection::cannon, comanchecd::WeaponSelection::rockets}) {
            auto ballistic_scene = scene;
            ballistic_scene.player_height = 100;
            ballistic_scene.vehicle_parameters[static_cast<unsigned>(weapon)] = 1;
            comanchecd::FlightController ballistic(setup, ballistic_scene, tables, 20);
            comanchecd::FlightControls launch;
            launch.weapon = weapon;
            launch.fire = true;
            std::uint64_t launch_work = 0, stores_at = 0;
            unsigned updates = 0, samples = 0;
            const comanchecd::FlightStatusUpdates status{[&](const comanchecd::FlightState& state) {
                require(state.ammunition[static_cast<unsigned>(weapon)] == 0,
                        "Ballistic stores must see saturated, decremented ammunition");
                stores_at = launch_work;
                ++updates;
            }, {}};
            ballistic.step(launch, assets, {}, nullptr, comanchecd::ThreatProximity::clear,
                [&](comanchecd::CpuOperation, unsigned count) { launch_work += count; }, nullptr,
                [&](const comanchecd::SoundCue&) {
                    require(updates == 1 && launch_work == stores_at + 5,
                            "Ballistic sound must follow its stores update and five caller instructions");
                    ++samples;
                }, status);
            require(updates == 1 && samples == 1 && ballistic.scene().entities.back().active,
                    "A ballistic launch must allocate once and redraw stores before its sound");
            comanchecd::FlightController empty(setup, scene, tables, 20);
            updates = 0;
            empty.step(launch, assets, {}, nullptr, comanchecd::ThreatProximity::clear, {}, nullptr, {}, status);
            require(updates == 1 && empty.sounds().empty(),
                    "An empty ballistic request must redraw stores without playing a launch sound");
        }
        scene.random_seed = 7;
        comanchecd::FlightController interrupted(setup, scene, tables, 20);
        comanchecd::IrqState expected_irq{scene.random_seed};
        expected_irq.interrupt();
        unsigned work_calls = 0;
        interrupted.step({}, assets, {}, nullptr, comanchecd::ThreatProximity::clear,
            [&](comanchecd::CpuOperation, unsigned) {
                if (work_calls++ == 0) { interrupted.timer_interrupt(); }
            });
        require(work_calls != 0 && interrupted.scene().random_seed == expected_irq.random_seed,
                "An IRQ during controller CPU work must survive the transaction commit");
        controller.step({}, assets);
        require(controller.state().range == 99 && controller.state().engine_power == 15 &&
                controller.state().view.height == 5, "Neutral flight retains the original low hover state");
        comanchecd::FlightControls controls;
        controls.throttle = 1;
        controls.fire = true;
        require(controller.step(controls, assets) == std::vector<std::uint16_t>{4} &&
                controller.state().ammunition[2] == 1 && controller.scene().entities[140].active &&
                controller.scene().camera_subject == 140 && controller.state().firing,
                "A weapon edge allocates a missile, consumes ammunition and emits its original sound ID");
        require(controller.step(controls, assets).empty() && controller.state().ammunition[2] == 1 && !controller.state().firing,
                "Held nonautomatic fire must consume only one latched edge");
        const auto before = controller.state().range;
        assets.terrain.height.assign(1024 * 1024, 120);
        controller.step({}, assets);
        require(controller.state().range + 1 == before && controller.state().shake_ticks == 10,
                "Terrain contact must advance through the recovered collision response");
        controller.abort();
        require(controller.state().ended && controller.state().shake_ticks == 1 && controller.state().shake_offset == 0 &&
                controller.state().crash_rotation == 0,
                "Abort must use the shared mission-return presentation reset");
        constexpr auto exhausted_fuel = static_cast<std::uint32_t>(-270);
        scene.range = exhausted_fuel;
        comanchecd::FlightController fuel_return(setup, scene, tables, 20);
        fuel_return.step({}, assets);
        require(fuel_return.state().ended && fuel_return.state().range == exhausted_fuel &&
                fuel_return.state().shake_ticks == 1 && fuel_return.state().shake_offset == 0 &&
                fuel_return.state().crash_rotation == 0,
                "Fuel exhaustion must reach the same return boundary without another flight update");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
