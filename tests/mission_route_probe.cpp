#include "comanchecd/training.hpp"

#include <fstream>
#include <iostream>

namespace {
void checkpoint(std::ostream& output, int frame, const comanchecd::FlightState& state) {
    output << "{\"frame\":" << frame << ",\"state\":{";
    bool first = true;
    const auto field = [&](const std::string& name, std::int64_t value) {
        output << (first ? "" : ",") << '"' << name << "\":" << value;
        first = false;
    };
    field("x", state.view.x); field("y", state.view.y);
    field("heading", state.view.heading); field("height", state.view.height);
    field("pitch", state.view.pitch); field("roll", state.view.roll);
    field("forward_speed", state.forward_speed); field("side_speed", state.side_speed);
    field("yaw_rate", state.yaw_rate); field("range", state.range);
    field("control_power", state.control_power); field("previous_power", state.previous_power);
    field("engine_power", state.engine_power); field("height_gap", state.height_gap);
    field("previous_height_gap", state.previous_height_gap);
    for (std::size_t slot = 0; slot < state.ammunition.size(); ++slot) {
        field("ammunition_" + std::to_string(slot), state.ammunition[slot]);
    }
    for (std::size_t slot = 0; slot < state.system_condition.size(); ++slot) {
        field("system_" + std::to_string(slot), state.system_condition[slot]);
    }
    field("armor", state.armor); field("shake_ticks", state.shake_ticks);
    field("shake_offset", state.shake_offset); field("crash_rotation", state.crash_rotation);
    field("cockpit_mode", state.cockpit_mode); field("exit_countdown", state.exit_countdown);
    field("exit_requested", state.exit_requested); field("weapon", state.weapon);
    output << "}}\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 6) {
            throw std::invalid_argument("Usage: mission_route_probe INSTALLATION PAYLOAD_DIRECTORY SEED CONTROLS OUTPUT");
        }
        std::ifstream input(argv[4]);
        std::ofstream output(argv[5]);
        if (!input || !output) {
            throw std::runtime_error("Cannot open mission route input/output");
        }
        comanchecd::TrainingFlight flight(comanchecd::Installation(argv[1], argv[2]), std::stoul(argv[3], nullptr, 0));
        checkpoint(output, -1, flight.state());
        int frame = 0;
        comanchecd::FlightControls controls;
        unsigned throttle, fire, weapon;
        while (input >> controls.horizontal) {
            if (!(input >> controls.vertical >> throttle >> fire >> weapon) || throttle > 15 || fire > 1 ||
                weapon < static_cast<unsigned>(comanchecd::WeaponSelection::previous) || weapon > 255 || frame >= 4096) {
                throw std::invalid_argument("Invalid retained mission control stream");
            }
            controls.throttle = static_cast<std::uint8_t>(throttle);
            controls.fire = fire != 0;
            controls.weapon = static_cast<comanchecd::WeaponSelection>(weapon);
            flight.advance(controls);
            flight.render_audio(comanchecd::training_samples_per_tick);
            checkpoint(output, frame++, flight.state());
        }
        if (!input.eof() || frame == 0 || !output) {
            throw std::runtime_error("Truncated mission route");
        }
        std::cout << "Replayed " << frame << " retained mission frames\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
