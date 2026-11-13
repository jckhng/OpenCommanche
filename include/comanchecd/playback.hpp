#pragma once

#include "comanchecd/menu_renderer.hpp"
#include "comanchecd/timing.hpp"
#include "comanchecd/cpu_work.hpp"
#include "comanchecd/execution_clock.hpp"

namespace comanchecd {
class KdvMovie {
public:
    static constexpr std::size_t width = 320;
    static constexpr std::uint64_t frame_irq_ticks = 3;
    explicit KdvMovie(std::vector<std::uint8_t> bytes);
    std::size_t frame_count() const;
    std::size_t height() const;
    const Palette& palette() const;
    std::vector<std::uint8_t> frame(std::size_t index, const CpuWorkSink& work = {}) const;
    void blit_work(const CpuWorkSink& work) const;

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<std::size_t> offsets_;
    std::size_t height_;
    Palette palette_{};
};

// Nominal VGA/PIT wait ordering; does not account for decoder CPU overruns.
class KdvClock {
public:
    explicit KdvClock(std::uint64_t elapsed_samples);
    std::size_t advance(std::size_t samples);
private:
    IrqClock irq_;
    std::uint64_t video_phase_ = 0, frame_irqs_ = 0;
    bool presenting_ = false;
};

// The movie and execution clock must outlive this player.
class KdvPlayer {
public:
    KdvPlayer(const KdvMovie& movie, ExecutionClock& clock, std::size_t hold_frames = 0);
    std::uint64_t clear_pages();
    void prepare();
    const std::vector<std::uint8_t>& pixels() const;
    std::uint64_t ready_sample() const;
    bool finished() const;

private:
    enum class RetraceState { inactive, active };
    const KdvMovie& movie_;
    ExecutionClock& clock_;
    std::size_t prepared_ = 0, hold_frames_;
    std::uint64_t frame_irq_, ready_sample_ = 0;
    std::vector<std::uint8_t> pixels_;
    void wait_until(RetraceState state);
    void wait_retrace();
    void present();
    void program_page();
    void clear_page();
    void palette_work();
};

struct DemoPose {
    std::uint32_t x, y, heading;
    std::uint8_t height;
};

struct DemoInput {
    std::uint8_t key, power, trigger, target;
    std::int16_t horizontal, vertical;
};

class DemoRecording {
public:
    explicit DemoRecording(const std::vector<std::uint8_t>& bytes);
    const DemoPose& pose() const;
    const std::vector<DemoInput>& inputs() const;

private:
    DemoPose pose_{};
    std::vector<DemoInput> inputs_;
};
} // namespace comanchecd
