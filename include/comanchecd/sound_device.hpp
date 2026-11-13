#pragma once

#include <cstdint>
#include <memory>

namespace comanchecd {
// Portable device output. Callers own sample delivery and register timing.
// FM uses one-sample blocks; DOS mixer callback scheduling is not modeled here.
class SoundDevice {
public:
    explicit SoundDevice(unsigned output_rate);
    ~SoundDevice();
    void write_fm(std::uint8_t reg, std::uint8_t value);
    void queue_digital(std::uint8_t sample);
    std::int16_t render_sample();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace comanchecd
