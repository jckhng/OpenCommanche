#include "comanchecd/options.hpp"
#include "comanchecd/flight.hpp"

#include <algorithm>
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
        std::vector<std::uint8_t> bytes(MenuSettings::record_bytes, 0);
        bytes[36] = bytes[37] = bytes[38] = bytes[39] = bytes[40] = 125;
        MenuSettings settings(bytes);
        bytes[28] = bytes[29] = bytes[30] = bytes[31] = 125;
        MenuSettings scenery(bytes);
        require(scenery.scenery_policy().pixels == PixelSize::small && scenery.scenery_policy().reflections,
                "Enabled scenery markers select small pixels and reflections");
        scenery.activate(1, 5);
        scenery.activate(1, 7);
        require(scenery.scenery_policy().pixels == PixelSize::large && !scenery.scenery_policy().reflections &&
                scenery.scenery_policy().clouds == CloudMode::panoramic && scenery.scenery_policy().haze,
                "Pixel size and reflection toggles preserve clouds and haze");
        require(settings.flight_policy().chaff == DefenseMode::automatic &&
                settings.flight_policy().flares == DefenseMode::automatic &&
                settings.flight_policy().enemy_range == EnemyRange::stealth,
                "Original enabled markers must enable automatic defenses and stealth");
        settings.toggle(DefenseOption::chaff);
        require(settings.record()[39] == 123 && settings.flight_policy().chaff == DefenseMode::manual,
                "The original option toggles marker bits with XOR 6");
        SettingsMenu menu;
        menu.consume(SettingsKey::left);
        menu.consume(SettingsKey::up);
        require(menu.category() == 3 && menu.row() == 8, "Settings must wrap tabs and rows");
        menu.consume(SettingsKey::left);
        menu.consume(SettingsKey::up);
        require(menu.category() == 2 && menu.row() == 16, "Control has seventeen rows");
        for (unsigned step = 0; step < 3; ++step) {
            menu.consume(SettingsKey::up);
        }
        require(menu.row() == 12, "Control navigation must skip its separator row");
        require(settings.activate(2, 5) == SettingsAction::changed && settings.music_level() == MusicLevel::soft &&
                settings.activate(2, 2) == SettingsAction::changed && settings.sound_level() == SoundLevel::off,
                "Sound and music selections must update the separate original fields");

        require(altitude_response(AltitudeControl::stabilized, 10, 0, 0, 0).descent_shift == 3 &&
                altitude_response(AltitudeControl::manual, 10, 0, 0, 0).descent_shift == 2,
                "Stabilization changes the descent shift, while both paths add three");
        require(altitude_response(AltitudeControl::manual, 10, -(4 << 21), 0, 16).power == 10 &&
                altitude_response(AltitudeControl::manual, 10, -(4 << 21), 0, 4).power == 13,
                "Manual lift uses ones-complement speed magnitude and skips a borrowed bank penalty");
        settings.activate(3, 4);
        require(settings.flight_policy().rotor == RotorControl::independent,
                "Rotor mixing follows the original option marker");
        settings.activate(3, 4);
        SceneAssets assets;
        assets.terrain.height.assign(terrain_side * terrain_side, 0);
        TerrainTables tables;
        tables.sine[128] = 0x7fffffff;
        GameplaySetup setup;
        setup.x = 100u << 22;
        setup.y = 400u << 22;
        setup.ammunition = 30;
        setup.map_position = {3, 3};
        MissionScene scene;
        scene.range = 1000;
        scene.player_height = 100;
        FlightController mixed(setup, scene, tables, 20), independent(setup, scene, tables, 20);
        FlightPolicy separate_controls;
        separate_controls.rotor = RotorControl::independent;
        independent.set_policy(separate_controls);
        mixed.step({100, 0, 1}, assets);
        independent.step({100, 0, 1}, assets);
        require(mixed.state().yaw_rate == 100 * 0x4000 && independent.state().yaw_rate == 0 &&
                mixed.state().side_speed == independent.state().side_speed,
                "Independent rotor control removes cyclic yaw while retaining lateral input");
        FlightController flight(setup, scene, tables, 20);
        settings.toggle(DefenseOption::chaff);
        flight.set_policy(settings.flight_policy());
        flight.step({}, assets, {}, nullptr, ThreatProximity::nearby);
        require(flight.state().flares == 2 && flight.state().active_flares == 1 &&
                flight.state().chaff == 3, "Nearby threat must deploy a flare");
        flight.step({}, assets, {}, nullptr, ThreatProximity::nearby);
        require(flight.state().flares == 2, "An active flare must suppress automatic replacement");
        for (unsigned tick = 0; tick < 20; ++tick) {
            flight.update_entities(assets);
        }
        flight.step({}, assets, {}, nullptr, ThreatProximity::nearby);
        require(flight.state().flares == 1, "A persistent threat must redeploy after expiry");
        for (const auto terrain_height : {0, 50}) {
            std::fill(assets.terrain.height.begin(), assets.terrain.height.end(), terrain_height);
            FlightController deploying(setup, scene, tables, 20);
            deploying.set_policy(settings.flight_policy());
            FlightControls controls;
            controls.throttle = 1;
            controls.vertical = -350;
            deploying.step(controls, assets, {}, nullptr, ThreatProximity::nearby);
            const auto& flare = deploying.scene().entities.back();
            const auto saved_height = terrain_height == 0 ? scene.player_height : 102;
            require(flare.active && flare.kind == flare_kind && flare.height == saved_height,
                    "Automatic defense must sample the height backup after terrain response");
            require(flare.x == setup.x && flare.y == setup.y && deploying.state().view.y != setup.y,
                    "Automatic defense must retain the saved horizontal position while the player moves");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
