#pragma once

#include "comanchecd/assets.hpp"
#include "comanchecd/flight.hpp"

namespace comanchecd {
constexpr std::size_t scene_pixel_aspect = 2;
constexpr unsigned training_ticks_per_second = 20;
constexpr unsigned training_samples_per_tick = audio_sample_rate / training_ticks_per_second;
constexpr std::uint32_t training_scene_seed = 0x12345678;
static_assert(audio_sample_rate % training_ticks_per_second == 0);

struct SceneImage {
    std::array<std::uint8_t, scene_columns * scene_buffer_side> pixels{};
    Palette palette{};
};

SceneImage scene_image(const TerrainFrame& frame, const Palette& palette);

struct FlightMission {
    GameplaySetup setup;
    MissionResources resources;
    std::vector<std::uint8_t> settings;
    std::shared_ptr<const DemoRecording> recording;
    // Full startup allocation margins survive mission changes and detail settings.
    std::shared_ptr<const std::vector<std::uint8_t>> depth_padding;
    std::size_t back_page = 0;
};

enum class ObjectiveState : std::uint8_t { none, active, destroyed };
enum class FlightNotice { none, unavailable_view, unavailable_calibration, unavailable_input_mode, unavailable_rockets };

struct FlightInput {
    std::function<std::uint8_t(ExecutionClock&)> take_key;
    std::function<bool()> alt_down;
    std::function<void()> settings;
    std::function<void(FlightNotice)> notice;
};

// Owns a mission. Standalone previews use fixed ticks; sessions carry an audio-clock timeline.
class TrainingFlight {
public:
    explicit TrainingFlight(Installation installation, std::uint32_t seed = training_scene_seed);
    TrainingFlight(Installation installation, FlightMission mission, std::uint32_t seed = training_scene_seed);
    TrainingFlight(Installation installation, FlightMission mission, std::shared_ptr<MissionAudio> audio,
                   std::uint32_t seed = training_scene_seed, std::shared_ptr<ReflectionState> reflections = {},
                   std::shared_ptr<EffectsState> effects = {}, IrqState* loading_state = nullptr);
    ~TrainingFlight();
    void apply_settings(const MenuSettings& settings);
    FrontendPicture begin_settings();
    const FlightState& state() const;
    IrqState interrupt_state() const;
    const SceneImage& image() const;
    const CockpitImage& cockpit_image() const;
    const std::vector<CockpitImage>& intermediate_images() const;
    void advance(const FlightControls& controls, const CockpitControls& cockpit = {}, const FlightInput& input = {});
    std::vector<std::int16_t> render_audio(std::size_t samples);
    void reset();
    MissionResult result(const CpuWorkSink& work = {}) const;
    void abort();
    std::uint8_t objectives_remaining() const;
    std::array<ObjectiveState, player_slot - 1> objectives() const;
    std::size_t demo_position() const;
    bool demo_finished() const;

private:
    class Impl;
    Installation installation_;
    std::uint32_t seed_;
    IrqState* loading_state_ = nullptr;
    std::unique_ptr<FlightMission> mission_;
    std::shared_ptr<ReflectionState> reflection_state_ = std::make_shared<ReflectionState>();
    std::shared_ptr<EffectsState> effects_state_ = std::make_shared<EffectsState>();
    std::unique_ptr<Impl> impl_;
};
} // namespace comanchecd
