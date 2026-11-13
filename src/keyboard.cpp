#include "comanchecd/keyboard.hpp"
#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::size_t pending_offset = keyboard_scan_count * 2;
constexpr std::size_t last_scan = pending_offset + 5;
constexpr std::uint8_t release_bit = 0x80, throttle_step = 0x10, throttle_ceiling = 0xe6;
}
KeyboardImage::KeyboardImage() {
    std::fill_n(bytes.begin(), keyboard_scan_count, 0xff);
}
KeyboardInput::KeyboardInput(KeyboardImage image) : image_(std::move(image)) {}

void KeyboardInput::initialize(ExecutionClock& clock) {
    // Cold 18fd installer: clear canonical storage before attaching IRQ1.
    clock.execute(CpuOperation::instructions, 9);
    clock.execute(CpuOperation::repeat_store_dwords, keyboard_scan_count / 4);
    std::fill_n(image_.bytes.begin(), keyboard_scan_count, 0xff);
    clock.execute(CpuOperation::instructions, 2);
    constexpr auto cleared_bytes = keyboard_scan_count + 4;
    clock.execute(CpuOperation::repeat_store_dwords, cleared_bytes / 4);
    std::fill_n(image_.bytes.begin() + keyboard_scan_count, cleared_bytes, 0);
    clock.execute(CpuOperation::instructions, 24);
}

void KeyboardInput::interrupt(ExecutionClock& clock, KeyboardPort& port) {
    const auto instructions = [&](unsigned count) { clock.execute(CpuOperation::instructions, count); };
    instructions(1);
    const auto scan = port.read(); // IN 60h also schedules the next controller byte.
    instructions(1);
    clock.execute(CpuOperation::port_read_byte, 1);
    instructions(1);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(2);
    clock.instruction([&] { image_.bytes[scan & ~release_bit] = scan; });
    instructions(2);
    if ((scan & release_bit) == 0) {
        clock.instruction([&] { image_.bytes[keyboard_scan_count + scan] = scan; });
        instructions(2);
        auto lower = scan == 0x20;
        if (!lower) { instructions(2); lower = scan == 0x4e; }
        if (!lower) { instructions(2); lower = scan == 0x0d; }
        if (lower) {
            instructions(2);
            if (image_.throttle >= throttle_step) {
                clock.instruction([&] { image_.throttle -= throttle_step; });
                instructions(1);
            }
        } else {
            instructions(2);
            auto raise = scan == 0x12;
            if (!raise) { instructions(2); raise = scan == 0x4a; }
            if (!raise) { instructions(2); raise = scan == 0x0c; }
            if (raise) {
                instructions(2);
                if (image_.throttle < throttle_ceiling) {
                    clock.instruction([&] { image_.throttle += throttle_step; });
                    instructions(1);
                }
            }
        }
        std::array<std::uint8_t, 4> pending{};
        clock.instruction([&] { std::copy_n(image_.bytes.begin() + pending_offset, pending.size(), pending.begin()); });
        clock.instruction([&] { std::copy(pending.begin(), pending.end(), image_.bytes.begin() + pending_offset + 1); });
        clock.instruction([&] { image_.bytes[pending_offset] = scan; });
        clock.instruction([&] { image_.bytes[last_scan] = scan; });
    }
    instructions(2);
    if (scan == 0xff) {
        instructions(6);
        clock.execute(CpuOperation::repeat_store_dwords, keyboard_scan_count / 4);
        std::fill_n(image_.bytes.begin(), keyboard_scan_count, 0xff);
        instructions(2);
        clock.execute(CpuOperation::repeat_store_dwords, keyboard_scan_count / 4 + 1);
        std::fill_n(image_.bytes.begin() + keyboard_scan_count, keyboard_scan_count + 4, 0);
        instructions(3);
    } else {
        instructions(4);
        if ((image_.bytes[0x38] | image_.bytes[0x1d] | image_.bytes[0x53]) < release_bit) {
            throw std::invalid_argument("DOS keyboard reboot is not a native session action");
        }
        instructions(1);
    }
    port.acknowledge();
    instructions(3);
}

bool KeyboardInput::held(std::uint8_t scan) const {
    if (scan >= keyboard_scan_count) { throw std::invalid_argument("Invalid keyboard scan index"); }
    return image_.bytes[scan] < release_bit;
}
bool KeyboardInput::take_press(std::uint8_t scan) {
    if (scan >= keyboard_scan_count) { throw std::invalid_argument("Invalid keyboard scan index"); }
    auto& value = image_.bytes[keyboard_scan_count + scan];
    const auto pressed = value != 0;
    value = 0;
    return pressed;
}
std::uint8_t KeyboardInput::take_scan() {
    const auto scan = image_.bytes[pending_offset];
    std::copy_n(image_.bytes.begin() + pending_offset + 1, 3, image_.bytes.begin() + pending_offset);
    image_.bytes[pending_offset + 3] = 0;
    return scan;
}
std::uint8_t KeyboardInput::take_scan(ExecutionClock& clock) {
    std::uint8_t scan = 0;
    clock.execute(CpuOperation::disable_interrupts, 1);
    clock.instruction([&] { scan = pending_scan(); });
    clock.instruction([&] { take_scan(); });
    clock.execute(CpuOperation::enable_interrupts, 1);
    return scan;
}
std::uint8_t KeyboardInput::throttle() const { return image_.throttle >> 4; }
const KeyboardImage& KeyboardInput::image() const { return image_; }
std::uint8_t KeyboardInput::pending_scan() const { return image_.bytes[pending_offset]; }
void KeyboardInput::clear_pending() { image_.bytes[pending_offset] = 0; }
} // namespace comanchecd
