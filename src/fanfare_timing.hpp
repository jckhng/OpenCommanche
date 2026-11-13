#pragma once
#include "loading_work.hpp"

#include "comanchecd/audio.hpp"
#include "comanchecd/audio_scheduler.hpp"

namespace comanchecd {
enum class FanfareBoundary { resource, picture, decoder, picture_ready, first_page, presented,
                             palette, second_page, colors, latch_copy, ready };
std::vector<std::uint8_t> prepare_fanfare(AudioScheduler& scheduler, ExecutionClock& clock,
    const FanfareAssets& assets, const std::function<void(FanfareBoundary)>& observer = {});
void prepare_menu_picture(AudioScheduler& scheduler, ExecutionClock& clock,
                          const StartupResource& picture, unsigned filename_bytes);
GameplayPayload read_loading_picture(AudioScheduler& scheduler, ExecutionClock& clock,
    const StartupResource& picture, unsigned filename_bytes, loading_work::InputBuffer* buffer = nullptr);
void draw_loading_picture(ExecutionClock& clock, unsigned height);
void apply_loading_palette(ExecutionClock& clock);
void copy_loading_page(ExecutionClock& clock, unsigned height);
}
