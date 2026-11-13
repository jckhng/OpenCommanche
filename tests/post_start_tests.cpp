#include "../src/movie_timing.hpp"
#include "../src/loading_work.hpp"
#include "../src/startup_resource.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace comanchecd;
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

class Clock final : public ExecutionClock {
public:
    explicit Clock(AudioScheduler& scheduler) : scheduler_(scheduler) {}
    void execute(CpuOperation operation, unsigned count) override { scheduler_.execute(operation, count); }
    void instruction(const std::function<void()>& action) override { scheduler_.instruction(action); }
    void read_file(unsigned bytes) override { reads.push_back(bytes); scheduler_.read_file(bytes); }
    bool read_retrace() override { return scheduler_.read_retrace(); }
    std::uint64_t cycles() const override { return scheduler_.cycles(); }
    std::uint64_t irq_ticks() const override { return 0; }
    std::uint64_t sample_irq_ticks() override { scheduler_.instruction([] {}); return 0; }
    std::vector<unsigned> reads;
private:
    AudioScheduler& scheduler_;
};

std::vector<std::uint8_t> resource(const std::vector<std::uint8_t>& bytes, char encoding = '0') {
    std::vector<std::uint8_t> result{'K', 'R', 'L', static_cast<std::uint8_t>(encoding)};
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result.push_back(static_cast<std::uint8_t>(bytes.size() >> shift));
    }
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
}

StartupFiles startup(char sound) {
    StartupFiles result;
    result.configuration = {'0', '1', '3', '2', '7', '1'};
    result.campaigns.emplace();
    result.loading.emplace();
    auto& loading = *result.loading;
    loading.active_settings.assign(MenuSettings::record_bytes, 125);
    loading.active_settings[24] = '1';
    loading.active_settings[25] = sound;
    loading.active_settings[26] = '0';
    loading.active_settings[27] = '4';
    loading.detail.assign(5004, 0);
    for (auto& menu : loading.menus) { menu = {' ', 255}; }
    return result;
}

FanfareAssets fanfare() {
    FanfareAssets result;
    std::vector<std::uint8_t> score(50, 0);
    score[0] = 'M'; score[1] = 'S'; score[48] = 255; score[49] = 47;
    result.score = resource(score);
    result.picture.resize(128, 0);
    constexpr unsigned width_minus_one = line_stride * plane_count - 1;
    result.picture[8] = width_minus_one & 255;
    result.picture[9] = width_minus_one >> 8;
    unsigned remaining = width_minus_one + 1;
    while (remaining) {
        const auto count = std::min(remaining, 63u);
        result.picture.push_back(192 | count);
        result.picture.push_back(0);
        remaining -= count;
    }
    result.picture.push_back(12);
    result.picture.resize(result.picture.size() + 768, 0);
    return result;
}

MissionAudio audio(char sound) {
    AudioTables tables;
    for (auto& envelope : tables.envelopes) { envelope = {128}; }
    for (unsigned index = 0; index < tables.sample_conversion.size(); ++index) {
        tables.sample_conversion[index] = static_cast<std::uint8_t>(index / 2);
    }
    AudioEntry entry;
    entry.execution = AudioExecution::scheduled;
    entry.startup = AudioStartup::cold;
    entry.music_start = MusicStart::deferred;
    entry.startup_files = startup(sound);
    entry.opl_delay_iterations = 239;
    return MissionAudio(tables, MusicPlaylist{}, entry);
}
}

int main() {
    try {
        using namespace comanchecd;
        auto files = startup('2');
        AudioScheduler cold(audio_sample_rate), warm(audio_sample_rate);
        bool cold_detail = false, warm_detail = false;
        const auto music = [](MusicLevel) {};
        apply_startup_settings(cold, files, SettingsPass::cold, music, {}, [&](LoadingBoundary boundary) {
            cold_detail = cold_detail || boundary == LoadingBoundary::detail;
        });
        apply_startup_settings(warm, files, SettingsPass::post_start, music, {}, [&](LoadingBoundary boundary) {
            warm_detail = warm_detail || boundary == LoadingBoundary::detail;
        });
        require(cold_detail && !warm_detail && cold.cycles() > warm.cycles(),
                "Post-start settings must retain common work while skipping an unchanged detail file");
        for (const unsigned size : {8192u, 8197u}) {
            AudioScheduler scheduler(audio_sample_rate);
            Clock clock(scheduler);
            const auto encoded = resource(std::vector<std::uint8_t>(size, 130), '1');
            const auto decoded = loading_work::compressed_resource(encoded, clock);
            require(decoded.bytes == std::vector<std::uint8_t>(size, 130) &&
                    clock.reads == std::vector<unsigned>{8192, size - 8192},
                    "Compressed resources must retain full/short/zero refills and the previous buffer tail at EOF");
        }
        const auto intrinsic = [](const CpuAccounting& work) {
            return work.dispatches - work.repeat_restarts - 13 * work.idle_calls;
        };
        AudioScheduler local(audio_sample_rate), cdrom(audio_sample_rate), decoder(audio_sample_rate);
        Clock local_clock(local), cdrom_clock(cdrom), decoder_clock(decoder);
        const auto compressed = resource(std::vector<std::uint8_t>(8197, 130), '1');
        read_startup_resource(local, local_clock, {compressed, StartupLocation::local}, 9);
        read_startup_resource(cdrom, cdrom_clock, {compressed, StartupLocation::cdrom}, 9);
        require(intrinsic(cdrom.accounting()) == intrinsic(local.accounting()) + 3,
                "CD fallback must execute all six instructions between failed local find and open");
        loading_work::compressed_resource(compressed, decoder_clock);
        // 16941 header/allocator and 17021/169fa close/return, with a nine-byte filename.
        constexpr unsigned compressed_wrapper_dispatches = 364;
        require(intrinsic(local.accounting()) - intrinsic(decoder.accounting()) == compressed_wrapper_dispatches,
                "The shared compressed decoder closes with RET directly, without a carry test");
        StartupMovieAssets movie;
        movie.cues.encoded = {'0', '0', 13};
        movie.score.encoded = resource(std::vector<std::uint8_t>(8197, 130), '1');
        movie.sound.encoded = resource(std::vector<std::uint8_t>(8192, 17));
        movie.movie.encoded = resource(std::vector<std::uint8_t>(8197, 17));
        auto whole = audio('0'), split = audio('0');
        const auto before = whole.execution_clock().cycles();
        bool rejected = false;
        try { whole.load_startup_movie(startup('0'), movie); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected && whole.execution_clock().cycles() == before,
                "Movie loading must reject an invalid lifecycle before clock work");
        for (auto* instance : {&whole, &split}) {
            instance->start_fanfare(fanfare());
            const auto origin = instance->load_startup_movie(startup('0'), movie);
            require(origin <= instance->irq_ticks(), "The fanfare counter must reset before movie loading completes");
        }
        constexpr unsigned frames = audio_sample_rate / 2, fragment = 733;
        const auto expected = whole.render(frames);
        std::vector<std::int16_t> actual;
        while (actual.size() < frames) {
            const auto block = split.render(std::min<std::size_t>(fragment, frames - actual.size()));
            actual.insert(actual.end(), block.begin(), block.end());
        }
        require(!whole.digital_pcm().empty() && whole.digital_pcm().size() % 2048 == 0 &&
                std::all_of(whole.digital_pcm().begin(), whole.digital_pcm().end(), [](auto sample) { return sample == 128; }) &&
                whole.digital_pcm() == split.digital_pcm() && expected == actual &&
                whole.execution_clock().cycles() == split.execution_clock().cycles(),
                "Post-start silence must own real producer halves and preserve DMA/PCM across host partitions");
        auto disabled = audio('2');
        disabled.start_fanfare(fanfare());
        disabled.load_startup_movie(startup('2'), movie);
        disabled.render(frames);
        require(disabled.digital_pcm().empty(), "Disabled digital settings must not start silence DMA producers");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
