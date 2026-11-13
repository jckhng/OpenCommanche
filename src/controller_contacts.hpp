#pragma once
#include "comanchecd/flight.hpp"
namespace comanchecd {
// Inline DOS contact scan 19e62..19ec6, or its early branch to 1a071.
bool scan_player_contacts(const FlightState& state, const MissionScene& scene, const CpuWorkSink& work = {});
}
