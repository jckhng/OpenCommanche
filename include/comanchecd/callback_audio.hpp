#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <functional>
#include <vector>

namespace comanchecd {
enum class AudioChannel { fm, digital };
constexpr std::int32_t audio_gain_one = 1 << 13;

struct AudioChannelConfig {
    bool enabled = false;
    unsigned source_rate = 0;
    std::array<std::int32_t, 2> gain{audio_gain_one, audio_gain_one};
    // One output low-pass stage, in 16.16 fixed point; zero disables it.
    std::int32_t lowpass_alpha = 0;
};

using AudioAccumulator = std::array<std::int32_t, 2>;

// Device replay with explicit callback boundaries. The caller supplies the
// schedule and DMA bytes; this does not infer DOS CPU or DMA timing.
class CallbackAudio {
public:
    explicit CallbackAudio(unsigned output_rate);
    ~CallbackAudio();
    void configure(AudioChannel channel, const AudioChannelConfig& config);
    void write_fm(std::uint8_t reg, std::uint8_t value);
    void generate_fm(unsigned frames);
    void queue_digital(const std::vector<std::uint8_t>& samples);
    std::vector<AudioAccumulator> mix(AudioChannel channel, unsigned through_frame);
    std::vector<AudioAccumulator> service(AudioChannel channel, unsigned through_frame,
        const std::function<std::vector<std::uint8_t>(unsigned)>& read_digital = {});
    void end_frame(AudioChannel channel, unsigned frames);
    static std::array<std::int16_t, 2> output(const AudioAccumulator& sample,
                                            const std::array<std::int32_t, 2>& gain);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace comanchecd
