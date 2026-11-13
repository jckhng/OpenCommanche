#include "../src/fanfare_timing.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
class Clock final : public comanchecd::ExecutionClock {
public:
    explicit Clock(comanchecd::AudioScheduler& scheduler) : scheduler_(scheduler) {}
    void execute(comanchecd::CpuOperation operation, unsigned count) override { scheduler_.execute(operation, count); }
    void instruction(const std::function<void()>& action) override { scheduler_.instruction(action); }
    void read_file(unsigned bytes) override { scheduler_.read_file(bytes); }
    bool read_retrace() override { return scheduler_.read_retrace(); }
    std::uint64_t cycles() const override { return scheduler_.cycles(); }
    std::uint64_t irq_ticks() const override { return 0; }
    std::uint64_t sample_irq_ticks() override { scheduler_.instruction([] {}); return 0; }
private:
    comanchecd::AudioScheduler& scheduler_;
};

comanchecd::FanfareAssets fixture(unsigned bytes) {
    comanchecd::FanfareAssets assets;
    assets.score = {'K', 'R', 'L', '0'};
    for (unsigned shift = 0; shift < 32; shift += 8) {
        assets.score.push_back(static_cast<std::uint8_t>(bytes >> shift));
    }
    assets.score.resize(8 + bytes, 17);
    assets.picture.resize(128, 0);
    constexpr unsigned width_minus_one = comanchecd::line_stride * comanchecd::plane_count - 1;
    assets.picture[8] = width_minus_one & 255;
    assets.picture[9] = width_minus_one >> 8;
    unsigned remaining = width_minus_one + 1;
    while (remaining) {
        const auto count = std::min(remaining, 63u);
        assets.picture.push_back(192 | count);
        assets.picture.push_back(0);
        remaining -= count;
    }
    assets.picture.push_back(12);
    assets.picture.resize(assets.picture.size() + 768, 0);
    return assets;
}
}

int main() {
    try {
        using namespace comanchecd;
        for (const auto bytes : {50u, 8192u, 8197u}) {
            AudioScheduler scheduler(audio_sample_rate);
            Clock clock(scheduler);
            const auto assets = fixture(bytes);
            std::vector<FanfareBoundary> boundaries;
            const auto score = prepare_fanfare(scheduler, clock, assets,
                [&](FanfareBoundary boundary) { boundaries.push_back(boundary); });
            if (score != std::vector<std::uint8_t>(bytes, 17) || boundaries.size() != 11 ||
                    boundaries.front() != FanfareBoundary::resource || boundaries.back() != FanfareBoundary::ready ||
                    scheduler.accounting().string_iterations == 0) {
                throw std::runtime_error("Raw fanfare loading must preserve exact/full/partial refill payloads and ordered preparation");
            }
        }
        AudioScheduler scheduler(audio_sample_rate);
        Clock clock(scheduler);
        auto invalid = fixture(50);
        invalid.picture[8] = 0;
        invalid.picture[9] = 0;
        bool rejected = false;
        try { prepare_fanfare(scheduler, clock, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected || scheduler.cycles() != 0) {
            throw std::runtime_error("Unsupported fanfare geometry must reject before clock work");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
