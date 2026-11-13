#include "comanchecd/audio_scheduler.hpp"
#include "comanchecd/timing.hpp"
#include "dsp_timing.hpp"
#include "opl_timing.hpp"
#include "pit2_timing.hpp"
#include "pit0_timing.hpp"
#include "rtc_timing.hpp"
#include "video_clock.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <deque>

namespace comanchecd {
namespace {
constexpr unsigned milliseconds_per_second = 1000;
constexpr unsigned byte_io_delay_ns = 1020, word_io_delay_ns = 660;
constexpr unsigned nanoseconds_per_ms = 1000000;
constexpr unsigned repeat_packet_limit = 4, special_store_tail = 5;
constexpr unsigned opl_status_delay_divisor = 2048;
constexpr std::uint64_t no_deadline = std::numeric_limits<std::uint64_t>::max();
constexpr float keyboard_byte_delay_ms = 0.300f;
constexpr unsigned keyboard_buffer_bytes = 96;
}

class AudioScheduler::Impl {
    enum class DiskWait { none, transfer };
public:
    Impl(unsigned rate, unsigned budget) : device_(rate), rate_(rate), budget_(budget), next_mix_(budget) {
        if (rate < milliseconds_per_second || rate > 192000 || !budget || budget > 1000000) {
            throw std::invalid_argument("Unsupported audio scheduler clock");
        }
        frames_ = rate / milliseconds_per_second;
        fraction_ = rate % milliseconds_per_second;
        update_rtc_deadline();
    }

    void run(std::uint64_t remaining) {
        core_end_ = no_deadline;
        if (remaining > no_deadline - now_) { throw std::overflow_error("Audio scheduler clock overflow"); }
        while (remaining) {
            const auto deadline = next_deadline();
            const auto step = std::min(remaining, deadline - now_);
            now_ += step;
            remaining -= step;
            service_due();
        }
    }

    void execute(CpuOperation operation, unsigned count) {
        if (operation == CpuOperation::repeat_store_bytes || operation == CpuOperation::repeat_store_dwords ||
                operation == CpuOperation::repeat_copy_bytes || operation == CpuOperation::repeat_copy_dwords ||
                operation == CpuOperation::repeat_copy_words ||
                operation == CpuOperation::repeat_scan_bytes) {
            repeat_string(operation, count);
            return;
        }
        if (operation == CpuOperation::instructions && !cpu_observer_) {
            while (count) {
                if (interrupt_shadow_) {
                    execute_one(operation);
                    --count;
                    continue;
                }
                begin_instruction();
                const auto chunk = static_cast<unsigned>(std::min<std::uint64_t>(count, core_end_ - now_));
                now_ += chunk;
                accounting_.dispatches += chunk;
                count -= chunk;
                finish_instruction();
            }
            return;
        }
        for (unsigned i = 0; i < count; ++i) {
            execute_one(operation);
        }
    }

    CpuAccounting accounting() const { return accounting_; }
    void cpu_observer(std::function<void(CpuOperation, std::uint64_t)> observer) { cpu_observer_ = std::move(observer); }
    void instruction(const std::function<void()>& action) { execute_one(CpuOperation::instructions, action); }

    void video_timing(const VideoTiming& timing) {
        video_ = VideoClock(timing);
        video_.advance(static_cast<long double>(now_) / budget_);
        core_end_ = no_deadline;
    }

    void read_file(unsigned bytes) {
        file_call(bytes, DiskWait::transfer);
    }

    void write_file(unsigned bytes) {
        file_call(bytes, DiskWait::transfer);
    }

    void file_metadata(DosFileCall call) {
        constexpr unsigned open_bytes = 1024, close_bytes = 512, create_bytes = 2048;
        if (call == DosFileCall::find) {
            file_call(0, DiskWait::none);
            return;
        }
        const auto bytes = call == DosFileCall::create ? create_bytes :
            call == DosFileCall::open_read ? open_bytes : close_bytes;
        file_call(bytes, DiskWait::transfer);
    }

    void initialize_video() {
        const auto saved_interrupts = interrupts_enabled_;
        const auto saved_mode = real_mode_irq_;
        execute(CpuOperation::instructions, 1); // MOV AX,12.
        execute_one(CpuOperation::instructions, [&] { interrupts_enabled_ = false; }); // INT 22.
        execute_one(CpuOperation::instructions, [&] { real_mode_irq_ = true; }); // INT 32.
        execute(CpuOperation::instructions, 3);
        execute(CpuOperation::stack_segment, 1);
        execute(CpuOperation::instructions, 5);
        execute(CpuOperation::stack_segment, 1);
        execute(CpuOperation::instructions, 3);
        execute(CpuOperation::enable_interrupts, 1);
        execute_one(CpuOperation::instructions, [&] { interrupts_enabled_ = false; }); // INT 10.
        execute(CpuOperation::instructions, 1); // VGA option-ROM far jump.
        execute_one(CpuOperation::callback_return, [&] {
            constexpr float resize_delay_ms = 1.0f;
            video_setup_time_ = static_cast<long double>(now_) / budget_ + resize_delay_ms;
            video_setup_deadline_ = static_cast<std::uint64_t>(std::ceil(video_setup_time_ * budget_));
        });
        execute_one(CpuOperation::interrupt_return, [&] { interrupts_enabled_ = true; }); // BIOS IRET.
        execute(CpuOperation::instructions, 4);
        execute(CpuOperation::disable_interrupts, 1);
        execute(CpuOperation::instructions, 6);
        execute(CpuOperation::stack_segment, 1);
        execute(CpuOperation::instructions, 7);
        execute_one(CpuOperation::interrupt_return, [&] { // Protected caller IRET.
            interrupts_enabled_ = saved_interrupts;
            real_mode_irq_ = saved_mode;
        });
    }

private:
    void file_call(unsigned bytes, DiskWait wait) {
        constexpr unsigned disk_bytes_per_second = 8333333;
        constexpr unsigned bridge_prefix = 26, bridge_suffix = 23, idle_dispatches = 13;
        if (bytes > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("DOS read exceeds its 16-bit byte count");
        }
        const auto saved_interrupts = interrupts_enabled_;
        const auto saved_mode = real_mode_irq_;
        struct RestoreMode {
            bool& mode;
            bool& interrupts;
            bool saved_mode, saved_interrupts;
            ~RestoreMode() { mode = saved_mode; interrupts = saved_interrupts; }
        } restore{real_mode_irq_, interrupts_enabled_, saved_mode, saved_interrupts};
        for (unsigned index = 0; index < bridge_prefix; ++index) {
            auto operation = CpuOperation::instructions;
            if (index == 0 || index == 23) { operation = CpuOperation::disable_interrupts; }
            if (index == 5 || index == 11) { operation = CpuOperation::stack_segment; }
            if (index == 22 || index == 24) { operation = CpuOperation::enable_interrupts; }
            if (index + 1 == bridge_prefix) { operation = CpuOperation::callback_return; }
            execute_one(operation);
            if (index == 0) { real_mode_irq_ = true; }
        }
        const double end = double(now_) / budget_ + double(bytes) / disk_bytes_per_second * milliseconds_per_second;
        if (wait == DiskWait::transfer) {
            do {
                ++accounting_.idle_calls;
                execute(CpuOperation::instructions, idle_dispatches - 1);
                execute_one(CpuOperation::callback_return);
                if (core_end_ != no_deadline && now_ < core_end_) {
                    accounting_.idle_cycles += core_end_ - now_;
                    now_ = core_end_;
                }
                core_end_ = no_deadline;
            } while (double(now_) / budget_ < end);
        }
        for (unsigned index = 0; index < bridge_suffix; ++index) {
            auto operation = CpuOperation::instructions;
            if (index == 1) { operation = CpuOperation::disable_interrupts; }
            if (index == 14) { operation = CpuOperation::stack_segment; }
            if (index + 1 == bridge_suffix) { operation = CpuOperation::interrupt_return; }
            execute_one(operation, index + 1 == bridge_suffix ? std::function<void()>([&] {
                interrupts_enabled_ = saved_interrupts;
                real_mode_irq_ = saved_mode;
            }) : std::function<void()>{});
        }
    }

public:
    void flush() {
        // Match the reference FillUp conversion through its float tick fraction.
        const auto fraction = static_cast<float>(double(now_ - frame_start_) / budget_);
        const auto units = static_cast<unsigned>(double(fraction) * frames_ * milliseconds_per_second);
        mix(std::min(frames_, units / milliseconds_per_second));
        deliver_dma();
    }

    void configure(AudioChannel channel, const AudioChannelConfig& config) {
        flush();
        device_.configure(channel, config);
        if (channel == AudioChannel::digital) { digital_rate_ = config.source_rate; }
        if (channel == AudioChannel::fm) { fm_config_ = config; }
    }

    void write_fm(std::uint8_t reg, std::uint8_t value) {
        flush();
        opl_timing_.write(reg, value, double(now_) / budget_);
        device_.write_fm(reg, value);
    }

    void write_fm_timed(std::uint8_t reg, std::uint8_t value, unsigned iterations,
                        const std::function<void()>& on_write) {
        if (!iterations || iterations > 1000000) { throw std::invalid_argument("Invalid OPL loop calibration"); }
        execute(CpuOperation::instructions, 6); // CALL, saved registers and address setup.
        execute_one(CpuOperation::port_write_byte, [&] {
            if (!fm_config_.enabled && fm_config_.source_rate) {
                // The address port enables OPL without a mixer FillUp.
                fm_config_.enabled = true;
                device_.configure(AudioChannel::fm, fm_config_);
            }
        });
        execute(CpuOperation::instructions, 1);
        for (unsigned round = 0; round < 2; ++round) {
            execute(CpuOperation::instructions, 2 + 2 * iterations);
        }
        execute(CpuOperation::instructions, 2);
        execute_one(CpuOperation::port_write_byte, [&] {
            if (on_write) { on_write(); }
            write_fm(reg, value);
        });
        execute(CpuOperation::instructions, 1);
        for (unsigned round = 0; round < 4; ++round) {
            execute(CpuOperation::instructions, 2 + 2 * iterations);
        }
        execute(CpuOperation::instructions, 3);
    }

    void halt() {
        flush();
        dma_active_ = false;
    }

    void arm(std::vector<std::uint8_t> bytes) {
        if (bytes.empty() || bytes.size() > 65536) { throw std::invalid_argument("Invalid single-cycle DMA block"); }
        flush();
        dma_ = std::move(bytes);
        dma_cursor_ = 0;
        dma_active_ = true;
        pending_dma_ = false;
    }

    bool read_dsp_busy() {
        bool busy = false;
        execute_one(CpuOperation::port_read_byte, [&] {
            busy = dsp_timing_.read(double(now_) / budget_) || dsp_busy_until_ != no_deadline;
        });
        return busy;
    }

    std::uint8_t read_fm_status() {
        std::uint8_t status = 0;
        execute_one(CpuOperation::fm_status_read, [&] {
            status = opl_timing_.read(double(now_) / budget_);
        });
        return status;
    }

    void pit2_control(std::uint8_t value) {
        execute_one(CpuOperation::port_write_byte, [&] { pit2_timing_.control(value, static_cast<long double>(now_) / budget_); });
    }
    void pit2_write(std::uint8_t value) {
        execute_one(CpuOperation::port_write_byte, [&] { pit2_timing_.write(value, static_cast<long double>(now_) / budget_); });
    }
    std::uint8_t pit2_read() {
        std::uint8_t value = 0;
        execute_one(CpuOperation::port_read_byte, [&] { value = pit2_timing_.read(); });
        return value;
    }
    void write_speaker(std::uint8_t value) {
        execute_one(CpuOperation::port_write_byte, [&] { pit2_timing_.speaker(value, static_cast<long double>(now_) / budget_); });
    }
    std::uint8_t read_speaker() {
        std::uint8_t value = 0;
        execute_one(CpuOperation::port_read_byte, [&] { value = pit2_timing_.speaker(); });
        return value;
    }

    bool read_retrace() {
        bool retrace = false;
        execute_one(CpuOperation::port_read_byte, [&] {
            retrace = video_.retrace(static_cast<long double>(now_) / budget_);
        });
        return retrace;
    }

    void write_dsp_timed(std::uint8_t, const std::function<void()>& on_write) {
        execute_one(CpuOperation::port_write_byte, [&] {
            if (dsp_busy_until_ != no_deadline) { return; }
            // PIC adds the double delay to its fractional millisecond index.
            // Preserve that rounding before deriving delivery and CPU slice limits.
            constexpr double busy_delay_ms = 1.0 / 1000;
            const auto time = (static_cast<long double>(now_ - frame_start_) / budget_ + busy_delay_ms) * budget_;
            dsp_busy_until_ = frame_start_ + static_cast<std::uint64_t>(std::ceil(time));
            dsp_busy_slice_ = frame_start_ + static_cast<std::uint64_t>(std::floor(time));
            if (dsp_busy_slice_ < core_end_) { core_end_ = now_; }
            if (on_write) { on_write(); }
        });
    }

    void write_dsp_reset(std::uint8_t value) {
        execute_one(CpuOperation::port_write_byte, [&] {
            if (value & 1) {
                dsp_reset_held_ = true;
                dsp_reset_ready_ = false;
                dsp_reset_deadline_ = no_deadline;
                dsp_timing_ = DspTiming{};
                dsp_busy_until_ = no_deadline;
            } else if (dsp_reset_held_) {
                constexpr float reset_ms = 20.0f / 1000.0f;
                dsp_reset_held_ = false;
                dsp_reset_time_ = now_ + static_cast<long double>(reset_ms) * budget_;
                dsp_reset_deadline_ = static_cast<std::uint64_t>(std::ceil(dsp_reset_time_));
                if (static_cast<std::uint64_t>(std::floor(dsp_reset_time_)) < core_end_) { core_end_ = now_; }
            }
        });
    }

    bool read_dsp_ready() {
        bool ready = false;
        execute_one(CpuOperation::port_read_byte, [&] { ready = dsp_reset_ready_; });
        return ready;
    }

    std::uint8_t read_dsp_data() {
        std::uint8_t value = 0xff;
        execute_one(CpuOperation::port_read_byte, [&] {
            if (dsp_reset_ready_) { value = 0xaa; }
            dsp_reset_ready_ = false;
        });
        return value;
    }

    void pit(unsigned divisor, std::function<void()> handler, PitStart start) {
        if (!divisor || divisor > 65536) { throw std::invalid_argument("Invalid PIT divisor"); }
        start = pit_start(start);
        pit0_.reload(divisor, static_cast<long double>(now_) / budget_);
        divisor_ = divisor;
        pit_fraction_ = 0;
        next_pit_ = now_;
        pit_handler_ = std::move(handler);
        pending_pit_ = false;
        schedule_pit();
        if (start == PitStart::rising_edge) {
            pending_pit_ = true;
            pit_ready_ = now_ + (std::uint64_t(irq_delay_ns_) * budget_ + nanoseconds_per_ms - 1) / nanoseconds_per_ms;
            request_irq_check(pit_ready_, now_ + std::uint64_t(irq_delay_ns_) * budget_ / nanoseconds_per_ms);
        }
    }

    void program_pit(unsigned divisor, std::function<void()> handler, PitStart start) {
        if (!divisor || divisor > bios_pit_divisor) { throw std::invalid_argument("Invalid PIT divisor"); }
        // Port handlers observe the clock after the OUT's dispatch and I/O delay.
        execute_one(CpuOperation::port_write_byte, [&] { start = pit_start(start); });
        execute(CpuOperation::instructions, 2);
        execute(CpuOperation::port_write_byte, 1);
        execute(CpuOperation::instructions, 2);
        execute_one(CpuOperation::port_write_byte, [&] { pit(divisor, std::move(handler), start); });
    }

    void interrupts(bool enabled) {
        interrupts_enabled_ = enabled;
        deliver_pit();
        deliver_keyboard();
        deliver_dma();
    }

    void keyboard_handler(std::function<void()> handler) { keyboard_handler_ = std::move(handler); }
    void queue_key(std::uint8_t scan) {
        if (keyboard_bytes_.size() == keyboard_buffer_bytes) {
            keyboard_bytes_.clear();
            keyboard_transfer_ = no_deadline;
            return;
        }
        keyboard_bytes_.push_back(scan);
        if (!keyboard_data_ready_ && keyboard_transfer_ == no_deadline) { schedule_keyboard(); }
        core_end_ = no_deadline;
    }
    void schedule_key(std::uint64_t cycle, std::uint8_t scan) {
        if (cycle < now_ || cycle == no_deadline || (!key_arrivals_.empty() && cycle < key_arrivals_.back().cycle)) {
            throw std::invalid_argument("Keyboard arrivals must use ordered, nonpast CPU cycles");
        }
        if (cycle == now_) { queue_key(scan); return; }
        key_arrivals_.push_back({cycle, scan});
        core_end_ = no_deadline;
    }
    std::uint8_t read_keyboard() {
        auto scan = keyboard_data_;
        execute_one(CpuOperation::port_read_byte, [&] {
            scan = keyboard_data_;
            pending_keyboard_ = keyboard_data_ready_ = false;
            if (!keyboard_bytes_.empty() && keyboard_transfer_ == no_deadline) { schedule_keyboard(); }
        });
        return scan;
    }
    void acknowledge_keyboard() {
        execute_one(CpuOperation::port_write_byte, [&] { keyboard_in_service_ = false; });
    }

    void delay(unsigned nanoseconds) {
        if (nanoseconds > 1000000) { throw std::invalid_argument("Interrupt arbitration delay exceeds one millisecond"); }
        irq_delay_ns_ = nanoseconds;
    }

    std::uint64_t now() const { return now_; }
    std::uint64_t consumed() const { return consumed_; }
    std::size_t remaining() const { return dma_.size() - dma_cursor_; }
    void dma_handler(std::function<void()> handler) { dma_handler_ = std::move(handler); }
    std::vector<std::array<std::int16_t, 2>> take() {
        auto result = std::move(output_);
        output_.clear();
        return result;
    }

private:
    std::uint64_t next_deadline() const {
        auto deadline = std::min({next_mix_, next_pit_, rtc_deadline_, dsp_busy_until_, dsp_reset_deadline_, video_deadline(), video_setup_deadline_, keyboard_transfer_, irq_check_});
        if (!key_arrivals_.empty()) { deadline = std::min(deadline, key_arrivals_.front().cycle); }
        if (interrupts_enabled_ && !interrupt_shadow_) {
            if (pending_pit_ && !in_pit_) { deadline = std::min(deadline, std::max(now_, pit_ready_)); }
            if (pending_keyboard_ && !in_pit_ && !keyboard_in_service_) {
                deadline = std::min(deadline, std::max(now_, keyboard_ready_));
            }
            if (pending_dma_ && !in_pit_ && !keyboard_in_service_ && !in_dma_) { deadline = std::min(deadline, std::max(now_, dma_ready_)); }
        }
        return deadline;
    }

    PitStart pit_start(PitStart start) const {
        if (start != PitStart::inherited_square_wave) { return start; }
        return pit0_.output_high(static_cast<long double>(now_) / budget_) ? PitStart::periodic : PitStart::rising_edge;
    }

    Pit0Timing pit0_;

    void update_rtc_deadline() {
        const auto cycle = rtc_.next() * budget_;
        rtc_deadline_ = static_cast<std::uint64_t>(std::ceil(cycle));
        rtc_slice_ = static_cast<std::uint64_t>(cycle);
    }

    void service_due() {
        video_.advance(static_cast<long double>(now_) / budget_);
        while (now_ >= rtc_deadline_) {
            rtc_.advance(static_cast<long double>(now_) / budget_);
            update_rtc_deadline();
        }
        if (now_ >= video_setup_deadline_) {
            VideoTiming timing;
            timing.frame_start_ms = video_setup_time_;
            timing.ideal_frame_ms = static_cast<double>(video_setup_time_);
            video_ = VideoClock(timing);
            video_.advance(static_cast<long double>(now_) / budget_);
            video_setup_deadline_ = no_deadline;
        }
        while (!key_arrivals_.empty() && key_arrivals_.front().cycle <= now_) {
            const auto scan = key_arrivals_.front().scan;
            key_arrivals_.pop_front();
            queue_key(scan);
        }
        if (now_ >= keyboard_transfer_) {
            keyboard_transfer_ = no_deadline;
            if (!keyboard_bytes_.empty()) {
                keyboard_data_ = keyboard_bytes_.front();
                keyboard_bytes_.pop_front();
                pending_keyboard_ = keyboard_data_ready_ = true;
                // PIC_AddEvent inside event service uses the scheduled event time,
                // even when the normal core serviced that event after its deadline.
                const auto ready = keyboard_transfer_time_ +
                    static_cast<long double>(irq_delay_ns_) * budget_ / nanoseconds_per_ms;
                keyboard_ready_ = static_cast<std::uint64_t>(std::ceil(ready));
                keyboard_ready_slice_ = static_cast<std::uint64_t>(std::floor(ready));
                if (!in_pit_ && !keyboard_in_service_) {
                    request_irq_check(keyboard_ready_, keyboard_ready_slice_);
                }
            }
        }
        if (now_ >= dsp_busy_until_) { dsp_busy_until_ = no_deadline; }
        if (now_ >= dsp_reset_deadline_) {
            dsp_reset_deadline_ = no_deadline;
            dsp_reset_ready_ = true;
        }
        while (now_ >= next_mix_) {
            mix(frames_);
            complete_frame();
            rendered_ = 0;
            frame_ended_ = false;
            frame_start_ = next_mix_;
            fraction_ += rate_ % milliseconds_per_second;
            frames_ = rate_ / milliseconds_per_second + fraction_ / milliseconds_per_second;
            fraction_ %= milliseconds_per_second;
            next_mix_ += budget_;
        }
        while (now_ >= next_pit_) {
            if (!pending_pit_) {
                constexpr std::uint64_t nanoseconds_per_ms = 1000000;
                const auto denominator = std::uint64_t(pit_frequency) * nanoseconds_per_ms;
                const auto numerator = pit_fraction_ * nanoseconds_per_ms + std::uint64_t(irq_delay_ns_) * budget_ * pit_frequency;
                pit_ready_ = next_pit_ - (pit_fraction_ != 0) + (numerator + denominator - 1) / denominator;
                request_irq_check(pit_ready_, next_pit_ - (pit_fraction_ != 0) + numerator / denominator);
            }
            schedule_pit();
            pending_pit_ = true;
        }
        if (now_ >= irq_check_) {
            irq_check_ = no_deadline;
            pic_irq_check_ = true;
        }
        if (!instruction_depth_ && !interrupt_shadow_ && interrupts_enabled_) { pic_irq_check_ = false; }
        deliver_pit();
        deliver_keyboard();
        deliver_dma();
    }

    void begin_instruction() {
        if (interrupt_shadow_) {
            // The normal core forces the instruction after a pending STI before returning to PIC.
            ++instruction_depth_;
            return;
        }
        auto end = next_deadline();
        if (now_ >= end || pic_irq_check_) {
            service_due();
            end = next_deadline();
        }
        if (end == next_pit_ && pit_fraction_) { --end; }
        if (end == rtc_deadline_) { end = rtc_slice_; }
        if (end == video_deadline()) { end = video_slice_; }
        if (end == video_setup_deadline_) { end = static_cast<std::uint64_t>(std::floor(video_setup_time_ * budget_)); }
        if (end == keyboard_transfer_) { end = keyboard_transfer_slice_; }
        if (end == irq_check_) { end = irq_check_slice_; }
        if (end == dsp_reset_deadline_) { end = static_cast<std::uint64_t>(std::floor(dsp_reset_time_)); }
        if (end == dsp_busy_until_) { end = dsp_busy_slice_; }
        if (pending_keyboard_ && end == keyboard_ready_) { end = std::min(end, keyboard_ready_slice_); }
        core_end_ = std::min(core_end_, std::max(now_ + 1, end));
        ++instruction_depth_;
    }

    void finish_instruction() {
        --instruction_depth_;
        if (interrupt_shadow_) {
            --interrupt_shadow_;
            if (interrupt_shadow_) { return; }
        }
        core_end_ = std::min(core_end_, next_deadline());
        if (now_ < core_end_) { return; }
        ++now_;
        ++accounting_.terminal_charges;
        core_end_ = no_deadline;
        service_due();
    }

    void execute_one(CpuOperation operation, const std::function<void()>& action = {}) {
        begin_instruction();
        observe_cpu(operation);
        unsigned cost = 1;
        ++accounting_.dispatches;
        if (operation == CpuOperation::stack_segment) {
            cost = 0;
            ++accounting_.stack_segment_credits;
        }
        if (operation == CpuOperation::string_bytes || operation == CpuOperation::vga_store_words) {
            ++accounting_.string_iterations;
            ++cost;
        }
        if (operation == CpuOperation::port_write_word || operation == CpuOperation::port_write_byte ||
                operation == CpuOperation::port_read_byte || operation == CpuOperation::fm_status_read) {
            const auto nanoseconds = operation == CpuOperation::port_write_word ? word_io_delay_ns : byte_io_delay_ns;
            auto delay = operation == CpuOperation::port_read_byte || operation == CpuOperation::fm_status_read ? std::uint64_t(budget_) * nanoseconds / nanoseconds_per_ms :
                std::uint64_t(budget_) * nanoseconds * 3 / (4 * nanoseconds_per_ms);
            if (operation == CpuOperation::fm_status_read) {
                // The OPL handler caps its extra delay at the remaining core budget.
                delay = std::min(delay + budget_ / opl_status_delay_divisor, core_end_ - now_ - cost);
            }
            accounting_.io_cycles += delay;
            cost += static_cast<unsigned>(delay);
        }
        now_ += cost;
        if (operation == CpuOperation::disable_interrupts) {
            interrupts_enabled_ = false;
            interrupt_shadow_ = 0;
        } else if (operation == CpuOperation::enable_interrupts) {
            interrupts_enabled_ = true;
            if (pic_irq_check_ || (pending_pit_ && now_ >= pit_ready_) || (pending_keyboard_ && now_ >= keyboard_ready_) ||
                    (pending_dma_ && now_ >= dma_ready_)) {
                interrupt_shadow_ = 2;
                core_end_ = now_ + 1;
            }
        }
        try {
            if (action) { action(); }
        } catch (...) {
            --instruction_depth_;
            throw;
        }
        if (operation == CpuOperation::callback_return ||
                (operation == CpuOperation::interrupt_return && interrupts_enabled_ && pic_irq_check_)) {
            --instruction_depth_;
            interrupt_shadow_ = 0;
            if (now_ >= core_end_) { core_end_ = no_deadline; }
            if (operation == CpuOperation::interrupt_return) { service_due(); }
        } else {
            finish_instruction();
        }
    }

    void repeat_string(CpuOperation operation, unsigned remaining) {
        do {
            begin_instruction();
            observe_cpu(operation);
            ++accounting_.dispatches;
            ++now_;
            auto chunk = std::min(remaining, repeat_packet_limit);
            const bool special = operation == CpuOperation::repeat_store_bytes && remaining == special_store_tail;
            if (special) {
                chunk = special_store_tail;
                ++accounting_.special_stosb_iterations;
            }
            unsigned completed = 0;
            while (completed < chunk) {
                ++now_;
                ++completed;
                if (!special && now_ >= core_end_) { break; }
            }
            accounting_.string_iterations += completed - (special ? 1 : 0);
            remaining -= completed;
            if (remaining) { ++accounting_.repeat_restarts; }
            finish_instruction();
        } while (remaining);
    }

    void mix(unsigned through) {
        if (through <= rendered_) { return; }
        auto digital = device_.service(AudioChannel::digital, through, [this](unsigned count) {
            const auto left = dma_active_ ? remaining() : 0;
            if (!left) { return std::vector<std::uint8_t>{}; }
            const auto minimum = digital_rate_ * 3 / milliseconds_per_second;
            if (left <= minimum) { count = static_cast<unsigned>(left); }
            count = static_cast<unsigned>(std::min<std::size_t>(count, left));
            std::vector<std::uint8_t> bytes(dma_.begin() + dma_cursor_, dma_.begin() + dma_cursor_ + count);
            dma_cursor_ += count;
            consumed_ += count;
            if (!remaining()) {
                pending_dma_ = true;
                const auto ready = now_ + static_cast<long double>(irq_delay_ns_) * budget_ / nanoseconds_per_ms;
                dma_ready_ = static_cast<std::uint64_t>(std::ceil(ready));
                request_irq_check(dma_ready_, static_cast<std::uint64_t>(std::floor(ready)));
            }
            return bytes;
        });
        auto fm = device_.service(AudioChannel::fm, through);
        for (std::size_t i = 0; i < fm.size(); ++i) {
            for (unsigned c = 0; c < fm[i].size(); ++c) { fm[i][c] += digital[i][c]; }
            output_.push_back(CallbackAudio::output(fm[i], {audio_gain_one, audio_gain_one}));
        }
        rendered_ = through;
        if (through == frames_) { complete_frame(); }
    }

    void observe_cpu(CpuOperation operation) {
        try {
            if (cpu_observer_) { cpu_observer_(operation, now_ + 1); }
        } catch (...) {
            --instruction_depth_;
            throw;
        }
    }

    void complete_frame() {
        if (frame_ended_) { return; }
        for (const auto channel : {AudioChannel::digital, AudioChannel::fm}) {
            device_.end_frame(channel, frames_);
        }
        frame_ended_ = true;
    }

    void schedule_pit() {
        const auto numerator = std::uint64_t(budget_) * milliseconds_per_second * divisor_;
        const auto total = numerator + pit_fraction_;
        // Ceil cumulative rational deadlines, retaining the fractional remainder.
        const auto previous_rounding = pit_fraction_ != 0;
        pit_fraction_ = total % pit_frequency;
        next_pit_ += total / pit_frequency + (pit_fraction_ != 0) - previous_rounding;
    }

    void deliver_pit() {
        while (!instruction_depth_ && !interrupt_shadow_ && interrupts_enabled_ && !in_pit_ && pending_pit_ && now_ >= pit_ready_) {
            pending_pit_ = false;
            in_pit_ = true;
            schedule_irq_check();
            const auto previous_interrupts = interrupts_enabled_;
            interrupts_enabled_ = false;
            try {
                const auto bridge = real_mode_irq_;
                if (bridge) { irq_bridge(22, 14, 14); }
                if (pit_handler_) { pit_handler_(); }
                if (bridge) { irq_bridge(18, 4, 11); }
            } catch (...) {
                in_pit_ = false;
                interrupts_enabled_ = previous_interrupts;
                throw;
            }
            in_pit_ = false;
            interrupts_enabled_ = previous_interrupts;
        }
    }

    void deliver_dma() {
        if (instruction_depth_ || interrupt_shadow_ || !interrupts_enabled_ || in_pit_ || keyboard_in_service_ ||
                in_dma_ || !pending_dma_ || now_ < dma_ready_) { return; }
        pending_dma_ = false;
        in_dma_ = true;
        schedule_irq_check();
        const auto previous_interrupts = interrupts_enabled_;
        interrupts_enabled_ = false;
        try {
            const auto bridge = real_mode_irq_;
            if (bridge) { irq_bridge(22, 14, 14); }
            if (dma_handler_) { dma_handler_(); }
            if (bridge) {
                execute(CpuOperation::disable_interrupts, 1);
                irq_bridge(17, 3, 10);
            }
        } catch (...) {
            in_dma_ = false;
            interrupts_enabled_ = previous_interrupts;
            throw;
        }
        in_dma_ = false;
        interrupts_enabled_ = previous_interrupts;
    }

    void schedule_keyboard() {
        keyboard_transfer_time_ = now_ + static_cast<long double>(keyboard_byte_delay_ms) * budget_;
        keyboard_transfer_ = static_cast<std::uint64_t>(std::ceil(keyboard_transfer_time_));
        keyboard_transfer_slice_ = static_cast<std::uint64_t>(std::floor(keyboard_transfer_time_));
    }
    void schedule_irq_check() {
        // The PIC schedules another arbitration event after accepting an IRQ.
        const auto ready = now_ + static_cast<long double>(irq_delay_ns_) * budget_ / nanoseconds_per_ms;
        request_irq_check(static_cast<std::uint64_t>(std::ceil(ready)), static_cast<std::uint64_t>(std::floor(ready)));
    }
    void request_irq_check(std::uint64_t ready, std::uint64_t slice) {
        // Arbitration is a queued device event even while IF prevents delivery.
        if (irq_check_ != no_deadline) { return; }
        irq_check_ = ready;
        irq_check_slice_ = slice;
        if (instruction_depth_ && core_end_ != no_deadline && slice < core_end_) { core_end_ = now_; }
    }
    void deliver_keyboard() {
        if (instruction_depth_ || interrupt_shadow_ || !interrupts_enabled_ || in_pit_ || keyboard_in_service_ ||
                !pending_keyboard_ || now_ < keyboard_ready_) { return; }
        const auto previous_interrupts = interrupts_enabled_;
        interrupts_enabled_ = false;
        keyboard_in_service_ = true;
        schedule_irq_check();
        try {
            const auto bridge = real_mode_irq_;
            if (bridge) { irq_bridge(22, 14, 14); }
            if (keyboard_handler_) { keyboard_handler_(); }
            if (bridge) {
                execute(CpuOperation::disable_interrupts, 1);
                irq_bridge(17, 3, 10);
            }
        } catch (...) {
            keyboard_in_service_ = false;
            interrupts_enabled_ = previous_interrupts;
            throw;
        }
        interrupts_enabled_ = previous_interrupts;
    }

    void irq_bridge(unsigned dispatches, unsigned first_stack, unsigned second_stack) {
        for (unsigned index = 0; index < dispatches; ++index) {
            execute_one(index == first_stack || index == second_stack ?
                CpuOperation::stack_segment : CpuOperation::instructions);
        }
    }

    CallbackAudio device_;
    RtcTiming rtc_;
    std::uint64_t rtc_deadline_ = no_deadline, rtc_slice_ = no_deadline;
    AudioChannelConfig fm_config_;
    VideoClock video_;
    long double video_setup_time_ = 0;
    std::uint64_t video_setup_deadline_ = no_deadline;
    mutable long double video_time_ = std::numeric_limits<long double>::infinity();
    mutable std::uint64_t video_deadline_ = 0, video_slice_ = 0;
    std::uint64_t video_deadline() const {
        // Retain cycle conversions until the next VGA event changes.
        if (video_time_ != video_.next()) {
            video_time_ = video_.next();
            const auto cycle = video_time_ * budget_;
            video_deadline_ = static_cast<std::uint64_t>(std::ceil(cycle));
            video_slice_ = static_cast<std::uint64_t>(cycle);
        }
        return video_deadline_;
    }
    unsigned rate_, budget_, frames_ = 0, fraction_ = 0, rendered_ = 0, digital_rate_ = 0, divisor_ = 0;
    unsigned irq_delay_ns_ = 0;
    std::uint64_t irq_check_ = no_deadline, irq_check_slice_ = no_deadline;
    bool pic_irq_check_ = false;
    std::uint64_t pit_ready_ = 0, dma_ready_ = 0;
    std::uint64_t now_ = 0, frame_start_ = 0, next_mix_, next_pit_ = no_deadline, pit_fraction_ = 0, consumed_ = 0;
    bool interrupts_enabled_ = true, pending_pit_ = false, in_pit_ = false, pending_dma_ = false, in_dma_ = false;
    std::function<void()> pit_handler_, dma_handler_;
    std::function<void()> keyboard_handler_;
    struct KeyArrival { std::uint64_t cycle; std::uint8_t scan; };
    std::deque<KeyArrival> key_arrivals_;
    std::deque<std::uint8_t> keyboard_bytes_;
    std::uint64_t keyboard_transfer_ = no_deadline, keyboard_ready_ = 0;
    std::uint64_t keyboard_transfer_slice_ = no_deadline, keyboard_ready_slice_ = 0;
    long double keyboard_transfer_time_ = 0;
    std::uint8_t keyboard_data_ = 0;
    bool pending_keyboard_ = false, keyboard_data_ready_ = false, keyboard_in_service_ = false;
    std::function<void(CpuOperation, std::uint64_t)> cpu_observer_;
    std::vector<std::uint8_t> dma_;
    std::size_t dma_cursor_ = 0;
    bool dma_active_ = false;
    std::vector<std::array<std::int16_t, 2>> output_;
    std::uint64_t core_end_ = no_deadline;
    CpuAccounting accounting_;
    unsigned instruction_depth_ = 0;
    unsigned interrupt_shadow_ = 0;
    bool frame_ended_ = false;
    bool real_mode_irq_ = false;
    DspTiming dsp_timing_;
    OplTiming opl_timing_;
    Pit2Timing pit2_timing_;
    std::uint64_t dsp_busy_until_ = no_deadline;
    std::uint64_t dsp_busy_slice_ = no_deadline;
    std::uint64_t dsp_reset_deadline_ = no_deadline;
    long double dsp_reset_time_ = 0;
    bool dsp_reset_held_ = false, dsp_reset_ready_ = false;
};

AudioScheduler::AudioScheduler(unsigned rate, unsigned budget) : impl_(std::make_unique<Impl>(rate, budget)) {}
AudioScheduler::~AudioScheduler() = default;
void AudioScheduler::run_cycles(std::uint64_t cycles) { impl_->run(cycles); }
void AudioScheduler::execute(CpuOperation operation, unsigned count) { impl_->execute(operation, count); }
void AudioScheduler::instruction(const std::function<void()>& action) { impl_->instruction(action); }
void AudioScheduler::read_file(unsigned bytes) { impl_->read_file(bytes); }
void AudioScheduler::file_metadata(DosFileCall call) { impl_->file_metadata(call); }
void AudioScheduler::write_file(unsigned bytes) { impl_->write_file(bytes); }
void AudioScheduler::set_video_timing(const VideoTiming& timing) { impl_->video_timing(timing); }
void AudioScheduler::initialize_video() { impl_->initialize_video(); }
void AudioScheduler::set_cpu_observer(std::function<void(CpuOperation, std::uint64_t)> observer) { impl_->cpu_observer(std::move(observer)); }
CpuAccounting AudioScheduler::accounting() const { return impl_->accounting(); }
void AudioScheduler::configure(AudioChannel channel, const AudioChannelConfig& config) { impl_->configure(channel, config); }
void AudioScheduler::write_fm(std::uint8_t reg, std::uint8_t value) { impl_->write_fm(reg, value); }
void AudioScheduler::write_fm_timed(std::uint8_t reg, std::uint8_t value, unsigned iterations,
                                  const std::function<void()>& on_write) { impl_->write_fm_timed(reg, value, iterations, on_write); }
void AudioScheduler::arm_dma(std::vector<std::uint8_t> bytes) { impl_->arm(std::move(bytes)); }
void AudioScheduler::halt_dma() { impl_->halt(); }
bool AudioScheduler::read_dsp_busy() { return impl_->read_dsp_busy(); }
void AudioScheduler::write_dsp_reset(std::uint8_t value) { impl_->write_dsp_reset(value); }
bool AudioScheduler::read_dsp_ready() { return impl_->read_dsp_ready(); }
std::uint8_t AudioScheduler::read_dsp_data() { return impl_->read_dsp_data(); }
std::uint8_t AudioScheduler::read_fm_status() { return impl_->read_fm_status(); }
void AudioScheduler::pit2_control(std::uint8_t value) { impl_->pit2_control(value); }
void AudioScheduler::pit2_write(std::uint8_t value) { impl_->pit2_write(value); }
std::uint8_t AudioScheduler::pit2_read() { return impl_->pit2_read(); }
void AudioScheduler::write_speaker(std::uint8_t value) { impl_->write_speaker(value); }
std::uint8_t AudioScheduler::read_speaker() { return impl_->read_speaker(); }
bool AudioScheduler::read_retrace() { return impl_->read_retrace(); }
void AudioScheduler::write_dsp_timed(std::uint8_t value, const std::function<void()>& on_write) { impl_->write_dsp_timed(value, on_write); }
void AudioScheduler::set_dma_handler(std::function<void()> handler) { impl_->dma_handler(std::move(handler)); }
void AudioScheduler::set_keyboard_handler(std::function<void()> handler) { impl_->keyboard_handler(std::move(handler)); }
void AudioScheduler::queue_key_byte(std::uint8_t scan) { impl_->queue_key(scan); }
void AudioScheduler::schedule_key_byte(std::uint64_t cycle, std::uint8_t scan) { impl_->schedule_key(cycle, scan); }
std::uint8_t AudioScheduler::read_keyboard() { return impl_->read_keyboard(); }
void AudioScheduler::acknowledge_keyboard() { impl_->acknowledge_keyboard(); }
void AudioScheduler::set_pit(unsigned divisor, std::function<void()> handler, PitStart start) { impl_->pit(divisor, std::move(handler), start); }
void AudioScheduler::program_pit(unsigned divisor, std::function<void()> handler, PitStart start) {
    impl_->program_pit(divisor, std::move(handler), start);
}
void AudioScheduler::enable_interrupts(bool enabled) { impl_->interrupts(enabled); }
void AudioScheduler::set_interrupt_delay(unsigned nanoseconds) { impl_->delay(nanoseconds); }
void AudioScheduler::flush() { impl_->flush(); }
std::vector<std::array<std::int16_t, 2>> AudioScheduler::take_output() { return impl_->take(); }
std::uint64_t AudioScheduler::cycles() const { return impl_->now(); }
std::uint64_t AudioScheduler::dma_consumed() const { return impl_->consumed(); }
std::size_t AudioScheduler::dma_remaining() const { return impl_->remaining(); }
}
