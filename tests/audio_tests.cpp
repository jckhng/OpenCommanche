#include "comanchecd/audio.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void integer(std::vector<std::uint8_t>& bytes, std::uint32_t value, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
    }
}

std::vector<std::uint8_t> bank(unsigned count) {
    std::vector<std::uint8_t> result{'K', '1'};
    integer(result, count, 2);
    for (unsigned index = 0; index < count; ++index) {
        integer(result, 4 + count * 4 + index * 8, 4);
    }
    for (unsigned index = 0; index < count; ++index) {
        integer(result, 8, 2);
        integer(result, 65535, 2);
        result.insert(result.end(), 4, static_cast<std::uint8_t>(160 + index * 8));
    }
    return result;
}
}

int main() {
    try {
        comanchecd::AudioTables tables;
        for (auto& envelope : tables.envelopes) {
            envelope = {0x80};
        }
        tables.volume.fill(128);
        tables.instruments[0][0] = 0x11;
        tables.instruments[0][5] = 0x22;
        tables.frequencies[120] = 0x0d57;
        for (std::size_t index = 0; index < tables.sample_conversion.size(); ++index) {
            tables.sample_conversion[index] = static_cast<std::uint8_t>(std::clamp<int>(int(index) - 128, 0, 255));
        }
        std::vector<comanchecd::GameplayPayload> payloads(17);
        payloads[0].bytes = bank(4);
        payloads[15].bytes = bank(7);
        payloads[13].bytes = bank(3);
        auto& music = payloads[12].bytes;
        music.resize(48);
        music[0] = 'M';
        music[1] = 'S';
        music.insert(music.end(), {0x90, 60, 100, 1, 0x80, 1, 0xff, 0x2f});
        comanchecd::AudioEntry drain_entry;
        drain_entry.execution = comanchecd::AudioExecution::scheduled;
        comanchecd::MissionAudio drained(tables, payloads, drain_entry);
        comanchecd::MissionAudio reference(tables, payloads, drain_entry);
        for (auto* audio : {&drained, &reference}) {
            audio->execution_clock().execute(comanchecd::CpuOperation::instructions,
                                            comanchecd::ExecutionClock::cycles_per_second / 50);
            audio->render(17);
            audio->execution_clock().execute(comanchecd::CpuOperation::instructions,
                                            comanchecd::ExecutionClock::cycles_per_second / 100);
        }
        const auto cycles = drained.execution_clock().cycles();
        const auto interrupts = drained.irq_ticks();
        const auto pending = drained.take_pending_audio();
        require(!pending.empty() && drained.take_pending_audio().empty() &&
                drained.execution_clock().cycles() == cycles && drained.irq_ticks() == interrupts,
                "Taking pending PCM must empty both output queues without executing CPU work or interrupts");
        require(pending == reference.render(pending.size()) && drained.render(1000) == reference.render(1000) &&
                drained.execution_clock().cycles() == reference.execution_clock().cycles(),
                "Taking pending PCM must preserve the complete sample sequence and subsequent execution");
        comanchecd::AudioEntry mission_entry;
        mission_entry.execution = comanchecd::AudioExecution::scheduled;
        mission_entry.music_start = comanchecd::MusicStart::deferred;
        std::uint64_t mission_dispatches = 0;
        mission_entry.cpu_observer = [&](comanchecd::CpuOperation, std::uint64_t) { ++mission_dispatches; };
        for (const auto power : {0u, 5u}) {
            comanchecd::MissionAudio mission_audio(tables, comanchecd::MusicPlaylist{{}, music}, mission_entry);
            comanchecd::StartupFiles files;
            files.configuration = {'0', '1', '3', '2', '7', '1'};
            files.loading.emplace();
            files.loading->active_settings.assign(comanchecd::MenuSettings::record_bytes, 125);
            files.loading->active_settings[25] = '2';
            files.loading->active_settings[26] = '0';
            files.loading->active_settings[27] = '0';
            for (auto& menu : files.loading->menus) { menu = {0, 255}; }
            mission_audio.prepare_mission_audio(payloads, files);
            const auto queued = mission_dispatches;
            mission_audio.queue_initial_engine(static_cast<std::uint8_t>(power));
            require(mission_dispatches - queued == (power == 0 ? 54u : 52u),
                    "Initial engine work must preserve the silent-throttle branch and both CLI/STI sections");
            const auto radio = mission_dispatches;
            mission_audio.play_launch_radio();
            require(mission_dispatches - radio == 50 && mission_audio.sample_requests().size() == 2,
                    "Launch radio must retain the free-voice path and follow the queued engine");
            auto dispatch = mission_dispatches;
            mission_audio.play_effect(1);
            require(mission_dispatches - dispatch == 36, "Effect voice zero must include bank lookup, routing and CLI/STI");
            dispatch = mission_dispatches;
            mission_audio.play_effect(2);
            require(mission_dispatches - dispatch == 41, "Effect voice one must include the occupied voice-zero branch");
            dispatch = mission_dispatches;
            mission_audio.play_effect(4, 1);
            require(mission_dispatches - dispatch == 35, "An attenuated effect must retain rejected-routing work");
            dispatch = mission_dispatches;
            mission_audio.play_cue({comanchecd::SampleBank::effects, 1, 1, comanchecd::EffectPolicy::restart_matching});
            require(mission_dispatches - dispatch == 35, "Matching-only effects must bypass the free-voice scan");
            dispatch = mission_dispatches;
            mission_audio.play_cue({comanchecd::SampleBank::speech, 2});
            require(mission_dispatches - dispatch == 18, "A busy radio must retain the speech routing and return work");
            const auto& request = mission_audio.sample_requests().front();
            require(request.mode == comanchecd::SampleMode::queued && request.voice == 2 &&
                    request.bank == (power == 0 ? comanchecd::SampleBank::silence : comanchecd::SampleBank::effects),
                    "A zero initial throttle must queue shared silence before the launch radio");
            mission_audio.stop_mission_samples();
            dispatch = mission_dispatches;
            mission_audio.clear_mission_samples();
            require(mission_dispatches - dispatch == 29,
                    "Flight return must clear three silent voices under CLI/STI without restarting DMA");
        }
        const std::vector<comanchecd::SampleRequest> requests{
            {500, comanchecd::SampleBank::announcements, 1, 0, 1, 0, comanchecd::SampleMode::queued},
            {1500, comanchecd::SampleBank::silence, 0, 0, 0, 0, comanchecd::SampleMode::queued}};
        comanchecd::MissionAudio whole(tables, payloads, {}, requests), split(tables, payloads, {}, requests);
        const auto expected = whole.render(comanchecd::MissionAudio::sample_rate / 5);
        auto actual = split.render(comanchecd::MissionAudio::sample_rate / 10);
        const auto tail = split.render(comanchecd::MissionAudio::sample_rate / 10);
        actual.insert(actual.end(), tail.begin(), tail.end());
        require(actual == expected && split.digital_pcm() == whole.digital_pcm(),
                "Audio render chunks must preserve device and sequencer phases");
        require(split.irq_ticks() == whole.irq_ticks() && whole.irq_ticks() == 13,
                "Music IRQ accounting must share the recovered timer divisor");
        comanchecd::MissionAudio clocked(tables, payloads);
        clocked.render(comanchecd::MissionAudio::sample_rate * 10);
        require(clocked.digital_pcm().size() == comanchecd::Sb16Playback::source_rate * 10,
                "DSP playback must use the integer 11111 Hz device rate without long-run producer drift");
        comanchecd::MissionAudio interrupt_whole(tables, payloads), interrupt_split(tables, payloads);
        std::vector<std::size_t> whole_edges, split_edges;
        const auto service = [](auto& audio, auto& edges) {
            edges.push_back(audio.digital_pcm().size());
            audio.play_effect(1);
        };
        const auto interrupt_expected = interrupt_whole.render(comanchecd::MissionAudio::sample_rate / 5, [&] { service(interrupt_whole, whole_edges); });
        std::vector<std::int16_t> interrupt_actual;
        for (unsigned chunk = 0; chunk < 20; ++chunk) {
            const auto block = interrupt_split.render(comanchecd::MissionAudio::sample_rate / 100, [&] { service(interrupt_split, split_edges); });
            interrupt_actual.insert(interrupt_actual.end(), block.begin(), block.end());
        }
        require(whole_edges.size() == 13 && whole_edges == split_edges && whole_edges.front() < whole_edges.back() &&
                interrupt_actual == interrupt_expected && interrupt_split.digital_pcm() == interrupt_whole.digital_pcm(),
                "IRQ callbacks and their sound cues must execute within the audio block at identical sample boundaries");
        comanchecd::AudioEntry carried_entry;
        carried_entry.irq_phase = comanchecd::IrqClock::period - comanchecd::pit_frequency;
        comanchecd::MissionAudio carried(tables, payloads, carried_entry);
        carried.render(1);
        require(carried.irq_ticks() == 1, "An explicit inherited timer phase must reach the music sequencer");
        comanchecd::MissionAudio streaming(tables, payloads, {}, requests, comanchecd::AudioTracePolicy::discard);
        require(streaming.render(comanchecd::MissionAudio::sample_rate / 10) == std::vector<std::int16_t>(expected.begin(), expected.begin() + comanchecd::MissionAudio::sample_rate / 10) &&
                streaming.render(comanchecd::MissionAudio::sample_rate / 10) == tail, "Discarding traces must preserve sample requests and render phases");
        streaming.play_effect(1);
        split.play_effect(1);
        require(streaming.render(comanchecd::MissionAudio::sample_rate / 10) == split.render(comanchecd::MissionAudio::sample_rate / 10) && streaming.writes().empty() &&
                streaming.digital_pcm().empty() && streaming.sample_requests().empty(),
                "Streaming effects must preserve output without retaining diagnostic history");
        require(whole.digital_pcm()[0] == 164 && whole.digital_pcm()[5] == 134,
                "K1 radio completion must resume the queued attenuated engine sample");
        auto soft_tables = tables;
        soft_tables.soft_sample_conversion.fill(142);
        comanchecd::MissionAudio soft(soft_tables, payloads);
        soft.set_sound_level(comanchecd::SoundLevel::soft);
        soft.render(comanchecd::MissionAudio::sample_rate / 100);
        require(std::all_of(soft.digital_pcm().begin(), soft.digital_pcm().end(), [](auto value) { return value == 142; }),
                "Soft sampled sound must use its separate original conversion table");
        const auto before_sound_off = soft.digital_pcm().size();
        soft.set_sound_level(comanchecd::SoundLevel::off);
        soft.render(comanchecd::MissionAudio::sample_rate / 100);
        require(std::all_of(soft.digital_pcm().begin() + before_sound_off, soft.digital_pcm().end(),
                           [](auto value) { return value == 128; }), "Sound Off must silence the sampled channel");
        require(comanchecd::engine_rate(0) == 0x4000 && comanchecd::engine_rate(15) == 0x4f00 &&
                comanchecd::engine_rate(200) == 0xc000, "Engine rate follows the original power clamp and fixed-point scale");
        require(std::any_of(whole.writes().begin(), whole.writes().end(), [](const auto& write) {
            return write.tick == 1 && write.reg == 0xa0 && write.value == 0x57;
        }), "MS notes must reach the recovered OPL frequency table");
        require(std::any_of(whole.writes().begin(), whole.writes().end(), [](const auto& write) {
            return write.tick == 1 && write.reg == 0x23 && write.value == 0x11;
        }), "Instrument records store the carrier operator before the modulator");
        require(std::any_of(whole.writes().begin(), whole.writes().end(), [](const auto& write) {
            return write.tick == 1 && write.reg == 0x43 && write.value == 7;
        }), "Music gain combines instrument volume with the original master volume");
        comanchecd::MissionAudio live(tables, payloads);
        live.play_effect(1);
        live.play_effect(2);
        live.play_effect(4, 1);
        require(live.sample_requests().size() == 2 && live.sample_requests()[0].voice == 0 &&
                live.sample_requests()[1].voice == 1, "Native effects choose idle virtual voices before replacing a sample");
        live.play_effect(1, 1);
        require(live.sample_requests().size() == 3 && live.sample_requests().back().voice == 0,
                "Busy voice selection reuses a matching effect before rejecting an attenuated request");
        live.set_engine_power(15);
        comanchecd::MissionAudio weapons(tables, payloads);
        weapons.play_cue({comanchecd::SampleBank::effects, 1, 0, comanchecd::EffectPolicy::restart_matching});
        weapons.play_cue({comanchecd::SampleBank::effects, 1, 0, comanchecd::EffectPolicy::restart_matching});
        weapons.play_cue({comanchecd::SampleBank::effects, 2, 0, comanchecd::EffectPolicy::available_first, 0x20000});
        require(weapons.sample_requests().size() == 3 && weapons.sample_requests()[0].voice == 0 &&
                weapons.sample_requests()[1].voice == 0 && weapons.sample_requests()[2].voice == 1 &&
                weapons.sample_requests()[2].step == 0x20000,
                "Cannon repeats restart the matching voice; rockets retain their explicit sample rate");
        comanchecd::MissionAudio radio(tables, payloads);
        radio.play_cue({comanchecd::SampleBank::speech, 2});
        radio.play_cue({comanchecd::SampleBank::speech, 2});
        radio.play_cue({comanchecd::SampleBank::speech, 3});
        radio.play_cue({comanchecd::SampleBank::speech, 4});
        require(radio.sample_requests().size() == 2 && radio.sample_requests()[0].voice == 0 &&
                radio.sample_requests()[1].voice == 1, "Radio cues must use idle voices and reject duplicates or busy channels");
        radio.render(comanchecd::MissionAudio::sample_rate / 100);
        radio.play_cue({comanchecd::SampleBank::speech, 4});
        require(radio.sample_requests().size() == 3 && radio.sample_requests().back().voice == 2,
                "Radio cues must prefer the resumed engine voice");
        comanchecd::MissionAudio announcement(tables, payloads);
        announcement.play_cue({comanchecd::SampleBank::announcements, 1});
        announcement.play_cue({comanchecd::SampleBank::announcements, 1});
        require(announcement.sample_requests().size() == 1 &&
                announcement.sample_requests()[0].bank == comanchecd::SampleBank::announcements,
                "Cockpit warnings must dispatch the announcement bank and suppress duplicate playback");
        const auto original_music = music;
        music.erase(music.begin() + 48, music.end());
        music.insert(music.end(), {0xbf, 7, 100, 0, 0xdf, 5, 0, 0xff, 0x51, 1});
        music.insert(music.end(), original_music.begin() + 48, original_music.end());
        comanchecd::MissionAudio ignored_events(tables, payloads);
        ignored_events.render(comanchecd::MissionAudio::sample_rate / 5);
        require(std::any_of(ignored_events.writes().begin(), ignored_events.writes().end(), [](const auto& write) {
            return write.tick == 2 && write.reg == 0xa0 && write.value == 0x57;
        }), "Non-note MS events consume their original byte widths without requiring an OPL channel");
        music = original_music;
        music.insert(music.begin() + 48, {0x8a, 0});
        comanchecd::MissionAudio unmapped_channel(tables, payloads);
        music = original_music;
        comanchecd::MissionAudio mapped_channel(tables, payloads);
        require(unmapped_channel.render(comanchecd::MissionAudio::sample_rate / 5) == mapped_channel.render(comanchecd::MissionAudio::sample_rate / 5),
                "Logical channel 10 note-off is accepted without changing the OPL output");
        auto introduction = music;
        auto repeat = music;
        introduction.resize(48);
        repeat.resize(48);
        introduction.insert(introduction.end(), {0x90, 60, 100, 1, 0xff, 0x2f});
        repeat.insert(repeat.end(), {0x90, 61, 100, 2, 0xff, 0x2f});
        auto bent_introduction = introduction;
        bent_introduction.insert(bent_introduction.begin() + 48, {0xe0, 14, 0});
        auto unbent_repeat = repeat;
        unbent_repeat[49] = 60;
        comanchecd::MissionAudio bend_transition(tables, comanchecd::MusicPlaylist{bent_introduction, unbent_repeat});
        bend_transition.render(comanchecd::MissionAudio::sample_rate / 10);
        require(std::any_of(bend_transition.writes().begin(), bend_transition.writes().end(), [](const auto& write) {
            return write.tick == 3 && write.reg == 0xa0 && write.value == 0x57;
        }), "The intro-to-repeat transition must clear the previous pitch bend");
        comanchecd::MissionAudio menu(tables, comanchecd::MusicPlaylist{introduction, repeat});
        menu.render(comanchecd::MissionAudio::sample_rate);
        require(!menu.writes().empty() && std::all_of(menu.digital_pcm().begin(), menu.digital_pcm().end(),
                    [](auto value) { return value == 128; }),
                "Menu music must advance without a mission engine or launch announcement");
        const auto before_mute = menu.writes().size();
        menu.set_music_level(comanchecd::MusicLevel::off);
        menu.render(comanchecd::MissionAudio::sample_rate);
        require(std::none_of(menu.writes().begin() + before_mute, menu.writes().end(), [](const auto& write) {
                    return write.reg >= 0xb0 && write.reg <= 0xb8 && (write.value & 0x20) != 0;
                }), "Muted music must continue its sequence without OPL key-on writes");
        menu.set_music_level(comanchecd::MusicLevel::loud);
        const auto before_resume = menu.writes().size();
        menu.render(comanchecd::MissionAudio::sample_rate);
        require(std::any_of(menu.writes().begin() + before_resume, menu.writes().end(), [](const auto& write) {
                    return write.reg >= 0xb0 && write.reg <= 0xb8 && (write.value & 0x20) != 0;
                }), "Re-enabled music must resume note playback");
        comanchecd::MissionAudio continuous(tables, payloads), fragmented(tables, payloads);
        comanchecd::MissionAudio settings_audio(tables, payloads);
        comanchecd::MissionAudio queued_silence(tables, payloads, {},
            {{0, comanchecd::SampleBank::silence, 0, 2, 0, 0, comanchecd::SampleMode::queued}});
        settings_audio.suspend_engine();
        require(settings_audio.render(comanchecd::MissionAudio::sample_rate / 100) == queued_silence.render(comanchecd::MissionAudio::sample_rate / 100),
                "Settings entry queues engine silence and lets the current radio sample finish");
        require(continuous.render(719) == fragmented.render(719), "Carried audio must begin identically");
        const auto before_transition = continuous.writes().size();
        const auto carried_ticks = continuous.irq_ticks();
        continuous.change_music({introduction, repeat});
        fragmented.change_music({introduction, repeat});
        require(continuous.irq_ticks() == carried_ticks &&
                std::none_of(continuous.writes().begin() + before_transition, continuous.writes().end(), [](const auto& write) {
                    return write.reg == 1 || write.reg == 8 || write.reg == 0xbd;
                }), "Changing music must retain IRQ phase and avoid resetting the FM device");
        const auto carried_expected = continuous.render(1207);
        auto carried_actual = fragmented.render(1);
        for (unsigned part : {67u, 511u, 628u}) {
            const auto block = fragmented.render(part);
            carried_actual.insert(carried_actual.end(), block.begin(), block.end());
        }
        require(carried_actual == carried_expected && continuous.digital_pcm() == fragmented.digital_pcm(),
                "Mixed FM/digital output must remain sample exact across arbitrary blocks after a music transition");
        continuous.suspend_engine();
        fragmented.suspend_engine();
        require(continuous.render(137) == fragmented.render(137), "Settings entry must preserve carried mixer state");
        continuous.resume_engine(75);
        fragmented.resume_engine(75);
        require(continuous.render(701) == fragmented.render(701), "Settings exit must resume the engine on the retained device");
        const auto mission_ticks = continuous.irq_ticks();
        continuous.enter_mission(payloads, 15);
        fragmented.enter_mission(payloads, 15);
        require(continuous.irq_ticks() == mission_ticks && continuous.render(2221) == fragmented.render(2221),
                "Replacing mission banks must retain the device clock and safely rebind sample voices");
        comanchecd::MissionAudio digital_reference(tables, payloads), music_changed(tables, payloads);
        digital_reference.render(719);
        music_changed.render(719);
        music_changed.change_music({introduction, repeat});
        digital_reference.render(1207);
        music_changed.render(1207);
        require(digital_reference.digital_pcm() == music_changed.digital_pcm(),
                "A music change must preserve digital voices, echo history and sample phase");
        auto cinematic_bank = bank(7);
        // Synthetic data reproduces the retail directory overread without embedding its samples.
        cinematic_bank[52] = 0x84;
        cinematic_bank[53] = 0x81;
        cinematic_bank[54] = cinematic_bank[55] = 0x7f;
        continuous.enter_cinematic(cinematic_bank, {{}, repeat});
        fragmented.enter_cinematic(cinematic_bank, {{}, repeat});
        continuous.play_effect(13);
        fragmented.play_effect(13);
        const auto cinematic_pcm = continuous.render(1000);
        auto cinematic_split = fragmented.render(413);
        const auto cinematic_tail = fragmented.render(587);
        cinematic_split.insert(cinematic_split.end(), cinematic_tail.begin(), cinematic_tail.end());
        require(cinematic_pcm == cinematic_split && continuous.sample_requests().back().sound == 13,
                "The bounded cinematic directory overread must retain mixer continuity across chunks");
        continuous.enter_mission(payloads, 15);
        bool stale_tail = false;
        try {
            continuous.play_effect(13);
        } catch (const std::invalid_argument&) {
            stale_tail = true;
        }
        require(stale_tail, "Cinematic sample aliases must not survive replacement by mission banks");
        comanchecd::AudioEntry scheduled_entry;
        scheduled_entry.execution = comanchecd::AudioExecution::scheduled;
        comanchecd::MissionAudio menu_device(tables, comanchecd::MusicPlaylist{music, {}}, scheduled_entry);
        menu_device.render(comanchecd::MissionAudio::sample_rate);
        require(menu_device.digital_pcm().empty(),
                "An inactive menu sample driver must not produce DMA buffers");
        menu_device.enter_mission(payloads, 0);
        menu_device.render(comanchecd::MissionAudio::sample_rate);
        require(!menu_device.digital_pcm().empty(),
                "Mission entry must start DMA after an inactive menu sample driver");
        comanchecd::MissionAudio scheduled_whole(tables, payloads, scheduled_entry);
        comanchecd::MissionAudio scheduled_split(tables, payloads, scheduled_entry);
        const auto scheduled_expected = scheduled_whole.render(comanchecd::MissionAudio::sample_rate);
        std::vector<std::int16_t> scheduled_actual;
        for (unsigned i = 0; i < 100; ++i) {
            const auto block = scheduled_split.render(comanchecd::MissionAudio::sample_rate / 100);
            scheduled_actual.insert(scheduled_actual.end(), block.begin(), block.end());
        }
        require(scheduled_actual == scheduled_expected && scheduled_whole.digital_pcm() == scheduled_split.digital_pcm(),
                "Scheduled MissionAudio must preserve DMA ownership and output across host chunks");
        require(!scheduled_whole.digital_pcm().empty() && scheduled_whole.digital_pcm().size() % 2048 == 0,
                "Scheduled digital production must commit complete alternate half-buffers");
        auto& scheduled_clock = scheduled_whole.execution_clock();
        const auto next_irq = scheduled_whole.irq_ticks() + 1;
        const auto numerator = next_irq * comanchecd::ExecutionClock::cycles_per_second * comanchecd::mission_pit_divisor;
        constexpr unsigned keyoff_irq_margin = 256;
        const auto before_edge = (numerator + comanchecd::pit_frequency - 1) / comanchecd::pit_frequency - keyoff_irq_margin;
        require(before_edge > scheduled_clock.cycles(), "Transition fixture must begin before the next timer edge");
        while (scheduled_clock.cycles() < before_edge) {
            scheduled_clock.execute(comanchecd::CpuOperation::instructions, 1);
        }
        auto short_music = music;
        short_music.resize(48);
        short_music.insert(short_music.end(), {0xff, 0x2f});
        const auto before_music_change = scheduled_whole.music_irq_work().size();
        scheduled_whole.change_music({short_music, {}});
        require(scheduled_whole.music_irq_work().size() > before_music_change,
                "Transition fixture must deliver a timer interrupt inside the foreground key-off writes");
        for (auto index = before_music_change; index < scheduled_whole.music_irq_work().size(); ++index) {
            const auto& work = scheduled_whole.music_irq_work()[index];
            require(work.first_write == work.last_write,
                    "Foreground music replacement must publish its cursor and disable event dispatch before key-off writes can deliver IRQs");
        }
        comanchecd::MissionAudio observers(tables, payloads, scheduled_entry);
        unsigned fallback = 0, old_flight = 0, new_flight = 0;
        observers.set_interrupt_handler([&] { ++fallback; });
        const auto old_token = observers.add_interrupt_handler([&] { ++old_flight; });
        const auto new_token = observers.add_interrupt_handler([&] { ++new_flight; });
        observers.remove_interrupt_handler(old_token);
        observers.execution_clock().execute(comanchecd::CpuOperation::instructions,
                                             comanchecd::ExecutionClock::cycles_per_second / 20);
        require(new_flight != 0 && old_flight == 0 && fallback == 0,
                "Destroying an old flight must preserve the newer IRQ receiver");
        const auto temporary = observers.add_interrupt_handler([&] { ++old_flight; });
        observers.remove_interrupt_handler(temporary);
        const auto received = new_flight;
        observers.execution_clock().execute(comanchecd::CpuOperation::instructions,
                                             comanchecd::ExecutionClock::cycles_per_second / 20);
        require(new_flight > received && old_flight == 0,
                "Unwinding a replacement flight must restore the prior IRQ receiver");
        observers.remove_interrupt_handler(new_token);
        observers.execution_clock().execute(comanchecd::CpuOperation::instructions,
                                             comanchecd::ExecutionClock::cycles_per_second / 20);
        require(fallback != 0, "Removing all flight receivers must restore the default timer observer");
        payloads[0].bytes[4] = 255;
        bool rejected = false;
        try {
            comanchecd::MissionAudio invalid(tables, payloads);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Out-of-range K1 samples must reject");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
