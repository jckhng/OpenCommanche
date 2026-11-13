#include "comanchecd/playback.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace comanchecd {
namespace {
constexpr std::size_t kdv_header_bytes = 28, palette_bytes = 768;
constexpr std::size_t demo_header_bytes = 17, demo_record_bytes = 8;
constexpr std::uint64_t vga_pixel_clock = 25175000;
constexpr std::uint64_t vga_line_pixels = 800, vga_frame_lines = 525, vga_retrace_line = 490;
constexpr auto video_period = audio_sample_rate * vga_line_pixels * vga_frame_lines;
constexpr auto retrace_start = audio_sample_rate * vga_line_pixels * vga_retrace_line;

std::uint64_t sample_at(std::uint64_t cycles) {
    return cycles / ExecutionClock::cycles_per_second * audio_sample_rate +
        (cycles % ExecutionClock::cycles_per_second * audio_sample_rate + ExecutionClock::cycles_per_second - 1) /
            ExecutionClock::cycles_per_second;
}

std::uint32_t integer(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count = 4) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        throw std::invalid_argument("Truncated playback resource");
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        value |= std::uint32_t(bytes[offset + index]) << (index * 8);
    }
    return value;
}
}

KdvClock::KdvClock(std::uint64_t elapsed_samples) {
    irq_.advance(elapsed_samples);
    video_phase_ = ((elapsed_samples % video_period) * vga_pixel_clock) % video_period;
}

std::size_t KdvClock::advance(std::size_t samples) {
    std::size_t frames = 0;
    for (std::size_t sample = 0; sample < samples; ++sample) {
        frame_irqs_ += irq_.advance(1);
        const auto previous = video_phase_;
        video_phase_ = (video_phase_ + vga_pixel_clock) % video_period;
        if (previous >= retrace_start || video_phase_ < retrace_start) {
            continue;
        }
        if (presenting_) {
            ++frames;
            frame_irqs_ = 0;
            presenting_ = false;
        } else if (frame_irqs_ >= KdvMovie::frame_irq_ticks) {
            // The polling wait returns in retrace; the presenter waits for the next one.
            presenting_ = true;
        }
    }
    return frames;
}

KdvMovie::KdvMovie(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)), height_(integer(bytes_, 24)) {
    const auto count = integer(bytes_, 8);
    const auto palette = integer(bytes_, 16);
    if (integer(bytes_, 0) != 0x3056444b || integer(bytes_, 4) != bytes_.size() ||
            integer(bytes_, 20) != width || height_ == 0 || height_ > 240 || count == 0 ||
            count > (bytes_.size() - kdv_header_bytes) / 4 ||
            palette > bytes_.size() || palette_bytes > bytes_.size() - palette) {
        throw std::invalid_argument("Unsupported KDV movie header");
    }
    for (std::size_t index = 0; index < palette_bytes; ++index) {
        palette_[index / 3][index % 3] = bytes_[palette + index] >> 2;
    }
    const auto data_begin = palette + palette_bytes;
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = integer(bytes_, kdv_header_bytes + index * 4);
        if (offset < data_begin || offset >= bytes_.size()) {
            throw std::invalid_argument("KDV frame offset exceeds resource");
        }
        offsets_.push_back(offset);
    }
}

std::size_t KdvMovie::frame_count() const { return offsets_.size(); }
std::size_t KdvMovie::height() const { return height_; }
const Palette& KdvMovie::palette() const { return palette_; }

std::vector<std::uint8_t> KdvMovie::frame(std::size_t index, const CpuWorkSink& work) const {
    auto cursor = offsets_.at(index);
    std::vector<std::uint8_t> result;
    const auto size = width * height_;
    result.reserve(size);
    if (work) { work(CpuOperation::instructions, 2); }
    while (result.size() < size) {
        const auto code = integer(bytes_, cursor++, 1);
        const auto count = code >= 0xc0 ? code & 63 : 1;
        const auto value = code >= 0xc0 ? integer(bytes_, cursor++, 1) : code;
        if (count > size - result.size()) {
            throw std::invalid_argument("KDV run exceeds frame");
        }
        if (work) {
            work(CpuOperation::string_bytes, 1);
            work(CpuOperation::instructions, 2);
            if (code >= 0xc0) {
                work(CpuOperation::instructions, 2);
                work(CpuOperation::string_bytes, 1);
                work(CpuOperation::repeat_store_bytes, count);
            } else {
                work(CpuOperation::string_bytes, 1);
            }
            work(CpuOperation::instructions, 2);
            if (code < 0xc0 && result.size() + count == size) { work(CpuOperation::instructions, 1); }
        }
        result.insert(result.end(), count, static_cast<std::uint8_t>(value));
    }
    if (work) { work(CpuOperation::instructions, 1); }
    return result;
}

void KdvMovie::blit_work(const CpuWorkSink& work) const {
    if (!work) { return; }
    work(CpuOperation::instructions, 1);
    for (unsigned plane = 0; plane < 4; ++plane) {
        work(CpuOperation::instructions, 2);
        work(CpuOperation::port_write_word, 1);
        work(CpuOperation::instructions, 2);
        work(CpuOperation::instructions, 4);
        for (std::size_t row = 0; row < height_; ++row) {
            for (unsigned pair = 0; pair < width / 8; ++pair) {
                work(CpuOperation::instructions, 4);
                work(CpuOperation::vga_store_words, 1);
            }
            work(CpuOperation::instructions, 3);
        }
        work(CpuOperation::instructions, 1);
    }
    work(CpuOperation::instructions, 1);
}

KdvPlayer::KdvPlayer(const KdvMovie& movie, ExecutionClock& clock, std::size_t hold_frames)
    : movie_(movie), clock_(clock), hold_frames_(hold_frames) {
    if (hold_frames > std::numeric_limits<std::size_t>::max() - movie.frame_count()) {
        throw std::invalid_argument("Movie hold count exceeds playback range");
    }
    // CALL 31238, header fields and 384 packed palette-word conversions.
    clock_.execute(CpuOperation::instructions, 18);
    for (unsigned word = 0; word < palette_bytes / 2; ++word) {
        clock_.execute(CpuOperation::string_bytes, 1);
        clock_.execute(CpuOperation::instructions, 2);
        clock_.execute(CpuOperation::string_bytes, 1);
        clock_.execute(CpuOperation::instructions, 1);
    }
    clock_.execute(CpuOperation::instructions, 5);
    frame_irq_ = clock_.sample_irq_ticks();
    clock_.execute(CpuOperation::instructions, 1);
}

void KdvPlayer::wait_until(RetraceState state) {
    bool active;
    do {
        active = clock_.read_retrace();
        clock_.execute(CpuOperation::instructions, 2); // TEST and LOOPcc.
    } while (active != (state == RetraceState::active));
}

void KdvPlayer::wait_retrace() {
    clock_.execute(CpuOperation::instructions, 2);
    wait_until(RetraceState::inactive);
    clock_.execute(CpuOperation::instructions, 1);
    wait_until(RetraceState::active);
    clock_.execute(CpuOperation::instructions, 1);
}

void KdvPlayer::present() {
    // 16648..166e3 programs the hidden page before the next retrace.
    clock_.execute(CpuOperation::instructions, 2);
    wait_until(RetraceState::inactive);
    program_page();
}

void KdvPlayer::program_page() {
    clock_.execute(CpuOperation::disable_interrupts, 1);
    clock_.execute(CpuOperation::instructions, 2);
    frame_irq_ = clock_.sample_irq_ticks();
    clock_.execute(CpuOperation::enable_interrupts, 1);
    clock_.execute(CpuOperation::instructions, 12);
    clock_.execute(CpuOperation::port_write_word, 1);
    clock_.execute(CpuOperation::instructions, 4);
    clock_.execute(CpuOperation::port_write_byte, 1);
    clock_.execute(CpuOperation::instructions, 2);
    clock_.execute(CpuOperation::port_write_byte, 1);
    clock_.execute(CpuOperation::instructions, 3);
    clock_.execute(CpuOperation::port_write_byte, 1);
    clock_.execute(CpuOperation::instructions, 2);
    clock_.execute(CpuOperation::port_write_byte, 1);
    clock_.execute(CpuOperation::instructions, 3);
    wait_until(RetraceState::active);
    clock_.execute(CpuOperation::instructions, 7);
}

void KdvPlayer::clear_page() {
    constexpr unsigned clear_dwords = 0x139e;
    clock_.execute(CpuOperation::instructions, 4);
    clock_.execute(CpuOperation::port_write_word, 1);
    clock_.execute(CpuOperation::instructions, 4);
    clock_.execute(CpuOperation::repeat_store_dwords, clear_dwords);
    clock_.execute(CpuOperation::instructions, 1);
}

std::uint64_t KdvPlayer::clear_pages() {
    clear_page();
    clock_.execute(CpuOperation::instructions, 1); // CALL 16656 skips the inactive-retrace poll.
    program_page();
    const auto visible_sample = sample_at(clock_.cycles());
    clear_page();
    return visible_sample;
}

void KdvPlayer::palette_work() {
    // CALL 2e2f6 and its DAC index/data loop.
    clock_.execute(CpuOperation::instructions, 4);
    clock_.execute(CpuOperation::port_write_byte, 1);
    clock_.execute(CpuOperation::instructions, 4);
    for (unsigned channel = 0; channel < palette_bytes; ++channel) {
        clock_.execute(CpuOperation::string_bytes, 1);
        clock_.execute(CpuOperation::port_write_byte, 1);
        clock_.execute(CpuOperation::instructions, 2);
    }
    clock_.execute(CpuOperation::instructions, 2); // RET and first-palette flag.
}

void KdvPlayer::prepare() {
    if (finished()) { throw std::logic_error("Movie playback has finished"); }
    const auto index = std::min(prepared_, movie_.frame_count() - 1);
    const auto work = [this](CpuOperation operation, unsigned count) { clock_.execute(operation, count); };
    clock_.execute(CpuOperation::instructions, 8); // Caller CALL and decoder setup.
    pixels_ = movie_.frame(index, work);
    clock_.execute(CpuOperation::instructions, 1);
    movie_.blit_work(work);
    for (;;) {
        const auto elapsed = clock_.sample_irq_ticks() - frame_irq_;
        clock_.execute(CpuOperation::instructions, 2);
        if (elapsed >= KdvMovie::frame_irq_ticks) { break; }
        clock_.execute(CpuOperation::instructions, 1);
        wait_retrace();
        clock_.execute(CpuOperation::instructions, 1);
    }
    clock_.execute(CpuOperation::instructions, 1);
    present();
    frame_irq_ = clock_.sample_irq_ticks();
    clock_.execute(CpuOperation::instructions, 2);
    if (prepared_ == 0) { palette_work(); }
    const auto suffix = index + 1 < movie_.frame_count() ? 6u :
        prepared_ + 1 < movie_.frame_count() + hold_frames_ ? 10u : 9u;
    clock_.execute(CpuOperation::instructions, suffix);
    ++prepared_;
    ready_sample_ = sample_at(clock_.cycles());
}

const std::vector<std::uint8_t>& KdvPlayer::pixels() const { return pixels_; }
std::uint64_t KdvPlayer::ready_sample() const { return ready_sample_; }
bool KdvPlayer::finished() const { return prepared_ >= movie_.frame_count() + hold_frames_; }

DemoRecording::DemoRecording(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < demo_header_bytes || integer(bytes, 0) != bytes.size() - 4 ||
            (bytes.size() - demo_header_bytes) % demo_record_bytes != 0) {
        throw std::invalid_argument("Invalid demo recording length");
    }
    pose_ = {integer(bytes, 4), integer(bytes, 8), integer(bytes, 12), bytes[16]};
    for (auto offset = demo_header_bytes; offset < bytes.size(); offset += demo_record_bytes) {
        inputs_.push_back({bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3],
            static_cast<std::int16_t>(integer(bytes, offset + 4, 2)),
            static_cast<std::int16_t>(integer(bytes, offset + 6, 2))});
    }
}

const DemoPose& DemoRecording::pose() const { return pose_; }
const std::vector<DemoInput>& DemoRecording::inputs() const { return inputs_; }
} // namespace comanchecd
