#include "comanchecd/audio.hpp"

#include "comanchecd/sound_device.hpp"
#include "comanchecd/audio_scheduler.hpp"
#include "comanchecd/pilot.hpp"
#include "startup_timing.hpp"
#include "flight_work.hpp"
#include "fanfare_timing.hpp"
#include "movie_timing.hpp"
#include "menu_entry_timing.hpp"
#include "startup_resource.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <deque>
#include <cmath>

namespace comanchecd {
namespace {
enum class MusicEntry { external, continuation };
enum class MusicRepeat { once, loop };
constexpr std::size_t echo_samples = 32;
constexpr std::size_t engine_sound = 3;
constexpr std::size_t launch_radio = 6;
constexpr std::uint16_t cinematic_tail_sound = 13;
constexpr std::uint32_t cinematic_tail_offset = 0x7f7f8184;
constexpr std::size_t unmapped_sample_length = 0xffff;
constexpr std::size_t initialized_music_channels = 10;
constexpr unsigned dma_half_bytes = 2048;
constexpr std::uint8_t dsp_pause_command = 0xd0, dsp_single_cycle_command = 0x14;
constexpr unsigned producer_setup_instructions = 5;
constexpr unsigned producer_sample_instructions = 44;
constexpr unsigned voice_reload_instructions = 17;
constexpr unsigned changed_voice_instructions = 2;
constexpr unsigned dos_cycles_per_second = 20000000;
constexpr std::uint8_t loud_music_volume = 250, soft_music_volume = 115;
constexpr std::uint8_t loaded_soft_music_volume = 100;
constexpr std::array<std::uint8_t, 16> music_channels{0, 1, 2, 3, 4, 5, 6, 7, 99, 8, 99, 99, 99, 99, 99, 99};
constexpr std::array<std::uint8_t, 9> modulator{0, 1, 2, 8, 9, 10, 16, 17, 18};
constexpr std::array<std::uint8_t, 9> carrier{3, 4, 5, 11, 12, 13, 19, 20, 21};
constexpr std::array<std::uint8_t, 8> sample_bias{0, 64, 96, 112, 120, 124, 126, 126};

std::uint32_t integer(const std::vector<std::uint8_t>& bytes, std::size_t position, std::size_t width) {
    if (position > bytes.size() || width > bytes.size() - position) {
        throw std::invalid_argument("Truncated audio resource");
    }
    std::uint32_t result = 0;
    for (std::size_t byte = 0; byte < width; ++byte) {
        result |= std::uint32_t(bytes[position + byte]) << (byte * 8);
    }
    return result;
}

struct Sample {
    std::vector<std::uint8_t> bytes;
    std::uint32_t step = 0;
};

std::vector<Sample> sample_bank(const std::vector<std::uint8_t>& bytes) {
    if (integer(bytes, 0, 2) != 0x314b) {
        throw std::invalid_argument("Expected K1 sampled sound bank");
    }
    const auto count = integer(bytes, 2, 2);
    if (count > 256 || 4 + count * 4 > bytes.size()) {
        throw std::invalid_argument("Invalid sampled sound directory");
    }
    std::vector<Sample> result;
    for (std::size_t index = 0; index < count; ++index) {
        const auto start = integer(bytes, 4 + index * 4, 4);
        const auto size = integer(bytes, start, 2);
        const auto step = integer(bytes, start + 2, 2);
        if (size <= 4 || start > bytes.size() || size > bytes.size() - start) {
            throw std::invalid_argument("Invalid sampled sound range");
        }
        result.push_back({{bytes.begin() + start, bytes.begin() + start + size}, step});
    }
    return result;
}
}

std::uint32_t engine_rate(std::uint8_t power) {
    if (power > 200) {
        throw std::invalid_argument("Engine power exceeds the controller range");
    }
    return 0x4000u + std::min<std::uint32_t>(std::uint32_t(power) << 8, 0x8000u);
}

class MissionAudio::Impl final : public ExecutionClock, public KeyboardPort {
public:
    Impl(AudioTables tables, const std::vector<GameplayPayload>& payloads, AudioEntry entry,
         std::vector<SampleRequest> requests, AudioTracePolicy trace_policy)
        : tables_(std::move(tables)), device_(sample_rate), irq_clock_(entry.irq_phase),
          trace_policy_(trace_policy), requests_(std::move(requests)) {
        if (payloads.size() < 17) {
            throw std::invalid_argument("Missing mission audio payloads");
        }
        effects_ = sample_bank(payloads[0].bytes);
        speech_ = sample_bank(payloads[payloads.size() - 2].bytes);
        announcements_ = sample_bank(payloads[payloads.size() - 4].bytes);
        music_ = payloads[payloads.size() - 5].bytes;
        if (music_.size() < 50 || music_[0] != 'M' || music_[1] != 'S') {
            throw std::invalid_argument("Expected MS music stream");
        }
        if (effects_.size() < engine_sound || speech_.size() < launch_radio) {
            throw std::invalid_argument("Missing initial engine or radio sample");
        }
        silence_.bytes.assign(15, 128);
        for (auto& voice : voices_) {
            voice.current = voice.next = &silence_;
        }
        voices_[2].current = &speech_[launch_radio - 1];
        voices_[2].next = &effects_[engine_sound - 1];
        voices_[2].current_step = voices_[2].current->step;
        voices_[2].next_step = engine_rate(entry.engine_power);
        voices_[2].next_shift = 2;
        for (std::size_t index = 0; index < requests_.size(); ++index) {
            if (requests_[index].voice >= voices_.size() || requests_[index].attenuation >= sample_bias.size() ||
                    requests_[index].bank > SampleBank::silence || requests_[index].mode > SampleMode::queued ||
                    (index > 0 && requests_[index].sample < requests_[index - 1].sample)) {
                throw std::invalid_argument("Invalid sampled sound request order or voice");
            }
        }
        initialize(entry);
    }

    Impl(AudioTables tables, MusicPlaylist playlist, AudioEntry entry, AudioTracePolicy trace_policy)
        : tables_(std::move(tables)), device_(sample_rate),
          irq_clock_(entry.irq_phase), trace_policy_(trace_policy) {
        music_ = std::move(playlist.introduction);
        next_music_ = std::move(playlist.repeat);
        loop_music_ = false;
        if (music_.empty()) {
            music_ = std::move(next_music_);
            next_music_.clear();
            loop_music_ = true;
        }
        digital_enabled_ = false;
        silence_.bytes.assign(15, 128);
        for (auto& voice : voices_) {
            voice.current = voice.next = &silence_;
            voice.reload_pending = entry.startup_files.has_value();
        }
        initialize(entry);
    }

    void set_music_level(MusicLevel level, MusicChange change) {
        music_muted_ = level == MusicLevel::off;
        if (music_muted_) {
            silence_music();
        } else {
            music_volume_ = level == MusicLevel::loud ? loud_music_volume :
                change == MusicChange::loaded ? loaded_soft_music_volume : soft_music_volume;
        }
    }

    void set_sound_level(SoundLevel level) { sound_level_ = level; }
    void set_interrupt_handler(std::function<void()> handler) { timer_observer_ = std::move(handler); }
    void set_keyboard_handler(std::function<void(ExecutionClock&, KeyboardPort&)> handler) {
        if (!scheduler_) { throw std::logic_error("Keyboard IRQs require scheduled audio"); }
        if (!handler) { throw std::invalid_argument("Keyboard interrupt receiver is empty"); }
        scheduler_->set_keyboard_handler([this, handler = std::move(handler)] { handler(*this, *this); });
    }
    void queue_key_byte(std::uint8_t scan) {
        if (!scheduler_) { throw std::logic_error("Keyboard IRQs require scheduled audio"); }
        scheduler_->queue_key_byte(scan);
    }
    void schedule_key_byte(std::uint64_t cycle, std::uint8_t scan) {
        if (!scheduler_) { throw std::logic_error("Keyboard arrivals require scheduled audio"); }
        scheduler_->schedule_key_byte(cycle, scan);
    }
    std::uint8_t read() override { return scheduler_->read_keyboard(); }
    void acknowledge() override { scheduler_->acknowledge_keyboard(); }
    std::uint64_t add_interrupt_handler(std::function<void()> handler) {
        if (!handler || ++interrupt_token_ == 0) {
            throw std::invalid_argument("Invalid timer interrupt subscription");
        }
        interrupt_handlers_.emplace_back(interrupt_token_, std::move(handler));
        return interrupt_token_;
    }
    void remove_interrupt_handler(std::uint64_t token) {
        interrupt_handlers_.erase(std::remove_if(interrupt_handlers_.begin(), interrupt_handlers_.end(),
            [=](const auto& entry) { return entry.first == token; }), interrupt_handlers_.end());
    }

    void change_music(MusicPlaylist playlist) {
        const auto looping = playlist.introduction.empty();
        if (looping) {
            playlist.introduction = std::move(playlist.repeat);
            playlist.repeat.clear();
        }
        cold_music_pending_ = false;
        music_cpu_work([&] { start_music(MusicEntry::external, &playlist, looping ? MusicRepeat::loop : MusicRepeat::once); });
    }

    void enter_mission(const std::vector<GameplayPayload>& payloads, std::uint8_t power) {
        if (payloads.size() < 17) {
            throw std::invalid_argument("Missing mission audio payloads");
        }
        auto effects = sample_bank(payloads[0].bytes);
        auto speech = sample_bank(payloads[payloads.size() - 2].bytes);
        auto announcements = sample_bank(payloads[payloads.size() - 4].bytes);
        if (effects.size() < engine_sound || speech.size() < launch_radio) {
            throw std::invalid_argument("Missing initial engine or radio sample");
        }
        const auto step = engine_rate(power);
        silence_.bytes.assign(15, 128);
        for (auto& voice : voices_) {
            voice = Voice{};
            voice.current = voice.next = &silence_;
        }
        effects_ = std::move(effects);
        speech_ = std::move(speech);
        announcements_ = std::move(announcements);
        cinematic_tail_.reset();
        voices_[2].current = &speech_[launch_radio - 1];
        voices_[2].next = &effects_[engine_sound - 1];
        voices_[2].current_step = voices_[2].current->step;
        voices_[2].next_step = step;
        voices_[2].next_shift = 2;
        digital_enabled_ = true;
        start_dma();
        change_music({{}, payloads[payloads.size() - 5].bytes});
    }

    void begin_mission_loading() {
        digital_enabled_ = false;
        stop_music();
    }

    void prepare_mission_audio(const std::vector<GameplayPayload>& payloads, const StartupFiles& files) {
        if (!scheduler_ || payloads.size() < 17 || digital_enabled_) {
            throw std::logic_error("Mission audio settings require stopped scheduled samples");
        }
        auto effects = sample_bank(payloads[0].bytes);
        auto speech = sample_bank(payloads[payloads.size() - 2].bytes);
        auto announcements = sample_bank(payloads[payloads.size() - 4].bytes);
        if (effects.size() < engine_sound || speech.size() < launch_radio) {
            throw std::invalid_argument("Missing initial engine or radio sample");
        }
        for (const auto& voice : voices_) {
            if (voice.current != &silence_ || voice.next != &silence_) {
                throw std::logic_error("Mission sound banks require cleared voices");
            }
        }
        effects_ = std::move(effects);
        speech_ = std::move(speech);
        announcements_ = std::move(announcements);
        cinematic_tail_.reset();
        apply_startup_settings(*scheduler_, files, SettingsPass::post_start,
            [&](MusicLevel level) { apply_loaded_music_level(level); },
            [&](DigitalCommand command) { startup_digital(command); });
    }

    void queue_initial_engine(std::uint8_t control_power) {
        if (!scheduler_ || effects_.size() < engine_sound) {
            throw std::logic_error("Initial engine requires scheduled mission samples");
        }
        // 194ea tail-calls 5174. A zero throttle queues the shared silence sample.
        execute(CpuOperation::instructions, control_power == 0 ? 10 : 8);
        execute(CpuOperation::disable_interrupts, 1);
        execute(CpuOperation::instructions, 14);
        const auto silent = voices_[2].current == &silence_;
        const SampleRequest request{digital_samples_, control_power == 0 ? SampleBank::silence : SampleBank::effects,
            control_power == 0 ? std::uint16_t(0) : static_cast<std::uint16_t>(engine_sound), 2, 2, 0, SampleMode::queued};
        auto& voice = voices_[2];
        voice.next = control_power == 0 ? &silence_ : &effects_[engine_sound - 1];
        voice.next_step = voice.next->step;
        voice.next_shift = 2;
        if (trace_policy_ == AudioTracePolicy::retain) { resolved_requests_.push_back(request); }
        execute(CpuOperation::instructions, 2);
        execute(CpuOperation::enable_interrupts, 1);
        execute(CpuOperation::instructions, 1);
        if (silent) {
            execute(CpuOperation::disable_interrupts, 1);
            execute(CpuOperation::instructions, 3 + 7 + 5 + 2 + 3 + 2);
            voice.current = voice.next;
            voice.current_step = voice.next_step;
            voice.shift = voice.next_shift;
            voice.position = 4u << 16;
        }
        if (silent) {
            execute(CpuOperation::enable_interrupts, 1);
            execute(CpuOperation::instructions, 1);
        }
    }

    void play_launch_radio() {
        if (!scheduler_ || speech_.size() < launch_radio) {
            throw std::logic_error("Launch radio requires scheduled mission samples");
        }
        // 501d chooses a free speech voice before entering the shared 50a8 setter.
        execute(CpuOperation::instructions, 3 + 3 + 4);
        unsigned selected = 2;
        if (voices_[2].current != voices_[2].next) {
            execute(CpuOperation::instructions, 3);
            selected = 0;
            if (voices_[0].current != &silence_) {
                execute(CpuOperation::instructions, 3);
                selected = 1;
                if (voices_[1].current != &silence_) {
                    execute(CpuOperation::instructions, 2);
                    return;
                }
            }
        }
        const auto* sample = &speech_.at(launch_radio - 1);
        execute(CpuOperation::instructions, 5);
        for (const auto& voice : voices_) {
            if (voice.current == sample) {
                execute(CpuOperation::instructions, 2 + 2);
                return;
            }
            execute(CpuOperation::instructions, 2);
        }
        execute(CpuOperation::instructions, 1);
        execute(CpuOperation::disable_interrupts, 1);
        execute(CpuOperation::instructions, 3 + 7 + 1 + 5);
        if (selected != 0) { execute(CpuOperation::instructions, 2); }
        execute(CpuOperation::instructions, selected == 2 ? 3 : 4);
        execute(CpuOperation::instructions, 2);
        apply_request({digital_samples_, SampleBank::speech, launch_radio, static_cast<std::uint8_t>(selected), 0, 0, SampleMode::immediate});
        execute(CpuOperation::enable_interrupts, 1);
        execute(CpuOperation::instructions, 1 + 2);
    }

    void stop_mission_samples() {
        if (!scheduler_) { throw std::logic_error("Mission loading requires scheduled audio"); }
        execute(CpuOperation::instructions, 2); // 5fd6 selector and CALL.
        disable_menu_samples(*scheduler_, digital_enabled_,
            [&](DigitalCommand command) { startup_digital(command); }, [&] { return reset_silence_voices(); });
        digital_enabled_ = false;
    }

    void clear_mission_samples() {
        if (!scheduler_) { throw std::logic_error("Mission return requires scheduled audio"); }
        execute(CpuOperation::instructions, 1);
        clear_sample_voices(*scheduler_, [&] { return reset_silence_voices(); });
    }

    void save_pilot_record() {
        if (!scheduler_) { throw std::logic_error("Pilot save requires scheduled audio"); }
        constexpr unsigned filename_bytes = 12, filename_byte_work = 12;
        execute(CpuOperation::instructions, 3 + 4 + filename_bytes * filename_byte_work + 8);
        scheduler_->file_metadata(DosFileCall::create);
        execute(CpuOperation::instructions, 6);
        scheduler_->write_file(pilot_record_bytes);
        execute(CpuOperation::instructions, 6);
        scheduler_->file_metadata(DosFileCall::close);
        execute(CpuOperation::instructions, 2);
    }

    void present_debrief(MusicPlaylist playlist, unsigned height) {
        if (!scheduler_) { throw std::logic_error("Debrief presentation requires scheduled audio"); }
        execute(CpuOperation::instructions, 3); // 2beaa score arguments and CALL.
        leave_mission(std::move(playlist));
        execute(CpuOperation::instructions, 1);
        draw_loading_picture(*this, height);
        execute(CpuOperation::instructions, 1);
        flight_work::present(*this);
        execute(CpuOperation::instructions, 1);
        apply_loading_palette(*this);
        execute(CpuOperation::instructions, 1);
        copy_loading_page(*this, height);
    }

    GameplayPayload load_mission_payload(const StartupResource& resource, PayloadKind kind, unsigned filename_bytes) {
        if (!scheduler_) { throw std::logic_error("Mission loading requires scheduled audio"); }
        return kind == PayloadKind::picture ?
            read_loading_picture(*scheduler_, *this, resource, filename_bytes, &mission_input_) :
            read_startup_resource(*scheduler_, *this, resource, filename_bytes, &mission_input_);
    }

    void load_menu_picture(const StartupResource& resource, unsigned filename_bytes) {
        if (!scheduler_) { throw std::logic_error("Menu picture loading requires scheduled audio"); }
        prepare_menu_picture(*scheduler_, *this, resource, filename_bytes);
    }

    void load_campaign(const StartupCampaign& campaign) {
        if (!scheduler_) { throw std::logic_error("Campaign loading requires scheduled audio"); }
        execute(CpuOperation::instructions, 8);
        read_campaign(*scheduler_, campaign);
    }

    void stop_music() {
        music_cpu_work([&] {
            if (startup_stop_pending_) {
                execute(CpuOperation::instructions, 1); // 58cf CALL.
                startup_stop_pending_ = false;
            }
            instruction([&] { music_active_ = false; });
            execute(CpuOperation::instructions, 2);
            silence_music();
            execute(CpuOperation::instructions, 1);
        });
    }

    void start_fanfare(const FanfareAssets& assets) {
        if (!scheduler_ || !cold_music_pending_) {
            throw std::logic_error("Fanfare startup requires deferred cold music");
        }
        stop_music();
        auto score = prepare_fanfare(*scheduler_, *this, assets);
        change_music({std::move(score), {}});
        startup_movie_pending_ = true;
    }

    std::uint64_t load_startup_movie(const StartupFiles& files, const StartupMovieAssets& assets) {
        if (!scheduler_ || !startup_movie_pending_) {
            throw std::logic_error("Startup movie loading must follow cold fanfare entry");
        }
        std::uint64_t origin = 0;
        prepare_startup_movie(*scheduler_, *this, files, assets,
            [&](MusicLevel level) { apply_loaded_music_level(level); },
            [&](DigitalCommand command) { startup_digital(command); }, [&] { origin = irq_ticks(); });
        startup_movie_pending_ = false;
        startup_menu_pending_ = true;
        return origin;
    }

    void enter_pilot_menu(const PilotMenuAssets& assets) {
        if (!scheduler_ || !startup_menu_pending_) {
            throw std::logic_error("Pilot-menu loading requires completed startup movie loading");
        }
        prepare_pilot_menu(*scheduler_, *this, assets, digital_enabled_,
            [&](DigitalCommand command) { startup_digital(command); },
            [&] { return reset_silence_voices(); },
            [&] { change_music({assets.scores[0].encoded, assets.scores[1].encoded}); });
        startup_menu_pending_ = false;
    }

    void read_file(unsigned bytes) override {
        if (scheduler_) { scheduler_->read_file(bytes); }
    }

    void suspend_engine() {
        if (digital_enabled_) {
            apply_request({digital_samples_, SampleBank::silence, 0, 2, 0, 0, SampleMode::queued});
        }
    }

    void enter_cinematic(const std::vector<std::uint8_t>& bank, MusicPlaylist playlist, CinematicLoopStart loop) {
        auto effects = sample_bank(bank);
        if (effects.size() < engine_sound) {
            throw std::invalid_argument("Missing cinematic loop sample");
        }
        silence_.bytes.assign(15, 128);
        leave_mission(std::move(playlist));
        effects_ = std::move(effects);
        // TANIM.SND's final CR indexes past SND1's directory into sample bytes.
        // The qualified DOSBox-X call reads an unmapped FFFF/FFFF sample header.
        if (effects_.size() == 7 && bank.size() >= cinematic_tail_sound * 4 + 4 &&
                integer(bank, cinematic_tail_sound * 4, 4) == cinematic_tail_offset) {
            cinematic_tail_ = std::make_unique<Sample>(Sample{std::vector<std::uint8_t>(unmapped_sample_length, 255), 0xffff});
        }
        digital_enabled_ = true;
        start_dma();
        cinematic_loop_pending_ = true;
        if (loop == CinematicLoopStart::immediate) { start_cinematic_loop(); }
    }

    void start_cinematic_loop() {
        if (!cinematic_loop_pending_) { throw std::logic_error("No cinematic loop is pending"); }
        if (scheduler_) {
            // 5c0d arguments, queued request 5174, then immediate 50a8 on a silent voice.
            execute(CpuOperation::instructions, 7);
            execute(CpuOperation::disable_interrupts, 1);
            execute(CpuOperation::instructions, 16);
            execute(CpuOperation::enable_interrupts, 1);
            execute(CpuOperation::instructions, 1);
            execute(CpuOperation::disable_interrupts, 1);
            execute(CpuOperation::instructions, 22);
        }
        apply_request({digital_samples_, SampleBank::effects, engine_sound, 2, 1, 0, SampleMode::queued});
        cinematic_loop_pending_ = false;
        if (scheduler_) {
            execute(CpuOperation::enable_interrupts, 1);
            execute(CpuOperation::instructions, 1);
        }
    }

    void leave_mission(MusicPlaylist playlist) {
        digital_enabled_ = false;
        digital_value_ = 128;
        for (auto& voice : voices_) {
            voice = Voice{};
            voice.current = voice.next = &silence_;
        }
        cinematic_tail_.reset();
        cinematic_loop_pending_ = false;
        change_music(std::move(playlist));
    }

    void resume_engine(std::uint8_t power) {
        if (digital_enabled_) {
            apply_request({digital_samples_, SampleBank::effects, engine_sound, 2, 2,
                           engine_rate(power), SampleMode::queued});
        }
    }

    std::vector<std::int16_t> take_pending_audio() {
        std::vector<std::int16_t> result(scheduled_output_.begin(), scheduled_output_.end());
        scheduled_output_.clear();
        if (scheduler_) {
            const auto available = scheduler_->take_output();
            result.reserve(result.size() + available.size());
            for (const auto& sample : available) { result.push_back(sample[0]); }
        }
        return result;
    }

    std::vector<std::int16_t> render_foreground(std::size_t count, const std::function<void()>& foreground) {
        if (!scheduler_ || !foreground) { throw std::invalid_argument("Foreground rendering requires a scheduled clock and CPU work"); }
        return render_scheduled(count, {}, foreground);
    }

    std::vector<std::int16_t> render(std::size_t count, const std::function<void()>& interrupt) {
        if (scheduler_) { return render_scheduled(count, interrupt); }
        std::vector<std::int16_t> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            if (irq_clock_.advance(1) != 0) {
                ++tick_;
                music_tick();
                if (interrupt) {
                    interrupt();
                } else {
                    notify_interrupt();
                }
            }
            digital_phase_ += Sb16Playback::source_rate;
            while (digital_phase_ >= sample_rate) {
                digital_phase_ -= sample_rate;
                while (request_cursor_ < requests_.size() && requests_[request_cursor_].sample == digital_samples_) {
                    apply_request(requests_[request_cursor_++]);
                }
                digital_value_ = digital_enabled_ && sound_level_ != SoundLevel::off ? mix_sample() : 128;
                device_.queue_digital(digital_value_);
                ++digital_samples_;
                if (trace_policy_ == AudioTracePolicy::retain) {
                    digital_.push_back(digital_value_);
                }
            }
            result.push_back(device_.render_sample());
        }
        return result;
    }

    const std::vector<OplWrite>& writes() const { return writes_; }
    const std::vector<MusicIrqWork>& music_irq_work() const { return music_irq_work_; }
    const std::vector<std::uint8_t>& digital_pcm() const { return digital_; }
    const std::vector<SampleRequest>& sample_requests() const { return resolved_requests_; }
    std::uint64_t irq_ticks() const { return tick_; }

    ExecutionClock& execution_clock() {
        if (!scheduler_) { throw std::logic_error("CPU work requires scheduled audio"); }
        return *this;
    }

    void execute(CpuOperation operation, unsigned count) override {
        if (collecting_irq_work_) {
            irq_state_work_.emplace_back(operation, count);
            return;
        }
        if (scheduler_) { scheduler_->execute(operation, count); }
    }
    void instruction(const std::function<void()>& action) override {
        if (scheduler_) {
            scheduler_->instruction(action);
        } else {
            action();
        }
    }
    bool read_retrace() override { return scheduler_->read_retrace(); }
    std::uint64_t cycles() const override { return scheduler_->cycles(); }
    std::uint64_t sample_irq_ticks() override {
        std::uint64_t value = 0;
        scheduler_->instruction([&] { value = tick_; });
        return value;
    }

    void play_effect(std::uint16_t sound, std::uint8_t attenuation,
                     EffectPolicy policy = EffectPolicy::available_first, std::uint32_t step = 0) {
        const auto* sample = effect_sample(sound);
        if (sample == nullptr || attenuation >= sample_bias.size()) {
            throw std::invalid_argument("Invalid native effect request");
        }
        execute(CpuOperation::instructions, 5); // 5094 bank lookup.
        execute(CpuOperation::disable_interrupts, 1);
        execute(CpuOperation::instructions, 5); // PUSH, signed policy gate and FE test.
        auto selected = voices_.size();
        if (policy == EffectPolicy::available_first) {
            execute(CpuOperation::instructions, 2);
            for (std::size_t index = 0; index < voices_.size(); ++index) {
                if (index != 0) { execute(CpuOperation::instructions, 1); }
                if (voice_matches(index, &silence_)) { selected = index; break; }
            }
        }
        if (selected == voices_.size()) {
            execute(CpuOperation::instructions, 1);
            for (std::size_t index = 0; index < voices_.size(); ++index) {
                if (index != 0) { execute(CpuOperation::instructions, 1); }
                if (voice_matches(index, sample)) { selected = index; break; }
            }
        }
        if (selected == voices_.size()) {
            execute(CpuOperation::instructions, 2);
            if (attenuation != 0) {
                finish_sample_dispatch();
                return;
            }
            execute(CpuOperation::instructions, 1);
            selected = 0;
        }
        dispatch_sample({digital_samples_, SampleBank::effects, sound, static_cast<std::uint8_t>(selected), attenuation,
                         step, SampleMode::immediate});
    }

    void set_engine_power(std::uint8_t power) {
        voices_[2].next_step = engine_rate(power);
    }

    void play_cue(const SoundCue& cue) {
        if (cue.bank == SampleBank::effects) {
            play_effect(cue.sound, cue.attenuation, cue.policy, cue.step);
            return;
        }
        const auto& bank = cue.bank == SampleBank::speech ? speech_ : announcements_;
        if ((cue.bank != SampleBank::speech && cue.bank != SampleBank::announcements) ||
                cue.sound == 0 || cue.sound > bank.size() || cue.attenuation != 0) {
            throw std::invalid_argument("Unsupported native sound cue");
        }
        execute(CpuOperation::instructions, cue.bank == SampleBank::speech ? 3 + (cue.sound == 1 ? 2 : 0) : 2);
        execute(CpuOperation::instructions, 4); // PUSH ECX, bank test and MOV BL,2.
        const Sample* next = nullptr;
        instruction([&] { next = voices_[2].next; });
        auto selected = std::size_t(2);
        if (!voice_matches(2, next)) {
            execute(CpuOperation::instructions, 1);
            selected = 0;
            if (!voice_matches(0, &silence_)) {
                execute(CpuOperation::instructions, 1);
                selected = 1;
                if (!voice_matches(1, &silence_)) {
                    execute(CpuOperation::instructions, 2);
                    return;
                }
            }
        }
        const auto* sample = &bank[cue.sound - 1];
        execute(CpuOperation::instructions, 5);
        for (std::size_t index = 0; index < voices_.size(); ++index) {
            if (voice_matches(index, sample)) {
                execute(CpuOperation::instructions, 2);
                return;
            }
        }
        execute(CpuOperation::instructions, 1);
        execute(CpuOperation::disable_interrupts, 1);
        execute(CpuOperation::instructions, 3);
        dispatch_sample({digital_samples_, cue.bank, cue.sound, static_cast<std::uint8_t>(selected), 0, 0, SampleMode::immediate});
        execute(CpuOperation::instructions, 2);
    }

private:
    bool voice_matches(std::size_t index, const Sample* sample) {
        bool matches = false;
        instruction([&] { matches = voices_[index].current == sample; });
        execute(CpuOperation::instructions, 1);
        return matches;
    }

    void finish_sample_dispatch() {
        execute(CpuOperation::instructions, 1);
        execute(CpuOperation::enable_interrupts, 1);
        execute(CpuOperation::instructions, 1);
    }

    void dispatch_sample(const SampleRequest& request) {
        // 50fb..5173 runs with IRQs masked. The caller owns the CLI and routing gates.
        execute(CpuOperation::instructions, 7 + unsigned(request.step == 0) + 5);
        if (request.voice != 0) { execute(CpuOperation::instructions, 2); }
        execute(CpuOperation::instructions, request.voice == 2 ? 3 : 4);
        execute(CpuOperation::instructions, 1);
        apply_request(request);
        finish_sample_dispatch();
    }

    unsigned reset_silence_voices() {
        unsigned changed = 0;
        for (auto& voice : voices_) {
            voice.next = &silence_;
            voice.next_step = 0;
            voice.shift = voice.next_shift = 0;
            if (voice.current == &silence_) { continue; }
            ++changed;
            voice.current = &silence_;
            voice.position = 0;
            voice.current_step = 4u << 16;
            voice.reload_pending = false;
        }
        return changed;
    }

    const Sample* effect_sample(std::uint16_t sound) const {
        if (sound == cinematic_tail_sound && cinematic_tail_) {
            return cinematic_tail_.get();
        }
        return sound != 0 && sound <= effects_.size() ? &effects_[sound - 1] : nullptr;
    }
    void apply_loaded_music_level(MusicLevel level) {
        if (level == MusicLevel::off) {
            music_cpu_work([&] { silence_music(); });
            music_muted_ = true;
        } else {
            set_music_level(level, MusicChange::loaded);
        }
    }

    void startup_digital(DigitalCommand command) {
        if (command == DigitalCommand::stop) {
            scheduler_->halt_dma();
            digital_enabled_ = dma_running_ = false;
            return;
        }
        digital_enabled_ = true;
        start_dma();
    }

    void initialize(const AudioEntry& entry) {
        if (entry.startup_files && (entry.startup != AudioStartup::cold ||
                entry.execution != AudioExecution::scheduled || digital_enabled_)) {
            throw std::invalid_argument("Startup files require cold scheduled music-only entry");
        }
        if (entry.calibration == AudioCalibration::pit2 && (entry.startup != AudioStartup::cold || digital_enabled_)) {
            throw std::invalid_argument("PIT2 calibration requires cold music-only startup");
        }
        if (entry.execution != AudioExecution::scheduled &&
            (entry.startup == AudioStartup::cold || entry.before_music_device || entry.cpu_observer || entry.pit_start != PitStart::periodic)) {
            throw std::invalid_argument("Cold device startup requires scheduled execution");
        }
        music_callback_installed_ = entry.startup != AudioStartup::cold;
        if (entry.interrupt) {
            timer_observer_ = [this, interrupt = entry.interrupt] {
                interrupt([this](CpuOperation operation, unsigned count) { execute(operation, count); });
            };
        }
        if (entry.execution == AudioExecution::scheduled) {
            if (entry.irq_phase != 0 || entry.opl_delay_iterations == 0 || entry.opl_delay_iterations > 1000000) {
                throw std::invalid_argument("Unsupported scheduled audio entry timing");
            }
            scheduler_ = std::make_unique<AudioScheduler>(sample_rate);
            scheduler_->set_cpu_observer(entry.cpu_observer);
            scheduler_->enable_interrupts(false);
            scheduler_->set_interrupt_delay(838);
            opl_delay_iterations_ = entry.opl_delay_iterations;
            scheduler_->configure(AudioChannel::fm, {entry.startup != AudioStartup::cold, sample_rate,
                {audio_gain_one * 3 / 2, audio_gain_one * 3 / 2}, 0});
            const double interval = 1.0 / sample_rate;
            const double tau = 1.0 / (5555 * 2 * 3.14159265358979323846);
            // SB16 keeps its mixer channel enabled while DMA is halted.
            digital_config_ = {true, Sb16Playback::source_rate,
                {audio_gain_one, audio_gain_one}, static_cast<std::int32_t>(interval / (tau + interval) * 65536)};
            scheduler_->configure(AudioChannel::digital, digital_config_);
            for (auto& half : dma_buffers_) { half.assign(dma_half_bytes, 128); }
            start_dma();
            scheduler_->set_dma_handler([this] {
                if (!digital_enabled_) {
                    dma_running_ = false;
                    return;
                }
                produce_dma();
            });
            if (entry.calibration == AudioCalibration::pit2) {
                opl_delay_iterations_ = calibrate_opl_delay(*scheduler_);
                // Timer descriptor setup and writer prefix through the high-byte OUT.
                scheduler_->execute(CpuOperation::instructions, 29);
            }
            const auto timer = [this] {
                scheduler_->execute(CpuOperation::instructions, 2);
                scheduler_->execute(CpuOperation::port_write_byte, 1);
                scheduler_->execute(CpuOperation::disable_interrupts, 1);
                scheduler_->execute(CpuOperation::instructions, 7);
                ++tick_;
                const auto before = scheduler_->accounting();
                const auto first_write = writes_.size();
                if (music_callback_installed_) {
                    music_cpu_work([&] { music_tick(); });
                } else {
                    scheduler_->execute(CpuOperation::instructions, 1); // Uninstalled callback is RET.
                }
                if (trace_policy_ == AudioTracePolicy::retain) {
                    auto work = scheduler_->accounting();
                    work.dispatches -= before.dispatches;
                    work.string_iterations -= before.string_iterations;
                    work.repeat_restarts -= before.repeat_restarts;
                    work.special_stosb_iterations -= before.special_stosb_iterations;
                    work.terminal_charges -= before.terminal_charges;
                    work.io_cycles -= before.io_cycles;
                    music_irq_work_.push_back({tick_, first_write, writes_.size(), work});
                }
                scheduler_->execute(CpuOperation::instructions, 7); // Counter/input test and RNG through ROR.
                irq_state_work_.clear();
                scheduler_->instruction([this] {
                    // Receivers commit canonical state in the RNG store. Their
                    // reported shake work retires after the shared four-instruction test.
                    collecting_irq_work_ = true;
                    try {
                        if (active_interrupt_) { active_interrupt_(); }
                        else { notify_interrupt(); }
                    } catch (...) {
                        collecting_irq_work_ = false;
                        throw;
                    }
                    collecting_irq_work_ = false;
                });
                scheduler_->execute(CpuOperation::instructions, 4);
                for (const auto& work : irq_state_work_) { scheduler_->execute(work.first, work.second); }
                scheduler_->execute(CpuOperation::instructions, 8); // Saved registers and IRET.
            };
            if (entry.calibration == AudioCalibration::pit2) {
                scheduler_->program_pit(mission_pit_divisor, timer, entry.pit_start);
            } else {
                scheduler_->set_pit(mission_pit_divisor, timer, entry.pit_start);
            }
            if (entry.startup == AudioStartup::cold) { scheduler_->enable_interrupts(true); }
            if (entry.before_music_device) { entry.before_music_device(*this); }
        }
        echo_ = entry.echo;
        echo_cursor_ = entry.echo_cursor;
        music_volume_ = entry.music_volume;
        if (entry.startup == AudioStartup::cold) {
            if (entry.startup_files) { pre_fm_startup(*scheduler_, *entry.startup_files); }
            music_cpu_work([&] { initialize_cold_fm(); });
            if (entry.startup_files && entry.startup_files->campaigns) {
                prepare_startup_campaigns(*scheduler_, *entry.startup_files);
                prepare_startup_video(*scheduler_);
                scheduler_->execute(CpuOperation::instructions, 1); // 5834 presenter CALL.
                flight_work::present(*this);
                if (entry.startup_files->loading) {
                    prepare_startup_loading(*scheduler_, *entry.startup_files, entry.startup_state,
                        [&](MusicLevel level) { apply_loaded_music_level(level); }, {},
                        [&](DigitalCommand command) { startup_digital(command); });
                    startup_stop_pending_ = entry.music_start == MusicStart::deferred;
                }
            }
            cold_music_pending_ = entry.music_start == MusicStart::deferred;
            if (entry.music_start == MusicStart::immediate) { start_music(); }
            return;
        }
        write(1, 0x20);
        write(8, 0);
        write(0xbd, 0xc0);
        for (std::size_t channel = 0; channel < carrier.size(); ++channel) {
            write(0x40 | carrier[channel], 63);
            write(0xb0 | channel, 0);
            if (entry.frequency_high[channel] > 0x1f) {
                throw std::invalid_argument("Audio entry frequency cache must have key-off state");
            }
            frequency_high_[channel] = entry.frequency_high[channel];
            program(channel, 0);
        }
        music_callback_installed_ = true;
        if (entry.music_start == MusicStart::immediate) {
            start_music();
        } else {
            music_active_ = false;
        }
        if (scheduler_) { scheduler_->enable_interrupts(true); }
    }

    void initialize_cold_fm() {
        music_work(3); // Detection CALL, port and first timer word.
        write(4, 0x60);
        music_work(1);
        write(4, 0x80);
        const auto initial_status = scheduler_->read_fm_status() & 0xe0;
        music_work(3);
        write(2, 0xff);
        music_work(1);
        write(4, 0x21);
        music_work(1);
        constexpr unsigned detection_wait_rounds = 19;
        for (unsigned round = 0; round < detection_wait_rounds; ++round) {
            music_work(2 + 2 * opl_delay_iterations_);
        }
        const auto overflow_status = (scheduler_->read_fm_status() & 0xe0) ^ 0xc0;
        music_work(4);
        write(4, 0x60);
        music_work(1);
        write(4, 0x80);
        music_work(5);
        if (initial_status != 0 || overflow_status != 0) {
            throw std::runtime_error("Portable OPL timer detection failed");
        }
        music_work(25); // Detection dispatcher return, caller and initializer dispatcher.
        music_work(26); // Fresh-driver selection and hardware initializer CALL.
        music_work(1);
        write(1, 0x20);
        music_work(1);
        write(8, 0);
        music_work(1);
        write(0xbd, 0xc0);
        music_work(3); // Physical reset CALL, count and first voice.
        for (std::size_t physical = 0; physical < carrier.size(); ++physical) {
            music_work(5);
            write(0x40 | carrier[physical], 63);
            music_work(2);
            write(0xb0 | physical, 0);
            music_work(2);
        }
        music_work(3); // Reset RET and logical programming loop setup.
        for (std::size_t channel = 0; channel < carrier.size(); ++channel) {
            music_work(3);
            instruction([&] { frequency_high_[channel] = 0x1f; });
            music_work(3);
            program(channel, 0);
            music_work(4);
        }
        music_work(5); // Hardware result/RET, caller test and silence CALL.
        instruction([&] { music_active_ = false; });
        music_work(2);
        silence_music();
        music_work(1);
        constexpr unsigned channel_initializer_instructions = 11;
        music_work(2 + initialized_music_channels * channel_initializer_instructions + 1);
        instruction([&] { music_callback_installed_ = true; });
        music_work(2);
    }

    struct Voice {
        const Sample* current = nullptr;
        const Sample* next = nullptr;
        std::uint32_t position = 4u << 16;
        std::uint32_t current_step = 0, next_step = 0;
        std::uint8_t shift = 0, next_shift = 0;
        bool reload_pending = false;
    };

    void write(std::uint8_t reg, std::uint8_t value) {
        if (scheduler_) {
            scheduler_->write_fm_timed(reg, value, opl_delay_iterations_, [&] {
                if (trace_policy_ == AudioTracePolicy::retain) { writes_.push_back({tick_, reg, value}); }
            });
            return;
        }
        if (trace_policy_ == AudioTracePolicy::retain) {
            writes_.push_back({tick_, reg, value});
        }
        device_.write_fm(reg, value);
    }

    void apply_request(const SampleRequest& request) {
        const Sample* sample = &silence_;
        if (request.bank == SampleBank::effects) {
            sample = effect_sample(request.sound);
            if (sample == nullptr) {
                throw std::invalid_argument("Sample request exceeds its sound bank");
            }
        } else if (request.bank != SampleBank::silence) {
            const auto& bank = request.bank == SampleBank::effects ? effects_ :
                               request.bank == SampleBank::speech ? speech_ : announcements_;
            if (request.sound == 0 || request.sound > bank.size()) {
                throw std::invalid_argument("Sample request exceeds its sound bank");
            }
            sample = &bank[request.sound - 1];
        }
        auto& voice = voices_[request.voice];
        const auto step = request.step == 0 ? sample->step : request.step;
        if (trace_policy_ == AudioTracePolicy::retain) {
            resolved_requests_.push_back(request);
        }
        if (request.mode == SampleMode::queued) {
            voice.next = sample;
            voice.next_shift = request.attenuation;
            voice.next_step = step;
            if (voice.current != &silence_) {
                return;
            }
        }
        voice.current = sample;
        voice.current_step = step;
        voice.shift = request.attenuation;
        voice.position = 4u << 16;
    }

    void music_work(unsigned instructions) {
        if (scheduler_ && music_work_depth_ != 0) { scheduler_->execute(CpuOperation::instructions, instructions); }
    }
    void notify_interrupt() {
        if (!interrupt_handlers_.empty()) {
            const auto handler = interrupt_handlers_.back().second;
            handler();
        } else if (timer_observer_) {
            timer_observer_();
        }
    }
    void music_cpu_work(const std::function<void()>& action) {
        ++music_work_depth_;
        try {
            action();
        } catch (...) {
            --music_work_depth_;
            throw;
        }
        --music_work_depth_;
    }

    void program(std::size_t channel, std::uint8_t instrument) {
        music_work(4);
        const auto physical = music_channels.at(channel);
        if (physical >= carrier.size()) {
            music_work(1);
            return;
        }
        const auto& data = tables_.instruments.at(instrument);
        const auto voice_operator = carrier[physical];
        const auto modulation_operator = modulator[physical];
        music_work(10);
        write(0xb0 | physical, frequency_high_[physical] & 0xdf);
        music_work(3);
        write(0x20 | voice_operator, data[0]);
        music_work(3);
        write(0x60 | voice_operator, data[2]);
        music_work(3);
        write(0x80 | voice_operator, data[3]);
        music_work(3);
        write(0xe0 | voice_operator, data[4]);
        music_work(3);
        write(0x20 | modulation_operator, data[5]);
        music_work(4);
        write(0x40 | modulation_operator, data[6] ^ 63);
        music_work(3);
        write(0x60 | modulation_operator, data[7]);
        music_work(3);
        write(0x80 | modulation_operator, data[8]);
        music_work(3);
        write(0xe0 | modulation_operator, data[9]);
        music_work(3);
        write(0xc0 | physical, data[10]);
        level_flags_[physical] = data[1];
        music_work(3);
    }

    void select_program(std::size_t channel, std::uint8_t instrument) {
        if (instrument >= tables_.instruments.size()) {
            throw std::invalid_argument("Unsupported music instrument");
        }
        music_work(12);
        programs_[channel] = instrument;
        cached_programs_[channel] = instrument;
        program(channel, instrument);
        envelope_positions_[channel] = 0;
    }

    void note(std::size_t channel, std::uint8_t pitch, std::uint8_t velocity) {
        music_work(music_muted_ ? 3 : 5);
        const auto physical = music_channels.at(channel);
        if (pitch != 0 && !music_muted_) {
            music_work(16);
            const auto instrument = programs_[channel];
            const auto volume = tables_.volume[instrument];
            const auto scale = static_cast<std::uint8_t>(((unsigned(volume) * music_volume_) >> 8) + 1);
            const auto doubled = static_cast<std::uint8_t>(velocity << 1);
            const auto scaled = static_cast<std::uint8_t>((unsigned(scale) * doubled) >> 8);
            envelope_positions_[channel] = 0;
            current_pitch_[channel] = static_cast<std::uint8_t>((pitch << 1) + tables_.pitch[instrument]);
            last_pitch_[channel] = 255;
            music_work(3);
            if (physical < carrier.size()) {
                music_work(8);
                const auto unclamped_level = (scaled >> 2) + 32;
                if (unclamped_level >= 63) { music_work(1); }
                music_work(5);
                const auto level = std::min<unsigned>((scaled >> 2) + 32, 63);
                write(0x40 | carrier[physical], (level_flags_[physical] | level) ^ 63);
                music_work(5);
                frequency_high_[physical] |= 0x20;
                write(0xb0 | physical, frequency_high_[physical]);
                music_work(1);
            }
            music_work(1);
            return;
        }
        music_work(6);
        if (physical < carrier.size()) {
            music_work(9);
            frequency_high_[physical] &= 0xdf;
            write(0xb0 | physical, frequency_high_[physical]);
        }
        music_work(1);
    }

    void silence_music() {
        music_work(2);
        for (std::size_t channel = 0; channel < initialized_music_channels; ++channel) {
            music_work(8);
            pitch_bend_[channel] = 0;
            note(channel, 0, 0);
            music_work(6);
        }
        music_work(1);
    }

    void start_music(MusicEntry entry = MusicEntry::external, MusicPlaylist* incoming = nullptr,
                     MusicRepeat repeat = MusicRepeat::once) {
        const auto& score = incoming ? incoming->introduction : music_;
        if (score.size() < 50 || score[0] != 'M' || score[1] != 'S') {
            throw std::invalid_argument("Expected MS music stream");
        }
        if (incoming) {
            music_work(4);
            instruction([&] { loop_music_ = repeat == MusicRepeat::loop; });
            music_work(5);
        } else {
            music_work(entry == MusicEntry::external ? 10 : 6);
        }
        if (scheduler_ && music_work_depth_ != 0) { scheduler_->execute(CpuOperation::repeat_copy_bytes, 10); }
        music_work(3);
        for (std::size_t channel = 0; channel < initialized_music_channels; ++channel) {
            const auto program = score.at(32 + channel);
            music_work(1);
            instruction([&] { initial_programs_[channel] = program; });
            instruction([&] { cached_programs_[channel] = 255; });
            music_work(3);
        }
        music_work(1);
        // 2f778 publishes the new decoder cursor. Keep the old allocation alive until then.
        instruction([&] {
            if (incoming) {
                music_ = std::move(incoming->introduction);
                next_music_ = std::move(incoming->repeat);
            }
            position_ = 48;
        });
        music_work(1);
        instruction([&] { music_active_ = false; });
        music_work(2);
        silence_music();
        music_work(4);
        wait_ = 1;
        music_active_ = true;
    }

    std::uint8_t byte() {
        if (position_ >= music_.size()) {
            throw std::invalid_argument("Truncated MS music event");
        }
        return music_[position_++];
    }

    enum class MusicEventResult { scheduled, stream_end };

    MusicEventResult event() {
        music_work(5); // CALL 2f794 and stream/end-marker test.
        if (position_ + 2 <= music_.size() && music_[position_] == 0xff && music_[position_ + 1] == 0x2f) {
            music_work(6); // Parser return, caller test, wait reset and silence call.
            silence_music();
            if (!next_music_.empty()) {
                music_work(7);
                music_ = std::move(next_music_);
                next_music_.clear();
                loop_music_ = true;
            } else if (!loop_music_) {
                music_work(7);
                music_active_ = false;
                wait_ = 1;
                return MusicEventResult::stream_end;
            } else {
                music_work(8);
            }
            start_music(MusicEntry::continuation);
            return MusicEventResult::stream_end;
        }
        music_work(8);
        const auto status = byte();
        const auto channel = status & 15;
        const auto command = status & 0xf0;
        if ((command == 0x90 || command == 0x80 || command == 0xc0 || command == 0xe0) &&
                channel >= music_channels.size()) {
            throw std::invalid_argument("Unsupported MS music channel at " + std::to_string(position_ - 1) +
                                        " status=" + std::to_string(status));
        }
        std::uint8_t pitch = 0, velocity = 0;
        if (command == 0x90) {
            music_work(7);
            pitch = byte();
            velocity = byte();
        } else if (command == 0x80) {
            music_work(8);
            pitch = 0;
        } else if (command == 0xc0) {
            music_work(9);
            initial_programs_[channel] = byte();
            select_program(channel, initial_programs_[channel]);
            music_work(7);
        } else if (command == 0xe0) {
            music_work(15);
            pitch_bend_[channel] = byte();
        } else if (command == 0xb0) {
            music_work(16);
            byte();
            byte();
        } else {
            music_work(command == 0xd0 ? 12 : 16);
            // 2f7fb..2f80c skips one data byte for all remaining status classes.
            byte();
        }
        auto delay = unsigned(byte());
        music_work(3);
        if (delay & 128) {
            music_work(7);
            delay = ((delay & 127) << 7) | byte();
        }
        wait_ = static_cast<std::uint16_t>(delay);
        music_work(9); // Parser return and caller event/channel tests.
        if (command != 0x90 && command != 0x80) {
            music_work(2);
            return MusicEventResult::scheduled;
        }
        channel_link(channel, pitch, velocity);
        std::uint16_t visited = std::uint16_t(1u << channel);
        std::size_t linked = channel;
        for (;;) {
            const auto next = linked < initialized_music_channels ? music_.at(16 + linked) : 0;
            music_work(4);
            if (next == 0) {
                music_work(2);
                return MusicEventResult::scheduled;
            }
            music_work(2);
            linked = next - 1;
            if (linked >= music_channels.size() || (visited & (1u << linked))) {
                throw std::invalid_argument("Invalid or cyclic MS linked channel");
            }
            visited |= std::uint16_t(1u << linked);
            channel_link(linked, pitch, velocity);
        }
    }

    void channel_link(std::size_t channel, std::uint8_t pitch, std::uint8_t velocity) {
        music_work(7);
        if (cached_programs_[channel] != initial_programs_[channel]) {
            music_work(4);
            select_program(channel, initial_programs_[channel]);
            music_work(2);
        }
        music_work(4);
        note(channel, pitch, velocity);
        music_work(2);
    }

    void music_tick() {
        music_work(2);
        if (music_active_) {
            music_work(3);
        }
        if (music_active_ && --wait_ == 0) {
            auto event_result = MusicEventResult::scheduled;
            std::size_t budget = music_.size();
            do {
                if (budget-- == 0) {
                    throw std::invalid_argument("MS zero-delay event loop exceeds stream bound");
                }
                event_result = event();
            } while (wait_ == 0);
            if (event_result == MusicEventResult::scheduled) { music_work(1); }
        } else if (music_active_) {
            music_work(1);
        }
        music_work(1);
        for (std::size_t reverse = initialized_music_channels; reverse > 0; --reverse) {
            music_work(8);
            const auto channel = reverse - 1;
            auto pitch = static_cast<std::uint8_t>(current_pitch_[channel] + pitch_bend_[channel]);
            const auto& envelope = tables_.envelopes[programs_[channel]];
            auto& position = envelope_positions_[channel];
            if (envelope.empty()) {
                throw std::invalid_argument("Missing instrument envelope");
            }
            if (envelope.at(position) == 0x81) {
                music_work(9);
                position = 0;
            }
            if (envelope.at(position) != 0x80) {
                music_work(5);
                pitch = static_cast<std::uint8_t>(pitch + envelope.at(position++));
            }
            music_work(3);
            if (pitch == last_pitch_[channel]) {
                music_work(2);
                continue;
            }
            last_pitch_[channel] = pitch;
            music_work(7);
            const auto physical = music_channels[channel];
            if (physical >= carrier.size()) {
                music_work(4);
                continue;
            }
            const auto frequency = tables_.frequencies[pitch];
            frequency_high_[physical] = static_cast<std::uint8_t>((frequency >> 8) | (frequency_high_[physical] & 0xe0));
            music_work(11);
            write(0xa0 | physical, static_cast<std::uint8_t>(frequency));
            music_work(3);
            write(0xb0 | physical, frequency_high_[physical]);
            music_work(4);
        }
        music_work(2);
    }

    void poll_dsp() {
        bool busy;
        do {
            busy = scheduler_->read_dsp_busy();
            scheduler_->execute(CpuOperation::instructions, 3); // JMP, OR and JS.
        } while (busy);
    }

    void begin_dma() {
        // 21d0..22be, active primary IRQ7 path. Port ordering affects DSP polling.
        const auto instructions = [this](unsigned count) { scheduler_->execute(CpuOperation::instructions, count); };
        const auto port_write = [this] { scheduler_->execute(CpuOperation::port_write_byte, 1); };
        instructions(6);
        port_write();
        scheduler_->execute(CpuOperation::port_read_byte, 1);
        instructions(12);
        scheduler_->execute(CpuOperation::port_read_byte, 1);
        instructions(2);
        poll_dsp();
        instructions(1);
        scheduler_->write_dsp_timed(dsp_pause_command);
        instructions(2);
        port_write();
        instructions(2);
        port_write();
        instructions(3);
        port_write();
        instructions(6);
        port_write();
        instructions(2);
        port_write();
        instructions(5);
        port_write();
        instructions(4);
        port_write();
        instructions(2);
        port_write();
        instructions(2);
        port_write();
        instructions(3);
        poll_dsp();
        instructions(1);
        scheduler_->write_dsp_timed(dsp_single_cycle_command);
        poll_dsp();
        instructions(1);
        scheduler_->write_dsp_timed((dma_half_bytes - 1) & 255);
        poll_dsp();
        instructions(1);
        scheduler_->write_dsp_timed((dma_half_bytes - 1) >> 8, [this] {
            scheduler_->arm_dma(dma_buffers_[producer_half_]);
        });
        instructions(2);
        port_write();
        instructions(1); // CALL 5420.
    }

    void start_dma() {
        if (!scheduler_ || !digital_enabled_ || dma_running_) { return; }
        if (!digital_config_.enabled) {
            digital_config_.enabled = true;
            scheduler_->configure(AudioChannel::digital, digital_config_);
        }
        for (auto& half : dma_buffers_) { std::fill(half.begin(), half.end(), 128); }
        producer_half_ = 0;
        scheduler_->arm_dma(dma_buffers_[producer_half_]);
        dma_running_ = true;
    }

    void produce_dma() {
        begin_dma();
        producer_half_ ^= 1;
        constexpr unsigned producer_prologue_instructions = 7, producer_return_and_irq_tail = 9;
        scheduler_->execute(CpuOperation::instructions, producer_prologue_instructions + producer_setup_instructions);
        for (auto& sample : dma_buffers_[producer_half_]) {
            while (request_cursor_ < requests_.size() && requests_[request_cursor_].sample == digital_samples_) {
                apply_request(requests_[request_cursor_++]);
            }
            std::uint64_t work = 0;
            sample = digital_enabled_ && sound_level_ != SoundLevel::off ? mix_sample(&work) : 128;
            if (!work) { work = producer_sample_instructions; }
            scheduler_->execute(CpuOperation::instructions, static_cast<unsigned>(work));
            ++digital_samples_;
            if (trace_policy_ == AudioTracePolicy::retain) { digital_.push_back(sample); }
        }
        scheduler_->execute(CpuOperation::instructions, producer_return_and_irq_tail);
    }

    std::vector<std::int16_t> render_scheduled(std::size_t count, const std::function<void()>& interrupt,
                                            const std::function<void()>& foreground = {}) {
        std::vector<std::int16_t> result;
        result.reserve(count);
        active_interrupt_ = interrupt;
        try {
            while (result.size() < count) {
                auto available = scheduler_->take_output();
                for (const auto& sample : available) { scheduled_output_.push_back(sample[0]); }
                if (!scheduled_output_.empty()) {
                    result.push_back(scheduled_output_.front());
                    scheduled_output_.pop_front();
                    continue;
                }
                scheduled_phase_ += dos_cycles_per_second;
                scheduled_target_ += scheduled_phase_ / sample_rate;
                scheduled_phase_ %= sample_rate;
                while (scheduler_->cycles() < scheduled_target_) {
                    if (!foreground) { scheduler_->run_cycles(scheduled_target_ - scheduler_->cycles()); break; }
                    const auto before = scheduler_->cycles();
                    foreground();
                    if (scheduler_->cycles() == before) { throw std::logic_error("Foreground callback did not advance its clock"); }
                }
            }
        } catch (...) {
            active_interrupt_ = {};
            throw;
        }
        active_interrupt_ = {};
        return result;
    }

    std::uint8_t mix_sample(std::uint64_t* work = nullptr) {
        if (work) { *work += producer_sample_instructions; }
        unsigned sum = 0;
        for (auto& voice : voices_) {
            if (voice.reload_pending || (voice.position >> 16) >= voice.current->bytes.size()) {
                if (work) {
                    *work += voice_reload_instructions;
                    if (voice.current != voice.next) { *work += changed_voice_instructions; }
                }
                voice.reload_pending = false;
                voice.current = voice.next;
                voice.position = 4u << 16;
                voice.shift = voice.next_shift;
                voice.current_step = voice.next_step;
            }
            sum += (voice.current->bytes.at(voice.position >> 16) >> voice.shift) + sample_bias[voice.shift];
            voice.position += voice.current_step;
        }
        const auto cursor = echo_cursor_++ % echo_samples;
        sum += echo_[cursor];
        ++sum;
        const auto value = static_cast<std::uint8_t>(sum >> 2);
        echo_[cursor] = value;
        const auto& conversion = sound_level_ == SoundLevel::soft ? tables_.soft_sample_conversion : tables_.sample_conversion;
        return conversion[(unsigned(value) << 1) | ((sum >> 1) & 1)];
    }

    AudioTables tables_;
    std::unique_ptr<AudioScheduler> scheduler_;
    AudioChannelConfig digital_config_;
    bool dma_running_ = false;
    std::array<std::vector<std::uint8_t>, 2> dma_buffers_;
    unsigned producer_half_ = 0, opl_delay_iterations_ = 0;
    std::uint64_t scheduled_target_ = 0, scheduled_phase_ = 0;
    std::deque<std::int16_t> scheduled_output_;
    std::function<void()> active_interrupt_;
    std::function<void()> timer_observer_;
    SoundDevice device_;
    IrqClock irq_clock_;
    std::uint64_t tick_ = 0, digital_phase_ = 0;
    std::uint8_t digital_value_ = 128;
    std::vector<Sample> effects_, speech_, announcements_;
    std::unique_ptr<Sample> cinematic_tail_;
    Sample silence_;
    std::array<Voice, 3> voices_{};
    std::array<std::uint8_t, echo_samples> echo_{};
    std::size_t echo_cursor_ = 0;
    std::vector<std::uint8_t> music_, next_music_, digital_;
    bool loop_music_ = true, music_active_ = true, music_muted_ = false, digital_enabled_ = true;
    bool music_callback_installed_ = true;
    bool cold_music_pending_ = false;
    SoundLevel sound_level_ = SoundLevel::loud;
    std::vector<OplWrite> writes_;
    std::vector<MusicIrqWork> music_irq_work_;
    unsigned music_work_depth_ = 0;
    std::uint64_t interrupt_token_ = 0;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> interrupt_handlers_;
    std::vector<std::pair<CpuOperation, unsigned>> irq_state_work_;
    bool collecting_irq_work_ = false;
    bool startup_stop_pending_ = false, startup_movie_pending_ = false;
    bool startup_menu_pending_ = false, cinematic_loop_pending_ = false;
    loading_work::InputBuffer mission_input_{};
    std::array<std::uint8_t, music_channels.size()> programs_{}, cached_programs_{}, initial_programs_{}, current_pitch_{}, last_pitch_{}, pitch_bend_{};
    std::array<std::size_t, music_channels.size()> envelope_positions_{};
    std::array<std::uint8_t, 9> frequency_high_{}, level_flags_{};
    std::uint8_t music_volume_ = 250;
    std::size_t position_ = 0;
    std::uint16_t wait_ = 1;
    AudioTracePolicy trace_policy_;
    std::uint64_t digital_samples_ = 0;
    std::vector<SampleRequest> requests_;
    std::vector<SampleRequest> resolved_requests_;
    std::size_t request_cursor_ = 0;
};

MissionAudio::MissionAudio(AudioTables tables, const std::vector<GameplayPayload>& payloads, AudioEntry entry,
                           std::vector<SampleRequest> requests, AudioTracePolicy trace_policy)
    : impl_(std::make_unique<Impl>(std::move(tables), payloads, entry, std::move(requests), trace_policy)) {}
MissionAudio::MissionAudio(AudioTables tables, MusicPlaylist playlist, AudioEntry entry, AudioTracePolicy trace_policy)
    : impl_(std::make_unique<Impl>(std::move(tables), std::move(playlist), entry, trace_policy)) {}
void MissionAudio::set_music_level(MusicLevel level, MusicChange change) { impl_->set_music_level(level, change); }
std::vector<std::int16_t> MissionAudio::take_pending_audio() { return impl_->take_pending_audio(); }
void MissionAudio::set_sound_level(SoundLevel level) { impl_->set_sound_level(level); }
void MissionAudio::set_interrupt_handler(std::function<void()> handler) { impl_->set_interrupt_handler(std::move(handler)); }
void MissionAudio::set_keyboard_handler(std::function<void(ExecutionClock&, KeyboardPort&)> handler) {
    impl_->set_keyboard_handler(std::move(handler));
}
void MissionAudio::queue_key_byte(std::uint8_t scan) { impl_->queue_key_byte(scan); }
void MissionAudio::schedule_key_byte(std::uint64_t cycle, std::uint8_t scan) { impl_->schedule_key_byte(cycle, scan); }
std::uint64_t MissionAudio::add_interrupt_handler(std::function<void()> handler) {
    return impl_->add_interrupt_handler(std::move(handler));
}
void MissionAudio::remove_interrupt_handler(std::uint64_t token) { impl_->remove_interrupt_handler(token); }
ExecutionClock& MissionAudio::execution_clock() { return impl_->execution_clock(); }
CpuWorkSink MissionAudio::cpu_work() {
    return [state = impl_.get()](CpuOperation operation, unsigned count) { state->execute(operation, count); };
}
void MissionAudio::enter_mission(const std::vector<GameplayPayload>& payloads, std::uint8_t power) {
    impl_->enter_mission(payloads, power);
}
void MissionAudio::prepare_mission_audio(const std::vector<GameplayPayload>& payloads, const StartupFiles& files) {
    impl_->prepare_mission_audio(payloads, files);
}
void MissionAudio::queue_initial_engine(std::uint8_t control_power) { impl_->queue_initial_engine(control_power); }
void MissionAudio::play_launch_radio() { impl_->play_launch_radio(); }
void MissionAudio::begin_mission_loading() { impl_->begin_mission_loading(); }
void MissionAudio::stop_mission_samples() { impl_->stop_mission_samples(); }
void MissionAudio::clear_mission_samples() { impl_->clear_mission_samples(); }
void MissionAudio::save_pilot_record() { impl_->save_pilot_record(); }
void MissionAudio::present_debrief(MusicPlaylist playlist, unsigned height) { impl_->present_debrief(std::move(playlist), height); }
GameplayPayload MissionAudio::load_mission_payload(const StartupResource& resource, PayloadKind kind, unsigned filename_bytes) {
    return impl_->load_mission_payload(resource, kind, filename_bytes);
}
void MissionAudio::load_menu_picture(const StartupResource& resource, unsigned filename_bytes) {
    impl_->load_menu_picture(resource, filename_bytes);
}
void MissionAudio::load_campaign(const StartupCampaign& campaign) { impl_->load_campaign(campaign); }
void MissionAudio::stop_music() { impl_->stop_music(); }
void MissionAudio::start_fanfare(const FanfareAssets& assets) { impl_->start_fanfare(assets); }
std::uint64_t MissionAudio::load_startup_movie(const StartupFiles& files, const StartupMovieAssets& assets) {
    return impl_->load_startup_movie(files, assets);
}
void MissionAudio::enter_pilot_menu(const PilotMenuAssets& assets) { impl_->enter_pilot_menu(assets); }
void MissionAudio::change_music(MusicPlaylist playlist) { impl_->change_music(std::move(playlist)); }
void MissionAudio::leave_mission(MusicPlaylist playlist) { impl_->leave_mission(std::move(playlist)); }
void MissionAudio::enter_cinematic(const std::vector<std::uint8_t>& bank, MusicPlaylist playlist, CinematicLoopStart loop) {
    impl_->enter_cinematic(bank, std::move(playlist), loop);
}
void MissionAudio::start_cinematic_loop() { impl_->start_cinematic_loop(); }
void MissionAudio::suspend_engine() { impl_->suspend_engine(); }
void MissionAudio::resume_engine(std::uint8_t power) { impl_->resume_engine(power); }
MissionAudio::~MissionAudio() = default;
MissionAudio::MissionAudio(MissionAudio&&) noexcept = default;
MissionAudio& MissionAudio::operator=(MissionAudio&&) noexcept = default;
std::vector<std::int16_t> MissionAudio::render(std::size_t samples, const std::function<void()>& interrupt) {
    return impl_->render(samples, interrupt);
}
void MissionAudio::play_effect(std::uint16_t sound, std::uint8_t attenuation) { impl_->play_effect(sound, attenuation); }
void MissionAudio::play_cue(const SoundCue& cue) { impl_->play_cue(cue); }
void MissionAudio::set_engine_power(std::uint8_t power) { impl_->set_engine_power(power); }
std::uint64_t MissionAudio::irq_ticks() const { return impl_->irq_ticks(); }
std::vector<std::int16_t> MissionAudio::render_foreground(std::size_t samples, const std::function<void()>& foreground) {
    return impl_->render_foreground(samples, foreground);
}
const std::vector<OplWrite>& MissionAudio::writes() const { return impl_->writes(); }
const std::vector<MusicIrqWork>& MissionAudio::music_irq_work() const { return impl_->music_irq_work(); }
const std::vector<std::uint8_t>& MissionAudio::digital_pcm() const { return impl_->digital_pcm(); }
const std::vector<SampleRequest>& MissionAudio::sample_requests() const { return impl_->sample_requests(); }
} // namespace comanchecd
