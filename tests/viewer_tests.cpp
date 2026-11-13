#include "flight_host.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

class TestHost final : public comanchecd::host::FlightHost {
public:
    enum class Scenario { pause_reset, unsupported, weapon_selection };
    explicit TestHost(Scenario scenario) : scenario_(scenario) {}

    void set_input_context(comanchecd::host::InputContext context) override {
        require(context == comanchecd::host::InputContext::training, "The standalone viewer must select training input");
    }
    void reset_controls() override { ++resets_; }

    comanchecd::host::HostInput poll() override {
        using comanchecd::host::PauseAction;
        comanchecd::host::HostInput input;
        ++polls_;
        if (scenario_ == Scenario::weapon_selection) {
            if (polls_ == 2) {
                input.controls.weapon = comanchecd::WeaponSelection::previous;
            }
            return input;
        }
        if (scenario_ == Scenario::unsupported) {
            input.controls.horizontal = 351;
            input.quit = polls_ > 1;
            return input;
        }
        if (polls_ == 2 || polls_ == 4) {
            input.pause = PauseAction::toggle;
        }
        input.reset = polls_ == 5;
        input.quit = polls_ == 6;
        return input;
    }
    void present(const comanchecd::CockpitImage&, const comanchecd::FlightState& state, const std::string& status) override {
        weapon_ = state.weapon;
        paused_seen_ = paused_seen_ || status.find("Paused") != std::string::npos;
        fault_seen_ = fault_seen_ || status.find("Unsupported") != std::string::npos;
    }
    void queue_audio(const std::vector<std::int16_t>& samples) override {
        require(playback_ == comanchecd::host::Playback::running && samples.size() == comanchecd::training_samples_per_tick,
                "The viewer must queue one running audio block per completed tick");
        ++audio_blocks_;
        queued_ += samples.size();
    }
    std::size_t queued_audio_samples() const override { return queued_; }
    void clear_audio() override { queued_ = 0; }
    void set_playback(comanchecd::host::Playback playback) override { playback_ = playback; }
    void idle() override {
        if (scenario_ == Scenario::weapon_selection) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void verify() const {
        require(queued_ == 0 && playback_ == comanchecd::host::Playback::paused,
                "Viewer exit must stop and clear the audio device");
        if (scenario_ == Scenario::unsupported) {
            require(fault_seen_ && audio_blocks_ == 0, "Unsupported simulation must display the fault and pause audio");
            return;
        }
        if (scenario_ == Scenario::weapon_selection) {
            require(weapon_ == 1 && audio_blocks_ == 3,
                    "A weapon command between ticks must survive polling and be consumed exactly once");
            return;
        }
        require(paused_seen_ && audio_blocks_ == 2 && resets_ == 1,
                "Pause must suspend ticks and reset must clear host controls before resuming");
    }

private:
    Scenario scenario_;
    unsigned polls_ = 0, audio_blocks_ = 0, resets_ = 0;
    std::uint8_t weapon_ = 2;
    std::size_t queued_ = 0;
    comanchecd::host::Playback playback_ = comanchecd::host::Playback::paused;
    bool paused_seen_ = false, fault_seen_ = false;
};

class SessionPauseHost final : public comanchecd::host::FlightHost {
public:
    enum class Context { menu, pilot_editor };
    explicit SessionPauseHost(Context context = Context::menu) : context_(context) {}
    comanchecd::host::HostInput poll() override {
        comanchecd::host::HostInput input;
        ++polls_;
        input.quit = polls_ == 4;
        input.pause = polls_ == 3 ? comanchecd::host::PauseAction::pause : comanchecd::host::PauseAction::toggle;
        return input;
    }
    void present(const comanchecd::CockpitImage&, const comanchecd::FlightState&, const std::string& status) override {
        const bool expected_pause = polls_ == 3 || (context_ == Context::menu && polls_ == 1);
        require((status == "Paused") == expected_pause,
                "Session pause must toggle once in menus and settings, and focus loss must retain pause");
        require((playback_ == comanchecd::host::Playback::paused) == expected_pause,
                "Session pause status and audio playback must agree");
        ++presentations_;
    }
    void queue_audio(const std::vector<std::int16_t>&) override {}
    std::size_t queued_audio_samples() const override { return 0; }
    void clear_audio() override {}
    void set_playback(comanchecd::host::Playback playback) override { playback_ = playback; }
    void idle() override {}
    void verify() const {
        require(presentations_ == 3 && playback_ == comanchecd::host::Playback::paused,
                "The session pause route must complete and stop audio on exit");
    }
private:
    Context context_;
    unsigned polls_ = 0, presentations_ = 0;
    comanchecd::host::Playback playback_ = comanchecd::host::Playback::paused;
};

class SettingsReturnHost final : public comanchecd::host::FlightHost {
public:
    explicit SettingsReturnHost(comanchecd::GameSession& session) : session_(session) {}
    comanchecd::host::HostInput poll() override {
        comanchecd::host::HostInput input;
        require(++polls_ < 1000, "Raw Escape must close settings through the session IRQ/input loop");
        if (polls_ == 1) {
            input.scan_bytes = {0x01, 0x81};
        }
        input.quit = session_.menu_state().mode != comanchecd::FrontendMode::settings;
        return input;
    }
    void present(const comanchecd::CockpitImage&, const comanchecd::FlightState&, const std::string&) override {
        if (session_.menu_state().mode != comanchecd::FrontendMode::settings) {
            require(resets_ == 1, "Settings return must clear held host controls after IRQ-delivered input");
        }
    }
    void queue_audio(const std::vector<std::int16_t>&) override {}
    std::size_t queued_audio_samples() const override { return 0; }
    void clear_audio() override {}
    void set_playback(comanchecd::host::Playback) override {}
    void reset_controls() override { ++resets_; }
    void idle() override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
private:
    comanchecd::GameSession& session_;
    unsigned polls_ = 0, resets_ = 0;
};

class SessionAudioHost final : public comanchecd::host::FlightHost {
public:
    explicit SessionAudioHost(comanchecd::GameSession& session) : session_(session) {}
    comanchecd::host::HostInput poll() override {
        comanchecd::host::HostInput input;
        input.quit = blocks_ == 20;
        return input;
    }
    void present(const comanchecd::CockpitImage&, const comanchecd::FlightState&, const std::string&) override {}
    void queue_audio(const std::vector<std::int16_t>& samples) override {
        const auto frame = session_.presentation();
        const auto generated = frame->cpu_cycles * comanchecd::audio_sample_rate /
            comanchecd::ExecutionClock::cycles_per_second;
        require(generated <= frame->audio_samples + comanchecd::audio_sample_rate / 100,
                "Live flight output must drain audio generated by loading and each flight frame");
        queued_ += samples.size();
        ++blocks_;
    }
    std::size_t queued_audio_samples() const override { return 0; }
    void clear_audio() override {}
    void set_playback(comanchecd::host::Playback) override {}
    void idle() override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    void verify() const {
        const auto elapsed = std::chrono::steady_clock::now() - started_;
        const auto scheduled = std::chrono::duration<double>(double(queued_) / comanchecd::audio_sample_rate);
        require(blocks_ == 20 && elapsed + std::chrono::milliseconds(75) >= scheduled,
                "Live flight pacing must account for every queued sample");
    }
private:
    comanchecd::GameSession& session_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::size_t queued_ = 0;
    unsigned blocks_ = 0;
};
}

int main(int argc, char** argv) {
    try {
        using comanchecd::host::InputContext;
        comanchecd::FrontendState context{};
        context.mode = comanchecd::FrontendMode::pilot;
        context.pilot_mode = comanchecd::PilotMode::editor;
        require(comanchecd::host::input_context(context, false) == InputContext::name_entry,
                "Pilot editing must select text input independently of presentation cadence");
        context.mode = comanchecd::FrontendMode::settings;
        require(comanchecd::host::input_context(context, true) == InputContext::menu,
                "An active mission's settings must use menu input");
        context.mode = comanchecd::FrontendMode::request;
        require(comanchecd::host::input_context(context, true) == InputContext::flight,
                "A flight must select flight input outside settings");
        require(argc == 3, "Usage: viewer_tests INSTALLATION PAYLOAD_DIRECTORY");
        comanchecd::GameSession session(comanchecd::Installation(argv[1], argv[2]), [](const auto&) {});
        comanchecd::GameSession live(comanchecd::Installation(argv[1], argv[2]), [](const auto&) {});
        live.consume({0x1c});
        if (live.menu_state().pilot_mode == comanchecd::PilotMode::editor) {
            live.consume({0x1e});
            live.consume({0x1c});
            live.consume({0x1c});
        }
        for (unsigned screen = 0; screen < 4; ++screen) {
            while (live.menu_state().selection != 0) { live.consume({0x48}); }
            live.consume({0x1c});
        }
        require(live.flying(), "Audio latency regression must reach a constructed mission");
        SessionAudioHost live_audio(live);
        comanchecd::host::run_session(live, live_audio);
        live_audio.verify();
        SessionPauseHost menu_pause;
        comanchecd::host::run_session(session, menu_pause);
        menu_pause.verify();
        session.consume({0x01});
        require(session.menu_state().mode == comanchecd::FrontendMode::settings, "The pause regression must enter settings");
        SessionPauseHost settings_pause;
        comanchecd::host::run_session(session, settings_pause);
        settings_pause.verify();
        SettingsReturnHost settings_return(session);
        comanchecd::host::run_session(session, settings_return);
        comanchecd::PilotStorage pilot_storage(comanchecd::Installation(argv[1], argv[2]).load_frontend().pilot_record);
        pilot_storage.select(0);
        auto pilots = pilot_storage.record();
        constexpr std::size_t pilot_name_offset = 9;
        pilots[pilot_name_offset] = ' ';
        comanchecd::GameSession editor(comanchecd::Installation(argv[1], argv[2]), [](const auto&) {}, pilots);
        editor.consume({0x1c});
        require(editor.menu_state().pilot_mode == comanchecd::PilotMode::editor, "The text-input regression must enter name editing");
        SessionPauseHost text_pause(SessionPauseHost::Context::pilot_editor);
        comanchecd::host::run_session(editor, text_pause);
        text_pause.verify();
        comanchecd::TrainingFlight flight(comanchecd::Installation(argv[1], argv[2]));
        TestHost controls(TestHost::Scenario::pause_reset);
        require(comanchecd::host::run_viewer(flight, controls) == 2, "Pause and reset must produce the declared tick count");
        controls.verify();
        comanchecd::FlightControls launch;
        launch.throttle = 15;
        for (unsigned tick = 0; tick < 10; ++tick) {
            flight.advance(launch);
        }
        launch.fire = true;
        flight.advance(launch);
        launch.fire = false;
        for (unsigned tick = 0; tick < 180; ++tick) {
            flight.advance(launch);
        }
        TestHost fault(TestHost::Scenario::unsupported);
        require(comanchecd::host::run_viewer(flight, fault) == 0, "Unsupported simulation must stop before completing another tick");
        fault.verify();
        flight.reset();
        TestHost buffered(TestHost::Scenario::weapon_selection);
        require(comanchecd::host::run_viewer(flight, buffered, 3) == 3, "Weapon input route must complete three ticks");
        buffered.verify();
        std::cout << "Viewer pause/reset, unsupported-path stop and audio lifecycle passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
