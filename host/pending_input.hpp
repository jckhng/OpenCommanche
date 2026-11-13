#pragma once

#include "flight_host.hpp"

namespace comanchecd::host {
class PendingFlightInput {
public:
    void queue(const HostInput& input) {
        if (input.controls.weapon != WeaponSelection::unchanged) { weapon_ = input.controls.weapon; }
        pressed_.fire = pressed_.fire || input.pressed.fire;
        pressed_.target = pressed_.target || input.pressed.target;
        pressed_.chaff = pressed_.chaff || input.pressed.chaff;
        pressed_.flare = pressed_.flare || input.pressed.flare;
    }
    FlightControls apply(FlightControls controls) {
        controls.weapon = weapon_;
        controls.fire = controls.fire || pressed_.fire;
        controls.cycle_target = controls.cycle_target || pressed_.target;
        controls.chaff = controls.chaff || pressed_.chaff;
        controls.flare = controls.flare || pressed_.flare;
        clear();
        return controls;
    }
    void clear() { weapon_ = WeaponSelection::unchanged; pressed_ = {}; }
    void clear_presses() { pressed_ = {}; }
private:
    WeaponSelection weapon_ = WeaponSelection::unchanged;
    FlightPresses pressed_;
};
}
