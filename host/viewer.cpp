#include "flight_host.hpp"
#include "pending_input.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace comanchecd::host {
namespace {
constexpr auto tick_duration = std::chrono::milliseconds(1000 / training_ticks_per_second);
constexpr auto catchup_limit = tick_duration * 5;
constexpr auto samples_per_tick = training_samples_per_tick;
constexpr auto audio_queue_limit = samples_per_tick * 4;
constexpr auto menu_tick_duration = std::chrono::milliseconds(10);
constexpr auto menu_samples_per_tick = audio_sample_rate / 100;
}

InputContext input_context(const FrontendState& menu, bool flying) {
    if (flying && menu.mode != FrontendMode::settings) { return InputContext::flight; }
    if (menu.mode == FrontendMode::pilot && menu.pilot_mode == PilotMode::editor) { return InputContext::name_entry; }
    return InputContext::menu;
}

std::size_t run_viewer(TrainingFlight& flight, FlightHost& host, std::size_t tick_limit) {
    using Clock = std::chrono::steady_clock;
    auto previous = Clock::now();
    Clock::duration accumulated = tick_duration;
    std::size_t ticks = 0;
    bool paused = false;
    std::string fault;
    PendingFlightInput pending;
    host.set_input_context(InputContext::training);
    host.set_playback(Playback::running);
    while (tick_limit == 0 || ticks < tick_limit) {
        auto input = host.poll();
        pending.queue(input);
        if (input.quit || input.escape) {
            break;
        }
        const auto now = Clock::now();
        accumulated += std::min(now - previous, std::chrono::duration_cast<Clock::duration>(catchup_limit));
        previous = now;
        if (input.reset) {
            flight.reset();
            host.reset_controls();
            input.controls = {};
            input.cockpit = {};
            pending.clear();
            input.pressed = {};
            previous = Clock::now();
            fault.clear();
            paused = false;
            accumulated = tick_duration;
            host.clear_audio();
            host.set_playback(Playback::running);
        }
        if (input.pause == PauseAction::pause || (input.pause == PauseAction::toggle && fault.empty())) {
            paused = input.pause == PauseAction::pause || !paused;
            pending.clear_presses();
            host.clear_audio();
            host.set_playback(paused ? Playback::paused : Playback::running);
        }
        if (paused) {
            accumulated = Clock::duration::zero();
        }
        while (!paused && accumulated >= tick_duration && (tick_limit == 0 || ticks < tick_limit)) {
            try {
                const auto controls = pending.apply(input.controls);
                flight.advance(controls, input.cockpit);
            } catch (const std::invalid_argument& error) {
                if (tick_limit != 0) {
                    throw;
                }
                fault = error.what();
                std::cerr << fault << " — R resets the training preview\n";
                paused = true;
                host.set_playback(Playback::paused);
                host.clear_audio();
                break;
            }
            for (const auto& image : flight.intermediate_images()) {
                host.present(image, flight.state(), "Training preview");
            }
            if (host.queued_audio_samples() > audio_queue_limit) {
                host.clear_audio();
            }
            host.queue_audio(flight.render_audio(samples_per_tick));
            accumulated -= tick_duration;
            ++ticks;
            if (flight.state().ended) {
                fault = "Flight ended";
                paused = true;
                host.set_playback(Playback::paused);
                host.clear_audio();
                if (tick_limit != 0) {
                    return ticks;
                }
            }
        }
        const auto status = fault.empty() ? (paused ? "Paused — P resumes, R resets" : "Training preview") : fault + " — R resets";
        host.present(flight.cockpit_image(), flight.state(), status);
        host.idle();
    }
    host.set_playback(Playback::paused);
    host.clear_audio();
    return ticks;
}

void run_session(GameSession& session, FlightHost& host) {
    using Clock = std::chrono::steady_clock;
    auto previous = Clock::now();
    Clock::duration accumulated{};
    bool paused = false;
    PendingFlightInput pending;
    if (session.flying()) { session.take_pending_audio(); }
    host.set_playback(Playback::running);
    while (!session.quit_requested()) {
        const auto menu = session.menu_state();
        host.set_input_context(input_context(menu, session.flying()));
        const auto input = host.poll();
        if (input.quit) {
            break;
        }
        const auto was_flying = session.flying();
        for (const auto scan : input.scan_bytes) { session.queue_key_byte(scan); }
        const auto was_demo = session.demo_playing();
        const auto was_settings = session.menu_state().mode == FrontendMode::settings;
        const auto editing_name = session.menu_state().mode == FrontendMode::pilot &&
            session.menu_state().pilot_mode == PilotMode::editor;
        if (was_flying && !was_settings) { pending.queue(input); }
        if (input.abort_mission && was_flying && !was_demo && !was_settings) {
            session.abort_mission();
            host.clear_audio();
        } else if (input.escape && was_flying && !was_settings && !was_demo) {
            paused = false;
            host.set_playback(Playback::running);
            pending.clear();
            host.reset_controls();
        } else if (input.pause == PauseAction::pause || (input.pause == PauseAction::toggle && !editing_name)) {
            paused = input.pause == PauseAction::pause || !paused;
            pending.clear_presses();
            host.clear_audio();
            host.set_playback(paused ? Playback::paused : Playback::running);
        }
        const auto now = Clock::now();
        accumulated += std::min(now - previous, std::chrono::duration_cast<Clock::duration>(catchup_limit));
        previous = now;
        if (was_flying != session.flying() || (was_demo && !session.demo_playing())) {
            accumulated = Clock::duration::zero();
        }
        if (paused) {
            accumulated = Clock::duration::zero();
        }
        while (!paused && accumulated >= (session.flying() ? tick_duration : menu_tick_duration)) {
            if (!session.flying()) {
                accumulated -= menu_tick_duration;
                if (host.queued_audio_samples() > audio_queue_limit) {
                    host.clear_audio();
                }
                host.queue_audio(session.render_audio(menu_samples_per_tick));
                for (const auto& image : session.intermediate_images()) {
                    host.present(image, session.flight_state(), "Overview");
                }
                if (session.flying() || (was_settings && session.menu_state().mode != FrontendMode::settings)) {
                    break;
                }
                continue;
            }
            const auto controls = pending.apply(input.controls);
            session.advance(controls, input.cockpit);
            for (const auto& image : session.intermediate_images()) {
                host.present(image, session.flight_state(), "Flight");
            }
            if (!session.flying()) {
                break;
            }
            if (host.queued_audio_samples() > audio_queue_limit) {
                host.clear_audio();
            }
            auto samples = session.render_audio(samples_per_tick);
            const auto pending_audio = session.take_pending_audio();
            samples.insert(samples.end(), pending_audio.begin(), pending_audio.end());
            host.queue_audio(samples);
            accumulated -= std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(double(samples.size()) / audio_sample_rate));
            if (was_settings && session.menu_state().mode != FrontendMode::settings) {
                break;
            }
        }
        if (was_settings && session.menu_state().mode != FrontendMode::settings) {
            pending.clear();
            host.reset_controls();
        }
        if (was_flying != session.flying() || (was_demo && !session.demo_playing())) {
            // Match the device queue reset at this presentation boundary.
            session.take_pending_audio();
            host.clear_audio();
            pending.clear();
            host.reset_controls();
            paused = false;
            accumulated = Clock::duration::zero();
            host.set_playback(Playback::running);
        }
        const auto frame = session.presentation();
        std::string status = frame->flying ? "Flight" : "Operations";
        switch (frame->flight_notice) {
        case FlightNotice::none: break;
        case FlightNotice::unavailable_view: status = "View unavailable in this preview"; break;
        case FlightNotice::unavailable_calibration: status = "Calibration unavailable in this preview"; break;
        case FlightNotice::unavailable_input_mode: status = "Input mode unavailable in this preview"; break;
        case FlightNotice::unavailable_rockets: status = "M shortcut unavailable in this preview"; break;
        }
        host.present_snapshot(frame, paused ? "Paused" : status);
        host.idle();
    }
    host.set_playback(Playback::paused);
    host.clear_audio();
}
} // namespace comanchecd::host
