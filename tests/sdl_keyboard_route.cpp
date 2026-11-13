#include "sdl_host.hpp"
#include "profile.hpp"
#include "settings_profile.hpp"
#include "training_route.hpp"
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace comanchecd;
using namespace comanchecd::host;

namespace {
enum class RouteMode { play, restart, abort };
void require(bool value, const std::string& message) {
    if (!value) { throw std::runtime_error(message); }
}

class KeyboardRoute final : public FlightHost {
public:
    KeyboardRoute(GameSession& session, const Installation& installation, const PilotProfile& profile,
                  const SettingsProfile& settings, const std::filesystem::path& output, RouteMode mode)
        : session_(session), route_(installation), profile_(profile), settings_(settings), log_(output / "events.jsonl"),
          states_(output / "states.jsonl"), mode_(mode), begin_(Clock::now()) {}

    void set_input_context(InputContext context) override { host_.set_input_context(context); }

    HostInput poll() override {
        require(Clock::now() - begin_ < std::chrono::minutes(6), "Keyboard route exceeded six minutes");
        const auto menu = session_.menu_state();
        if (menu.mode == FrontendMode::startup) {
            if (Clock::now() - last_menu_key_ > std::chrono::milliseconds(300)) {
                tap(SDL_SCANCODE_RETURN);
                last_menu_key_ = Clock::now();
            }
        } else if (menu.mode == FrontendMode::pilot) {
            pilot(menu);
        } else if (menu.mode == FrontendMode::main) {
            if (debrief_seen_ || mode_ == RouteMode::restart) {
                const auto record = profile_.read();
                const PilotStorage saved(record);
                require(saved.completed(saved.selected(), 0, 0) == (mode_ != RouteMode::abort),
                        "Completed or aborted result must persist correctly");
                SDL_Event event{};
                event.type = SDL_QUIT;
                require(SDL_PushEvent(&event) == 1, "Quit must enter the SDL queue");
                finished_ = true;
            } else {
                navigate(menu, 0);
            }
        } else if (menu.mode == FrontendMode::campaign || menu.mode == FrontendMode::mission) {
            navigate(menu, 0);
        } else if (menu.mode == FrontendMode::briefing) {
            navigate(menu, 0);
        } else if (menu.mode == FrontendMode::debrief) {
            require(mode_ != RouteMode::restart, "Restart must return to operations without another mission");
            const PilotStorage saved(profile_.read());
            require(saved.completed(saved.selected(), 0, 0) == (mode_ != RouteMode::abort),
                    "Keyboard mission must persist its natural completion or explicit abort");
            debrief_seen_ = true;
            navigate(menu, 0);
        }
        if (session_.flying() && !session_.demo_playing()) {
            if (check_context(menu)) {
                if (mode_ == RouteMode::abort) {
                    event(SDL_SCANCODE_LALT, true, KMOD_ALT);
                    tap(SDL_SCANCODE_A, KMOD_ALT);
                    event(SDL_SCANCODE_LALT, false);
                } else {
                    fly();
                }
            }
        }
        auto input = host_.poll();
        throttle_ = input.controls.throttle;
        last_input_ = input;
        return input;
    }

    void present(const CockpitImage& image, const FlightState& state, const std::string& status) override {
        host_.present(image, state, status);
        status_ = status;
    }
    void queue_audio(const std::vector<std::int16_t>& samples) override { host_.queue_audio(samples); }
    std::size_t queued_audio_samples() const override { return host_.queued_audio_samples(); }
    void clear_audio() override { host_.clear_audio(); }
    void set_playback(Playback playback) override { host_.set_playback(playback); }
    void idle() override { host_.idle(); }
    void reset_controls() override {
        host_.reset_controls();
        held_.fill(false);
        throttle_ = 0;
        ++control_resets_;
    }
    void verify() const {
        require(finished_ && (mode_ == RouteMode::restart || debrief_seen_), "Keyboard route must finish debrief and return to operations");
        require(mode_ == RouteMode::restart || context_ == ContextCheck::complete,
                "Keyboard route must finish its pause, focus and settings checks");
        require(mode_ != RouteMode::restart || reloaded_, "Restart must observe the saved pilot selection");
    }
    unsigned observations() const { return flight_ticks_; }

private:
    using Clock = std::chrono::steady_clock;
    GameSession& session_;
    training_test::Route route_;
    const PilotProfile& profile_;
    const SettingsProfile& settings_;
    SdlFlightHost host_;
    std::ofstream log_, states_;
    RouteMode mode_;
    bool finished_ = false, debrief_seen_ = false, edited_ = false, reloaded_ = false;
    bool awaiting_menu_ = false;
    FrontendMode previous_menu_ = FrontendMode::startup;
    std::uint8_t previous_selection_ = 0;
    PilotMode previous_pilot_mode_ = PilotMode::list;
    unsigned name_index_ = 0, flight_ticks_ = 0, throttle_ = 0;
    unsigned pause_polls_ = 0, control_resets_ = 0, settings_entry_resets_ = 0;
    std::size_t unavailable_command_ = 0;
    constexpr static std::array<SDL_Scancode, 6> unavailable_commands_{
        SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_J, SDL_SCANCODE_M};
    HostInput last_input_;
    std::string status_;
    enum class ContextCheck { setup, r_press, r_verify, pause, paused, focus, focused_pause,
        settings_enter, settings_wait, settings_keys, settings_verify, settings_paused,
        settings_resumed, save_select, save_wait, settings_leave, commands, command_wait, command_clear, complete };
    ContextCheck context_ = ContextCheck::setup;
    std::uint64_t paused_cycle_ = 0;
    std::int32_t steering_carry_ = 0;
    std::uint64_t last_flight_cycle_ = 0;
    std::array<bool, SDL_NUM_SCANCODES> held_{};
    Clock::time_point begin_, last_menu_key_{};

    void event(SDL_Scancode code, bool down, Uint16 modifiers = KMOD_NONE) {
        SDL_Event event{};
        event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.keysym.scancode = code;
        event.key.keysym.mod = modifiers;
        require(SDL_PushEvent(&event) == 1, "Keyboard event must enter SDL");
        log_ << "{\"cycle\":" << session_.cpu_cycles() << ",\"scancode\":" << int(code)
             << ",\"down\":" << down << ",\"modifiers\":" << int(modifiers) << "}\n";
    }
    void tap(SDL_Scancode code, Uint16 modifiers = KMOD_NONE) {
        event(code, true, modifiers);
        event(code, false, modifiers);
    }
    void hold(SDL_Scancode code, bool down) {
        if (held_[code] == down) { return; }
        event(code, down);
        held_[code] = down;
    }
    bool changed(const FrontendState& menu) {
        const bool result = menu.mode != previous_menu_ || menu.selection != previous_selection_ ||
            menu.pilot_mode != previous_pilot_mode_;
        if (result) {
            awaiting_menu_ = false;
            previous_menu_ = menu.mode;
            previous_selection_ = menu.selection;
            previous_pilot_mode_ = menu.pilot_mode;
        }
        return result;
    }
    void navigate(const FrontendState& menu, unsigned row) {
        changed(menu);
        if (awaiting_menu_) { return; }
        tap(menu.mode == FrontendMode::debrief ? SDL_SCANCODE_ESCAPE :
            menu.selection == row ? SDL_SCANCODE_RETURN : SDL_SCANCODE_UP);
        awaiting_menu_ = true;
    }
    void pilot(const FrontendState& menu) {
        changed(menu);
        if (mode_ == RouteMode::restart) {
            const PilotStorage saved(profile_.read());
            const auto name = saved.name(saved.selected());
            require(std::string(name.begin(), name.begin() + 4) == "RPED", "Restart must load the keyboard-entered pilot name");
            require(menu.selected_pilot == saved.selected(), "Restart must restore the saved pilot identity");
            reloaded_ = true;
            if (!awaiting_menu_) { tap(SDL_SCANCODE_RETURN); awaiting_menu_ = true; }
            return;
        }
        if (!edited_ && menu.pilot_mode == PilotMode::list) {
            tap(SDL_SCANCODE_DELETE);
            edited_ = true;
            awaiting_menu_ = true;
            return;
        }
        if (menu.pilot_mode == PilotMode::editor) {
            if (menu.editor_length != name_index_) { return; }
            constexpr std::array<SDL_Scancode, 4> name{SDL_SCANCODE_R, SDL_SCANCODE_P, SDL_SCANCODE_E, SDL_SCANCODE_D};
            if (name_index_ < name.size()) {
                if (name_index_ == 0) { event(SDL_SCANCODE_LSHIFT, true, KMOD_SHIFT); }
                tap(name[name_index_++], KMOD_SHIFT);
            } else if (!awaiting_menu_) {
                event(SDL_SCANCODE_LSHIFT, false);
                tap(SDL_SCANCODE_RETURN);
                awaiting_menu_ = true;
            }
            return;
        }
        if (!awaiting_menu_) { tap(SDL_SCANCODE_RETURN); awaiting_menu_ = true; }
    }
    bool check_context(const FrontendState& menu) {
        switch (context_) {
        case ContextCheck::setup:
            for (unsigned count = 0; count < 4; ++count) { tap(SDL_SCANCODE_E); }
            tap(SDL_SCANCODE_F4);
            tap(SDL_SCANCODE_F11);
            context_ = ContextCheck::r_press;
            break;
        case ContextCheck::r_press:
            require(last_input_.controls.throttle == 4, "Flight setup must use keyboard throttle");
            tap(SDL_SCANCODE_R);
            context_ = ContextCheck::r_verify;
            break;
        case ContextCheck::r_verify:
            require(!last_input_.reset && last_input_.controls.throttle == 4 &&
                    last_input_.cockpit.left == InstrumentDisplay::objectives &&
                    last_input_.cockpit.right == InstrumentDisplay::systems,
                    "Integrated flight R must preserve throttle and displays");
            tap(SDL_SCANCODE_P);
            context_ = ContextCheck::pause;
            break;
        case ContextCheck::pause:
            require(status_ == "Paused", "P must pause the real session host");
            paused_cycle_ = session_.cpu_cycles();
            pause_polls_ = 0;
            context_ = ContextCheck::paused;
            break;
        case ContextCheck::paused:
            require(session_.cpu_cycles() == paused_cycle_, "Pause must stop simulation and device clock advancement");
            if (++pause_polls_ == 10) { tap(SDL_SCANCODE_P); context_ = ContextCheck::focus; }
            break;
        case ContextCheck::focus: {
            require(status_ != "Paused", "P must resume the real session host");
            hold(SDL_SCANCODE_RIGHT, true);
            hold(SDL_SCANCODE_SPACE, true);
            event(SDL_SCANCODE_LSHIFT, true, KMOD_SHIFT);
            SDL_Event event{};
            event.type = SDL_WINDOWEVENT;
            event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            require(SDL_PushEvent(&event) == 1, "Focus event must enter SDL");
            held_.fill(false);
            pause_polls_ = 0;
            paused_cycle_ = session_.cpu_cycles();
            context_ = ContextCheck::focused_pause;
            break;
        }
        case ContextCheck::focused_pause:
            require(status_ == "Paused" && session_.cpu_cycles() == paused_cycle_ &&
                    !last_input_.controls.fire && last_input_.controls.horizontal == 0,
                    "Focus loss must pause and release firing and steering");
            if (++pause_polls_ == 10) { tap(SDL_SCANCODE_P); context_ = ContextCheck::settings_enter; }
            break;
        case ContextCheck::settings_enter:
            require(status_ != "Paused", "The focus pause must resume");
            tap(SDL_SCANCODE_ESCAPE);
            context_ = ContextCheck::settings_wait;
            break;
        case ContextCheck::settings_wait:
            if (menu.mode == FrontendMode::settings) {
                settings_entry_resets_ = control_resets_;
                tap(SDL_SCANCODE_R);
                tap(SDL_SCANCODE_E);
                tap(SDL_SCANCODE_D);
                tap(SDL_SCANCODE_F4);
                context_ = ContextCheck::settings_keys;
            }
            break;
        case ContextCheck::settings_keys:
            require(!last_input_.reset && last_input_.controls.throttle == 0 &&
                    last_input_.cockpit.left == InstrumentDisplay::map,
                    "Settings letters must not change host flight controls");
            tap(SDL_SCANCODE_P);
            context_ = ContextCheck::settings_verify;
            break;
        case ContextCheck::settings_verify:
            require(status_ == "Paused", "P must pause in settings");
            paused_cycle_ = session_.cpu_cycles();
            pause_polls_ = 0;
            context_ = ContextCheck::settings_paused;
            break;
        case ContextCheck::settings_paused:
            require(session_.cpu_cycles() == paused_cycle_, "Settings pause must retain the shared clock");
            if (++pause_polls_ == 10) { tap(SDL_SCANCODE_P); context_ = ContextCheck::settings_resumed; }
            break;
        case ContextCheck::settings_resumed:
            require(status_ != "Paused", "P must resume settings");
            awaiting_menu_ = false;
            context_ = ContextCheck::save_select;
            break;
        case ContextCheck::save_select:
            changed(menu);
            if (!awaiting_menu_) {
                tap(menu.selection == 2 ? SDL_SCANCODE_RETURN : SDL_SCANCODE_DOWN);
                awaiting_menu_ = true;
                if (menu.selection == 2) { context_ = ContextCheck::save_wait; }
            }
            break;
        case ContextCheck::save_wait:
            if (!settings_.read().empty()) {
                context_ = ContextCheck::settings_leave;
                last_menu_key_ = {};
            }
            break;
        case ContextCheck::settings_leave:
            if (menu.mode != FrontendMode::settings) {
                require(control_resets_ > settings_entry_resets_ && !last_input_.controls.fire &&
                        !last_input_.controls.cycle_target, "Settings return must clear stale host actions");
                context_ = ContextCheck::commands;
                std::cout << "Keyboard pause, R, focus, settings save/return verified\n" << std::flush;
                break;
            }
            if (Clock::now() - last_menu_key_ > std::chrono::milliseconds(300)) {
                tap(SDL_SCANCODE_ESCAPE);
                last_menu_key_ = Clock::now();
            }
            break;
        case ContextCheck::commands:
            tap(unavailable_commands_.at(unavailable_command_));
            context_ = ContextCheck::command_wait;
            break;
        case ContextCheck::command_wait: {
            const auto expected = unavailable_command_ < 4 ? FlightNotice::unavailable_view :
                unavailable_command_ == 4 ? FlightNotice::unavailable_calibration : FlightNotice::unavailable_rockets;
            if (session_.presentation()->flight_notice != expected) { break; }
            require(status_.find("unavailable in this preview") != std::string::npos,
                    "Unimplemented keyboard commands must display the preview limit");
            if (++unavailable_command_ == unavailable_commands_.size()) {
                tap(SDL_SCANCODE_1);
                context_ = ContextCheck::command_clear;
            } else {
                context_ = ContextCheck::commands;
            }
            break;
        }
        case ContextCheck::command_clear:
            if (session_.presentation()->flight_notice != FlightNotice::none) { break; }
            context_ = ContextCheck::complete;
            std::cout << "Keyboard unavailable-command notices and supported recovery verified\n" << std::flush;
            return true;
        case ContextCheck::complete: return true;
        }
        return false;
    }
    void fly() {
        const auto cycle = session_.cpu_cycles();
        if (cycle == last_flight_cycle_) { return; }
        last_flight_cycle_ = cycle;
        const auto& state = session_.flight_state();
        require(flight_ticks_ < 6000, "Keyboard route must complete within its flight bound");
        auto desired = route_.controls(state, flight_ticks_++);
        // Distribute steering requests across whole keyboard-control frames.
        constexpr int steering_step = 350;
        steering_carry_ += desired.horizontal;
        const auto steering = steering_carry_ > steering_step / 2 ? steering_step :
            steering_carry_ < -steering_step / 2 ? -steering_step : 0;
        steering_carry_ -= steering;
        hold(SDL_SCANCODE_RIGHT, steering > 0);
        hold(SDL_SCANCODE_LEFT, steering < 0);
        hold(SDL_SCANCODE_UP, desired.vertical < 0);
        hold(SDL_SCANCODE_DOWN, desired.vertical > 0);
        hold(SDL_SCANCODE_TAB, desired.cycle_target);
        hold(SDL_SCANCODE_SPACE, desired.fire);
        for (unsigned value = throttle_; value < desired.throttle; ++value) { tap(SDL_SCANCODE_E); }
        for (unsigned value = throttle_; value > desired.throttle; --value) { tap(SDL_SCANCODE_D); }
        if (unsigned(desired.weapon) != state.weapon) { tap(SDL_SCANCODE_LEFTBRACKET); }
        states_ << "{\"observation\":" << flight_ticks_ << ",\"cycle\":" << cycle << ",\"x\":" << state.view.x
                << ",\"y\":" << state.view.y << ",\"heading\":" << state.view.heading
                << ",\"armor\":" << state.armor << ",\"height\":" << unsigned(state.view.height)
                << ",\"weapon\":" << unsigned(state.weapon) << ",\"target\":" << state.target.slot << "}\n";
        if (flight_ticks_ % 100 == 0) {
            std::cout << "Flight " << flight_ticks_ << " armor " << state.armor << " ammo "
                      << state.ammunition[0] << ',' << state.ammunition[1] << ',' << state.ammunition[2] << '\n' << std::flush;
        }
    }
};
}

int main(int argc, char** argv) {
    try {
        require(argc == 6, "INSTALLATION PAYLOAD PROFILE OUTPUT play|restart|abort");
        const Installation installation(argv[1], argv[2]);
        const auto profile_path = std::filesystem::absolute(argv[3]).lexically_normal();
        for (const auto root : {argv[1], argv[2]}) {
            require(profile_path != std::filesystem::absolute(root).lexically_normal() &&
                (!std::filesystem::exists(profile_path) || !std::filesystem::equivalent(profile_path, root)),
                "Use a separate native profile directory");
        }
        const PilotProfile profile(argv[3]);
        const SettingsProfile settings(argv[3]);
        const std::filesystem::path output(argv[4]);
        require(!std::filesystem::exists(output), "Use fresh output");
        std::filesystem::create_directories(output);
        const std::string mode_name = argv[5];
        require(mode_name == "play" || mode_name == "restart" || mode_name == "abort", "Unknown keyboard route mode");
        const auto mode = mode_name == "restart" ? RouteMode::restart : mode_name == "abort" ? RouteMode::abort : RouteMode::play;
        require(mode == RouteMode::restart || !std::filesystem::exists(argv[3]), "Play requires a fresh profile");
        const auto before = profile.read();
        unsigned pilot_writes = 0, settings_writes = 0;
        GameSession session(installation, [&](const auto& bytes) { profile.write(bytes); ++pilot_writes; }, before, settings.read(),
            [&](const auto& bytes) { settings.write(bytes); ++settings_writes; }, SessionStart::cold);
        KeyboardRoute host(session, installation, profile, settings, output, mode);
        run_session(session, host);
        host.verify();
        require(pilot_writes == (mode == RouteMode::restart ? 0u : 2u), "Pilot commit and debrief must each save exactly once");
        require(settings_writes == (mode == RouteMode::restart ? 0u : 1u), "Keyboard settings save must execute exactly once");
        require(mode != RouteMode::restart || before == profile.read(), "Restart navigation must preserve every saved pilot byte");
        std::ofstream summary(output / "summary.json");
        summary << "{\"passed\":true,\"mode\":\"" << mode_name << "\",\"flight_observations\":" << host.observations()
                << ",\"pilot_writes\":" << pilot_writes << ",\"settings_writes\":" << settings_writes
                << ",\"completed\":" << (mode != RouteMode::abort ? "true" : "false") << "}\n";
        require(bool(summary), "Keyboard route summary must be written");
        std::cout << (mode == RouteMode::restart ? "Restart verified\n" : mode == RouteMode::abort ?
            "Keyboard abort, debrief and save verified\n" : "Natural keyboard mission completion and save verified\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
