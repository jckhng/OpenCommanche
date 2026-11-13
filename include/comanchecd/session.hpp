#pragma once

#include "comanchecd/frontend.hpp"
#include "comanchecd/training.hpp"
#include "comanchecd/presentation.hpp"

namespace comanchecd {
using SettingsWriter = std::function<void(const std::vector<std::uint8_t>&)>;
enum class SessionStart { pilot_menu, cold };
class GameSession {
public:
    GameSession(Installation installation, PilotWriter writer, std::vector<std::uint8_t> pilots = {},
                std::vector<std::uint8_t> settings = {}, SettingsWriter settings_writer = {},
                SessionStart start = SessionStart::pilot_menu);
    void consume(PilotKey key);
    void queue_key_byte(std::uint8_t scan);
    void schedule_key_byte(std::uint64_t cycle, std::uint8_t scan);
    std::uint64_t cpu_cycles() const;
    void advance(const FlightControls& controls, const CockpitControls& displays);
    std::vector<std::int16_t> render_audio(std::size_t samples);
    std::vector<std::int16_t> take_pending_audio();
    const CockpitImage& image() const;
    // Capture on the authoritative owner thread after a completed session call.
    std::shared_ptr<const SessionPresentation> presentation() const;
    const std::vector<CockpitImage>& intermediate_images() const;
    const FlightState& flight_state() const;
    FrontendState menu_state() const;
    bool flying() const;
    bool demo_playing() const;
    std::uint64_t irq_ticks() const;
    std::uint32_t random_seed() const;
    bool quit_requested() const;
    void abort_mission();

private:
    Installation installation_;
    MenuSettings settings_;
    SettingsWriter settings_writer_;
    PilotWriter pilot_writer_;
    std::vector<std::uint8_t> pilot_bytes_;
    bool startup_ = false, startup_skip_ = false, hardware_input_ = false;
    std::unique_ptr<Frontend> frontend_;
    IrqState session_irq_{training_scene_seed};
    KeyboardInput keyboard_;
    std::uint64_t menu_input_resets_ = 0;
    std::unique_ptr<TrainingFlight> flight_;
    std::shared_ptr<const std::vector<std::uint8_t>> depth_padding_;
    std::shared_ptr<MissionAudio> audio_;
    std::shared_ptr<ReflectionState> reflections_ = std::make_shared<ReflectionState>();
    std::shared_ptr<EffectsState> effects_ = std::make_shared<EffectsState>();
    CockpitImage menu_image_;
    std::size_t menu_back_page_ = 0;
    std::vector<CockpitImage> intermediate_images_;
    FlightState inactive_state_;
    FlightNotice flight_notice_ = FlightNotice::none;
    std::unique_ptr<KdvMovie> overview_;
    std::unique_ptr<KdvPlayer> overview_player_;
    std::uint64_t elapsed_samples_ = 0;
    bool quit_ = false;
    enum class DemoPhase { none, fanfare, animation, title_wait, title, hangar, flight };
    DemoPhase demo_phase_ = DemoPhase::none;
    std::unique_ptr<DemoCinematics> cinematics_;
    std::unique_ptr<KdvPlayer> demo_player_;
    std::uint8_t demo_index_ = 0;
    std::size_t demo_cue_ = 0, demo_loops_ = 0;
    std::uint64_t demo_wait_ = 0;
    std::uint64_t demo_clear_sample_ = 0;
    void start_demo_cycle();
    void start_demo_flight();
    void demo_movie_boundary();
    void demo_sound();
    std::vector<std::int16_t> render_presentation_audio(std::size_t samples);
    void start_menu_audio(MusicPlaylist playlist);
    void release_flight();
    void finish_flight();
    void start_frontend();
    void finish_startup();
    void begin_fanfare_movie();
    void poll_input();
};
} // namespace comanchecd
