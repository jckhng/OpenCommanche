#include "comanchecd/training.hpp"
#include "loading_work.hpp"
#include "flight_work.hpp"
#include "flight_input.hpp"
#include "mission_loading.hpp"
#include "fanfare_timing.hpp"
#include "startup_storage.hpp"

#include <algorithm>

namespace comanchecd {
namespace {
constexpr std::uint8_t previous_weapon_scan = 0x1a;
constexpr std::uint8_t next_weapon_scan = 0x1b;
constexpr unsigned cockpit_view = 1, chase_view = 6, fixed_view = 7;

CockpitEntry mission_cockpit_entry(const FlightState& player, const FlightMission* mission) {
    CockpitEntry entry;
    if (mission != nullptr) { entry.back_page = mission->back_page; }
    entry.previous_height = player.view.height; // 70ae seeds the first vertical-rate comparison.
    return entry;
}

class MissionIrqBinding {
public:
    MissionIrqBinding(std::shared_ptr<MissionAudio> audio, std::function<void()> interrupt)
        : audio_(std::move(audio)) {
        if (audio_) { token_ = audio_->add_interrupt_handler(std::move(interrupt)); }
    }
    ~MissionIrqBinding() {
        if (audio_) { audio_->remove_interrupt_handler(token_); }
    }
    MissionIrqBinding(const MissionIrqBinding&) = delete;
    MissionIrqBinding& operator=(const MissionIrqBinding&) = delete;
private:
    std::shared_ptr<MissionAudio> audio_;
    std::uint64_t token_ = 0;
};

class FlightIrqTarget {
public:
    FlightIrqTarget(FlightController*& target, FlightController& active) : target_(target), previous_(target) {
        target_ = &active;
    }
    ~FlightIrqTarget() { target_ = previous_; }
    FlightIrqTarget(const FlightIrqTarget&) = delete;
    FlightIrqTarget& operator=(const FlightIrqTarget&) = delete;
private:
    FlightController*& target_;
    FlightController* previous_;
};

struct TrainingResources {
    GameplaySetup setup;
    std::vector<GameplayPayload> payloads;
    SceneAssets assets;
    CockpitAssets cockpit;
    CockpitTables cockpit_tables;
    std::shared_ptr<const DemoRecording> recording;
};

MenuSettings mission_settings(const Installation& installation, const FlightMission* selected) {
    return selected != nullptr && !selected->settings.empty() ? MenuSettings(selected->settings) : installation.load_menu_settings();
}

TerrainTables mission_terrain_tables(const Installation& installation, const MenuSettings& settings,
                                    const FlightMission* mission) {
    auto tables = installation.load_terrain_tables(static_cast<TerrainDetail>(settings.detail()));
    if (mission && mission->depth_padding) { apply_depth_padding(tables, *mission->depth_padding); }
    return tables;
}

TrainingResources load_training(const Installation& installation, const FlightMission* selected,
                                const std::shared_ptr<MissionAudio>& audio, IrqState* loading_state) {
    const auto frontend = installation.load_frontend();
    auto resources = frontend.mission_resources;
    const auto missions = installation.load_missions(0);
    const auto& mission = missions.missions.at(0);
    for (std::size_t index = 11; index < resources.size(); ++index) {
        resources[index][0] = '-';
    }
    std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
    auto setup = selected == nullptr ? parse_gameplay(mission, resources, frontend.gameplay_presets, {}) : selected->setup;
    if (selected != nullptr) {
        resources = selected->resources;
    }
    const auto settings = mission_settings(installation, selected);
    MissionIrqBinding loading_interrupt(loading_state ? audio : nullptr, [&] { loading_state->interrupt(audio->cpu_work()); });
    if (audio && !loading_state) { audio->stop_mission_samples(); }
    auto payloads = audio && loading_state ?
        load_mission_payloads(installation, setup, resources, frontend.transfer, static_cast<TerrainDetail>(settings.detail()), *audio) :
        load_gameplay(setup, resources, [&](const auto& name, auto kind) { return installation.load_payload(name, kind); });
    auto assets = materialize_scene(setup, payloads, frontend.transfer);
    auto cockpit = materialize_cockpit(payloads.back(), installation.load_payload("CONSOL5S.DTA", PayloadKind::picture),
                                       assets, frontend.transfer);
    auto cockpit_tables = installation.load_cockpit_tables();
    cockpit_tables.center_marker = settings.display(DisplayOption::artificial_horizon);
    cockpit_tables.flight_hud = settings.display(DisplayOption::indicators);
    cockpit_tables.heading_tape = settings.display(DisplayOption::compass);
    cockpit_tables.category_count = setup.category_count;
    return {std::move(setup), std::move(payloads), std::move(assets), std::move(cockpit), std::move(cockpit_tables),
            selected == nullptr ? nullptr : selected->recording};
}

MissionScene construct_loaded_scene(const Installation& installation, const TrainingResources& resources,
    const SceneAssets& assets, std::uint32_t seed, const std::shared_ptr<MissionAudio>& audio, IrqState* loading_state) {
    MissionIrqBinding loading_interrupt(loading_state ? audio : nullptr, [&] { loading_state->interrupt(audio->cpu_work()); });
    return construct_scene(resources.setup, assets, loading_state ? loading_state->random_seed : seed,
        installation.load_scene_entry(), loading_state, loading_state ? &audio->execution_clock() : nullptr);
}
}

SceneImage scene_image(const TerrainFrame& frame, const Palette& palette) {
    SceneImage result;
    result.palette = palette;
    for (std::size_t y = 0; y < scene_buffer_side; ++y) {
        for (std::size_t x = 0; x < scene_columns; ++x) {
            const auto column = 16 + (x >> 1) + ((x & 1) << 7);
            result.pixels[y * scene_columns + x] = frame.pixels[(scene_buffer_side - 1 - y) * scene_buffer_side + column];
        }
    }
    return result;
}

class TrainingFlight::Impl {
public:
    ~Impl() = default;
    Impl(const Installation& installation, std::uint32_t seed, std::shared_ptr<ReflectionState> reflections,
         std::shared_ptr<EffectsState> effects_state,
         const FlightMission* mission = nullptr, std::shared_ptr<MissionAudio> audio = {}, IrqState* loading_state = nullptr)
        : Impl(installation, seed, load_training(installation, mission, audio, loading_state),
               mission_terrain_tables(installation, mission_settings(installation, mission), mission),
               mission_settings(installation, mission), audio, std::move(reflections), std::move(effects_state), loading_state, mission) {}

    MissionResult result(const CpuWorkSink& work) const {
        if (!flight.state().ended) {
            throw std::logic_error("Debrief requires a completed flight");
        }
        return evaluate_mission(initial_setup, flight.state(), flight.scene(), work);
    }

    void advance(const FlightControls& controls, const CockpitControls& displays, const FlightInput& input) {
        if (flight.state().ended || demo_finished()) {
            return;
        }
        // Commit a complete supported tick, preserving the last frame on failure.
        auto candidate = flight;
        FlightIrqTarget interrupt_target(interrupt_target_, candidate);
        auto next_cockpit = cockpit;
        auto next_clock = irq_clock;
        const auto interrupts = external_clock ? 0 : next_clock.advance(training_samples_per_tick);
        for (std::uint64_t tick = 0; tick < interrupts; ++tick) {
            candidate.timer_interrupt();
        }
        std::vector<SoundCue> sounds;
        std::vector<CockpitImage> intermediate;
        auto next_reflections = *reflection_state;
        auto next_effects = effects;
        auto next_playback = playback;
        auto next_warnings = warned_systems;
        if (!recording) {
            const auto work = external_clock ? audio->cpu_work() : CpuWorkSink{};
            flight_work::loop_prefix(tick, candidate.state(), next_warnings,
                [&](std::uint8_t sound) { audio->play_cue({SampleBank::speech, sound}); }, work,
                [&] { next_cockpit.update_pending(candidate.state(), work); });
            next_cockpit.prepare_frame(work);
            if (work) { work(CpuOperation::instructions, 5); } // CALL/RET 19512, then CALL b8e0.
        }
        const auto keyboard = external_clock && bool(input.take_key) && !recording;
        const auto active_displays = keyboard ? next_cockpit.state().controls : displays;
        auto frame_controls = controls;
        const auto weapon_scan = controls.weapon == WeaponSelection::previous ? previous_weapon_scan :
            controls.weapon == WeaponSelection::next ? next_weapon_scan : 0;
        if (!recording && weapon_scan != 0) {
            // The DOS main loop dispatches bracket keys after its controller call.
            frame_controls.weapon = WeaponSelection::unchanged;
        }
        auto frame = draw_tick(candidate, next_cockpit, frame_controls, active_displays, tick + 1, sounds, intermediate, next_reflections, next_effects, next_playback);
        const auto apply_key = [&](std::uint8_t scan) {
            const auto action = candidate.recorded_key(scan, assets);
            if (action == RecordedAction::stores) {
                next_cockpit.refresh_stores(candidate.state());
            } else if (action == RecordedAction::view) {
                const auto selected = next_cockpit.state().controls;
                draw_tick(candidate, next_cockpit, {}, selected, tick + 1, sounds, intermediate, next_reflections, next_effects, next_playback);
                next_cockpit.restore_console(candidate.state());
                intermediate.push_back(next_cockpit.present(candidate.state()));
                next_cockpit.copy_front_page();
                frame = draw_tick(candidate, next_cockpit, {}, selected, tick + 1, sounds, intermediate, next_reflections, next_effects, next_playback);
                next_cockpit.restore_console(candidate.state());
            }
        };
        if (recording || weapon_scan != 0) {
            const auto scan = recording ? next_playback.key : weapon_scan;
            next_playback.key = 0;
            apply_key(scan);
        }
        if (external_clock && !candidate.state().ended && !recording) {
            auto& clock = audio->execution_clock();
            const auto work = audio->cpu_work();
            const FlightKeyHandler command = [&](FlightKeyAction action, unsigned value) {
                auto notice = FlightNotice::none;
                switch (action) {
                case FlightKeyAction::weapon:
                    candidate.select_weapon(static_cast<std::uint8_t>(value));
                    next_cockpit.refresh_stores(candidate.state(), work);
                    break;
                case FlightKeyAction::left_panel: case FlightKeyAction::right_panel:
                    next_cockpit.select_display(action == FlightKeyAction::left_panel ? InstrumentPanel::left : InstrumentPanel::right,
                                                static_cast<InstrumentDisplay>(value), work);
                    break;
                case FlightKeyAction::chaff: case FlightKeyAction::flare:
                    if (candidate.deploy_countermeasure(action == FlightKeyAction::chaff ? DefenseAction::chaff : DefenseAction::flare, work)) {
                        next_cockpit.refresh_countermeasures(candidate.state(), work);
                    }
                    break;
                case FlightKeyAction::zoom:
                    next_cockpit.set_zoom(static_cast<MapZoom>(value));
                    break;
                case FlightKeyAction::view:
                    // Only these live views have recovered drawing children.
                    if (value == cockpit_view || value == chase_view || value == fixed_view) {
                        apply_key(static_cast<std::uint8_t>(value + 1));
                    } else {
                        notice = FlightNotice::unavailable_view;
                    }
                    break;
                case FlightKeyAction::settings:
                    if (input.settings) { input.settings(); }
                    break;
                case FlightKeyAction::calibrate:
                    notice = FlightNotice::unavailable_calibration;
                    break;
                case FlightKeyAction::input_flags:
                    notice = FlightNotice::unavailable_input_mode;
                    break;
                case FlightKeyAction::rockets:
                    notice = FlightNotice::unavailable_rockets;
                    break;
                }
                if (input.notice) { input.notice(notice); }
            };
            std::uint8_t scan = 0;
            if (input.take_key) {
                scan = input.take_key(clock);
            } else {
                clock.execute(CpuOperation::disable_interrupts, 1);
                clock.execute(CpuOperation::instructions, 2);
                clock.execute(CpuOperation::enable_interrupts, 1);
            }
            clock.execute(CpuOperation::instructions, 2);
            if (scan != 0) {
                if (input.notice) { input.notice(FlightNotice::none); }
                clock.execute(CpuOperation::instructions, 1);
                dispatch_flight_key(scan, {candidate.state().weapon, candidate.state().cockpit_mode,
                    static_cast<std::uint8_t>(next_cockpit.state().controls.left),
                    static_cast<std::uint8_t>(next_cockpit.state().controls.right), alternate_keys, input.alt_down}, command, clock);
            }
            clock.execute(CpuOperation::instructions, 1);
            dispatch_flight_view(no_view_request, candidate.state().cockpit_mode, command, clock);
        }
        auto next_image = scene_image(*frame, assets.terrain.palette);
        auto next_presentation = candidate.state().ended ? presentation : next_cockpit.present(candidate.state());
        audio->set_engine_power(candidate.state().engine_power);
        if (external_clock && !candidate.state().ended) {
            audio->execution_clock().execute(CpuOperation::instructions, 1);
            flight_work::present(audio->execution_clock());
            if (!recording) { audio->execution_clock().execute(CpuOperation::instructions, 1); } // 731b JMP.
        }
        interrupt_target_ = &flight;
        flight = std::move(candidate);
        cockpit = std::move(next_cockpit);
        irq_clock = next_clock;
        *reflection_state = next_reflections;
        effects = std::move(next_effects);
        *effects_state = effects.state();
        playback = next_playback;
        warned_systems = next_warnings;
        image = std::move(next_image);
        presentation = std::move(next_presentation);
        intermediate_presentations = std::move(intermediate);
        ++tick;
        for (const auto& sound : sounds) {
            audio->play_cue(sound);
        }
    }

    SceneAssets assets;
    FlightController flight;
    TerrainRenderer renderer;
    std::shared_ptr<MissionAudio> audio;
    bool external_clock;
private:
    FlightController* interrupt_target_ = &flight;
    MissionIrqBinding interrupt_binding_;
public:
    bool missile_camera = true;

    void apply_settings(const Installation& installation, const MenuSettings& settings, const FlightMission* mission) {
        auto next_renderer = TerrainRenderer(mission_terrain_tables(installation, settings, mission),
                                             0x03000000, settings.scenery_policy());
        flight.set_policy(settings.flight_policy());
        cockpit.apply_settings(settings);
        missile_camera = settings.display(DisplayOption::missile_camera);
        reflections_enabled = settings.scenery_policy().reflections;
        alternate_keys = settings.alternate_keys();
        renderer = std::move(next_renderer);
        audio->set_sound_level(settings.sound_level());
    }
    SceneImage image;
    CockpitImage presentation;
    std::vector<CockpitImage> intermediate_presentations;
    std::uint8_t terrain_variant;
    std::size_t demo_position() const { return playback.position; }
    bool demo_finished() const { return recording && playback.position == recording->inputs().size(); }

private:
    GameplaySetup initial_setup;
    Installation view_installation;
    Transfer view_transfer;
    IrqClock irq_clock;
    CockpitRenderer cockpit;
    WaterReflections water;
    std::shared_ptr<ReflectionState> reflection_state;
    ScriptedEffects effects;
    std::shared_ptr<EffectsState> effects_state;
    bool reflections_enabled = false, alternate_keys = false;
    std::uint32_t tick = 0;
    std::array<std::uint8_t, 6> warned_systems{};
    std::shared_ptr<const DemoRecording> recording;
    struct PlaybackState {
        std::size_t position = 0;
        std::uint8_t key = 0;
    } playback;

    unsigned load_crash_picture() {
        auto& clock = audio->execution_clock();
        const auto work = audio->cpu_work();
        clock.execute(CpuOperation::instructions, 5); // 1a473..1a488, including the loader CALL.
        const auto picture = view_installation.load_payload("CONSOL5S.DTA", PayloadKind::picture, *audio, 13);
        clock.execute(CpuOperation::instructions, 4); // JMP, view latches and span-builder CALL.
        loading_work::cockpit_spans(picture, work);
        clock.execute(CpuOperation::instructions, 1);
        loading_work::cockpit_remap(picture, assets.terrain.palette, view_transfer, work);
        clock.execute(CpuOperation::instructions, 1);
        draw_loading_picture(clock, picture.height);
        return picture.height;
    }

    void charge_camera(FlightController& controller, SceneView& view) {
        if (external_clock) {
            flight_work::CameraInputs camera;
            camera.exiting = controller.state().exit_requested != 0;
            camera.eye_height = controller.state().view.height;
            if (view.mode == SceneViewMode::chase) {
                const auto position = std::size_t(view.y >> 22) * terrain_side + (view.x >> 22);
                camera.ground_height = static_cast<std::uint8_t>(assets.terrain.height.at(position) << 1);
            } else if (view.mode == SceneViewMode::fixed) {
                camera.subject_dx = controller.scene().entities[player_slot].x - view.x;
                camera.subject_dy = controller.scene().entities[player_slot].y - view.y;
            }
            flight_work::camera(view, audio->execution_clock(), [&] {
                return static_cast<std::int32_t>(controller.state().shake_offset);
            }, camera);
        }
    }

    std::unique_ptr<TerrainFrame> draw_world(FlightController& controller, const SceneView& view,
        std::uint32_t frame_tick, std::vector<SoundCue>& sounds, ReflectionState& reflections,
        ScriptedEffects& next_effects, const CpuWorkSink& work) {
        // Construct directly on the heap: nested view transitions exceed small host stacks.
        if (work) { work(CpuOperation::instructions, 4); } // GS setup and sky callback in e3e0.
        std::unique_ptr<TerrainFrame> frame(new TerrainFrame(renderer.render(assets, view,
            external_clock ? audio->cpu_work() : CpuWorkSink{})));
        if (work) { work(CpuOperation::instructions, 2); } // b8f9 fog callback.
        renderer.shade_distance(*frame, assets, terrain_variant, work);
        if (work) { work(CpuOperation::instructions, 2); } // b900 background callback.
        controller.draw_effects(next_effects, *frame, view, frame_tick, EffectLayer::background,
                                external_clock ? &audio->execution_clock() : nullptr);
        const auto reflect = [&] {
            if (work) { work(CpuOperation::instructions, 2); } // Load the callback and call it.
            if (reflections_enabled) {
                if (terrain_variant != 0) {
                    water.draw(*frame, reflections, work);
                    return;
                }
                if (work) { work(CpuOperation::instructions, 2); } // 2784c tests the terrain flag.
            }
            if (work) { work(CpuOperation::instructions, 1); } // Callback RET.
        };
        if (work) { work(CpuOperation::instructions, 2 + unsigned(initial_setup.lights.empty())); }
        if (controller.state().cockpit_mode == 6) { reflect(); }
        if (work) { work(CpuOperation::instructions, 1); } // Entity update CALL.
        controller.update_entities(assets, external_clock ? &audio->execution_clock() : nullptr,
            external_clock ? SoundCueHandler([&](const SoundCue& cue) { audio->play_cue(cue); }) : SoundCueHandler{});
        if (!external_clock) { sounds.insert(sounds.end(), controller.sounds().begin(), controller.sounds().end()); }
        renderer.draw_models(*frame, assets, controller.scene(), view, work);
        controller.observe_projection(frame->targeting);
        if (work) { work(CpuOperation::instructions, 2 + unsigned(initial_setup.lights.empty())); }
        controller.draw_effects(next_effects, *frame, view, frame_tick, EffectLayer::foreground,
                                external_clock ? &audio->execution_clock() : nullptr);
        if (work) { work(CpuOperation::instructions, 2); }
        if (controller.state().cockpit_mode != 6) { reflect(); }
        if (work) { work(CpuOperation::instructions, 5); }
        return frame;
    }

    std::unique_ptr<InstrumentCamera> draw_instrument_camera(FlightController& controller,
        const TerrainFrame& frame, std::uint32_t frame_tick, ScriptedEffects& next_effects,
        const CpuWorkSink& work) {
        std::unique_ptr<InstrumentCamera> instruments;
        const auto camera_slot = missile_camera ? controller.scene().camera_subject : 0;
        const auto target_slot = controller.state().target.slot;
        if (camera_slot != 0 || target_slot != 0) {
            const auto& subject = controller.scene().entities.at(camera_slot != 0 ? camera_slot : target_slot);
            SceneView camera_view;
            camera_view.x = subject.x;
            camera_view.y = subject.y;
            camera_view.height = subject.height;
            camera_view.heading = std::uint32_t(subject.angle) << 24;
            if (camera_slot == 0) {
                camera_view = renderer.target_camera(subject, controller.state().target.projection.bearing);
            }
            instruments = std::make_unique<InstrumentCamera>();
            instruments->view = camera_view;
            instruments->frame = renderer.render_monitor(assets, camera_view, frame, work);
            instruments->frame.camera.pitch = camera_view.pitch;
            instruments->frame.camera.roll = camera_view.roll;
            if (work) { work(CpuOperation::instructions, 1); }
            renderer.draw_models(instruments->frame, assets, controller.scene(), camera_view, work);
            if (work) { work(CpuOperation::instructions, 2); } // 813c / 822b foreground callback.
            controller.draw_effects(next_effects, instruments->frame, camera_view, frame_tick, EffectLayer::foreground,
                                    external_clock ? &audio->execution_clock() : nullptr);
            if (work && initial_setup.lights.empty()) { work(CpuOperation::instructions, 1); }
            controller.observe_projection(instruments->frame.targeting);
        }
        return instruments;
    }

    std::unique_ptr<TerrainFrame> draw_tick(FlightController& controller, CockpitRenderer& compositor, const FlightControls& controls,
        const CockpitControls& displays, std::uint32_t frame_tick, std::vector<SoundCue>& sounds,
        std::vector<CockpitImage>& intermediate, ReflectionState& reflections, ScriptedEffects& next_effects,
        PlaybackState& next_playback) {
        FlightIrqTarget irq_target(interrupt_target_, controller);
        auto view = controller.render_view(assets);
        const auto work = external_clock ? audio->cpu_work() : CpuWorkSink{};
        if (work) { work(CpuOperation::instructions, 7); } // b8e0 wrapper and e3e0 camera CALL.
        charge_camera(controller, view);
        auto frame = draw_world(controller, view, frame_tick, sounds, reflections, next_effects, work);
        compositor.begin_frame(*frame, controller.state().cockpit_mode, displays, work);
        auto input = controls;
        if (recording) {
            if (next_playback.position == recording->inputs().size()) {
                return frame;
            }
            if (next_playback.key != 0) {
                throw std::invalid_argument("Recorded key interrupts a nested demo view transition");
            }
            const auto& record = recording->inputs().at(next_playback.position++);
            if (record.trigger > 1 || record.target > 1) {
                throw std::invalid_argument("Unsupported counted demo button event");
            }
            next_playback.key = record.key;
            input = {};
            input.source = ControlSource::recording;
            input.recorded_power = record.power;
            input.horizontal = record.horizontal;
            input.vertical = record.vertical;
            input.fire = record.trigger != 0;
            input.cycle_target = record.target != 0;
        }
        if (work) { work(CpuOperation::instructions, 1); } // B94a controller CALL.
        controller.step(input, assets, [&](FlightController& nested) {
            FlightIrqTarget view_interrupt(interrupt_target_, nested);
            const auto height = external_clock ? load_crash_picture() : 0;
            const auto prepare_camera = [&] {
                if (external_clock) {
                    auto& clock = audio->execution_clock();
                    clock.execute(CpuOperation::instructions, 5); // Save pose and CALL 8635.
                    auto camera = nested.render_view(assets);
                    charge_camera(nested, camera);
                    clock.execute(CpuOperation::instructions, 5); // Restore pose and CALL b8e0.
                }
            };
            prepare_camera();
            auto nested_controls = controls;
            nested_controls.weapon = WeaponSelection::unchanged;
            draw_tick(nested, compositor, nested_controls, displays, frame_tick, sounds, intermediate, reflections, next_effects, next_playback);
            if (external_clock) {
                audio->execution_clock().execute(CpuOperation::instructions, 2); // Mode test skips 7b04.
                compositor.prepare_frame(work);
                audio->execution_clock().execute(CpuOperation::instructions, 1);
            }
            intermediate.push_back(compositor.present(nested.state()));
            if (external_clock) {
                flight_work::present(audio->execution_clock());
                audio->execution_clock().execute(CpuOperation::instructions, 1);
                copy_loading_page(audio->execution_clock(), height);
            }
            compositor.copy_front_page();
            prepare_camera();
            *frame = std::move(*draw_tick(nested, compositor, nested_controls, displays, frame_tick, sounds, intermediate, reflections, next_effects, next_playback));
            if (external_clock) { audio->execution_clock().execute(CpuOperation::instructions, 3); } // Mode test and RET.
        }, &frame->targeting, compositor.state().nearby != 0 ? ThreatProximity::nearby : ThreatProximity::clear, work,
           external_clock ? &audio->execution_clock() : nullptr,
           external_clock ? SoundCueHandler([&](const SoundCue& cue) { audio->play_cue(cue); }) : SoundCueHandler{},
           {[&](const FlightState& player) { compositor.refresh_stores(player, work); },
            [&](const FlightState& player) { compositor.refresh_countermeasures(player, work); }});
        if (controller.state().ended) { return frame; }
        if (work) { work(CpuOperation::instructions, 1); } // B94f wind CALL.
        controller.apply_wind(next_effects, frame_tick, view.heading, external_clock ? &audio->execution_clock() : nullptr);
        if (!external_clock) { sounds.insert(sounds.end(), controller.sounds().begin(), controller.sounds().end()); }
        if (!controller.state().ended) {
            const auto& target = controller.state().target;
            if (work) {
                const auto target_work = controller.state().cockpit_mode > 5 ? 8u :
                    target.slot == 0 ? 13u : frame->targeting.entities.at(target.slot).x == 0 ? 20u : 23u;
                work(CpuOperation::instructions, target_work);
            }
            controller.refresh_target(frame->targeting);
            std::unique_ptr<InstrumentCamera> instruments;
            const InstrumentCameraProvider camera_provider = [&](InstrumentPanel) {
                auto next = draw_instrument_camera(controller, instruments ? instruments->frame : *frame, frame_tick, next_effects, work);
                instruments = std::move(next);
                return instruments.get();
            };
            const auto instrument_sounds = compositor.draw_instruments(*frame, view, controller.state(),
                controller.scene(), assets, frame_tick, displays, nullptr, work, camera_provider,
                external_clock ? SoundCueHandler([&](const SoundCue& cue) { audio->play_cue(cue); }) : SoundCueHandler{});
            if (!external_clock) { sounds.insert(sounds.end(), instrument_sounds.begin(), instrument_sounds.end()); }
        }
        return frame;
    }

    Impl(const Installation& installation, std::uint32_t seed, TrainingResources resources, TerrainTables tables,
         const MenuSettings& settings, std::shared_ptr<MissionAudio> carried_audio, std::shared_ptr<ReflectionState> reflections,
         std::shared_ptr<EffectsState> carried_effects, IrqState* loading_state, const FlightMission* mission)
        : assets(std::move(resources.assets)),
          flight(resources.setup, construct_loaded_scene(installation, resources, assets, seed, carried_audio, loading_state), tables, resources.payloads.size()),
          renderer(tables),
          audio(carried_audio ? carried_audio : std::make_shared<MissionAudio>(installation.load_audio(), resources.payloads,
                AudioEntry{}, std::vector<SampleRequest>{}, AudioTracePolicy::discard)),
          external_clock(carried_audio != nullptr),
          interrupt_binding_(external_clock ? audio : nullptr, [this] {
              interrupt_target_->timer_interrupt(audio->cpu_work());
          }),
          terrain_variant(resources.setup.terrain_variant),
          initial_setup(resources.setup),
          view_installation(installation), view_transfer(tables.transfer),
          cockpit(std::move(resources.cockpit), std::move(resources.cockpit_tables), flight.state(),
                  mission_cockpit_entry(flight.state(), mission)),
          water(assets.terrain.palette, tables.transfer), reflection_state(std::move(reflections)),
          effects(resources.setup, assets.terrain.palette, tables, *carried_effects), effects_state(std::move(carried_effects)),
          recording(std::move(resources.recording)) {
        if (external_clock) {
            if (loading_state) { audio->execution_clock().execute(CpuOperation::instructions, 22); }
            audio->begin_mission_loading();
            if (loading_state) {
                audio->execution_clock().execute(CpuOperation::instructions, 3);
                installation.load_payload("CONSOL1S.DTA", PayloadKind::picture, *audio, 13);
            } else {
                installation.load_picture_payload("CONSOL1S.DTA", audio->execution_clock());
            }
            if (loading_state) {
                audio->execution_clock().execute(CpuOperation::instructions, 2); // Cockpit mode and 782f CALL.
                loading_work::cockpit_spans(resources.payloads.back(), audio->cpu_work());
                audio->execution_clock().execute(CpuOperation::instructions, 1);
            }
            loading_work::cockpit_remap(resources.payloads.back(), assets.terrain.palette, tables.transfer, audio->cpu_work());
            if (loading_state) {
                audio->execution_clock().execute(CpuOperation::instructions, 1);
                draw_loading_picture(audio->execution_clock(), resources.payloads.back().height);
            }
        }
        apply_settings(installation, settings, mission);
        audio->set_music_level(settings.music_level(), MusicChange::loaded);
        audio->set_sound_level(settings.sound_level());
        std::vector<SoundCue> sounds;
        auto next_reflections = *reflection_state;
        if (recording) {
            flight.enter_recording(recording->pose());
        }
        if (external_clock) {
            cockpit.initialize_displays(flight.state(), audio->cpu_work());
            audio->execution_clock().execute(CpuOperation::instructions, 1 + 4);
            audio->execution_clock().execute(CpuOperation::repeat_store_dwords, 32);
            audio->execution_clock().execute(CpuOperation::instructions, 1 + 1);
            loading_work::scenery_palette(resources.setup, assets.terrain.palette, tables.transfer, audio->cpu_work());
            audio->execution_clock().execute(CpuOperation::instructions, 2 + 1);
        }
        auto frame = draw_tick(flight, cockpit, {}, {}, 0, sounds, intermediate_presentations, next_reflections, effects, playback);
        image = scene_image(*frame, assets.terrain.palette);
        presentation = cockpit.present(flight.state());
        if (external_clock) {
            audio->execution_clock().execute(CpuOperation::instructions, 1);
            flight_work::present(audio->execution_clock());
            audio->execution_clock().execute(CpuOperation::instructions, 1);
            apply_loading_palette(audio->execution_clock());
            auto startup = installation.load_startup_files();
            startup.loading->active_settings = settings.record();
            audio->execution_clock().execute(CpuOperation::instructions, 1);
            audio->prepare_mission_audio(resources.payloads, startup);
            audio->execution_clock().execute(CpuOperation::instructions, 1);
            audio->queue_initial_engine(flight.state().control_power);
            audio->execution_clock().execute(CpuOperation::instructions, 1);
            copy_loading_page(audio->execution_clock(), resources.payloads.back().height);
            audio->execution_clock().execute(CpuOperation::instructions, 2 + 3);
            audio->change_music({{}, resources.payloads[resources.payloads.size() - 5].bytes});
            audio->execution_clock().execute(CpuOperation::instructions, 3);
            audio->play_launch_radio();
            audio->execution_clock().execute(CpuOperation::instructions, 1);
        }
        cockpit.copy_front_page();
        audio->set_engine_power(flight.state().engine_power);
        *reflection_state = next_reflections;
        *effects_state = effects.state();
    }
};

TrainingFlight::TrainingFlight(Installation installation, std::uint32_t seed)
    : installation_(std::move(installation)), seed_(seed), impl_(std::make_unique<Impl>(installation_, seed_, reflection_state_, effects_state_)) {}
TrainingFlight::TrainingFlight(Installation installation, FlightMission mission, std::uint32_t seed)
    : installation_(std::move(installation)), seed_(seed), mission_(std::make_unique<FlightMission>(std::move(mission))),
      impl_(std::make_unique<Impl>(installation_, seed_, reflection_state_, effects_state_, mission_.get())) {}
TrainingFlight::TrainingFlight(Installation installation, FlightMission mission, std::shared_ptr<MissionAudio> audio,
                               std::uint32_t seed, std::shared_ptr<ReflectionState> reflections, std::shared_ptr<EffectsState> effects,
                               IrqState* loading_state)
    : installation_(std::move(installation)), seed_(seed), loading_state_(loading_state), mission_(std::make_unique<FlightMission>(std::move(mission))),
      reflection_state_(reflections ? std::move(reflections) : std::make_shared<ReflectionState>()),
      effects_state_(effects ? std::move(effects) : std::make_shared<EffectsState>()),
      impl_(std::make_unique<Impl>(installation_, seed_, reflection_state_, effects_state_, mission_.get(), std::move(audio), loading_state_)) {}
TrainingFlight::~TrainingFlight() = default;
const FlightState& TrainingFlight::state() const { return impl_->flight.state(); }
IrqState TrainingFlight::interrupt_state() const {
    const auto& state = impl_->flight.state();
    return {impl_->flight.scene().random_seed, state.shake_ticks, state.shake_offset};
}
const SceneImage& TrainingFlight::image() const { return impl_->image; }
const CockpitImage& TrainingFlight::cockpit_image() const { return impl_->presentation; }
const std::vector<CockpitImage>& TrainingFlight::intermediate_images() const { return impl_->intermediate_presentations; }
void TrainingFlight::advance(const FlightControls& controls, const CockpitControls& cockpit, const FlightInput& input) { impl_->advance(controls, cockpit, input); }
std::vector<std::int16_t> TrainingFlight::render_audio(std::size_t samples) {
    return impl_->audio->render(samples, [&] {
        if (impl_->external_clock) {
            impl_->flight.timer_interrupt(impl_->audio->cpu_work());
        }
    });
}

FrontendPicture TrainingFlight::begin_settings() {
    impl_->flight.begin_settings();
    FrontendPicture result;
    result.palette = impl_->presentation.palette;
    for (auto& plane : result.background) {
        plane.assign(background_bytes, 0);
    }
    for (std::size_t y = 0; y < cockpit_height; ++y) {
        for (std::size_t x = 0; x < cockpit_width; ++x) {
            result.background[x % plane_count][1 + y * line_stride + x / plane_count] =
                impl_->presentation.pixels[y * cockpit_width + x];
        }
    }
    return result;
}
void TrainingFlight::apply_settings(const MenuSettings& settings) { impl_->apply_settings(installation_, settings, mission_.get()); }
void TrainingFlight::reset() {
    if (loading_state_) { *loading_state_ = interrupt_state(); }
    impl_ = std::make_unique<Impl>(installation_, seed_, reflection_state_, effects_state_, mission_.get(),
                                  impl_->external_clock ? impl_->audio : nullptr, loading_state_);
}
MissionResult TrainingFlight::result(const CpuWorkSink& work) const { return impl_->result(work); }
void TrainingFlight::abort() { impl_->flight.abort(); }
std::uint8_t TrainingFlight::objectives_remaining() const { return impl_->flight.scene().remaining_targets; }
std::array<ObjectiveState, player_slot - 1> TrainingFlight::objectives() const {
    std::array<ObjectiveState, player_slot - 1> result{};
    const auto& scene = impl_->flight.scene();
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto& entity = scene.entities[index + 1];
        if (entity.target != 0) {
            result[index] = entity.active ? ObjectiveState::active : ObjectiveState::destroyed;
        }
    }
    return result;
}
std::size_t TrainingFlight::demo_position() const { return impl_->demo_position(); }
bool TrainingFlight::demo_finished() const { return impl_->demo_finished(); }
} // namespace comanchecd
