#include "flight_input.hpp"
#include "comanchecd/execution_clock.hpp"

namespace comanchecd {
namespace {
constexpr unsigned panel_count = 6, weapon_count = 6;
constexpr std::array<std::uint8_t, panel_count> right_panel_keys{0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
constexpr std::array<std::uint8_t, weapon_count> weapon_keys{0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31};
constexpr std::array<std::uint8_t, weapon_count> alternate_weapon_keys{0x30, 0x11, 0x13, 0x14, 0x2e, 0x21};

void view_key(unsigned mode, std::uint8_t current, const FlightKeyHandler& handler, ExecutionClock& clock) {
    if (mode != 7) {
        clock.execute(CpuOperation::instructions, 2);
    }
    // The view transaction owns resource loading, nested frames and page/palette work.
    if (mode == 7 || mode != current) { handler(FlightKeyAction::view, mode); }
    clock.execute(CpuOperation::instructions, 1);
}
}

void dispatch_flight_view(std::uint8_t request, std::uint8_t current,
                          const FlightKeyHandler& handler, ExecutionClock& clock) {
    constexpr std::array<std::uint8_t, 4> requests{0, 3, 1, 2};
    for (unsigned index = 0; index < requests.size(); ++index) {
        clock.execute(CpuOperation::instructions, 2);
        if (request == requests[index]) {
            view_key(index + 1, current, handler, clock);
            return;
        }
    }
    clock.execute(CpuOperation::instructions, 1);
}

void dispatch_flight_key(std::uint8_t scan, const FlightKeyContext& context,
                         const FlightKeyHandler& handler, ExecutionClock& clock) {
    const auto instructions = [&](unsigned count) { clock.execute(CpuOperation::instructions, count); };
    for (const auto key : {0x28, 0x27}) {
        instructions(2);
        if (scan == key) {
            instructions(1);
            handler(key == 0x28 ? FlightKeyAction::flare : FlightKeyAction::chaff, 0);
            return;
        }
    }
    for (unsigned panel = 0; panel < panel_count; ++panel) {
        instructions(2);
        if (scan == 0x3b + panel) {
            instructions(panel == 0 ? 6 : 7);
            handler(FlightKeyAction::left_panel, panel);
            return;
        }
    }
    for (unsigned panel = 0; panel < panel_count; ++panel) {
        instructions(2);
        if (scan == right_panel_keys[panel]) {
            instructions(panel == 0 ? 6 : 7);
            handler(FlightKeyAction::right_panel, panel);
            return;
        }
    }
    instructions(2);
    if (scan == 0x1a) {
        instructions(6 + unsigned(context.weapon == 0));
        handler(FlightKeyAction::weapon, (context.weapon + weapon_count - 1) % weapon_count);
        instructions(1);
        return;
    }
    instructions(2);
    if (scan == 0x1b) {
        instructions(6 + unsigned(context.weapon + 1 == weapon_count));
        handler(FlightKeyAction::weapon, (context.weapon + 1) % weapon_count);
        instructions(1);
        return;
    }
    for (unsigned mode = 1; mode <= 8; ++mode) {
        instructions(2);
        if (scan == mode + 1) {
            if ((mode == 5 || mode == 6) && mode == context.view) {
                instructions(2);
                continue;
            }
            if (mode == 8) {
                instructions(2);
                if (context.view == 8) { continue; }
                handler(FlightKeyAction::view, 7);
                instructions(1);
            } else {
                view_key(mode, context.view, handler, clock);
            }
            return;
        }
    }
    instructions(2);
    if (scan == 0x24) {
        handler(FlightKeyAction::calibrate, 0);
        return;
    }
    instructions(2);
    if (scan == 0x37) {
        instructions(1);
        handler(FlightKeyAction::input_flags, 32);
        instructions(1);
        return;
    }
    instructions(2);
    if (scan == 0x01) {
        instructions(1);
        handler(FlightKeyAction::settings, 0);
        instructions(2);
        return;
    }
    instructions(2); // Retail debug-recording gate at 1a5f6.
    instructions(2);
    if (context.alternate) {
        instructions(2);
        if (scan == 0x1e) {
            instructions(1);
            handler(FlightKeyAction::input_flags, 0);
        }
    } else {
        bool alt = false;
        clock.instruction([&] { alt = context.alt_down && context.alt_down(); });
        instructions(1);
        if (!alt) {
            for (unsigned panel = 0; panel < 2; ++panel) {
                instructions(2);
                if (scan == (panel == 0 ? 0x10 : 0x1e)) {
                    const auto selected = panel == 0 ? context.left_panel : context.right_panel;
                    instructions(8 + (selected == 0 ? 2 : 0));
                    handler(panel == 0 ? FlightKeyAction::left_panel : FlightKeyAction::right_panel,
                            (selected + panel_count - 1) % panel_count);
                    return;
                }
            }
        }
        for (unsigned panel = 0; panel < 2; ++panel) {
            instructions(2);
            if (scan == (panel == 0 ? 0x11 : 0x1f)) {
                const auto selected = panel == 0 ? context.left_panel : context.right_panel;
                instructions(9 + (selected + 1 == panel_count ? 2 : 0));
                handler(panel == 0 ? FlightKeyAction::left_panel : FlightKeyAction::right_panel,
                        (selected + 1) % panel_count);
                return;
            }
        }
    }
    const auto& keys = context.alternate ? alternate_weapon_keys : weapon_keys;
    for (unsigned weapon = 0; weapon < weapon_count; ++weapon) {
        instructions(2);
        if (scan == keys[weapon]) {
            instructions(5);
            handler(FlightKeyAction::weapon, weapon);
            instructions(1);
            return;
        }
    }
    if (!context.alternate) {
        instructions(2);
        if (scan == 0x32) {
            instructions(1);
            handler(FlightKeyAction::rockets, 0);
            instructions(1);
            handler(FlightKeyAction::rockets, 0);
            instructions(1);
            return;
        }
    }
    constexpr std::array<std::uint8_t, 3> zoom_keys{0x15, 0x14, 0x13}, alternate_zoom_keys{0x1f, 0x32, 0x22};
    const auto& zoom = context.alternate ? alternate_zoom_keys : zoom_keys;
    for (unsigned direction = 0; direction < zoom.size(); ++direction) {
        instructions(2);
        if (scan == zoom[direction]) {
            instructions(1);
            handler(FlightKeyAction::zoom, direction);
        }
    }
    instructions(1);
}
}
