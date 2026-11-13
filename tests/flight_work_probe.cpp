#include "../src/flight_work.hpp"
#include "comanchecd/world_renderer.hpp"
#include "comanchecd/flight.hpp"
#include "loading_clock.hpp"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        if (argc != 3) { throw std::invalid_argument("Usage: flight_work_probe INPUT OUTPUT"); }
        std::ifstream input(argv[1], std::ios::binary);
        input.exceptions(std::ios::failbit | std::ios::badbit);
        std::ofstream output(argv[2]);
        const auto integer = [&] {
            std::array<unsigned char, 4> b{};
            input.read(reinterpret_cast<char*>(b.data()), b.size());
            return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
        };
        const auto cases = integer();
        if (cases > 10000) { throw std::invalid_argument("Too many foreground cases"); }
        for (unsigned index = 0; index < cases; ++index) {
            const auto kind = integer();
            LoadingClock clock;
            const CpuWorkSink work = [&](CpuOperation operation, unsigned count) { clock.execute(operation, count); };
            if (kind == 0 || kind == 5) {
                SceneView view;
                view.pitch = static_cast<std::int32_t>(integer());
                view.roll = static_cast<std::int32_t>(integer());
                flight_work::CameraInputs camera;
                if (kind == 5) {
                    const auto mode = integer();
                    view.mode = mode == 6 ? SceneViewMode::chase : mode == 7 ? SceneViewMode::fixed : SceneViewMode::cockpit;
                    camera.exiting = integer() != 0;
                    camera.ground_height = static_cast<std::uint8_t>(integer());
                    camera.eye_height = static_cast<std::uint8_t>(integer());
                    camera.subject_dx = integer();
                    camera.subject_dy = integer();
                }
                flight_work::camera(view, clock, [] { return 0; }, camera);
            } else if (kind == 1) {
                const auto levels = integer();
                if (levels < 8 || levels > terrain_distance_capacity) { throw std::invalid_argument("Invalid fog levels"); }
                TerrainFrame frame;
                frame.depth.resize(levels * scene_buffer_side);
                input.read(reinterpret_cast<char*>(frame.depth.data()), frame.depth.size());
                input.read(reinterpret_cast<char*>(frame.camera.horizon.data()), frame.camera.horizon.size());
                flight_work::fog(frame, levels, work);
            } else if (kind == 2) {
                flight_work::model_projection(integer(), work);
            } else if (kind == 3) {
                std::array<std::int32_t, scene_slots> depth{};
                for (auto& value : depth) { value = static_cast<std::int32_t>(integer()); }
                flight_work::model_sort(depth, work);
            } else if (kind == 4) {
                FlightState state;
                state.control_power = static_cast<std::uint8_t>(integer());
                state.system_condition[0] = static_cast<std::uint8_t>(integer());
                state.system_condition[1] = static_cast<std::uint8_t>(integer());
                state.armor = static_cast<std::int32_t>(integer());
                state.forward_speed = static_cast<std::int32_t>(integer());
                state.side_speed = static_cast<std::int32_t>(integer());
                state.forward_limit = static_cast<std::int32_t>(integer());
                state.yaw_rate = static_cast<std::int32_t>(integer());
                state.view.pitch = static_cast<std::int32_t>(integer());
                state.view.roll = static_cast<std::int32_t>(integer());
                FlightPolicy policy;
                policy.rotor = integer() != 0 ? RotorControl::mixed : RotorControl::independent;
                const auto horizontal = static_cast<std::int32_t>(integer());
                const auto vertical = static_cast<std::int32_t>(integer());
                flight_work::dynamics(state, policy, horizontal, vertical, work);
            } else if (kind == 7) {
                FlightState state;
                state.view.height = static_cast<std::uint8_t>(integer());
                state.control_power = static_cast<std::uint8_t>(integer());
                state.forward_speed = static_cast<std::int32_t>(integer());
                state.view.pitch = static_cast<std::int32_t>(integer());
                state.view.roll = static_cast<std::int32_t>(integer());
                state.height_gap = static_cast<std::uint8_t>(integer());
                FlightPolicy policy;
                policy.altitude = integer() != 0 ? AltitudeControl::stabilized : AltitudeControl::manual;
                const auto ground = static_cast<std::uint8_t>(integer());
                const auto color = static_cast<std::uint8_t>(integer());
                const auto previous = static_cast<std::uint8_t>(integer());
                const auto variant = static_cast<std::uint8_t>(integer());
                flight_work::ground_response(state, policy, ground, color, previous, variant, work);
            } else if (kind == 8) {
                FlightState state;
                state.range = integer();
                state.control_power = static_cast<std::uint8_t>(integer());
                state.view.height = static_cast<std::uint8_t>(integer());
                state.armor = static_cast<std::int32_t>(integer());
                state.cockpit_mode = static_cast<std::uint8_t>(integer());
                state.exit_requested = static_cast<std::uint8_t>(integer());
                state.exit_countdown = static_cast<std::uint8_t>(integer());
                state.crash_rotation = integer();
                const auto seed = integer();
                const auto source = integer() == 0 ? ControlSource::keyboard : ControlSource::recording;
                const auto directions = static_cast<std::uint8_t>(integer());
                flight_work::polling(state, source, directions, [seed] { return seed; }, work);
            } else if (kind == 6) {
                DefenseStatus status;
                status.height = static_cast<std::uint8_t>(integer());
                status.active_chaff = static_cast<std::uint8_t>(integer());
                status.active_flares = static_cast<std::uint8_t>(integer());
                status.proximity = integer() != 0 ? ThreatProximity::nearby : ThreatProximity::clear;
                status.tracking = integer() != 0;
                status.incoming_hit = integer() != 0;
                FlightPolicy policy;
                policy.chaff = integer() != 0 ? DefenseMode::automatic : DefenseMode::manual;
                policy.flares = integer() != 0 ? DefenseMode::automatic : DefenseMode::manual;
                flight_work::motion(policy, status, work);
            } else {
                throw std::invalid_argument("Unknown foreground work kind");
            }
            const auto cost = clock.accounting();
            output << cost.dispatches << ' ' << cost.string_iterations << ' ' << cost.io_cycles << '\n';
        }
        if (!output) { throw std::runtime_error("Cannot write foreground costs"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
