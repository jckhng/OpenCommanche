#pragma once
#include "comanchecd/audio.hpp"
#include "comanchecd/audio_scheduler.hpp"
#include "startup_timing.hpp"

namespace comanchecd {
enum class MenuEntryBoundary { exit, digital, reset, entry, first_score, second_score, picture, colors };
void prepare_pilot_menu(AudioScheduler& scheduler, ExecutionClock& clock, const PilotMenuAssets& assets,
    bool digital_active, const StartupDigital& digital, const std::function<unsigned()>& reset_voices,
    const std::function<void()>& music, const std::function<void(MenuEntryBoundary)>& observer = {});
}
