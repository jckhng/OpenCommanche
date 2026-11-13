#include "comanchecd/flight.hpp"

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
        using namespace comanchecd;
        GameplaySetup setup;
        setup.x = 100u << 22;
        setup.y = 400u << 22;
        setup.ammunition = 30;
        setup.weapons.fill(4);
        MissionScene scene;
        scene.range = 1000;
        scene.player_height = 200;
        scene.vehicle_parameters = {5, 3, 2, 0, 0, 0};
        TerrainTables tables;
        tables.sine[128] = 0x7fffffff;
        SceneAssets assets;
        assets.terrain.height.assign(terrain_side * terrain_side, 0);
        FlightController cannon(setup, scene, tables, 20);
        FlightControls controls;
        controls.weapon = WeaponSelection::cannon;
        controls.fire = true;
        for (unsigned shot = 0; shot < 3; ++shot) {
            require(cannon.step(controls, assets) == std::vector<std::uint16_t>{7},
                    "Held cannon fire must launch on every controller pass");
        }
        require(cannon.state().ammunition[0] == 0 && cannon.scene().entities[138].active &&
                cannon.scene().entities[140].primary_payload == 11 && cannon.scene().camera_subject == 0,
                "Cannon consumes two rounds with saturation, uses BULL and leaves the camera alone");
        require(cannon.step(controls, assets).empty() && cannon.state().firing,
                "An empty cannon retains its trigger indication without allocating a projectile");
        FlightController rockets(setup, scene, tables, 20);
        controls.weapon = WeaponSelection::rockets;
        require(rockets.step(controls, assets) == std::vector<std::uint16_t>{4}, "Rocket press must launch");
        const auto first_x = rockets.scene().entities[140].x;
        require(rockets.scene().entities[140].primary_payload == 10 &&
                rockets.sounds().back().step == 0x20000 && rockets.scene().camera_subject == 0,
                "Rockets use ROCK and the doubled sample rate without assigning the missile camera");
        require(rockets.step(controls, assets).empty() && rockets.state().ammunition[1] == 2,
                "Held rocket fire must wait for a new press");
        rockets.step({}, assets);
        rockets.step(controls, assets);
        require(first_x < setup.x && rockets.scene().entities[139].x > setup.x,
                "Successive rocket launches must alternate sides");
        controls.fire = false;
        controls.weapon = WeaponSelection::previous;
        rockets.step(controls, assets);
        require(rockets.state().weapon == 0, "Previous weapon must select the cannon from rockets");
        rockets.step(controls, assets);
        require(rockets.state().weapon == 5, "Original weapon selection wraps over all six stores");
        controls.weapon = WeaponSelection::next;
        rockets.step(controls, assets);
        require(rockets.state().weapon == 0, "Next weapon must wrap back to cannon");

        auto support_scene = scene;
        support_scene.vehicle_parameters = {5, 3, 2, 2, 2, 2};
        support_scene.wingman = 60;
        auto& wingman = support_scene.entities[60];
        wingman.active = true;
        wingman.kind = 4;
        wingman.x = setup.x + 0x20000000;
        wingman.y = setup.y;
        wingman.height = 100;
        FlightController support(setup, support_scene, tables, 20);
        controls = {};
        controls.weapon = WeaponSelection::wingman;
        controls.fire = true;
        require(support.step(controls, assets) == std::vector<std::uint16_t>{4} &&
                support.scene().entities[140].x == wingman.x &&
                support.scene().entities[140].height == wingman.height &&
                support.scene().camera_subject == 0 && support.sounds()[0].attenuation == 1,
                "Wingman fire uses the allied pose, attenuated launch cue, and no missile camera");
        FlightController absent(setup, scene, tables, 20);
        absent.step(controls, assets);
        require(!absent.scene().entities[140].active, "Absent wingmen must not produce a projectile");

        auto strike_scene = support_scene;
        strike_scene.entities[60].active = false;
        auto& strike_target = strike_scene.entities[59];
        strike_target.active = true;
        strike_target.kind = 3;
        strike_target.x = setup.x + 0x30000000;
        strike_target.y = setup.y;
        strike_target.height = 11;
        strike_target.attribute7 = 20;
        FlightController strike(setup, strike_scene, tables, 20);
        TargetingFrame targeting;
        targeting.candidate = 59;
        targeting.entities[59].x = 100;
        controls = {};
        controls.cycle_target = true;
        strike.step(controls, assets, {}, &targeting);
        controls.cycle_target = false;
        controls.weapon = WeaponSelection::artillery;
        controls.fire = true;
        strike.step(controls, assets);
        require(strike.scene().entities[140].kind == artillery_kind &&
                strike.scene().entities[140].x == strike_target.x &&
                strike.sounds()[0].bank == SampleBank::announcements && strike.sounds()[0].sound == 1,
                "Artillery fixes the selected position and acknowledges through the announcement bank");
        for (unsigned tick = 1; tick <= 120; ++tick) {
            strike.update_entities(assets);
            if (tick == 70) {
                require(strike.sounds().size() == 1 && strike.sounds()[0].bank == SampleBank::speech &&
                        strike.sounds()[0].sound == 9, "Artillery speech must occur at age 70");
            }
            if (tick == 100) {
                require(strike.sounds().size() == 1 && strike.sounds()[0].sound == 2,
                        "Artillery approach effect must occur at age 100");
            }
        }
        require(strike.scene().entities[140].phase == EntityPhase::explosion &&
                strike.scene().entities[59].damage_stage != 0 && strike.state().armor == 30,
                "Artillery detonates at age 120 and damages nearby objects without hitting a distant player");

        FlightController stingers(setup, strike_scene, tables, 20);
        controls = {};
        controls.cycle_target = true;
        stingers.step(controls, assets, {}, &targeting);
        controls = {};
        controls.weapon = WeaponSelection::stinger;
        controls.fire = true;
        stingers.step(controls, assets);
        require(stingers.scene().entities[140].kind == stinger_kind &&
                stingers.scene().entities[140].homing_target == 59 &&
                stingers.scene().entities[140].primary_payload == 8,
                "Stinger launch stores its target independently and uses the STGR payload");

        auto defense_setup = setup;
        for (const auto kind : {1, 2}) {
            auto destroyed_setup = setup;
            destroyed_setup.ammunition = 0xffffffffu;
            auto damaged_scene = scene;
            auto& enemy = damaged_scene.entities[60];
            enemy.active = true;
            enemy.kind = static_cast<std::uint8_t>(kind);
            enemy.damage_stage = 1;
            enemy.height = 100;
            enemy.x = setup.x;
            enemy.y = setup.y;
            FlightController destroyed(destroyed_setup, damaged_scene, tables, 20);
            destroyed.update_entities(assets);
            for (const auto& cue : destroyed.sounds()) {
                require(cue.bank != SampleBank::speech || cue.sound != 1,
                        "Enemy destruction must suppress kill acknowledgement after player destruction");
            }
        }

        defense_setup.map_position = {2, 2};
        FlightController defense(defense_setup, scene, tables, 20);
        controls = {};
        controls.chaff = controls.flare = true;
        defense.step(controls, assets);
        defense.step(controls, assets);
        require(defense.state().chaff == 1 && defense.state().flares == 1 &&
                defense.state().active_chaff == 1 && defense.state().active_flares == 1,
                "Countermeasure presses consume mission supplies once per press");
        for (unsigned tick = 0; tick < 20; ++tick) {
            defense.update_entities(assets);
        }
        require(defense.state().active_flares == 0 && defense.state().active_chaff == 1,
                "Flare protection expires after 20 updates while chaff remains active");
        for (unsigned tick = 0; tick < 10; ++tick) {
            defense.update_entities(assets);
        }
        require(defense.state().active_chaff == 0, "Chaff protection expires after 30 updates");

        for (const auto weapon : {WeaponSelection::cannon, WeaponSelection::rockets}) {
            auto damaged_setup = setup;
            damaged_setup.weapons[weapon == WeaponSelection::cannon ? 5 : 3] = 255;
            FlightController damaged(damaged_setup, scene, tables, 20);
            controls.weapon = weapon;
            controls.fire = true;
            require(damaged.step(controls, assets).empty() && damaged.state().ammunition[unsigned(weapon)] == 0,
                    "Damaged weapon systems clear the corresponding store and prevent launch");
        }
        auto full_scene = scene;
        for (std::size_t slot = player_slot; slot < scene_slots; ++slot) {
            full_scene.entities[slot].active = true;
        }
        FlightController full(setup, full_scene, tables, 20);
        controls.weapon = WeaponSelection::cannon;
        require(full.step(controls, assets).empty() && full.state().ammunition[0] == 5,
                "An exhausted projectile pool must not consume ammunition");

        for (const auto kind : {cannon_round_kind, rocket_kind}) {
            auto impact_scene = scene;
            impact_scene.remaining_targets = 1;
            auto& projectile = impact_scene.entities[140];
            projectile.active = true;
            projectile.kind = kind;
            projectile.x = setup.x;
            projectile.y = setup.y - 0x10000000;
            projectile.height = 200;
            projectile.ballistic_height = 200u << 24;
            projectile.speed = kind == cannon_round_kind ? 0x08000000 : 0x04000000;
            projectile.animation_frame = 1;
            projectile.random_phase = 1;
            auto& target = impact_scene.entities[60];
            target.active = true;
            target.kind = 3;
            target.target = 1;
            target.x = projectile.x;
            target.y = projectile.y - (projectile.speed / 2 - 1);
            target.height = 200;
            FlightController impact(setup, impact_scene, tables, 20);
            impact.update_entities(assets);
            require(impact.scene().entities[60].damage_stage == 2 &&
                    impact.scene().entities[140].vertical_velocity == -0x100000 &&
                    impact.scene().entities[140].ballistic_height == (200u << 24) - 0x100000,
                    "Ballistic gravity and fractional altitude must precede object contact damage");
            const auto& hit = impact.scene().entities[140];
            require(kind == cannon_round_kind ? hit.animation_frame == 2 && hit.phase == EntityPhase::normal :
                                               hit.animation_frame == 1 && hit.phase == EntityPhase::explosion,
                    "Cannon impacts retain their sprite animation; rockets enter BOOM");
            for (unsigned tick = 0; tick < 25; ++tick) {
                impact.update_entities(assets);
            }
            require(impact.scene().remaining_targets == 0 && !impact.scene().entities[60].active &&
                    !impact.scene().entities[140].active && impact.state().exit_requested == 1,
                    "Completed target destruction must remove the last objective and request mission return once");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
