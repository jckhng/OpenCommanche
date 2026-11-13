#include "comanchecd/cockpit.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> sprites(unsigned count) {
    const auto header = (count + 1) * 8;
    std::vector<std::uint8_t> bytes(header + count * 64, 0);
    bytes[0] = 'S';
    bytes[1] = 'G';
    bytes[2] = static_cast<std::uint8_t>(count);
    for (unsigned frame = 1; frame <= count; ++frame) {
        const auto source = header + (frame - 1) * 64;
        bytes[frame * 8] = 4;
        bytes[frame * 8 + 1] = 1;
        for (unsigned byte = 0; byte < 4; ++byte) {
            bytes[frame * 8 + 4 + byte] = static_cast<std::uint8_t>(source >> (byte * 8));
        }
    }
    return bytes;
}
}

int main() {
    try {
        using namespace comanchecd;
        CockpitAssets assets;
        assets.normal_picture.assign(cockpit_backing_width * 290, 3);
        assets.crash_picture.assign(cockpit_backing_width * 245, 9);
        assets.console = sprites(57);
        assets.hud = sprites(33);
        assets.normal_picture[110 * cockpit_backing_width] = 255;
        assets.normal_picture[110 * cockpit_backing_width + 1] = 255;
        assets.normal_picture[109 * cockpit_backing_width + 325] = 255;
        assets.palette[7] = {63, 12, 0};
        CockpitTables tables;
        tables.shake_displacement = {0, 81, 164, 245};
        tables.map_scale = 0x08000000;
        FlightState player;
        CockpitRenderer renderer(assets, tables, player);
        std::uint64_t display_dispatches = 0, display_iterations = 0, display_ports = 0;
        renderer.initialize_displays(player, [&](CpuOperation operation, unsigned count) {
            if (operation == CpuOperation::repeat_copy_bytes) {
                ++display_dispatches;
                display_iterations += count;
            } else {
                display_dispatches += count;
                if (operation == CpuOperation::string_bytes) { display_iterations += count; }
                if (operation == CpuOperation::port_write_word) { display_ports += count; }
            }
        });
        require(display_dispatches == 2070 && display_iterations == 96 && display_ports == 98,
                "Initial display work must include repeated ammunition and lamps, and one countermeasure call");
        CockpitRenderer pending(assets, tables, player);
        std::uint64_t pending_work = 0;
        const CpuWorkSink count_pending = [&](CpuOperation, unsigned count) { pending_work += count; };
        pending.refresh_stores(player, count_pending);
        const auto stores_work = pending_work;
        pending_work = 0;
        pending.refresh_countermeasures(player, count_pending);
        const auto countermeasure_work = pending_work;
        auto copied_pending = pending;
        pending_work = 0;
        pending.update_pending(player, count_pending);
        require(pending_work == 6 + 2 + stores_work + 2 + countermeasure_work,
                "The following loop must redraw both pending status regions at their original guards");
        pending_work = 0;
        pending.update_pending(player, count_pending);
        require(pending_work == 6, "Status redraws must be consumed once");
        pending_work = 0;
        copied_pending.update_pending(player, count_pending);
        require(pending_work == 6 + 2 + stores_work + 2 + countermeasure_work,
                "A copied cockpit transaction must retain its own pending redraws");
        TerrainFrame frame;
        frame.pixels.fill(7);
        std::uint64_t preparation_work = 0;
        renderer.prepare_frame([&](CpuOperation operation, unsigned count) {
            require(operation == CpuOperation::instructions, "An empty marker requires no device work");
            preparation_work += count;
        });
        require(preparation_work == 8, "Loop preparation must include the empty target-marker caller");
        display_dispatches = display_iterations = display_ports = 0;
        renderer.begin_frame(frame, 1, {}, [&](CpuOperation operation, unsigned count) {
            display_dispatches += count;
            if (operation == CpuOperation::string_bytes) { display_iterations += count; }
            if (operation == CpuOperation::port_write_word) { display_ports += count; }
        });
        require(display_dispatches == 42 && display_iterations == 1 && display_ports == 4,
                "Composite work must include the cross-row leading edge and the single unaligned byte span");
        require(renderer.surface().read(0, 110 * line_stride) == 7 &&
                renderer.surface().read(1, 109 * line_stride + 81) == 7,
                "Scene spans must preserve the original cross-row partial-plane edge");
        require(renderer.surface().read(2, 110 * line_stride) == 3,
                "Opaque cockpit pixels must mask the scene copy");
        const auto image = renderer.present(player);
        require(image.palette == assets.palette && image.pixels.size() == 320 * 240 &&
                renderer.state().back_page == page_displacement,
                "Presentation must retain the palette and flip the planar drawing page");
        renderer.copy_front_page();
        require(renderer.surface().read(1, page_displacement + 109 * line_stride + 81) == 7,
                "View initialization must copy the presented front page into the new back page");
        renderer.begin_frame(frame, 6, {});
        require(renderer.surface().read(0, page_displacement + 200 * line_stride) == 9,
                "Crash view initialization must replace the cockpit with its own picture");
        CockpitEntry retained;
        retained.planes = renderer.surface().bytes();
        retained.nearby = 1;
        CockpitRenderer switching(assets, tables, player, retained);
        switching.begin_frame(frame, 6, {});
        require(switching.state().nearby == 0,
                "View initialization must clear the radar proximity latch before automatic defenses run");
        const auto before = renderer.surface().bytes();
        require(renderer.draw_instruments(frame, {}, player, {}, {}, 1, {}).empty() &&
                renderer.surface().bytes() == before, "Crash view must bypass the cockpit instrument program");
        tables.flight_hud = false;
        CockpitRenderer hidden_hud(assets, tables, player);
        const CockpitControls disabled{InstrumentDisplay::disabled, InstrumentDisplay::disabled};
        hidden_hud.begin_frame(frame, 1, disabled);
        player.view.height = 99;
        hidden_hud.draw_instruments(frame, {}, player, {}, {}, 1, disabled);
        require(hidden_hud.state().previous_height == 99,
                "The hidden flight HUD must still retain the previous-height byte");
        player.range = 65535;
        bool rejected = false;
        try {
            hidden_hud.draw_instruments(frame, {}, player, {}, {}, 1, disabled);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "A wrapped zero-height range gauge must reject at the original fatal boundary");

        player.range = 100;
        player.target.slot = 1;
        player.target.projection.x = 20;
        player.target.projection.y = 20;
        const CockpitControls cameras{InstrumentDisplay::camera, InstrumentDisplay::camera};
        CockpitRenderer panels(assets, tables, player);
        panels.begin_frame(frame, 1, cameras);
        auto monitor = std::make_unique<InstrumentCamera>();
        unsigned camera_calls = 0;
        std::uint64_t panel_work = 0, last_camera_work = 0;
        const InstrumentCameraProvider provider = [&](InstrumentPanel panel) {
            require(panel == (camera_calls == 0 ? InstrumentPanel::left : InstrumentPanel::right),
                    "Camera providers must follow left/right panel order");
            require(panel_work > last_camera_work, "Panel work must precede each camera invocation");
            last_camera_work = panel_work;
            monitor->frame.pixels.fill(static_cast<std::uint8_t>(7 + camera_calls * 2));
            ++camera_calls;
            return monitor.get();
        };
        panels.draw_instruments(frame, {}, player, {}, {}, 1, cameras, nullptr,
            [&](CpuOperation, unsigned count) { panel_work += count; }, provider);
        require(camera_calls == 2 && panels.surface().read(0, 0x37ce) == 7 &&
                panels.surface().read(0, 0x37ce + 26) == 9,
                "Each camera panel must consume its own completed view");
        std::uint64_t marker_work = 0, marker_bytes = 0;
        panels.prepare_frame([&](CpuOperation operation, unsigned count) {
            if (operation == CpuOperation::repeat_copy_bytes) { ++marker_work; marker_bytes += count; }
            else { marker_work += count; }
        });
        require(marker_work == 63 && marker_bytes == 4,
                "Saved target background must restore through the original sprite caller before rendering");
        player.system_condition[6] = 128;
        camera_calls = 1;
        panels.draw_instruments(frame, {}, player, {}, {}, 1, cameras, nullptr,
            [&](CpuOperation, unsigned count) { panel_work += count; }, provider);
        require(camera_calls == 2, "A damaged left monitor must invoke only the right provider");
        player.system_condition[7] = 128;
        panels.draw_instruments(frame, {}, player, {}, {}, 1, cameras, nullptr, {}, provider);
        require(camera_calls == 2, "Damaged monitors must bypass camera execution");

        {
            auto marker_tables = tables;
            marker_tables.center_marker = marker_tables.heading_tape = marker_tables.flight_hud = false;
            CockpitEntry marker_entry;
            marker_entry.mode = 5;
            marker_entry.planes.resize(plane_count * plane_bytes);
            FlightState marker_player;
            marker_player.target.slot = 1;
            marker_player.target.projection.x = 80;
            marker_player.target.projection.y = 90;
            CockpitRenderer marker_renderer(assets, marker_tables, marker_player, marker_entry);
            std::vector<std::uint64_t> ports;
            std::uint64_t dispatches = 0;
            marker_renderer.draw_instruments(frame, {}, marker_player, {}, {}, 1, {}, nullptr,
                [&](CpuOperation operation, unsigned count) {
                    if (operation == CpuOperation::port_write_word) { ports.push_back(dispatches); }
                    dispatches += operation == CpuOperation::repeat_copy_bytes ? 1 : count;
                });
            require(ports.size() == 8 && ports[1] - ports[0] == 6 && ports[2] - ports[1] == 6 &&
                    ports[3] - ports[2] == 7 && ports[5] - ports[4] == 10 && ports[6] - ports[5] == 10 &&
                    ports[7] - ports[6] == 11,
                    "Marker save and overlay must push the fourth plane's pointer before its word OUT");
        }

        CockpitEntry flashing_entry;
        flashing_entry.flash_frame = 1;
        auto flashing_player = player;
        flashing_player.weapon = static_cast<unsigned>(WeaponSelection::wingman);
        CockpitRenderer flashing(assets, tables, flashing_player, flashing_entry);
        const CockpitControls blank_panels{InstrumentDisplay::disabled, InstrumentDisplay::disabled};
        for (const auto expected : {2, 3, 0, 1}) {
            flashing.draw_instruments(frame, {}, flashing_player, {}, {}, 0, blank_panels);
            require(flashing.state().flash_frame == expected,
                    "Selected wingman weapon must repeat its cockpit flash cycle");
        }
        flashing_player.weapon = static_cast<unsigned>(WeaponSelection::cannon);
        flashing.refresh_stores(flashing_player);
        for (const auto expected : {2, 3, 0, 0}) {
            flashing.draw_instruments(frame, {}, flashing_player, {}, {}, 0, blank_panels);
            require(flashing.state().flash_frame == expected,
                    "Ordinary weapons must finish the active flash cycle and hold frame zero");
        }
        CockpitEntry entry;
        entry.planes.resize(plane_count * plane_bytes);
        for (unsigned plane = 0; plane < plane_count; ++plane) {
            for (unsigned offset = 0; offset < plane_bytes; ++offset) {
                entry.planes[plane * plane_bytes + offset] = static_cast<std::uint8_t>(offset * 4 + plane);
            }
        }
        CockpitRenderer scanout(assets, tables, player, entry);
        player.shake_offset = 3;
        player.crash_rotation = line_stride;
        const auto shifted = scanout.present(player);
        require(shifted.pixels[0] == entry.planes[3 * plane_bytes + 1 + 245 + line_stride],
                "Scanout must combine page origin, shake address, pel pan and crash scroll");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
