#include "comanchecd/audio.hpp"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr unsigned mixer_volume_shift = 13;
constexpr unsigned filter_shift = 16;
constexpr std::int32_t filter_unit = 1 << filter_shift;
constexpr double pi = 3.14159265358979323846;
}

Sb16Playback::Sb16Playback(unsigned output_rate) : output_rate_(output_rate) {
    if (output_rate != 44100 && output_rate != 48000) {
        throw std::invalid_argument("SB16 playback supports 44100 or 48000 Hz output");
    }
    const auto interval = 1.0 / output_rate;
    const auto time_constant = 1.0 / ((source_rate / 2) * 2 * pi);
    filter_alpha_ = static_cast<std::int32_t>(interval / (time_constant + interval) * filter_unit);
}

std::vector<std::int16_t> Sb16Playback::render(const std::vector<std::uint8_t>& pcm) {
    std::vector<std::int16_t> result;
    for (const auto sample : render_accumulator(pcm)) {
        result.push_back(static_cast<std::int16_t>(std::clamp(sample >> mixer_volume_shift, -32768, 32767)));
    }
    return result;
}

std::vector<std::int32_t> Sb16Playback::render_accumulator(const std::vector<std::uint8_t>& pcm) {
    std::vector<std::int32_t> result;
    for (const auto byte : pcm) {
        const auto current = (int(byte) - 128) * 256;
        if (!loaded_) {
            previous_ = current;
            loaded_ = true;
            continue;
        }
        const auto delta = current - previous_;
        while (phase_ < output_rate_) {
            const auto sample = previous_ + (std::int64_t(delta) * phase_) / output_rate_;
            const auto scaled = sample * (1 << mixer_volume_shift);
            // Filter the scaled mixer accumulator before reducing to signed PCM.
            filtered_ = static_cast<std::int32_t>((scaled * filter_alpha_ +
                std::int64_t(filtered_) * (filter_unit - filter_alpha_)) >> filter_shift);
            result.push_back(filtered_);
            phase_ += source_rate;
        }
        phase_ -= output_rate_;
        previous_ = current;
    }
    return result;
}
} // namespace comanchecd
