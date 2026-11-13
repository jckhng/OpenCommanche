// Interpolation, padding and filter arithmetic adapted from DOSBox-X mixer.cpp.
// Copyright (C) 2002-2021 The DOSBox Team; GPL-2.0-or-later. See THIRD_PARTY.md.
#include "comanchecd/callback_audio.hpp"
#include "dbopl.h"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr unsigned buffer_capacity = 2048;
constexpr unsigned fm_block_limit = 512;
constexpr unsigned volume_shift = 13;
constexpr unsigned filter_shift = 16;
constexpr std::int32_t filter_one = 1 << filter_shift;

class Channel {
public:
    explicit Channel(unsigned rate) : output_rate_(rate) {}

    void configure(const AudioChannelConfig& config) {
        if (!config.source_rate || config.source_rate > output_rate_ ||
                config.lowpass_alpha < 0 || config.lowpass_alpha > filter_one) {
            throw std::invalid_argument("Unsupported callback channel configuration");
        }
        for (const auto gain : config.gain) {
            if (gain < 0 || gain > (4 << volume_shift)) {
                throw std::invalid_argument("Callback channel gain exceeds supported range");
            }
        }
        if (config_.enabled && !config.enabled) {
            phase_ = 0;
        }
        config_ = config;
    }

    void add(const std::vector<AudioAccumulator>& samples) {
        if (!config_.source_rate) {
            throw std::logic_error("Configure the channel before delivering samples");
        }
        std::size_t cursor = 0;
        if (!loaded_) {
            if (samples.empty()) { return; }
            load(samples[cursor++]);
            if (cursor == samples.size()) {
                phase_ = interpolation_phase_ = output_rate_;
                return;
            }
            load(samples[cursor++]);
            phase_ = interpolation_phase_ = 0;
        }
        for (;;) {
            if (phase_ >= output_rate_) {
                if (cursor == samples.size()) { break; }
                load(samples[cursor++]);
                phase_ -= output_rate_;
                interpolation_phase_ = phase_;
            }
            interpolate(buffer_capacity);
            if (buffer_.size() == buffer_capacity) {
                throw std::length_error("Callback channel buffer exhausted");
            }
        }
    }

    std::vector<AudioAccumulator> mix(unsigned through) {
        if (through > buffer_capacity) {
            throw std::out_of_range("Callback interval exceeds channel buffer");
        }
        if (through <= rendered_) { return {}; }
        std::vector<AudioAccumulator> result(through - rendered_);
        if (!config_.enabled) {
            rendered_ = through;
            return result;
        }
        if (buffer_.size() < through) {
            if (loaded_) { interpolate(through); }
            if (buffer_.size() < through) {
                phase_ = std::min(phase_, output_rate_);
                // The reference pads with the unscaled current sample.
                buffer_.resize(through, current_);
            }
        }
        for (auto& sample : result) {
            auto& source = buffer_.at(consumed_++);
            if (config_.lowpass_alpha) {
                for (unsigned c = 0; c < source.size(); ++c) {
                    const auto filtered = std::int64_t(source[c]) * config_.lowpass_alpha +
                        std::int64_t(filter_[c]) * (filter_one - config_.lowpass_alpha);
                    source[c] = filter_[c] = static_cast<std::int32_t>(filtered >> filter_shift);
                }
            }
            sample = source;
        }
        rendered_ = through;
        return result;
    }

    void end_frame(unsigned frames) {
        if (frames > buffer_capacity) {
            throw std::out_of_range("Callback interval exceeds channel buffer");
        }
        rendered_ = 0;
        buffer_.erase(buffer_.begin(), buffer_.begin() + std::min<std::size_t>(frames, buffer_.size()));
        consumed_ = consumed_ >= frames ? consumed_ - frames : 0;
    }

    unsigned requested(unsigned through) const {
        if (!config_.enabled || through <= rendered_ || through <= buffer_.size()) { return 0; }
        const auto numerator = (through - buffer_.size()) * config_.source_rate + phase_;
        return static_cast<unsigned>((numerator + output_rate_ - 1) / output_rate_) + (loaded_ ? 0 : 1);
    }

private:
    void load(const AudioAccumulator& sample) {
        last_ = current_;
        current_ = sample;
        for (unsigned c = 0; c < sample.size(); ++c) {
            delta_[c] = current_[c] - last_[c];
        }
        loaded_ = true;
    }

    void interpolate(unsigned limit) {
        while (interpolation_phase_ < output_rate_ && buffer_.size() < limit) {
            AudioAccumulator sample{};
            for (unsigned c = 0; c < sample.size(); ++c) {
                const auto value = last_[c] + std::int64_t(delta_[c]) * interpolation_phase_ / output_rate_;
                sample[c] = static_cast<std::int32_t>(value * config_.gain[c]);
            }
            buffer_.push_back(sample);
            phase_ += config_.source_rate;
            interpolation_phase_ += config_.source_rate;
        }
        if (buffer_.size() >= limit) { return; }
        for (unsigned c = 0; c < current_.size(); ++c) {
            current_[c] = last_[c] + delta_[c];
        }
        while (phase_ < output_rate_ && buffer_.size() < limit) {
            buffer_.push_back({current_[0] * config_.gain[0], current_[1] * config_.gain[1]});
            phase_ += config_.source_rate;
        }
    }

    unsigned output_rate_, phase_ = 0, interpolation_phase_ = 0, rendered_ = 0;
    std::size_t consumed_ = 0;
    bool loaded_ = false;
    AudioChannelConfig config_;
    AudioAccumulator last_{}, current_{}, delta_{}, filter_{};
    std::vector<AudioAccumulator> buffer_;
};
}

class CallbackAudio::Impl {
public:
    explicit Impl(unsigned rate) : fm_(true), channels_{Channel(rate), Channel(rate)} {
        if (!rate || rate > 192000) {
            throw std::invalid_argument("Unsupported callback output rate");
        }
        static const bool initialized = [] { DBOPL::InitTables(); return true; }();
        (void)initialized;
        fm_.Setup(rate);
    }

    Channel& channel(AudioChannel channel) { return channels_.at(static_cast<std::size_t>(channel)); }
    void write_fm(std::uint8_t reg, std::uint8_t value) { fm_.WriteReg(reg, value); }
    void generate_fm(unsigned frames) {
        if (!frames || frames > fm_block_limit) {
            throw std::invalid_argument("Invalid FM callback length");
        }
        std::array<std::int32_t, fm_block_limit> raw{};
        fm_.GenerateBlock2(frames, raw.data());
        std::vector<AudioAccumulator> samples;
        samples.reserve(frames);
        for (unsigned i = 0; i < frames; ++i) { samples.push_back({raw[i], raw[i]}); }
        channel(AudioChannel::fm).add(samples);
    }

private:
    DBOPL::Chip fm_;
    std::array<Channel, 2> channels_;
};

CallbackAudio::CallbackAudio(unsigned rate) : impl_(std::make_unique<Impl>(rate)) {}
CallbackAudio::~CallbackAudio() = default;
void CallbackAudio::configure(AudioChannel channel, const AudioChannelConfig& config) { impl_->channel(channel).configure(config); }
void CallbackAudio::write_fm(std::uint8_t reg, std::uint8_t value) { impl_->write_fm(reg, value); }
void CallbackAudio::generate_fm(unsigned frames) { impl_->generate_fm(frames); }
void CallbackAudio::queue_digital(const std::vector<std::uint8_t>& bytes) {
    std::vector<AudioAccumulator> samples;
    samples.reserve(bytes.size());
    for (const auto byte : bytes) {
        const auto value = (std::int32_t(byte) - 128) * 256;
        samples.push_back({value, value});
    }
    impl_->channel(AudioChannel::digital).add(samples);
}
std::vector<AudioAccumulator> CallbackAudio::mix(AudioChannel channel, unsigned through) { return impl_->channel(channel).mix(through); }
std::vector<AudioAccumulator> CallbackAudio::service(AudioChannel channel, unsigned through,
        const std::function<std::vector<std::uint8_t>(unsigned)>& read_digital) {
    if (through > buffer_capacity) { throw std::out_of_range("Callback interval exceeds channel buffer"); }
    // The reference allows two attempts because a DSP callback can finish a transfer.
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto requested = impl_->channel(channel).requested(through);
        if (!requested) { break; }
        if (channel == AudioChannel::fm) {
            generate_fm(std::min(requested, fm_block_limit));
        } else if (read_digital) {
            queue_digital(read_digital(requested));
        }
    }
    return mix(channel, through);
}
void CallbackAudio::end_frame(AudioChannel channel, unsigned frames) { impl_->channel(channel).end_frame(frames); }
std::array<std::int16_t, 2> CallbackAudio::output(const AudioAccumulator& sample, const std::array<std::int32_t, 2>& gain) {
    std::array<std::int16_t, 2> result{};
    for (unsigned c = 0; c < result.size(); ++c) {
        const auto value = (std::int64_t(sample[c]) * gain[c]) >> (volume_shift * 2);
        result[c] = static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
    }
    return result;
}
} // namespace comanchecd
