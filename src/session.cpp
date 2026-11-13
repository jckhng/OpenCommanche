#include "comanchecd/session.hpp"
#include "menu_input.hpp"
#include "menu_work.hpp"
#include "startup_storage.hpp"

#include <algorithm>

namespace comanchecd {
namespace {
constexpr std::size_t title_row = 24, title_hold_frames = 40, hangar_loops = 2;
constexpr std::uint64_t fanfare_wait_irqs = 301, title_wait_irqs = 11;

CockpitImage demo_background(const FrontendPicture& picture) {
    CockpitImage image;
    image.palette = picture.palette;
    for (std::size_t y = 0; y < cockpit_height; ++y) {
        for (std::size_t x = 0; x < cockpit_width; ++x) {
            image.pixels[y * cockpit_width + x] = picture.background[x % plane_count].at(1 + y * line_stride + x / plane_count);
        }
    }
    return image;
}
}
GameSession::GameSession(Installation installation, PilotWriter writer, std::vector<std::uint8_t> pilots,
                         std::vector<std::uint8_t> settings, SettingsWriter settings_writer, SessionStart start)
    : installation_(std::move(installation)),
      settings_(settings.empty() ? installation_.load_menu_settings() : MenuSettings(std::move(settings))),
      settings_writer_(std::move(settings_writer)), pilot_writer_(std::move(writer)), pilot_bytes_(std::move(pilots)),
      startup_(start == SessionStart::cold) {
    if (startup_) {
        cinematics_ = std::make_unique<DemoCinematics>(installation_.load_demo_cinematics());
        start_menu_audio({cinematics_->fanfare, {}});
        const auto fanfare = installation_.load_fanfare();
        audio_->start_fanfare(fanfare);
        demo_index_ = 1;
        demo_phase_ = DemoPhase::fanfare;
        menu_image_ = demo_background(cinematics_->backdrop);
        const auto startup_movie = installation_.load_startup_movie();
        auto padding = startup_depth_padding(fanfare.score, startup_movie);
        demo_wait_ = audio_->load_startup_movie(installation_.load_startup_files(), startup_movie) + fanfare_wait_irqs;
        depth_padding_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(padding));
        return;
    }
    start_menu_audio(installation_.load_menu_music());
    start_frontend();
}

void GameSession::start_frontend() {
    auto assets = installation_.load_frontend();
    if (!pilot_bytes_.empty()) {
        assets.pilot_record = PilotStorage(std::move(pilot_bytes_)).record();
    }
    frontend_ = std::make_unique<Frontend>(std::move(assets), [&](FrontendScene scene) {
        auto picture = scene == FrontendScene::debrief || scene == FrontendScene::debrief_success ?
            installation_.load_debrief(frontend_->mission_presentation().resources, scene, *audio_) :
            scene == FrontendScene::campaign ? installation_.load_campaign_picture(*audio_) : installation_.load_picture(scene);
        menu_image_.palette = picture.palette;
        return picture;
    }, std::move(pilot_writer_), [&](const FrontendState&, const PilotController&, const MenuRenderer& renderer) {
        menu_back_page_ = renderer.back_page();
        for (std::size_t y = 0; y < cockpit_height; ++y) {
            for (std::size_t x = 0; x < cockpit_width; ++x) {
                menu_image_.pixels[y * cockpit_width + x] = renderer.surface().read(x % plane_count,
                    renderer.back_page() + 1 + y * line_stride + x / plane_count);
            }
        }
    }, [&](std::uint8_t campaign) { return installation_.load_missions(campaign, *audio_, CampaignLoad::catalog); }, FrontendPage::first,
       &audio_->execution_clock(), [this] { keyboard_.clear_pending(); ++menu_input_resets_; },
       [&](std::uint8_t campaign) { return installation_.load_missions(campaign, *audio_, CampaignLoad::briefing); });
    frontend_->start();
}

void GameSession::start_menu_audio(MusicPlaylist playlist) {
    if (audio_) {
        audio_->leave_mission(std::move(playlist));
    } else {
        AudioEntry entry;
        entry.execution = AudioExecution::scheduled;
        if (startup_) {
            entry.startup = AudioStartup::cold;
            entry.pit_start = PitStart::inherited_square_wave;
            entry.calibration = AudioCalibration::pit2;
            entry.music_start = MusicStart::deferred;
            entry.startup_files = installation_.load_startup_files();
            entry.startup_state = &session_irq_;
            entry.before_music_device = [this](ExecutionClock& clock) {
                clock.execute(CpuOperation::instructions, 3); // PIT returns and keyboard installer CALL.
                keyboard_.initialize(clock);
            };
        }
        entry.interrupt = [this](const CpuWorkSink& work) { session_irq_.interrupt(work); };
        audio_ = std::make_shared<MissionAudio>(installation_.load_audio(), std::move(playlist),
                                               entry, AudioTracePolicy::discard);
        audio_->set_keyboard_handler([this](ExecutionClock& clock, KeyboardPort& port) {
            keyboard_.interrupt(clock, port);
        });
    }
    audio_->set_music_level(settings_.music_level(), MusicChange::loaded);
}

void GameSession::release_flight() {
    if (!flight_) { return; }
    session_irq_ = flight_->interrupt_state();
    flight_.reset();
    flight_notice_ = FlightNotice::none;
}

void GameSession::queue_key_byte(std::uint8_t scan) {
    hardware_input_ = true;
    audio_->queue_key_byte(scan);
}

void GameSession::schedule_key_byte(std::uint64_t cycle, std::uint8_t scan) {
    audio_->schedule_key_byte(cycle, scan);
    hardware_input_ = true;
}

void GameSession::finish_startup() {
    release_flight();
    demo_player_.reset();
    cinematics_.reset();
    demo_phase_ = DemoPhase::none;
    startup_ = startup_skip_ = false;
    demo_wait_ = 0;
    demo_clear_sample_ = 0;
    audio_->enter_pilot_menu(installation_.load_pilot_menu());
    audio_->set_music_level(settings_.music_level(), MusicChange::loaded);
    start_frontend();
}

void GameSession::begin_fanfare_movie() {
    auto& clock = audio_->execution_clock();
    clock.execute(CpuOperation::instructions, 2); // 5bd8 row and hold arguments.
    demo_wait_ = 0;
    demo_phase_ = DemoPhase::animation;
    demo_player_ = std::make_unique<KdvPlayer>(cinematics_->animation, clock);
    clock.execute(CpuOperation::instructions, 3);
    audio_->enter_cinematic(cinematics_->sound_bank, {{}, cinematics_->canyon_music},
                            CinematicLoopStart::after_page_clear);
    demo_clear_sample_ = demo_player_->clear_pages();
    audio_->start_cinematic_loop();
    clock.execute(CpuOperation::instructions, 1 + 4); // Cue index and 19512 input sample.
    demo_sound();
    clock.execute(CpuOperation::instructions, 6);
    clock.instruction([&] { startup_skip_ = startup_skip_ || keyboard_.pending_scan() != 0; });
    clock.execute(CpuOperation::instructions, 1);
    if (startup_ && startup_skip_) { finish_startup(); return; }
    demo_player_->prepare();
}

void GameSession::poll_input() {
    auto& clock = audio_->execution_clock();
    if (startup_ && demo_phase_ == DemoPhase::fanfare) {
        // The original CMP/JBE samples the delivered counter in the foreground.
        const auto elapsed = clock.sample_irq_ticks();
        clock.execute(CpuOperation::instructions, 1);
        if (elapsed >= demo_wait_) { begin_fanfare_movie(); }
        return;
    }
    if (startup_) {
        clock.execute(CpuOperation::instructions, 2);
        startup_skip_ = startup_skip_ || keyboard_.pending_scan() != 0;
        return;
    }
    const auto state = frontend_->state();
    if (flight_ && state.mode != FrontendMode::settings) {
        std::uint8_t scan = 0;
        clock.instruction([&] { scan = keyboard_.pending_scan(); });
        if (scan == 0x01) {
            clock.instruction([&] { keyboard_.clear_pending(); });
            consume({scan});
        }
        return;
    }
    const auto list = state.mode == FrontendMode::main || state.mode == FrontendMode::campaign ||
        state.mode == FrontendMode::mission || (state.mode == FrontendMode::pilot && state.pilot_mode == PilotMode::list);
    std::uint8_t scan = 0;
    if (list) { scan = poll_list_key(keyboard_, clock); }
    else {
        clock.instruction([&] { scan = keyboard_.pending_scan(); });
        if (scan != 0) { clock.instruction([&] { keyboard_.clear_pending(); }); }
    }
    if (scan == 0) { return; }
    const auto left = keyboard_.held(0x2a), right = keyboard_.held(0x36);
    const auto input_resets = menu_input_resets_;
    consume({scan, left ? (right ? PilotShift::both : PilotShift::left) : right ? PilotShift::right : PilotShift::none});
    const auto next = frontend_->state();
    // A make delivered after the screen's own reset belongs to that screen.
    if (menu_input_resets_ == input_resets && keyboard_.pending_scan() == scan &&
            (next.mode != state.mode || next.pilot_mode != state.pilot_mode)) {
        clock.instruction([&] { keyboard_.clear_pending(); });
    }
}

void GameSession::consume(PilotKey key) {
    if (key.scan != 0) { flight_notice_ = FlightNotice::none; }
    if (startup_) {
        if (key.scan != 0) { startup_skip_ = true; }
        return;
    }
    if (quit_) {
        return;
    }
    if (demo_playing()) {
        if (key.scan != 0) {
            release_flight();
            demo_player_.reset();
            cinematics_.reset();
            demo_phase_ = DemoPhase::none;
            demo_clear_sample_ = 0;
            intermediate_images_.clear();
            frontend_->consume(key);
            start_menu_audio(installation_.load_menu_music());
        }
        return;
    }
    if (flight_ && frontend_->state().mode != FrontendMode::settings) {
        if (key.scan == 0x01) {
            const auto background = flight_->begin_settings();
            menu_image_.palette = background.palette;
            frontend_->open_flight_settings(settings_, background);
            audio_->suspend_engine();
            audio_->set_music_level(MusicLevel::off);
        }
        return;
    }
    const auto previous_mode = frontend_->state().mode;
    const auto request = frontend_->consume(key);
    if (previous_mode == FrontendMode::overview && frontend_->state().mode != FrontendMode::overview) {
        overview_player_.reset();
        overview_.reset();
        intermediate_images_.clear();
    }
    if (previous_mode == FrontendMode::debrief && frontend_->state().mode == FrontendMode::main) {
        start_menu_audio(installation_.load_menu_music());
    }
    if (request == FrontendRequest::launch_mission) {
        FlightMission mission{frontend_->gameplay_setup(), frontend_->mission_presentation().resources, settings_.record()};
        mission.depth_padding = depth_padding_;
        mission.back_page = menu_back_page_;
        flight_ = std::make_unique<TrainingFlight>(installation_, std::move(mission), audio_, session_irq_.random_seed, reflections_, effects_, &session_irq_);
    } else if (request == FrontendRequest::overview) {
        auto movie = std::make_unique<KdvMovie>(installation_.load_overview());
        frontend_->open_requested_page();
        overview_ = std::move(movie);
        overview_player_ = std::make_unique<KdvPlayer>(*overview_, audio_->execution_clock());
        overview_player_->prepare();
    } else if (request == FrontendRequest::demo) {
        cinematics_ = std::make_unique<DemoCinematics>(installation_.load_demo_cinematics());
        frontend_->open_requested_page();
        demo_index_ = 0;
        start_demo_cycle();
    } else if (request == FrontendRequest::statistics || request == FrontendRequest::credits ||
               request == FrontendRequest::mission_locked) {
        frontend_->open_requested_page();
    } else if (request == FrontendRequest::quit_prompt) {
        frontend_->open_settings(settings_);
        audio_->set_music_level(MusicLevel::off);
    } else if (request == FrontendRequest::calibration_unavailable) {
        flight_notice_ = FlightNotice::unavailable_calibration;
    } else if (request == FrontendRequest::input_mode_unavailable) {
        flight_notice_ = FlightNotice::unavailable_input_mode;
    } else if (request == FrontendRequest::settings_changed || request == FrontendRequest::settings_save ||
               request == FrontendRequest::settings_closed || request == FrontendRequest::settings_abort) {
        constexpr std::array<std::size_t, 3> unsupported_fields{27, 41, 42};
        for (const auto field : unsupported_fields) {
            if (settings_.record()[field] != frontend_->settings().record()[field]) {
                throw std::invalid_argument("This display or joystick setting is not yet integrated into native flight");
            }
        }
        const auto previous_music = settings_.music_level();
        settings_ = frontend_->settings();
        if (flight_) {
            flight_->apply_settings(settings_);
        }
        audio_->set_sound_level(settings_.sound_level());
        if (request == FrontendRequest::settings_closed) {
            audio_->set_music_level(settings_.music_level(), MusicChange::loaded);
            if (flight_) {
                audio_->resume_engine(flight_->state().engine_power);
            }
        } else if (previous_music != settings_.music_level()) {
            audio_->set_music_level(settings_.music_level());
        }
        if (request == FrontendRequest::settings_save) {
            if (!settings_writer_) {
                throw std::logic_error("Settings persistence requires a profile writer");
            }
            settings_writer_(settings_.record());
        }
        if (request == FrontendRequest::settings_abort) {
            abort_mission();
        }
    } else if (request == FrontendRequest::exit) {
        quit_ = true;
    } else if (request != FrontendRequest::none) {
        throw std::invalid_argument("This main-menu action has not been recovered");
    }
}

void GameSession::advance(const FlightControls& controls, const CockpitControls& displays) {
    if (startup_ && startup_skip_ && flight_) { finish_startup(); return; }
    intermediate_images_.clear();
    if (!flight_ || (frontend_ && frontend_->state().mode == FrontendMode::settings)) {
        return;
    }
    bool settings_requested = false;
    FlightInput input;
    if (hardware_input_ && !demo_playing()) {
        input.take_key = [&](ExecutionClock& clock) { return keyboard_.take_scan(clock); };
        input.alt_down = [&] { return keyboard_.held(0x38); };
        input.settings = [&] { settings_requested = true; };
        input.notice = [&](FlightNotice notice) { flight_notice_ = notice; };
    }
    flight_->advance(demo_playing() ? FlightControls{} : controls, demo_playing() ? CockpitControls{} : displays, input);
    intermediate_images_ = flight_->intermediate_images();
    if (demo_playing()) {
        if (flight_->demo_finished() || flight_->state().ended) {
            release_flight();
            start_demo_cycle();
        }
        return;
    }
    if (settings_requested && !flight_->state().ended) { consume({0x01}); }
    if (flight_->state().ended) { finish_flight(); }
}

void GameSession::finish_flight() {
    std::vector<std::pair<CpuOperation, unsigned>> statistics;
    const auto result = flight_->result([&](CpuOperation operation, unsigned count) {
        statistics.emplace_back(operation, count);
    });
    inactive_state_ = flight_->state();
    release_flight();
    auto& clock = audio_->execution_clock();
    clock.execute(CpuOperation::instructions, 28); // 2bbc8 resumes the sequencer and resets the return pose.
    audio_->stop_mission_samples();
    clock.execute(CpuOperation::instructions, 1); // Result CALL after the DSP stop.
    for (const auto& operation : statistics) { clock.execute(operation.first, operation.second); }
    audio_->clear_mission_samples();
    clock.execute(CpuOperation::instructions, 1 + 3); // 23ad2 clears all entity activity bytes.
    clock.execute(CpuOperation::repeat_store_bytes, scene_slots - 1);
    clock.execute(CpuOperation::instructions, 3 + 1);
    audio_->stop_music();
    clock.execute(CpuOperation::instructions, 21); // Allocator rewind, pointer clears and mission/demo guards.
    frontend_->finish_mission(result, [&] { audio_->save_pilot_record(); });
}

std::vector<std::int16_t> GameSession::render_audio(std::size_t samples) {
    if (overview_ || (demo_playing() && !flight_)) {
        return render_presentation_audio(samples);
    }
    auto result = flight_ ? flight_->render_audio(samples) : hardware_input_ ?
        audio_->render_foreground(samples, [this] { poll_input(); }) : audio_->render(samples);
    if (hardware_input_ && flight_) { poll_input(); }
    elapsed_samples_ += samples;
    return result;
}
const CockpitImage& GameSession::image() const { return flight_ && (!frontend_ || frontend_->state().mode != FrontendMode::settings) ? flight_->cockpit_image() : menu_image_; }
std::shared_ptr<const SessionPresentation> GameSession::presentation() const {
    auto frame = std::make_shared<SessionPresentation>();
    frame->menu = menu_state();
    frame->flight = flight_state();
    frame->flight_notice = flight_notice_;
    frame->image = image();
    frame->cpu_cycles = cpu_cycles();
    frame->irq_ticks = irq_ticks();
    frame->audio_samples = elapsed_samples_;
    frame->random_seed = random_seed();
    frame->flying = flying();
    frame->demo_playing = demo_playing();
    frame->quit_requested = quit_requested();
    return frame;
}
std::vector<std::int16_t> GameSession::take_pending_audio() {
    auto result = audio_->take_pending_audio();
    elapsed_samples_ += result.size();
    return result;
}
const std::vector<CockpitImage>& GameSession::intermediate_images() const { return intermediate_images_; }
const FlightState& GameSession::flight_state() const { return flight_ ? flight_->state() : inactive_state_; }
FrontendState GameSession::menu_state() const {
    if (!frontend_) { FrontendState state{}; state.mode = FrontendMode::startup; return state; }
    return frontend_->state();
}
std::uint64_t GameSession::irq_ticks() const { return audio_->irq_ticks(); }
std::uint64_t GameSession::cpu_cycles() const { return audio_->execution_clock().cycles(); }
std::uint32_t GameSession::random_seed() const {
    return flight_ ? flight_->interrupt_state().random_seed : session_irq_.random_seed;
}
bool GameSession::flying() const { return flight_ != nullptr; }
bool GameSession::demo_playing() const { return demo_phase_ != DemoPhase::none; }
bool GameSession::quit_requested() const { return quit_; }
void GameSession::abort_mission() {
    if (demo_playing()) {
        consume({0x01});
        return;
    }
    intermediate_images_.clear();
    if (flight_) {
        flight_->abort();
        finish_flight();
    }
}

void GameSession::start_demo_cycle() {
    demo_index_ = (demo_index_ + 1) % 4;
    demo_cue_ = demo_loops_ = 0;
    demo_clear_sample_ = 0;
    demo_player_.reset();
    if ((demo_index_ & 1) != 0) {
        demo_phase_ = DemoPhase::fanfare;
        menu_image_ = demo_background(cinematics_->backdrop);
        audio_->leave_mission({cinematics_->fanfare, {}});
    } else {
        demo_phase_ = DemoPhase::hangar;
        menu_image_ = demo_background(cinematics_->hangar_backdrop);
        audio_->leave_mission({{}, cinematics_->hangar_music});
    }
    audio_->set_music_level(settings_.music_level(), MusicChange::loaded);
    audio_->set_sound_level(settings_.sound_level());
    if (demo_phase_ == DemoPhase::fanfare) {
        demo_wait_ = audio_->irq_ticks() + fanfare_wait_irqs;
    } else {
        demo_player_ = std::make_unique<KdvPlayer>(cinematics_->hangar, audio_->execution_clock());
        demo_player_->prepare();
    }
}

void GameSession::start_demo_flight() {
    const auto frontend = installation_.load_frontend();
    auto resources = frontend.mission_resources;
    for (std::size_t index = 11; index < resources.size(); ++index) {
        resources[index][0] = '-';
    }
    const auto mission = installation_.load_missions(0).missions.at(demo_index_);
    std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
    FlightMission selected{parse_gameplay(mission, resources, frontend.gameplay_presets, {}), resources,
                           settings_.record(), std::make_shared<DemoRecording>(installation_.load_demo(demo_index_))};
    selected.depth_padding = depth_padding_;
    flight_ = std::make_unique<TrainingFlight>(installation_, std::move(selected), audio_, session_irq_.random_seed, reflections_, effects_);
    demo_phase_ = DemoPhase::flight;
    demo_player_.reset();
}

void GameSession::demo_sound() {
    const auto sound = cinematics_->cues.at(demo_cue_++) & 15;
    if (sound != 0) {
        try {
            audio_->play_effect(sound);
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument("Demo cue " + std::to_string(demo_cue_ - 1) + ": " + error.what());
        }
    }
}

void GameSession::demo_movie_boundary() {
    if (startup_ && startup_skip_) { finish_startup(); return; }
    const auto hangar = demo_phase_ == DemoPhase::hangar;
    const auto& movie = hangar ? cinematics_->hangar :
        demo_phase_ == DemoPhase::animation ? cinematics_->animation : cinematics_->title;
    const auto& pixels = demo_player_->pixels();
    const auto row = hangar ? 0 : title_row;
    if (row + movie.height() > cockpit_height) {
        throw std::invalid_argument("Demo movie exceeds presentation surface");
    }
    std::copy(pixels.begin(), pixels.end(), menu_image_.pixels.begin() + row * cockpit_width);
    menu_image_.palette = movie.palette();
    intermediate_images_.push_back(menu_image_);
    if (!demo_player_->finished()) {
        if (!hangar) {
            demo_sound();
        }
        demo_player_->prepare();
        return;
    }
    if (hangar && ++demo_loops_ < hangar_loops) {
        demo_player_ = std::make_unique<KdvPlayer>(movie, audio_->execution_clock());
        demo_player_->prepare();
    } else if (demo_phase_ == DemoPhase::animation) {
        demo_phase_ = DemoPhase::title_wait;
        demo_wait_ = audio_->irq_ticks() + title_wait_irqs;
        demo_player_.reset();
    } else {
        start_demo_flight();
    }
}

std::vector<std::int16_t> GameSession::render_presentation_audio(std::size_t samples) {
    std::vector<std::int16_t> result;
    result.reserve(samples);
    intermediate_images_.clear();
    for (std::size_t index = 0; index < samples; ++index) {
        auto sample = flight_ ? flight_->render_audio(1) : (startup_ || hardware_input_) ?
            audio_->render_foreground(1, [this] { poll_input(); }) : audio_->render(1);
        result.insert(result.end(), sample.begin(), sample.end());
        ++elapsed_samples_;
        if (demo_clear_sample_ != 0 && elapsed_samples_ >= demo_clear_sample_) {
            menu_image_.pixels.fill(0);
            intermediate_images_.push_back(menu_image_);
            demo_clear_sample_ = 0;
        }
        if (flight_) {
            continue;
        }
        if (!overview_ && !demo_playing()) { continue; }
        if (overview_player_) {
            if (elapsed_samples_ >= overview_player_->ready_sample()) {
                const auto& pixels = overview_player_->pixels();
                std::copy(pixels.begin(), pixels.end(), menu_image_.pixels.begin());
                menu_image_.palette = overview_->palette();
                intermediate_images_.push_back(menu_image_);
                if (overview_player_->finished()) {
                    overview_player_ = std::make_unique<KdvPlayer>(*overview_, audio_->execution_clock());
                }
                overview_player_->prepare();
            }
            continue;
        }
        if (demo_player_) {
            if (elapsed_samples_ >= demo_player_->ready_sample()) {
                demo_movie_boundary();
            }
            continue;
        }
        if (audio_->irq_ticks() < demo_wait_) {
            continue;
        }
        demo_wait_ = 0;
        if (demo_phase_ == DemoPhase::fanfare) {
            begin_fanfare_movie();
            continue;
        } else {
            demo_phase_ = DemoPhase::title;
            demo_player_ = std::make_unique<KdvPlayer>(cinematics_->title, audio_->execution_clock(), title_hold_frames);
        }
        demo_sound();
        if (startup_ && startup_skip_) { finish_startup(); continue; }
        demo_player_->prepare();
    }
    return result;
}
} // namespace comanchecd
