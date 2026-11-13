#include "comanchecd/session.hpp"
#include "loading_clock.hpp"
#include "menu_input.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
}

int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        constexpr std::size_t pending = keyboard_scan_count * 2;
        for (const auto& test : {std::pair<unsigned, unsigned>{0, 25}, {0x39, 3}, {0x0f, 8},
                                 {0x4b, 7}, {0x4d, 9}, {0x48, 11}, {0x50, 13}, {0x53, 21},
                                 {0x01, 23}, {0x1c, 25}, {0x1e, 25}}) {
            KeyboardImage image;
            image.bytes[pending] = static_cast<std::uint8_t>(test.first);
            image.bytes[pending + 1] = 0x31;
            KeyboardInput keyboard(image);
            LoadingClock clock;
            const auto scan = poll_list_key(keyboard, clock);
            require(scan == (test.first == 0x1e ? 0 : test.first) && clock.accounting().dispatches == test.second,
                    "List polling must preserve the original comparison prefix and unsupported-key loop");
            require(keyboard.image().bytes == image.bytes, "Polling must leave the pending history intact");
            keyboard.clear_pending();
            require(keyboard.pending_scan() == 0 && keyboard.image().bytes[pending + 1] == 0x31,
                    "Redraw clears only the pending byte, without shifting aliased history");
        }
        KeyboardImage shifted;
        shifted.bytes[pending] = 0x0f;
        shifted.bytes[0x2a] = 0;
        KeyboardInput keyboard(shifted);
        LoadingClock shift;
        require(poll_list_key(keyboard, shift) == 0x0f && shift.accounting().dispatches == 9,
                "Shift-Tab must include its backward branch");
        if (argc == 1) { return 0; }
        require(argc == 3, "Usage: startup_input_tests [INSTALLATION PAYLOAD_DIRECTORY]");
        const Installation installation(argv[1], argv[2]);
        const PilotWriter writer = [](const auto&) {};
        auto pilots = installation.load_frontend().pilot_record;
        pilots[9] = 'A';
        PilotStorage selected(pilots);
        selected.select(0);
        pilots = selected.record();
        GameSession measured(installation, writer, pilots);
        measured.consume({0x1c});
        require(measured.menu_state().mode == FrontendMode::main, "Transition fixture must begin with an accepted named pilot");
        measured.consume({0x1c});
        const auto campaign_ready = measured.cpu_cycles();
        GameSession queued(installation, writer, pilots);
        queued.consume({0x1c});
        constexpr unsigned first_make_delay = 100, release_delay = 2000, late_make_margin = 10000;
        const auto arrival = queued.cpu_cycles() + first_make_delay;
        require(campaign_ready > arrival + late_make_margin, "Campaign presentation must leave room for a late make");
        queued.schedule_key_byte(arrival, 0x1c);
        queued.schedule_key_byte(arrival + release_delay, 0x9c);
        queued.schedule_key_byte(campaign_ready - late_make_margin, 0x1c);
        queued.schedule_key_byte(campaign_ready - late_make_margin + release_delay, 0x9c);
        queued.render_audio(MissionAudio::sample_rate / 2);
        require(queued.menu_state().mode == FrontendMode::mission,
                "A second Enter after the campaign input reset must survive its remaining draw/presentation work");
        GameSession menu(installation, writer);
        const auto selection = menu.menu_state().selection;
        menu.queue_key_byte(0x50);
        menu.queue_key_byte(0xd0);
        require(menu.menu_state().selection == selection, "Raw input must wait for IRQ delivery and foreground polling");
        menu.render_audio(MissionAudio::sample_rate / 2);
        require(menu.menu_state().selection == (selection + 1) % pilot_count,
                "One delivered make must move once despite repeated polling");
        menu.render_audio(MissionAudio::sample_rate / 2);
        require(menu.menu_state().selection == (selection + 1) % pilot_count,
                "A stale pending key must not repeat after redraw");
        menu.queue_key_byte(0x48);
        menu.queue_key_byte(0xc8);
        menu.render_audio(MissionAudio::sample_rate / 2);
        require(menu.menu_state().selection == selection, "Raw Up must undo the delivered Down");

        GameSession continuous(installation, writer, {}, {}, {}, SessionStart::cold);
        GameSession fragmented(installation, writer, {}, {}, {}, SessionStart::cold);
        for (auto* session : {&continuous, &fragmented}) {
            require(session->cpu_cycles() > ExecutionClock::cycles_per_second / 25,
                    "Cold calibration and asset work must run before entering the fanfare");
            require(session->menu_state().mode == FrontendMode::startup && session->demo_playing(),
                    "Cold entry must begin the first attract cycle before constructing the pilot menu");
            const auto arrival = session->cpu_cycles() + ExecutionClock::cycles_per_second / 4;
            session->schedule_key_byte(arrival, 0x01);
            session->schedule_key_byte(arrival + ExecutionClock::cycles_per_second / 10, 0x81);
        }
        const auto initial_irq = continuous.irq_ticks();
        const auto held_frame = continuous.presentation();
        for (unsigned read = 0; read < 100; ++read) {
            const auto frame = continuous.presentation();
            require(frame->cpu_cycles == held_frame->cpu_cycles && frame->irq_ticks == held_frame->irq_ticks &&
                    frame->random_seed == held_frame->random_seed && frame->audio_samples == held_frame->audio_samples &&
                    frame->image.pixels == held_frame->image.pixels && frame->image.palette == held_frame->image.palette,
                    "Repeated presentation reads must preserve session clocks, RNG, indexed pixels and palette");
        }
        AudioEntry cold_entry;
        cold_entry.execution = AudioExecution::scheduled;
        cold_entry.startup = AudioStartup::cold;
        MissionAudio cold_audio(installation.load_audio(), installation.load_menu_music(), cold_entry);
        require(!cold_audio.music_irq_work().empty() && cold_audio.music_irq_work().front().work.dispatches == 1,
                "The uninstalled cold timer callback must execute only its original RET");
        const auto half = MissionAudio::sample_rate / 2;
        auto expected = continuous.render_audio(half);
        require(continuous.menu_state().mode == FrontendMode::startup && continuous.irq_ticks() - initial_irq < 301,
                "An early Escape must not bypass the unskippable fanfare gate");
        const auto tail = continuous.render_audio(MissionAudio::sample_rate * 6 - half);
        expected.insert(expected.end(), tail.begin(), tail.end());
        std::vector<std::int16_t> actual;
        constexpr unsigned chunk = 733;
        while (actual.size() < expected.size()) {
            const auto next = fragmented.render_audio(std::min<std::size_t>(chunk, expected.size() - actual.size()));
            actual.insert(actual.end(), next.begin(), next.end());
        }
        require(continuous.menu_state().mode == FrontendMode::pilot && continuous.irq_ticks() - initial_irq >= 301,
                "A retained startup key must hand off to pilots only after the 301-IRQ fanfare gate");
        require(actual == expected && fragmented.menu_state().mode == FrontendMode::pilot &&
                fragmented.irq_ticks() == continuous.irq_ticks() && fragmented.random_seed() == continuous.random_seed() &&
                fragmented.image().pixels == continuous.image().pixels,
                "Host audio chunking must preserve startup PCM, IRQ carry, RNG and final pixels");
        require(held_frame->cpu_cycles < continuous.cpu_cycles() && held_frame->menu.mode == FrontendMode::startup &&
                held_frame->random_seed != continuous.random_seed(),
                "A held presentation must retain its original state while the authoritative session advances");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
