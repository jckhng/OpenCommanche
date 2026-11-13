#pragma once

#include "comanchecd/world_renderer.hpp"
#include "comanchecd/audio.hpp"
#include "comanchecd/options.hpp"
#include "comanchecd/effects.hpp"
#include "comanchecd/playback.hpp"
#include <functional>

namespace comanchecd {
constexpr std::int32_t normal_forward_limit = 0x01800000;
enum class WeaponSelection : std::uint8_t { cannon, rockets, hellfire, stinger, artillery, wingman, previous = 253, next, unchanged };
enum class ControlSource { keyboard, recording };
enum class RecordedAction { none, stores, view };
struct FlightControls {
    std::int32_t horizontal = 0, vertical = 0;
    std::uint8_t throttle = 0;
    bool fire = false;
    bool cycle_target = false;
    bool chaff = false, flare = false;
    WeaponSelection weapon = WeaponSelection::unchanged;
    ControlSource source = ControlSource::keyboard;
    std::uint8_t recorded_power = 0;
};

struct TargetLock {
    std::size_t slot = 0;
    TargetProjection projection;
};

struct FlightState {
    SceneView view;
    TargetLock target;
    std::int32_t forward_speed = 0, side_speed = 0, yaw_rate = 0;
    std::int32_t forward_limit = normal_forward_limit;
    std::uint32_t range = 0;
    std::array<std::uint32_t, 6> ammunition{};
    std::uint8_t control_power = 0, previous_power = 0, engine_power = 0;
    std::uint8_t height_gap = 0, previous_height_gap = 0;
    std::array<std::uint8_t, 8> system_condition{};
    std::int32_t armor = 0;
    std::uint32_t shake_ticks = 0;
    std::uint32_t shake_offset = 0;
    std::uint32_t crash_rotation = 0;
    std::uint8_t cockpit_mode = 1;
    std::uint8_t exit_countdown = 100, exit_requested = 0;
    bool ended = false;
    bool firing = false;
    std::uint8_t weapon = 2;
    std::uint8_t active_chaff = 0, active_flares = 0;
    std::uint8_t chaff = 0, flares = 0;
    bool enemy_tracking = false, incoming_hit = false;
};

struct FlightStatusUpdates {
    std::function<void(const FlightState&)> stores;
    std::function<void(const FlightState&)> countermeasures;
};

class FlightController {
public:
    FlightController(const GameplaySetup& setup, MissionScene scene, TerrainTables tables, std::size_t payload_count);
    const FlightState& state() const;
    const MissionScene& scene() const;
    // Complete cues from the last update or step. The step return retains launch IDs.
    const std::vector<SoundCue>& sounds() const;
    SceneView render_view(const SceneAssets& assets) const;
    void timer_interrupt(const CpuWorkSink& shake_work = {});
    void begin_settings();
    void update_entities(const SceneAssets& assets, ExecutionClock* clock = nullptr, const SoundCueHandler& sound_handler = {});
    std::vector<std::uint16_t> step(const FlightControls& controls, const SceneAssets& assets,
                                  const std::function<void(FlightController&)>& view_transition = {},
                                  const TargetingFrame* targeting = nullptr,
                                  ThreatProximity proximity = ThreatProximity::clear, const CpuWorkSink& work = {},
                                  ExecutionClock* clock = nullptr, const SoundCueHandler& sound_handler = {},
                                  const FlightStatusUpdates& status_updates = {});
    void set_policy(FlightPolicy policy);
    void refresh_target(const TargetingFrame& targeting);
    void observe_projection(const TargetingFrame& targeting);
    void abort();
    void draw_effects(ScriptedEffects& effects, TerrainFrame& frame, const SceneView& view,
                      std::uint32_t tick, EffectLayer layer, ExecutionClock* clock = nullptr);
    void apply_wind(ScriptedEffects& effects, std::uint32_t tick, std::uint32_t camera_heading,
                    ExecutionClock* clock = nullptr);
    void enter_recording(const DemoPose& pose);
    RecordedAction recorded_key(std::uint8_t scan, const SceneAssets& assets);
    void select_weapon(std::uint8_t weapon);
    bool deploy_countermeasure(DefenseAction action, const CpuWorkSink& work = {});

private:
    FlightState state_;
    SceneView fixed_view_;
    TerrainTables tables_;
    MissionScene scene_;
    std::size_t missile_payload_, explosion_payload_;
    std::size_t cannon_payload_, rocket_payload_;
    std::int32_t launch_offset_ = 0x01800000;
    std::array<std::int32_t, 2> movement_basis_{};
    bool trigger_held_ = false;
    bool target_held_ = false;
    bool chaff_held_ = false, flare_held_ = false;
    std::uint8_t terrain_variant_ = 0;
    FlightPolicy policy_;
    std::vector<SoundCue> sounds_;
    MissionScene* interrupt_scene_ = nullptr;
    FlightState* interrupt_state_ = nullptr;
};
} // namespace comanchecd
