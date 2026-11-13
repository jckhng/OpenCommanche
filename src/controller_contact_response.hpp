#pragma once
#include "comanchecd/flight.hpp"
#include "comanchecd/execution_clock.hpp"

namespace comanchecd {
void respond_to_contact(FlightState& state, MissionScene& scene, const SceneView& previous,
                        std::uint8_t previous_height, std::vector<SoundCue>& cues,
                        const CpuWorkSink& work = {}, ExecutionClock* clock = nullptr, const SoundCueHandler& sound_handler = {});
}
