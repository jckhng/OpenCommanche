#pragma once

#include "comanchecd/menu_renderer.hpp"
#include "comanchecd/cpu_work.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace comanchecd::color_metric {
constexpr std::array<unsigned, 3> weights{900, 3025, 225};

inline std::array<int, 3> convert(const std::array<std::uint8_t, 3>& color, const Transfer& transfer) {
    if (std::any_of(color.begin(), color.end(), [](std::uint8_t channel) { return channel >= 64; })) {
        throw std::invalid_argument("Expected six-bit palette channels");
    }
    return {transfer[color[0]], transfer[color[1]], color[2] * 2};
}

inline std::uint8_t nearest(const Palette& palette, const Transfer& transfer,
                            const std::array<std::uint8_t, 3>& color, std::size_t first, std::size_t last,
                            const CpuWorkSink& work = {}) {
    const auto target = convert(color, transfer);
    auto best = std::numeric_limits<unsigned>::max();
    std::uint8_t result = 0;
    if (work) { work(CpuOperation::instructions, 16); }
    for (std::size_t candidate = first; candidate <= last; ++candidate) {
        const auto converted = convert(palette[candidate], transfer);
        unsigned distance = 0;
        for (std::size_t channel = 0; channel < target.size(); ++channel) {
            const int difference = converted[channel] - target[channel];
            distance += weights[channel] * static_cast<unsigned>(difference * difference);
        }
        if (work) {
            for (unsigned channel = 0; channel < target.size(); ++channel) {
                work(CpuOperation::string_bytes, 1);
                work(CpuOperation::instructions, channel == 2 ? 4 : 2);
            }
        }
        if (distance < best) {
            best = distance;
            result = static_cast<std::uint8_t>(candidate);
            if (work) { work(CpuOperation::instructions, 4); }
        }
        if (distance == 0) {
            if (work) { work(CpuOperation::instructions, 2); }
            return result;
        }
        if (work) { work(CpuOperation::instructions, 2); }
    }
    if (work) { work(CpuOperation::instructions, 2); }
    return result;
}
}
