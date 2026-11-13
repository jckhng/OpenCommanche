#pragma once

#include "comanchecd/frontend.hpp"
#include "comanchecd/training.hpp"

namespace comanchecd {
// Owned observations of a completed public session call. PCM remains ordered separately.
struct SessionPresentation {
    FrontendState menu{};
    FlightState flight{};
    FlightNotice flight_notice = FlightNotice::none;
    CockpitImage image{};
    std::uint64_t cpu_cycles = 0, irq_ticks = 0, audio_samples = 0;
    std::uint32_t random_seed = 0;
    bool flying = false, demo_playing = false, quit_requested = false;
};
}
