#include "comanchecd/playback.hpp"
#include "comanchecd/audio_scheduler.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
class MovieClock final : public comanchecd::ExecutionClock {
public:
    MovieClock() : scheduler_(comanchecd::audio_sample_rate) {
        scheduler_.set_pit(comanchecd::mission_pit_divisor, [&] { ++ticks_; });
    }
    void execute(comanchecd::CpuOperation operation, unsigned count) override { scheduler_.execute(operation, count); }
    void instruction(const std::function<void()>& action) override { scheduler_.instruction(action); }
    void read_file(unsigned bytes) override { scheduler_.read_file(bytes); }
    bool read_retrace() override { return scheduler_.read_retrace(); }
    std::uint64_t cycles() const override { return scheduler_.cycles(); }
    std::uint64_t irq_ticks() const override { return ticks_; }
    std::uint64_t sample_irq_ticks() override {
        std::uint64_t value = 0;
        scheduler_.instruction([&] { value = ticks_; });
        return value;
    }
    comanchecd::CpuAccounting accounting() const { return scheduler_.accounting(); }

private:
    comanchecd::AudioScheduler scheduler_;
    std::uint64_t ticks_ = 0;
};

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void put(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes.at(at + index) = static_cast<std::uint8_t>(value >> (index * 8));
    }
}
}

int main() {
    try {
        comanchecd::KdvClock whole_clock(0), split_clock(0);
        const auto whole_frames = whole_clock.advance(comanchecd::audio_sample_rate);
        std::size_t split_frames = 0;
        for (unsigned chunk = 0; chunk < 100; ++chunk) {
            split_frames += split_clock.advance(comanchecd::audio_sample_rate / 100);
        }
        require(whole_frames == 15 && split_frames == whole_frames,
                "KDV must wait for three IRQs, finish the polling retrace and present at the next retrace");
        std::vector<std::uint8_t> movie(800, 0);
        put(movie, 0, 0x3056444b);
        put(movie, 8, 1);
        put(movie, 12, 800);
        put(movie, 16, 32);
        put(movie, 20, 320);
        put(movie, 24, 1);
        put(movie, 28, 800);
        movie[32] = 255;
        for (unsigned run = 0; run < 5; ++run) {
            movie.insert(movie.end(), {255, 42});
        }
        movie.insert(movie.end(), {197, 42});
        put(movie, 4, static_cast<std::uint32_t>(movie.size()));
        const comanchecd::KdvMovie decoded(movie);
        const auto frame = decoded.frame(0);
        require(decoded.frame_count() == 1 && decoded.height() == 1 && decoded.palette()[0][0] == 63 &&
                frame.size() == 320 && std::all_of(frame.begin(), frame.end(), [](auto value) { return value == 42; }),
                "KDV must expand runs and convert eight-bit palette entries to DAC values");
        MovieClock timeline;
        comanchecd::KdvPlayer player(decoded, timeline);
        const auto initialized = timeline.accounting();
        require(initialized.dispatches == 1945 && initialized.string_iterations == 768 && initialized.io_cycles == 0,
                "Movie entry must charge its CALL, header setup and packed palette-word conversion loop");
        const auto cleared_at = player.clear_pages();
        const auto cleared = timeline.accounting();
        const auto io = cleared.io_cycles - initialized.io_cycles;
        constexpr unsigned clear_and_page_io = 87, byte_read_delay = 20;
        const auto reads = (io - clear_and_page_io) / byte_read_delay;
        const auto dispatches = cleared.dispatches - initialized.dispatches -
            (cleared.repeat_restarts - initialized.repeat_restarts);
        require(io >= clear_and_page_io && (io - clear_and_page_io) % byte_read_delay == 0 &&
                dispatches == 66 + 3 * reads && cleared.string_iterations - initialized.string_iterations == 10044,
                "Clearing both movie pages must execute both dword loops and the partial page presenter");
        player.prepare();
        require(player.pixels() == frame && player.finished() && player.ready_sample() > cleared_at && timeline.irq_ticks() >= 3,
                "Prepared movie pixels must retain their completion deadline after decoding and the delivered IRQ/retrace waits");
        movie[movie.size() - 2] = 198;
        bool rejected = false;
        try {
            comanchecd::KdvMovie(movie).frame(0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "KDV must reject an overflowing run");
        std::vector<std::uint8_t> demo(25, 0);
        put(demo, 0, 21);
        put(demo, 4, 0xdeadbeef);
        demo[16] = 81;
        demo[17] = 0x39;
        demo[18] = 124;
        demo[19] = 2;
        demo[21] = 0xff;
        demo[22] = 0xff;
        demo[23] = 0x22;
        demo[24] = 0xff;
        const comanchecd::DemoRecording recording(demo);
        require(recording.pose().x == 0xdeadbeef && recording.pose().height == 81 && recording.inputs().size() == 1 &&
                recording.inputs()[0].power == 124 && recording.inputs()[0].horizontal == -1 &&
                recording.inputs()[0].vertical == -222 && recording.inputs()[0].trigger == 2,
                "Demo records must preserve absolute power, button bytes and signed control words");
        demo.pop_back();
        rejected = false;
        try {
            comanchecd::DemoRecording invalid(demo);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Truncated demo input must reject");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
