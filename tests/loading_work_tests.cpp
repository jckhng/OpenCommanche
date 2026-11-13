#include "../src/color_metric.hpp"
#include "../src/loading_work.hpp"
#include "loading_clock.hpp"
#include "comanchecd/world_renderer.hpp"
#include "../src/flight_work.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        using namespace comanchecd;
        Transfer transfer{};
        for (std::size_t index = 0; index < transfer.size(); ++index) {
            transfer[index] = static_cast<std::uint8_t>(index);
        }
        Palette palette{};
        CpuAccounting totals;
        const CpuWorkSink work = [&](CpuOperation operation, unsigned count) {
            totals.dispatches += count;
            if (operation == CpuOperation::string_bytes) {
                totals.string_iterations += count;
            } else {
                require(operation == CpuOperation::instructions, "Palette search must be device-free");
            }
        };
        auto selected = color_metric::nearest(palette, transfer, {0, 0, 0}, 1, 254, work);
        require(selected == 1 && totals.dispatches == 33 && totals.string_iterations == 3,
                "An exact first palette candidate must return after the zero-distance branch");
        totals = {};
        selected = color_metric::nearest(palette, transfer, {1, 0, 0}, 1, 2, work);
        require(selected == 1 && totals.dispatches == 48 && totals.string_iterations == 6,
                "An equal nonzero distance must retain the first candidate and exhaust the range");
        palette[2] = {1, 0, 0};
        totals = {};
        selected = color_metric::nearest(palette, transfer, {1, 0, 0}, 1, 254, work);
        require(selected == 2 && totals.dispatches == 50 && totals.string_iterations == 6,
                "A later exact candidate must take the smaller-distance and early-return paths");
        std::vector<std::uint8_t> picture(130);
        picture[128] = 7;
        LoadingClock clock;
        const auto decoded = loading_work::picture(picture, clock);
        const auto cost = clock.accounting();
        const std::vector<std::uint8_t> short_resource{'K', 'R', 'L', '1', 1, 0, 0, 0, 7};
        loading_work::InputBuffer retained{};
        retained[1] = 12;
        LoadingClock retained_clock;
        const auto short_decoded = loading_work::compressed_resource(short_resource, retained_clock, retained);
        require(short_decoded.bytes == std::vector<std::uint8_t>{7} &&
                retained_clock.accounting().string_iterations == 1539 && retained[1] == 12,
                "A short KRL1 refill must retain its preceding buffer tail and consume a carried palette marker");
        GameplayPayload mask;
        mask.width = line_stride * plane_count;
        mask.height = 182;
        mask.bytes.assign(mask.width * mask.height, 255);
        std::uint64_t mask_instructions = 0;
        loading_work::cockpit_spans(mask, [&](CpuOperation operation, unsigned count) {
            require(operation == CpuOperation::instructions, "Span construction uses ordinary CPU work");
            mask_instructions += count;
        });
        require(mask_instructions == 166376,
                "A fully transparent cockpit must include both pair scans, all row spans and the final pointer fixups");
        require(decoded.bytes == std::vector<std::uint8_t>{7} && cost.string_iterations == 3 &&
                cost.dispatches - cost.repeat_restarts - 49 - 13 * cost.idle_calls - 19 - 1 == 18 &&
                cost.stack_segment_credits == 3,
                "Timed picture loading must preserve pixels and charge the decoder, refill helper and DOS bridge separately");
        LoadingClock display;
        flight_work::present(display);
        const auto display_cost = display.accounting();
        const auto polls = (display_cost.dispatches - 45) / 3;
        require(display_cost.dispatches == 45 + 3 * polls && display_cost.io_cycles == 69 + 20 * polls &&
                display.cycles() >= 311420 && display.cycles() < 311500,
                "Flight presentation must charge its port sequence and wait for the shared VGA retrace");
        CpuAccounting rendering;
        const CpuWorkSink render_work = [&](CpuOperation operation, unsigned count) {
            if (operation == CpuOperation::repeat_store_dwords || operation == CpuOperation::repeat_copy_dwords) {
                ++rendering.dispatches;
                rendering.string_iterations += count;
            } else {
                rendering.dispatches += count;
                if (operation == CpuOperation::string_bytes) { rendering.string_iterations += count; }
            }
        };
        flight_work::sky(0, render_work);
        require(rendering.dispatches == 88149 && rendering.string_iterations == 28800,
                "Panoramic sky CPU work must include the wrapped source column");
        rendering = {};
        flight_work::sky(0x40000000, render_work);
        require(rendering.dispatches == 88147 && rendering.string_iterations == 28800,
                "An unwrapped sky must omit the two wrap instructions");
        TerrainFrame terrain;
        terrain.depth.resize(scene_buffer_side);
        terrain.depth[32 + 95] = 3;
        rendering = {};
        flight_work::terrain(terrain, 1, render_work);
        require(rendering.dispatches == 1994 && rendering.string_iterations == 96,
                "Terrain CPU work must account for its span entry, unrolled RET and dword depth copy");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
