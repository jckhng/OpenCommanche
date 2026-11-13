#pragma once

#include "comanchecd/execution_clock.hpp"
#include <array>

namespace comanchecd {
constexpr std::size_t keyboard_scan_count = 128;
constexpr std::size_t keyboard_image_bytes = keyboard_scan_count * 2 + 6;
struct KeyboardImage {
    std::array<std::uint8_t, keyboard_image_bytes> bytes{};
    std::uint8_t throttle = 0;
    KeyboardImage();
};
class KeyboardPort {
public:
    virtual ~KeyboardPort() = default;
    virtual std::uint8_t read() = 0;
    virtual void acknowledge() = 0;
};

// Canonical held, pressed, overlapping pending-scan and last-scan storage.
class KeyboardInput {
public:
    explicit KeyboardInput(KeyboardImage image = {});
    void initialize(ExecutionClock& clock);
    void interrupt(ExecutionClock& clock, KeyboardPort& port);
    bool held(std::uint8_t scan) const;
    bool take_press(std::uint8_t scan);
    std::uint8_t take_scan();
    std::uint8_t take_scan(ExecutionClock& clock);
    std::uint8_t pending_scan() const;
    void clear_pending();
    std::uint8_t throttle() const;
    const KeyboardImage& image() const;

private:
    KeyboardImage image_;
};
} // namespace comanchecd
