#pragma once

#include "comanchecd/callback_audio.hpp"
#include "comanchecd/cpu_work.hpp"
#include "comanchecd/video_timing.hpp"
#include "comanchecd/timing.hpp"
#include <functional>

namespace comanchecd {
enum class DosFileCall { find, open_read, close, create };
// CPU work is supplied in normal-core cycle-budget units. Device deadlines
// and callback lengths follow from that clock and retained channel state.
class AudioScheduler {
public:
    explicit AudioScheduler(unsigned output_rate, unsigned cycles_per_ms = 20000);
    ~AudioScheduler();
    void run_cycles(std::uint64_t cycles);
    void execute(CpuOperation operation, unsigned count);
    void instruction(const std::function<void()>& action);
    void read_file(unsigned bytes);
    void write_file(unsigned bytes);
    void file_metadata(DosFileCall call);
    void initialize_video();
    void set_video_timing(const VideoTiming& timing);
    void set_cpu_observer(std::function<void(CpuOperation, std::uint64_t)> observer);
    CpuAccounting accounting() const;
    void configure(AudioChannel channel, const AudioChannelConfig& config);
    void write_fm(std::uint8_t reg, std::uint8_t value);
    void write_fm_timed(std::uint8_t reg, std::uint8_t value, unsigned delay_iterations,
                        const std::function<void()>& on_write = {});
    void arm_dma(std::vector<std::uint8_t> bytes);
    void halt_dma();
    bool read_dsp_busy();
    void write_dsp_reset(std::uint8_t value);
    bool read_dsp_ready();
    std::uint8_t read_dsp_data();
    std::uint8_t read_fm_status();
    void pit2_control(std::uint8_t value);
    void pit2_write(std::uint8_t value);
    std::uint8_t pit2_read();
    void write_speaker(std::uint8_t value);
    std::uint8_t read_speaker();
    bool read_retrace();
    void write_dsp_timed(std::uint8_t value, const std::function<void()>& on_write = {});
    void set_dma_handler(std::function<void()> handler);
    void set_keyboard_handler(std::function<void()> handler);
    void queue_key_byte(std::uint8_t scan);
    // Ordered arrivals in this scheduler's absolute CPU-cycle coordinate.
    void schedule_key_byte(std::uint64_t cycle, std::uint8_t scan);
    std::uint8_t read_keyboard();
    void acknowledge_keyboard();
    void set_pit(unsigned divisor, std::function<void()> handler, PitStart start = PitStart::periodic);
    void program_pit(unsigned divisor, std::function<void()> handler, PitStart start);
    void enable_interrupts(bool enabled);
    void set_interrupt_delay(unsigned nanoseconds);
    void flush();
    std::vector<std::array<std::int16_t, 2>> take_output();
    std::uint64_t cycles() const;
    std::uint64_t dma_consumed() const;
    std::size_t dma_remaining() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
