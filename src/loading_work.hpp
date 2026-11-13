#pragma once

#include "comanchecd/gameplay.hpp"
#include "comanchecd/menu_renderer.hpp"
#include "comanchecd/cpu_work.hpp"
#include "comanchecd/execution_clock.hpp"

namespace comanchecd::loading_work {
constexpr unsigned input_buffer_bytes = 8192;
using InputBuffer = std::array<std::uint8_t, input_buffer_bytes>;
GameplayPayload compressed_resource(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock);
GameplayPayload compressed_resource(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock, InputBuffer& buffer);
GameplayPayload picture(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock);
GameplayPayload picture(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock, InputBuffer& buffer);
void cockpit_remap(const GameplayPayload& picture, const Palette& destination, const Transfer& transfer,
                   const CpuWorkSink& work);
void cockpit_spans(const GameplayPayload& picture, const CpuWorkSink& work);
void scenery_palette(const GameplaySetup& setup, const Palette& palette, const Transfer& transfer,
                     const CpuWorkSink& work);
}
