#pragma once
#include "comanchecd/audio.hpp"
#include "comanchecd/audio_scheduler.hpp"
#include "loading_work.hpp"

namespace comanchecd {
GameplayPayload read_startup_resource(AudioScheduler& scheduler, ExecutionClock& clock,
    const StartupResource& resource, unsigned filename_bytes, loading_work::InputBuffer* buffer = nullptr);
}
