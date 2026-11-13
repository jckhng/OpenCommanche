#pragma once

#include "comanchecd/training.hpp"
#include "comanchecd/session.hpp"

#include <string>

namespace comanchecd::host {
enum class PauseAction { none, toggle, pause };
enum class Playback { paused, running };
enum class InputContext { menu, name_entry, flight, training };
enum class InputProfile { faithful, enhanced };

struct FlightPresses {
    bool fire = false, target = false, chaff = false, flare = false;
};

struct HostInput {
    std::vector<std::uint8_t> scan_bytes;
    FlightControls controls;
    CockpitControls cockpit;
    FlightPresses pressed;
    PauseAction pause = PauseAction::none;
    bool reset = false;
    bool quit = false;
    bool escape = false;
    bool abort_mission = false;
    bool key_pressed = false;
    std::vector<PilotKey> keys;
};

// Presentation and devices are replaceable without changing the training core.
class FlightHost {
public:
    virtual ~FlightHost() = default;
    virtual HostInput poll() = 0;
    virtual void set_input_context(InputContext) {}
    virtual void present(const CockpitImage& image, const FlightState& state, const std::string& status) = 0;
    virtual void present_snapshot(std::shared_ptr<const SessionPresentation> frame, const std::string& status) {
        present(frame->image, frame->flight, status);
    }
    virtual void queue_audio(const std::vector<std::int16_t>& samples) = 0;
    virtual std::size_t queued_audio_samples() const = 0;
    virtual void clear_audio() = 0;
    virtual void set_playback(Playback playback) = 0;
    virtual void idle() = 0;
    virtual void reset_controls() {}
};

// Zero tick limit selects an interactive session. Timing is a host preview policy.
std::size_t run_viewer(TrainingFlight& flight, FlightHost& host, std::size_t tick_limit = 0);
void run_session(GameSession& session, FlightHost& host);
InputContext input_context(const FrontendState& menu, bool flying);
} // namespace comanchecd::host
