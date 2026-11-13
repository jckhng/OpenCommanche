#include "../src/color_metric.hpp"
#include "../src/loading_work.hpp"
#include "comanchecd/assets.hpp"
#include "loading_clock.hpp"
#include "../src/flight_work.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        if (argc == 4 && std::string(argv[1]) == "--terrain") {
            std::ifstream input(argv[2], std::ios::binary);
            input.exceptions(std::ios::failbit | std::ios::badbit);
            std::ofstream output(argv[3]);
            const auto integer = [&] {
                std::array<unsigned char, 4> bytes{};
                input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                return std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 |
                    std::uint32_t(bytes[2]) << 16 | std::uint32_t(bytes[3]) << 24;
            };
            const auto count = integer();
            if (count > 10000) { throw std::invalid_argument("Too many terrain work cases"); }
            for (unsigned index = 0; index < count; ++index) {
                const auto levels = integer();
                if (!levels || levels > terrain_distance_capacity) { throw std::invalid_argument("Invalid terrain work levels"); }
                TerrainFrame frame;
                frame.depth.resize(levels * scene_buffer_side);
                input.read(reinterpret_cast<char*>(frame.depth.data()), frame.depth.size());
                CpuAccounting cost;
                flight_work::terrain(frame, levels, [&](CpuOperation operation, unsigned count) {
                    if (operation == CpuOperation::repeat_store_dwords || operation == CpuOperation::repeat_copy_dwords) {
                        ++cost.dispatches;
                        cost.string_iterations += count;
                    } else {
                        cost.dispatches += count;
                    }
                });
                output << cost.dispatches << ' ' << cost.string_iterations << '\n';
            }
            if (!output) { throw std::runtime_error("Cannot write terrain work results"); }
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--picture") {
            const Installation installation(argv[2], argv[3]);
            LoadingClock clock;
            const auto picture = installation.load_picture_payload("CONSOL1S.DTA", clock);
            const auto cost = clock.accounting();
            std::ofstream output(argv[4]);
            const auto dos_dispatches = 3 * 49 + 13 * cost.idle_calls;
            output << picture.bytes.size() << ' ' << cost.dispatches - cost.repeat_restarts - dos_dispatches - 3 * 19 - 1
                   << ' ' << cost.string_iterations + cost.special_stosb_iterations << ' ' << cost.idle_calls
                   << ' ' << cost.idle_cycles << ' ' << cost.stack_segment_credits << ' ' << clock.cycles() << '\n';
            if (!output) { throw std::runtime_error("Cannot write picture loading work"); }
            return 0;
        }
        if (argc == 7 && std::string(argv[1]) == "--mission") {
            const Installation installation(argv[2], argv[3]);
            const auto frontend = installation.load_frontend();
            const auto campaign = std::stoul(argv[4]);
            if (campaign >= frontend.campaign_count) {
                throw std::invalid_argument("Invalid loading-work campaign");
            }
            const auto missions = installation.load_missions(static_cast<std::uint8_t>(campaign));
            const auto& definition = missions.missions.at(std::stoul(argv[5]));
            auto resources = frontend.mission_resources;
            for (std::size_t index = 11; index < resources.size(); ++index) {
                resources[index][0] = '-';
            }
            std::copy(definition.resources.begin(), definition.resources.end(), resources.begin());
            const auto setup = parse_gameplay(definition, resources, frontend.gameplay_presets, {});
            const auto payloads = load_gameplay(setup, resources, [&](const auto& name, auto kind) {
                return installation.load_payload(name, kind);
            });
            const auto scene = materialize_scene(setup, payloads, frontend.transfer);
            std::ofstream output(argv[6]);
            for (unsigned stage = 0; stage < 2; ++stage) {
                CpuAccounting cost;
                const CpuWorkSink work = [&](CpuOperation operation, unsigned count) {
                    if (operation == CpuOperation::repeat_store_dwords) {
                        ++cost.dispatches;
                        cost.string_iterations += count;
                    } else {
                        cost.dispatches += count;
                        if (operation == CpuOperation::string_bytes) { cost.string_iterations += count; }
                    }
                };
                if (stage == 0) {
                    loading_work::cockpit_remap(payloads.back(), scene.terrain.palette, frontend.transfer, work);
                } else {
                    loading_work::scenery_palette(setup, scene.terrain.palette, frontend.transfer, work);
                }
                output << stage << ' ' << cost.dispatches << ' ' << cost.string_iterations << '\n';
            }
            if (!output) { throw std::runtime_error("Cannot write mission loading work"); }
            return 0;
        }
        if (argc != 3) {
            throw std::invalid_argument("Usage: loading_work_probe INPUT OUTPUT | --mission INSTALLATION PAYLOAD_DIRECTORY CAMPAIGN MISSION OUTPUT");
        }
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        const auto byte = [&]() {
            unsigned value;
            if (!(input >> value) || value > 255) {
                throw std::invalid_argument("Invalid loading-work byte");
            }
            return static_cast<std::uint8_t>(value);
        };
        Transfer transfer;
        for (auto& value : transfer) { value = byte(); }
        unsigned cases;
        if (!(input >> cases) || cases > 10000) {
            throw std::invalid_argument("Invalid loading-work case count");
        }
        for (unsigned index = 0; index < cases; ++index) {
            Palette palette;
            for (auto& color : palette) {
                for (auto& value : color) { value = byte(); }
            }
            const std::array<std::uint8_t, 3> target{byte(), byte(), byte()};
            unsigned first, last;
            if (!(input >> first >> last) || first > last || last >= palette.size()) {
                throw std::invalid_argument("Invalid palette search bounds");
            }
            CpuAccounting work;
            const auto selected = color_metric::nearest(palette, transfer, target, first, last,
                [&](CpuOperation operation, unsigned count) {
                    work.dispatches += count;
                    if (operation == CpuOperation::string_bytes) {
                        work.string_iterations += count;
                    } else if (operation != CpuOperation::instructions) {
                        throw std::runtime_error("Unexpected palette CPU operation");
                    }
                });
            output << unsigned(selected) << ' ' << work.dispatches << ' ' << work.string_iterations << '\n';
        }
        input >> std::ws;
        if (!input.eof() || !output) {
            throw std::runtime_error("Truncated or trailing loading-work input/output");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
