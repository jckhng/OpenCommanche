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
        setup.ammunition = 100;
        MissionScene scene;
        scene.range = 1000;
        scene.player_height = 100;
        scene.camera_subject = 140;
        SceneAssets assets;
        assets.terrain.height.assign(terrain_side * terrain_side, 0);
        FlightController flight(setup, scene, {}, 20);
        TargetingFrame targeting;
        targeting.candidate = 60;
        targeting.entities[60] = {0x10000000, 80, 90, 0};
        targeting.entities[2] = {0x20000000, 100, 95, 64};
        FlightControls controls;
        controls.cycle_target = true;
        flight.step(controls, assets, {}, &targeting);
        flight.refresh_target(targeting);
        require(flight.state().target.slot == 60 && flight.scene().camera_subject == 0,
                "First acquisition uses the renderer candidate and releases the missile camera");
        flight.step(controls, assets, {}, &targeting);
        require(flight.state().target.slot == 60, "Held acquisition must not cycle repeatedly");
        flight.step({}, assets, {}, &targeting);
        flight.step(controls, assets, {}, &targeting);
        require(flight.state().target.slot == 2, "Cycling wraps at slot sixty and skips invisible slots");
        targeting.entities[2].x = 0;
        flight.refresh_target(targeting);
        require(flight.state().target.slot == 0, "Occlusion releases the selected target");
        targeting.entities[60].x = 80;
        flight.step({}, assets, {}, &targeting);
        flight.step(controls, assets, {}, &targeting);
        flight.step({}, assets, {}, &targeting);
        flight.step(controls, assets, {}, &targeting);
        require(flight.state().target.slot == 0, "Cycling the only visible target clears selection");
        setup.weapons[2] = 128;
        FlightController damaged(setup, scene, {}, 20);
        damaged.step(controls, assets, {}, &targeting);
        require(damaged.state().target.slot == 0, "Damaged targeting prevents acquisition");
        scene.range = std::uint32_t(-269);
        FlightController exhausted(setup, scene, {}, 20);
        exhausted.step({}, assets);
        require(!exhausted.state().ended && exhausted.state().range == std::uint32_t(-270),
                "Fuel exhaustion must retain the last grace-period tick");
        exhausted.step({}, assets);
        require(exhausted.state().ended && exhausted.state().range == std::uint32_t(-270),
                "The original fuel threshold ends flight before another decrement");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
