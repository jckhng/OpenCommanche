#pragma once

#include "flight_host.hpp"

namespace comanchecd::host {
struct SdlHostOptions {
    InputProfile input = InputProfile::faithful;
    std::string controller_mappings;
};

class SdlFlightHost final : public FlightHost {
public:
    explicit SdlFlightHost(SdlHostOptions options = {});
    ~SdlFlightHost() override;
    HostInput poll() override;
    void set_input_context(InputContext context) override;
    void present(const CockpitImage& image, const FlightState& state, const std::string& status) override;
    void queue_audio(const std::vector<std::int16_t>& samples) override;
    std::size_t queued_audio_samples() const override;
    void clear_audio() override;
    void set_playback(Playback playback) override;
    void idle() override;
    void reset_controls() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace comanchecd::host
