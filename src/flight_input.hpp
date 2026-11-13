#pragma once

#include "comanchecd/flight.hpp"

namespace comanchecd {
constexpr std::uint8_t no_view_request = 99;
enum class FlightKeyAction {
    flare, chaff, left_panel, right_panel, weapon, view, zoom, settings, input_flags, rockets, calibrate
};

struct FlightKeyContext {
    std::uint8_t weapon = 2, view = 1, left_panel = 0, right_panel = 2;
    bool alternate = false;
    std::function<bool()> alt_down;
};

using FlightKeyHandler = std::function<void(FlightKeyAction, unsigned)>;

void dispatch_flight_key(std::uint8_t scan, const FlightKeyContext& context,
                         const FlightKeyHandler& handler, ExecutionClock& clock);
void dispatch_flight_view(std::uint8_t request, std::uint8_t current,
                          const FlightKeyHandler& handler, ExecutionClock& clock);
}
