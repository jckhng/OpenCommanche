#include "menu_input.hpp"

namespace comanchecd {
namespace {
constexpr std::uint8_t scan_space = 0x39, scan_tab = 0x0f, scan_delete = 0x53;
constexpr std::uint8_t scan_escape = 0x01, scan_enter = 0x1c;
constexpr std::uint8_t scan_left_shift = 0x2a, scan_right_shift = 0x36;
constexpr std::array<std::uint8_t, 4> directions{0x4b, 0x4d, 0x48, 0x50};
}
std::uint8_t poll_list_key(const KeyboardInput& keyboard, ExecutionClock& clock) {
    const auto instructions = [&](unsigned n) { clock.execute(CpuOperation::instructions, n); };
    std::uint8_t scan = 0;
    clock.instruction([&] { scan = keyboard.pending_scan(); });
    instructions(2);
    if (scan == scan_space) { return scan; }
    instructions(2);
    if (scan == scan_tab) {
        instructions(3);
        if (keyboard.held(scan_left_shift) || keyboard.held(scan_right_shift)) { instructions(1); }
        return scan;
    }
    for (const auto direction : directions) {
        instructions(2);
        if (scan == direction) { return scan; }
    }
    // PUSH/CALL 19512, ordinary Alt-up return, POP.
    instructions(6);
    instructions(2);
    if (scan == scan_delete) { return scan; }
    instructions(2);
    if (scan == scan_escape) { return scan; }
    instructions(2);
    return scan == scan_enter ? scan : 0;
}
}
