#include "../src/flight_work.hpp"
#include "../src/flight_input.hpp"
#include "comanchecd/flight.hpp"
#include "comanchecd/world_renderer.hpp"
#include "loading_clock.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
}

int main() {
    try {
        using namespace comanchecd;
        FlightKeyContext key_context;
        std::vector<std::pair<FlightKeyAction, unsigned>> actions;
        const FlightKeyHandler key_action = [&](FlightKeyAction action, unsigned value) {
            actions.emplace_back(action, value);
        };
        LoadingClock passive;
        dispatch_flight_key(0x12, key_context, key_action, passive);
        require(passive.accounting().dispatches == 89 && actions.empty(),
                "A passive make code must traverse the released-Alt action path");
        key_context.alt_down = [] { return true; };
        LoadingClock modified;
        dispatch_flight_key(0x12, key_context, key_action, modified);
        require(modified.accounting().dispatches == 85 && actions.empty(),
                "Held Alt must skip the two previous-panel key comparisons");
        LoadingClock view_clock;
        dispatch_flight_view(no_view_request, 1, key_action, view_clock);
        require(view_clock.accounting().dispatches == 9 && actions.empty(),
                "The idle view caller must test all four request codes and return");
        key_context.alt_down = {};
        for (const auto mode : {5, 6, 8}) {
            key_context.view = mode;
            LoadingClock repeated;
            dispatch_flight_key(mode + 1, key_context, key_action, repeated);
            require(repeated.accounting().dispatches == 91 && actions.empty(),
                    "Repeated auxiliary views must fall through the remaining action comparisons");
        }
        key_context.weapon = 5;
        LoadingClock weapon;
        dispatch_flight_key(0x1b, key_context, key_action, weapon);
        require(weapon.accounting().dispatches == 40 && actions ==
                std::vector<std::pair<FlightKeyAction, unsigned>>{{FlightKeyAction::weapon, 0}},
                "Next weapon must wrap once before the external ammunition child");
        LoadingClock tail;
        dispatch_flight_key(0x28, key_context, [&](FlightKeyAction action, unsigned) {
            require(action == FlightKeyAction::flare, "The apostrophe key must tail-call flare allocation");
            tail.execute(CpuOperation::instructions, 7);
        }, tail);
        require(tail.accounting().dispatches == 10,
                "A tail-called countermeasure must retain its child work without another parent RET");
        FlightState player;
        std::array<std::uint8_t, 6> warned{};
        std::vector<std::uint8_t> announcements;
        std::uint64_t loop_instructions = 0;
        const auto loop_work = [&](CpuOperation operation, unsigned count) {
            require(operation == CpuOperation::instructions, "Normal flight entry must contain instruction work");
            loop_instructions += count;
        };
        const auto announce = [&](std::uint8_t sound) { announcements.push_back(sound); };
        flight_work::loop_prefix(0, player, warned, announce, loop_work);
        require(loop_instructions == 47 && announcements.empty(), "Initial loop must scan all six healthy systems");
        loop_instructions = 0;
        flight_work::loop_prefix(1, player, warned, announce, loop_work);
        require(loop_instructions == 16, "Ordinary loop must skip the periodic system scan");
        player.system_condition.fill(255);
        loop_instructions = 0;
        flight_work::loop_prefix(32, player, warned, announce, loop_work);
        require(loop_instructions == 77 && announcements == std::vector<std::uint8_t>{14, 13, 15, 13, 4, 8},
                "System warnings must latch and dispatch in descending original order");
        loop_instructions = 0;
        announcements.clear();
        flight_work::loop_prefix(64, player, warned, announce, loop_work);
        require(loop_instructions == 35 && announcements.empty(), "Latched system warnings must remain suppressed");
        LoadingClock clock;
        SceneView view;
        unsigned samples = 0;
        flight_work::camera(view, clock, [&] {
            require(clock.accounting().dispatches == 52, "Camera must sample shake at SUB 86fd");
            ++samples;
            return 3;
        });
        require(samples == 1 && view.vertical_pan == 3 && clock.accounting().dispatches == 4091,
                "Cockpit camera must charge interpolation, callback, clamp and unrolled horizon work");
        view.pitch = 128;
        view.roll = 5;
        const auto before = clock.accounting().dispatches;
        flight_work::camera(view, clock, [] { return 0; });
        require(clock.accounting().dispatches - before == 4092, "Upper roll clamp must charge its conditional MOV");
        view.roll = -5;
        const auto lower = clock.accounting().dispatches;
        flight_work::camera(view, clock, [] { return 0; });
        require(clock.accounting().dispatches - lower == 4092, "Lower roll clamp must charge its conditional MOV");
        TerrainFrame frame;
        constexpr unsigned levels = 16;
        frame.depth.assign(levels * scene_buffer_side, 0);
        frame.camera.horizon.fill(250);
        for (unsigned column = 0; column < scene_columns; ++column) {
            frame.depth[15 * scene_buffer_side + 32 + column] = 20;
            frame.depth[14 * scene_buffer_side + 32 + column] = 10;
            frame.depth[12 * scene_buffer_side + 32 + column] = 5;
        }
        std::uint64_t dispatches = 0;
        TerrainFrame monitor;
        monitor.depth.resize(3 * scene_buffer_side);
        std::uint64_t monitor_dispatches = 0, monitor_iterations = 0;
        flight_work::monitor(monitor, 3, 0, CloudMode::flat, [&](CpuOperation operation, unsigned count) {
            if (operation == CpuOperation::instructions) { monitor_dispatches += count; }
            else {
                require(operation == CpuOperation::repeat_store_dwords || operation == CpuOperation::repeat_copy_dwords,
                        "Flat monitor work must contain only instructions and bounded dword strings");
                ++monitor_dispatches;
                monitor_iterations += count;
            }
        });
        require(monitor_dispatches == 1752 && monitor_iterations == 3860,
                "Monitor work must retain sky clear, three-row depth copies and both source-plane halves");
        const CpuWorkSink work = [&](CpuOperation operation, unsigned count) {
            require(operation == CpuOperation::instructions, "Foreground routines contain no string or I/O operations");
            dispatches += count;
        };
        flight_work::fog(frame, levels, work);
        require(dispatches == 2708 + 192 * 61,
                "Fog must use byte-wrapped band comparisons, including the skipped quarter band");
        dispatches = 0;
        std::array<std::int32_t, scene_slots> depth{};
        flight_work::model_sort(depth, work);
        require(dispatches == 69644, "Equal-depth sort must execute each replacement, including ties");
        dispatches = 0;
        flight_work::model_projection(3, work);
        require(dispatches == 761, "Model projection must charge all slots and only active coordinate transforms");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
