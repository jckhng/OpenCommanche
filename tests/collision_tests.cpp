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
        comanchecd::GameplaySetup setup;
        setup.x = 100u << 22;
        setup.y = 400u << 22;
        comanchecd::MissionScene scene;
        scene.range = 1000;
        scene.player_height = 200;
        auto& missile = scene.entities[140];
        missile.active = true;
        missile.kind = comanchecd::guided_missile_kind;
        missile.x = setup.x;
        missile.y = setup.y - 0x20000000;
        missile.height = 200;
        missile.speed = 0x04000000;
        missile.random_phase = 127;
        scene.camera_subject = 140;
        comanchecd::TerrainTables tables;
        tables.sine[128] = 0x7fffffff;
        comanchecd::SceneAssets assets;
        assets.terrain.height.assign(comanchecd::terrain_side * comanchecd::terrain_side, 0);
        comanchecd::FlightController controller(setup, scene, tables, 20);
        controller.update_entities(assets);
        const auto& explosion = controller.scene().entities[140];
        require(explosion.active && explosion.x == missile.x && explosion.y == missile.y,
                "Missile expiry must retain its previous pose for the explosion");
        require(controller.scene().camera_subject == 0, "Explosion transition must release the missile camera subject");
        for (unsigned tick = 0; tick < 19; ++tick) {
            controller.update_entities(assets);
        }
        require(!controller.scene().entities[140].active, "The original explosion must release its slot at frame twenty");
        for (const std::uint32_t distance : {0x60000000u, 0x70000000u}) {
            auto distant_scene = scene;
            distant_scene.entities[140].x = setup.x + distance;
            distant_scene.entities[140].y = setup.y + distance;
            comanchecd::FlightController distant(setup, distant_scene, tables, 20);
            distant.update_entities(assets);
            require(distant.sounds().size() == (distance == 0x60000000u ? 1 : 0),
                    "Explosion sound must stop beyond the original attenuation-four boundary");
            if (!distant.sounds().empty()) {
                require(distant.sounds()[0].attenuation == 4, "Explosion distance must preserve the original coarse attenuation");
            }
        }

        setup.ammunition = 30;
        setup.weapons.fill(4);
        scene.entities[140].active = false;
        scene.random_seed = 0x12345678;
        comanchecd::FlightController contact(setup, scene, tables, 20);
        assets.terrain.height.assign(comanchecd::terrain_side * comanchecd::terrain_side, 120);
        comanchecd::FlightControls controls;
        controls.horizontal = 350;
        controls.vertical = -350;
        contact.step(controls, assets);
        require(contact.state().forward_speed == -358400 && contact.state().side_speed == -716800 &&
                contact.state().view.height == 202 && contact.state().armor == 28 && contact.state().shake_ticks == 10,
                "Terrain collisions must reverse motion, halve forward velocity, raise the player and apply original damage");
        require(contact.state().system_condition[0] == 3 && contact.state().system_condition[3] == 3 &&
                contact.scene().random_seed == 0x782b3456,
                "Collision damage must preserve system-byte indices and the wrapped RNG add/rotate order");
        require(contact.sounds().size() == 1 && contact.sounds()[0].bank == comanchecd::SampleBank::speech &&
                contact.sounds()[0].sound == 2, "Collision feedback must use the recovered speech bank");
        comanchecd::IrqState expected_irq{contact.scene().random_seed, 10, 0};
        for (unsigned tick = 0; tick < 12; ++tick) {
            expected_irq.interrupt();
            contact.timer_interrupt();
            require(contact.scene().random_seed == expected_irq.random_seed &&
                    contact.state().shake_ticks == expected_irq.shake_ticks &&
                    contact.state().shake_offset == expected_irq.shake_offset && contact.state().armor == 28,
                    "Timer interrupts must mutate the shared scene RNG and expire shake without repeating damage");
        }
        auto alias_scene = scene;
        alias_scene.random_seed = 0;
        comanchecd::FlightController alias(setup, alias_scene, tables, 20);
        alias.step({}, assets);
        require(alias.state().system_condition[0] == 2 && alias.state().armor == 30,
                "Aliased damage indices must decrement the same system byte twice");

        assets.terrain.height.assign(comanchecd::terrain_side * comanchecd::terrain_side, 100);
        comanchecd::FlightController shallow(setup, scene, tables, 20);
        shallow.step({}, assets);
        require(shallow.state().view.height == 202 && shallow.state().armor == 30,
                "Shallow ground overlap must replace the carried height before averaging");

        assets.terrain.height.assign(comanchecd::terrain_side * comanchecd::terrain_side, 0);
        for (const unsigned health : {79u, 80u, 81u}) {
            auto hit_scene = scene;
            auto& shot = hit_scene.entities[140];
            shot.active = true;
            shot.random_phase = 3;
            auto& target = hit_scene.entities[60];
            target.active = true;
            target.kind = 3;
            target.x = shot.x;
            target.y = shot.y - 0x01ffffff;
            target.height = shot.height;
            target.attribute7 = static_cast<std::uint8_t>(health);
            comanchecd::FlightController hit(setup, hit_scene, tables, 20);
            hit.update_entities(assets);
            require(hit.scene().entities[140].phase == comanchecd::EntityPhase::explosion &&
                    hit.scene().entities[60].attribute7 == static_cast<std::uint8_t>(health - 80) &&
                    hit.scene().entities[60].damage_stage == (health < 80 ? 2 : 0),
                    "Missile hits subtract eighty and begin destruction only on strict byte underflow");
            require(hit.scene().entities[139].active == (health < 80),
                    "The lower prop update must allocate its explosion after the missile update");
        }

        setup.ammunition = 0;
        comanchecd::FlightController fatal(setup, scene, tables, 20);
        assets.terrain.height.assign(comanchecd::terrain_side * comanchecd::terrain_side, 120);
        fatal.step({}, assets);
        require(fatal.state().armor == -2 && fatal.state().exit_requested == 1,
                "Fatal contact must enter the original crash countdown");
        for (unsigned tick = 0; tick < 100; ++tick) {
            fatal.step({}, assets);
        }
        require(fatal.state().ended && fatal.state().exit_countdown == 0 && fatal.state().cockpit_mode == 6,
                "The crash path must reach its terminal handoff and changed view mode");
        comanchecd::FlightController transition(setup, scene, tables, 20);
        transition.step({}, assets);
        unsigned transitions = 0;
        for (unsigned tick = 0; tick < 45; ++tick) {
            transition.step({}, assets, [&](auto& nested) {
                ++transitions;
                require(nested.state().cockpit_mode == 6 && nested.state().crash_rotation == 0,
                        "Crash initialization must reset scanout rotation before nested rendering");
                nested.update_entities(assets);
                nested.step({}, assets);
                nested.update_entities(assets);
                nested.step({}, assets);
            });
        }
        require(transitions == 1 && transition.state().exit_countdown == 53 && transition.state().range == 952,
                "The crash transition must carry both nested controller passes into the outer tick");
        require(transition.render_view(assets).mode == comanchecd::SceneViewMode::chase,
                "The crash camera must use the external view projection");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
