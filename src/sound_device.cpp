#include "comanchecd/sound_device.hpp"
#include "comanchecd/audio.hpp"
#include "dbopl.h"

#include <algorithm>
#include <deque>

namespace comanchecd {
namespace {
constexpr unsigned mixer_volume_shift = 13;
constexpr std::int32_t fm_volume = (1 << mixer_volume_shift) * 3 / 2;
constexpr bool opl3_capable = true;
}

class SoundDevice::Impl {
public:
    explicit Impl(unsigned rate) : digital_(rate), fm_(opl3_capable) {
        static const bool tables_ready = [] {
            DBOPL::InitTables();
            return true;
        }();
        (void)tables_ready;
        fm_.Setup(rate);
    }

    void write_fm(std::uint8_t reg, std::uint8_t value) { fm_.WriteReg(reg, value); }

    void queue_digital(std::uint8_t sample) {
        const auto block = digital_.render_accumulator({sample});
        samples_.insert(samples_.end(), block.begin(), block.end());
    }

    std::int16_t render_sample() {
        std::int32_t fm;
        fm_.GenerateBlock2(1, &fm);
        std::int64_t mixed = std::int64_t(fm) * fm_volume;
        if (!samples_.empty()) {
            mixed += samples_.front();
            samples_.pop_front();
        }
        // Both channels retain fractional gain/filter bits until final clipping.
        return static_cast<std::int16_t>(std::clamp<std::int64_t>(mixed >> mixer_volume_shift, -32768, 32767));
    }

private:
    Sb16Playback digital_;
    DBOPL::Chip fm_;
    std::deque<std::int32_t> samples_;
};

SoundDevice::SoundDevice(unsigned rate) : impl_(std::make_unique<Impl>(rate)) {}
SoundDevice::~SoundDevice() = default;
void SoundDevice::write_fm(std::uint8_t reg, std::uint8_t value) { impl_->write_fm(reg, value); }
void SoundDevice::queue_digital(std::uint8_t sample) { impl_->queue_digital(sample); }
std::int16_t SoundDevice::render_sample() { return impl_->render_sample(); }
} // namespace comanchecd
