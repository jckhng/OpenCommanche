#pragma once

#include "comanchecd/gameplay.hpp"
#include "comanchecd/timing.hpp"
#include "comanchecd/execution_clock.hpp"
#include "comanchecd/keyboard.hpp"

#include <memory>
#include <functional>
#include <optional>

namespace comanchecd {
struct AudioTables {
    std::array<std::array<std::uint8_t, 16>, 128> instruments{};
    std::array<std::uint16_t, 256> frequencies{};
    std::array<std::uint8_t, 128> pitch{}, volume{};
    std::array<std::vector<std::uint8_t>, 128> envelopes;
    std::array<std::uint8_t, 512> sample_conversion{};
    std::array<std::uint8_t, 512> soft_sample_conversion{};
};

struct OplWrite {
    std::uint64_t tick;
    std::uint8_t reg, value;
};

struct MusicIrqWork {
    std::uint64_t tick;
    std::size_t first_write, last_write;
    CpuAccounting work;
};

enum class AudioExecution { sample_clock, scheduled };
enum class MusicStart { immediate, deferred };
enum class AudioStartup { ready, cold };
enum class AudioCalibration { supplied, pit2 };

enum class StartupLocation { local, cdrom };

struct FanfareAssets {
    std::vector<std::uint8_t> score, picture;
    StartupLocation score_location = StartupLocation::local;
    StartupLocation picture_location = StartupLocation::local;
    Transfer transfer{};
};

struct StartupResource {
    std::vector<std::uint8_t> encoded;
    StartupLocation location = StartupLocation::local;
};

struct StartupMovieAssets {
    StartupResource cues, score, sound, movie;
};

struct StartupCampaign {
    std::vector<std::uint8_t> encoded, decoded;
    StartupLocation find_location = StartupLocation::local;
    StartupLocation read_location = StartupLocation::local;
};

struct StartupLoading {
    std::optional<std::vector<std::uint8_t>> pilots, settings;
    std::vector<std::uint8_t> active_settings, detail;
    StartupLocation detail_location = StartupLocation::local;
    std::array<std::vector<std::uint8_t>, 5> menus;
    std::array<std::uint8_t, 256> glyph_widths{};
};

struct StartupFiles {
    std::optional<std::vector<std::uint8_t>> cdrom;
    std::vector<std::uint8_t> configuration;
    std::optional<std::vector<StartupCampaign>> campaigns;
    std::optional<StartupLoading> loading;
};

struct AudioEntry {
    AudioExecution execution = AudioExecution::sample_clock;
    MusicStart music_start = MusicStart::immediate;
    AudioStartup startup = AudioStartup::ready;
    AudioCalibration calibration = AudioCalibration::supplied;
    PitStart pit_start = PitStart::periodic;
    std::function<void(ExecutionClock&)> before_music_device;
    std::function<void(CpuOperation, std::uint64_t)> cpu_observer;
    std::optional<StartupFiles> startup_files;
    IrqState* startup_state = nullptr;
    unsigned opl_delay_iterations = 240;
    std::array<std::uint8_t, 9> frequency_high;
    std::array<std::uint8_t, 32> echo;
    std::uint32_t echo_cursor = 0;
    std::uint8_t engine_power = 15;
    std::uint8_t music_volume = 250;
    // PIT input cycles times output sample rate, below IrqClock::period.
    std::uint64_t irq_phase = 0;
    std::function<void(const CpuWorkSink&)> interrupt;
    AudioEntry() {
        frequency_high.fill(0x1f);
        echo.fill(128);
    }
};

std::uint32_t engine_rate(std::uint8_t power);

enum class SampleBank { effects, speech, announcements, silence };
enum class SampleMode { immediate, queued };
enum class AudioTracePolicy { retain, discard };
enum class EffectPolicy { available_first, restart_matching };
struct MusicPlaylist {
    std::vector<std::uint8_t> introduction, repeat;
};
struct PilotMenuAssets {
    std::array<StartupResource, 2> scores;
    StartupResource picture;
};
enum class CinematicLoopStart { immediate, after_page_clear };
constexpr unsigned dsp_sample_period_us = 90;

// The reference SB16 DMA channel uses its integer DSP rate and output-rate filter.
class Sb16Playback {
public:
    static constexpr unsigned source_rate = 1000000 / dsp_sample_period_us;
    explicit Sb16Playback(unsigned output_rate);
    std::vector<std::int16_t> render(const std::vector<std::uint8_t>& pcm);
    std::vector<std::int32_t> render_accumulator(const std::vector<std::uint8_t>& pcm);

private:
    unsigned output_rate_, phase_ = 0;
    std::int32_t previous_ = 0, filtered_ = 0;
    std::int32_t filter_alpha_;
    bool loaded_ = false;
};

struct SoundCue {
    SampleBank bank;
    std::uint16_t sound;
    std::uint8_t attenuation = 0;
    EffectPolicy policy = EffectPolicy::available_first;
    std::uint32_t step = 0;
};

using SoundCueHandler = std::function<void(const SoundCue&)>;

struct SampleRequest {
    std::uint64_t sample;
    SampleBank bank;
    std::uint16_t sound;
    std::uint8_t voice;
    std::uint8_t attenuation;
    std::uint32_t step;
    SampleMode mode;
};

class MissionAudio {
public:
    static constexpr unsigned sample_rate = audio_sample_rate;
    MissionAudio(AudioTables tables, const std::vector<GameplayPayload>& payloads, AudioEntry entry = {},
                 std::vector<SampleRequest> requests = {}, AudioTracePolicy trace_policy = AudioTracePolicy::retain);
    MissionAudio(AudioTables tables, MusicPlaylist playlist, AudioEntry entry = {},
                 AudioTracePolicy trace_policy = AudioTracePolicy::retain);
    ~MissionAudio();
    MissionAudio(MissionAudio&&) noexcept;
    MissionAudio& operator=(MissionAudio&&) noexcept;
    std::vector<std::int16_t> render(std::size_t samples, const std::function<void()>& interrupt = {});
    // Transfer already mixed samples without advancing CPU or device time.
    std::vector<std::int16_t> take_pending_audio();
    std::vector<std::int16_t> render_foreground(std::size_t samples, const std::function<void()>& foreground);
    void play_effect(std::uint16_t sound, std::uint8_t attenuation = 0);
    void play_cue(const SoundCue& cue);
    void set_engine_power(std::uint8_t power);
    void enter_mission(const std::vector<GameplayPayload>& payloads, std::uint8_t engine_power);
    void prepare_mission_audio(const std::vector<GameplayPayload>& payloads, const StartupFiles& files);
    void queue_initial_engine(std::uint8_t control_power);
    void play_launch_radio();
    void begin_mission_loading();
    void stop_mission_samples();
    void clear_mission_samples();
    void save_pilot_record();
    void present_debrief(MusicPlaylist playlist, unsigned height);
    GameplayPayload load_mission_payload(const StartupResource& resource, PayloadKind kind, unsigned filename_bytes);
    void load_menu_picture(const StartupResource& resource, unsigned filename_bytes);
    void load_campaign(const StartupCampaign& campaign);
    void stop_music();
    void start_fanfare(const FanfareAssets& assets);
    std::uint64_t load_startup_movie(const StartupFiles& files, const StartupMovieAssets& assets);
    void enter_pilot_menu(const PilotMenuAssets& assets);
    void change_music(MusicPlaylist playlist);
    void leave_mission(MusicPlaylist playlist);
    void enter_cinematic(const std::vector<std::uint8_t>& sound_bank, MusicPlaylist playlist,
                         CinematicLoopStart loop = CinematicLoopStart::immediate);
    void start_cinematic_loop();
    void suspend_engine();
    void resume_engine(std::uint8_t power);
    void set_music_level(MusicLevel level, MusicChange change = MusicChange::selected);
    void set_sound_level(SoundLevel level);
    void set_interrupt_handler(std::function<void()> handler);
    // The newest live flight receiver wins; removal restores the preceding receiver.
    std::uint64_t add_interrupt_handler(std::function<void()> handler);
    void remove_interrupt_handler(std::uint64_t token);
    void set_keyboard_handler(std::function<void(ExecutionClock&, KeyboardPort&)> handler);
    void queue_key_byte(std::uint8_t scan);
    void schedule_key_byte(std::uint64_t cycle, std::uint8_t scan);
    ExecutionClock& execution_clock();
    CpuWorkSink cpu_work();
    std::uint64_t irq_ticks() const;
    const std::vector<OplWrite>& writes() const;
    const std::vector<MusicIrqWork>& music_irq_work() const;
    const std::vector<std::uint8_t>& digital_pcm() const;
    const std::vector<SampleRequest>& sample_requests() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace comanchecd
