#pragma once

#include "flight_host.hpp"

namespace comanchecd::host {
class SdlAudioOutput {
public:
    SdlAudioOutput();
    ~SdlAudioOutput();
    void queue(const std::vector<std::int16_t>& samples);
    std::size_t queued_samples() const;
    void clear();
    void set_playback(Playback playback);
    void play_blocking(const std::vector<std::int16_t>& samples);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace comanchecd::host
