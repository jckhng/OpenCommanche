#pragma once
#include "startup_timing.hpp"
#include "comanchecd/audio.hpp"

namespace comanchecd {
enum class MovieLoadingBoundary { settings, find, counter, cues, score, sound, movie };
void prepare_startup_movie(AudioScheduler& scheduler, ExecutionClock& clock,
    const StartupFiles& files, const StartupMovieAssets& assets,
    const std::function<void(MusicLevel)>& music, const StartupDigital& digital,
    const std::function<void()>& reset_counter,
    const std::function<void(MovieLoadingBoundary)>& observer = {});
}
