#include "flight_work.hpp"
#include "comanchecd/world_renderer.hpp"
#include "comanchecd/flight.hpp"
#include <limits>
#include <stdexcept>

namespace comanchecd::flight_work {
void loop_prefix(std::uint32_t iteration, const FlightState& state, std::array<std::uint8_t, 6>& warned,
                 const std::function<void(std::uint8_t)>& announce, const CpuWorkSink& work,
                 const std::function<void()>& update_pending) {
    constexpr std::uint32_t warning_period_mask = 31;
    constexpr std::array<std::uint8_t, 6> warning_sounds{8, 4, 13, 15, 13, 14};
    const auto instructions = [&](unsigned count) { if (work) { work(CpuOperation::instructions, count); } };
    // 721a..72eb for a normal keyboard iteration. Host actions own pause/settings entry.
    instructions(9);
    if ((iteration & warning_period_mask) == 0) {
        instructions(1);
        for (std::size_t reverse = warned.size(); reverse != 0; --reverse) {
            const auto slot = reverse - 1;
            instructions(2);
            if (warned[slot] == 0) {
                instructions(2);
                if ((state.system_condition[slot] & 128) != 0) {
                    instructions(4);
                    warned[slot] = 1;
                    announce(warning_sounds[slot]);
                    instructions(1);
                }
            }
            instructions(1);
        }
    }
    instructions(1); // 72a8 iteration counter.
    if (update_pending) {
        update_pending();
    } else {
        instructions(6);
    }
}

void polling(const FlightState& state, ControlSource source, std::uint8_t directions,
             const std::function<std::uint32_t()>& random, const CpuWorkSink& work) {
    if (!work) { return; }
    const auto instructions = [&](unsigned n) { work(CpuOperation::instructions, n); };
    // Keyboard profile: 19870..199aa. Recorded input joins at 1995a.
    instructions(source == ControlSource::keyboard ? 17 : 4);
    if (static_cast<std::int32_t>(state.range) < 0) {
        instructions(2);
        if (state.range == static_cast<std::uint32_t>(-270)) { return; }
        instructions(4);
        if (state.control_power != 0) {
            instructions(2);
            if (state.control_power == 1) { instructions(1); }
        }
    }
    instructions(3);
    for (unsigned key = 0; key < 8; ++key) {
        instructions(2);
        if ((directions & (1u << key)) != 0) { instructions(key < 4 ? 2 : 1); }
    }
    instructions(3);
    if (state.exit_requested == 0) { return; }
    instructions(2);
    if (state.exit_countdown == state.exit_requested) { return; }
    instructions(4);
    if (state.armor >= 0) {
        instructions(3);
        if (state.view.height <= 250) { instructions(1); }
        instructions(2);
        const auto raised = unsigned(state.view.height) + (state.view.height <= 250 ? 4 : 0);
        if (raised >= 230) { instructions(2); }
        return;
    }
    instructions(4);
    if (state.cockpit_mode == 1) {
        constexpr unsigned crash_rotation_step = 82, crash_rotation_limit = 3690;
        instructions(3);
        if (state.crash_rotation + crash_rotation_step >= crash_rotation_limit) { instructions(3); }
    }
    instructions(2);
    if ((random() & 15) == 0) { instructions(2); }
    // Transition, announcement, explosion and fuel callback bodies are separate scopes.
}

void ground_response(const FlightState& state, const FlightPolicy& policy, std::uint8_t ground,
                     std::uint8_t color, std::uint8_t previous_height, std::uint8_t terrain_variant,
                     const CpuWorkSink& work) {
    if (!work) { return; }
    const auto instructions = [&](unsigned count) { work(CpuOperation::instructions, count); };
    instructions(12);
    if (state.view.height <= ground) {
        instructions(2);
        if (ground - state.view.height <= 2) { instructions(5); }
        return;
    }
    const auto gap = unsigned(state.view.height - ground);
    instructions(6);
    auto height = unsigned(state.view.height);
    if (gap < 7) {
        instructions(2);
        if (state.control_power <= 6) {
            instructions(7);
            if (terrain_variant == 0) { return; }
            instructions(2);
            if (color < 251) { return; }
            instructions(2);
            if (previous_height == 0) { return; }
            instructions(5);
            if (previous_height > 4) { instructions(1); }
            return;
        }
        instructions(2);
        if (height != 255) { ++height; instructions(1); }
    }
    instructions(4);
    if (policy.altitude == AltitudeControl::manual) {
        auto power = std::max(int(state.control_power) - 5, 0);
        instructions(2);
        if (state.control_power < 5) { instructions(1); }
        const auto speed = static_cast<std::int8_t>(state.forward_speed >> 21);
        const auto magnitude = static_cast<std::uint8_t>(speed < 0 ? ~speed : speed);
        instructions(7);
        if (power + magnitude > 255) { instructions(1); }
        power = std::min(power + magnitude, 255);
        const auto bank = static_cast<std::int8_t>(-static_cast<std::uint8_t>((state.view.roll * 3) >> 3));
        auto penalty = bank < 0 ? -int(bank) : int(bank);
        instructions(7);
        if (bank < 0) {
            instructions(3);
            if (speed < 0) { instructions(2); penalty -= magnitude; }
        }
        if (penalty >= 0) {
            instructions(2);
            if (power < penalty) { instructions(1); }
            power = std::max(power - penalty, 0);
        }
        const auto tilt = static_cast<std::uint8_t>(std::uint32_t(state.view.pitch < 0 ? ~state.view.pitch : state.view.pitch) >> 4);
        instructions(6);
        if (power < tilt) { instructions(1); }
        power = std::max(power - tilt, 0);
        instructions(2);
        if (power + 5 > 255) { instructions(1); }
        instructions(2);
    }
    const auto altitude = altitude_response(policy.altitude, state.control_power, state.forward_speed,
                                            state.view.pitch, state.view.roll);
    const auto average = (gap + state.height_gap + 1) >> 1;
    instructions(5);
    if (average == altitude.power) { return; }
    instructions(1);
    if (average < altitude.power) {
        instructions(5);
        if (height + ((altitude.power - average) >> 3) + 2 > 255) { instructions(2); }
    } else {
        instructions(4);
        if (height < ((average - altitude.power) >> altitude.descent_shift) + 3) { instructions(2); }
    }
}

void motion(const FlightPolicy& policy, const DefenseStatus& status, const CpuWorkSink& work) {
    if (!work) { return; }
    // 1a11d..1a192: height average, heading and four signed high products.
    work(CpuOperation::instructions, 25);
    auto gate = 2u;
    auto flare = status.proximity == ThreatProximity::nearby;
    auto chaff = false;
    if (!flare) {
        gate += 2;
        chaff = status.tracking;
        if (!chaff) { gate += 2; flare = status.incoming_hit; }
    }
    work(CpuOperation::instructions, gate);
    if (!flare && !chaff) { work(CpuOperation::instructions, 1); return; }
    work(CpuOperation::instructions, 2);
    const auto minimum_height = chaff ? 20 : 10;
    if (status.height <= minimum_height) { work(CpuOperation::instructions, 1); return; }
    work(CpuOperation::instructions, 2);
    const auto automatic = (chaff ? policy.chaff : policy.flares) == DefenseMode::automatic;
    if (!automatic) { work(CpuOperation::instructions, 1); return; }
    work(CpuOperation::instructions, 2);
    if ((chaff ? status.active_chaff : status.active_flares) != 0) { work(CpuOperation::instructions, 1); }
    // A successful conditional tail jump belongs to the deployment routine.
}

void dynamics(const FlightState& state, const FlightPolicy& policy, std::int32_t horizontal,
              std::int32_t vertical, const CpuWorkSink& work) {
    if (!work) { return; }
    // 19af5..19d12: keyboard profile, engine rate, damping and control clamps.
    const auto damaged_engine = (state.system_condition[1] & 128) != 0;
    const auto damaged_rotor = (state.system_condition[0] & 128) != 0;
    const auto mixed = policy.rotor == RotorControl::mixed;
    const auto pitch_input = vertical >> 5;
    const auto power = unsigned(pitch_input ^ (pitch_input >> 31)) + unsigned(state.control_power) * 3;
    const auto engine = std::min(power, 200u) >> unsigned(damaged_engine);
    unsigned count = 2 + unsigned(damaged_engine) + 9 + unsigned(power >= 200) + 2 + unsigned(damaged_engine);
    count += 5 + unsigned(engine > 128) + 5 + unsigned(state.control_power <= 16);
    const auto damp = [](std::int32_t value, unsigned shift) { return value - ((value >> shift) | std::int32_t(value > 0)); };
    const auto shift = state.control_power <= 16 ? 3u : 5u;
    auto forward = damp(state.forward_speed, shift);
    auto side = damp(state.side_speed, shift);
    auto yaw = damp(state.yaw_rate, shift - 1);
    auto pitch = damp(state.view.pitch, shift - 1);
    auto roll = damp(state.view.roll, shift - 2);
    count += 32 + 2 + 2;
    if (damaged_rotor) {
        count += 2 + unsigned(state.armor >= 0);
        if (state.armor >= 0) { yaw += 0x00600000; }
    }
    count += 3;
    if (mixed) { count += 4; yaw += horizontal * 0x4000; }
    constexpr std::int32_t yaw_limit = 0x04000000, pitch_limit = 133, roll_limit = 33;
    count += 4 + unsigned(yaw > yaw_limit) + unsigned(yaw < -yaw_limit);
    pitch += (horizontal + 8) >> 4;
    count += 9 + unsigned(pitch <= -pitch_limit) + unsigned(pitch >= pitch_limit);
    side += horizontal * 0x800;
    const auto side_limit = mixed ? 0x00800000 : 0x01000000;
    count += 8 + unsigned(side > side_limit) + unsigned(side < -side_limit);
    roll += (16 - vertical) >> 5;
    count += 13 + unsigned(roll >= roll_limit) + unsigned(std::min(roll, roll_limit) <= -roll_limit);
    forward -= vertical * 0x800;
    const auto speed_limit = damaged_engine ? 0x00800000 : state.forward_limit;
    count += 10 + unsigned(forward > speed_limit) + unsigned(forward < -(speed_limit >> 1));
    // IRQ0 observes neither these arithmetic temporaries nor their stores.
    work(CpuOperation::instructions, count);
}

void camera(SceneView& view, ExecutionClock& clock, const std::function<std::int32_t()>& shake,
            const CameraInputs& inputs) {
    const auto instructions = [&](unsigned count) { clock.execute(CpuOperation::instructions, count); };
    constexpr unsigned camera_prefix = 31, cockpit_callback = 1, clamp_prefix = 14;
    constexpr unsigned horizon_body = 4039;
    auto callback = cockpit_callback;
    if (view.mode == SceneViewMode::chase) {
        const auto raised = unsigned(inputs.ground_height) + 12;
        callback = 35 + unsigned(inputs.exiting) + unsigned(raised > 255) +
            unsigned(std::min(raised, 255u) >= inputs.eye_height);
    } else if (view.mode == SceneViewMode::fixed) {
        const auto magnitude = [](std::uint32_t value) { return value ^ std::uint32_t(static_cast<std::int32_t>(value) >> 31); };
        const auto x = magnitude(inputs.subject_dx), y = magnitude(inputs.subject_dy);
        callback = 51 + 2 * unsigned(x <= y) + 4 * unsigned((std::max(x, y) >> 10) != 0);
    }
    instructions(camera_prefix + callback + clamp_prefix);
    const auto pitch = view.pitch < 0 ? -std::int64_t(view.pitch) : view.pitch;
    const auto limit = 33 - static_cast<std::int32_t>((pitch * 64) >> 8);
    auto roll = view.roll;
    if (roll >= limit) { instructions(1); roll = limit; }
    instructions(3);
    if (roll <= -limit) { instructions(1); }
    instructions(2);
    // SUB at 86fd samples shake after any IRQ delivered by preceding work.
    clock.instruction([&] { view.vertical_pan = shake(); });
    instructions(horizon_body);
}

void fog(const TerrainFrame& frame, unsigned levels, const CpuWorkSink& work) {
    if (!work) { return; }
    if (levels < 8 || frame.depth.size() != std::size_t(levels) * scene_buffer_side) {
        throw std::invalid_argument("Fog CPU work requires complete distance rows");
    }
    constexpr unsigned depth_origin = 32;
    const std::array<unsigned, 2> boundaries{levels - (levels >> 3), levels - (levels >> 2)};
    work(CpuOperation::instructions, 19);
    for (unsigned column = 0; column < scene_columns; ++column) {
        work(CpuOperation::instructions, column % 2 == 0 ? 7 : 8);
        const auto horizon = frame.camera.horizon[column];
        auto y = static_cast<std::uint8_t>(frame.depth[(levels - 1) * scene_buffer_side + depth_origin + column] + horizon);
        for (const auto level : boundaries) {
            const auto end = static_cast<std::uint8_t>(frame.depth[level * scene_buffer_side + depth_origin + column] + horizon);
            work(CpuOperation::instructions, 2);
            if (y > end) {
                work(CpuOperation::instructions, 1 + 6 * (y - end));
                y = end;
            }
        }
        if (column % 2 != 0) { work(CpuOperation::instructions, 5); }
    }
    work(CpuOperation::instructions, 1);
}

void model_projection(unsigned active, const CpuWorkSink& work) {
    if (work) { work(CpuOperation::instructions, 1 + 5 * (scene_slots - 1) + 20 * active); }
}

void model_sort(const std::array<std::int32_t, scene_slots>& depth, const CpuWorkSink& work) {
    if (!work) { return; }
    std::array<unsigned, scene_slots> order{};
    for (unsigned slot = 1; slot < scene_slots; ++slot) { order[slot] = slot; }
    work(CpuOperation::instructions, 1);
    for (unsigned end = scene_slots - 1; end > 1; --end) {
        work(CpuOperation::instructions, 6);
        auto selected = end;
        for (unsigned candidate = end - 1; candidate > 0; --candidate) {
            work(CpuOperation::instructions, 4);
            if (depth[order[selected]] <= depth[order[candidate]]) {
                selected = candidate;
                work(CpuOperation::instructions, 2);
            }
            work(CpuOperation::instructions, 1);
        }
        std::swap(order[end], order[selected]);
        work(CpuOperation::instructions, 5);
    }
    work(CpuOperation::instructions, 4);
}

void sky(std::uint32_t heading, const CpuWorkSink& work) {
    if (!work) { return; }
    constexpr unsigned sky_columns = 512, sky_height = 150;
    auto source_column = ((~heading - 0x30000000u) >> 23);
    work(CpuOperation::instructions, 18);
    for (unsigned column = 0; column < scene_columns; ++column) {
        work(CpuOperation::instructions, 2);
        for (unsigned pixel = 0; pixel < sky_height; ++pixel) {
            work(CpuOperation::instructions, 1);
            work(CpuOperation::string_bytes, 1);
            work(CpuOperation::instructions, 1);
        }
        work(CpuOperation::instructions, 2);
        if (++source_column == sky_columns) {
            source_column = 0;
            work(CpuOperation::instructions, 2);
        }
        work(CpuOperation::instructions, 5);
    }
    work(CpuOperation::instructions, 1);
}

void terrain(const TerrainFrame& frame, unsigned levels, const CpuWorkSink& work) {
    if (!work) { return; }
    if (!levels || frame.depth.size() != std::size_t(levels) * scene_buffer_side) {
        throw std::invalid_argument("Terrain CPU work requires complete depth rows");
    }
    constexpr unsigned depth_origin = 32, depth_dwords = scene_columns / 4;
    constexpr unsigned half_columns = scene_columns / 2;
    std::array<std::uint8_t, scene_columns> previous{};
    work(CpuOperation::instructions, 4);
    work(CpuOperation::repeat_store_dwords, depth_dwords);
    work(CpuOperation::instructions, 9);
    for (unsigned level = 0; level < levels; ++level) {
        work(CpuOperation::instructions, 21);
        for (unsigned index = 0; index < scene_columns; ++index) {
            const auto column = index < half_columns ? half_columns - 1 - index : index;
            if (index == half_columns) { work(CpuOperation::instructions, 4); }
            if (index >= half_columns) { work(CpuOperation::instructions, 2); }
            work(CpuOperation::instructions, 8);
            const auto height = frame.depth[level * scene_buffer_side + depth_origin + column];
            if (height < previous[column]) { throw std::invalid_argument("Decreasing terrain occlusion"); }
            if (height > previous[column]) {
                work(CpuOperation::instructions, 11);
                // The unrolled span replaces its final INC CH with RET.
                work(CpuOperation::instructions, 2 * (height - previous[column]));
                previous[column] = height;
            }
            if (index < half_columns) { work(CpuOperation::instructions, 2); }
        }
        work(CpuOperation::instructions, 9);
        work(CpuOperation::repeat_copy_dwords, depth_dwords);
        work(CpuOperation::instructions, 6);
    }
    work(CpuOperation::instructions, 2);
}

void monitor(const TerrainFrame& frame, unsigned levels, std::uint32_t heading, CloudMode clouds,
             const CpuWorkSink& work) {
    if (!work) { return; }
    if (!levels || frame.depth.size() != std::size_t(levels) * scene_buffer_side) {
        throw std::invalid_argument("Monitor CPU work requires complete depth rows");
    }
    constexpr unsigned columns = 40, half_columns = columns / 2, depth_origin = 88;
    constexpr unsigned distance_step = 3, sky_rows = 60, copy_rows = 120, sky_columns = 512;
    const auto instructions = [&](unsigned count) { work(CpuOperation::instructions, count); };
    instructions(10);
    work(CpuOperation::repeat_store_dwords, half_columns);
    instructions(2);
    if (clouds == CloudMode::flat) {
        instructions(12);
        for (unsigned row = 0; row < scene_buffer_side; ++row) {
            work(CpuOperation::repeat_store_dwords, columns / 4);
            instructions(row + 1 == scene_buffer_side ? 1 : 2);
        }
        instructions(1);
    } else {
        auto column = (~heading - 0x14000000u) >> 23;
        instructions(18);
        for (unsigned index = 0; index < columns; ++index) {
            instructions(3);
            for (unsigned row = 0; row < sky_rows; ++row) {
                instructions(1);
                work(CpuOperation::string_bytes, 1);
                instructions(1);
            }
            instructions(2 + (column + 1 == sky_columns ? 2 : 0) + 5 + (column + 2 == sky_columns ? 1 : 0) + 2);
            column = (column + 2) % sky_columns;
        }
        instructions(1);
    }
    instructions(6);
    work(CpuOperation::repeat_store_dwords, half_columns);
    instructions(9);
    std::array<std::uint8_t, columns> previous{};
    for (unsigned level = 0; level < levels; level += distance_step) {
        instructions(25);
        for (unsigned index = 0; index < columns; ++index) {
            const auto column = index < half_columns ? half_columns - 1 - index : index;
            if (index == half_columns) { instructions(4); }
            if (index >= half_columns) { instructions(2); }
            instructions(8);
            const auto height = frame.depth[level * scene_buffer_side + depth_origin + column * 2];
            if (height < previous[column]) { throw std::invalid_argument("Decreasing monitor occlusion"); }
            if (height > previous[column]) {
                instructions(11 + 2 * (height - previous[column]));
                previous[column] = height;
            }
            if (index < half_columns) { instructions(2); }
        }
        instructions(11);
        work(CpuOperation::repeat_copy_dwords, half_columns);
        instructions(4);
        work(CpuOperation::repeat_copy_dwords, half_columns);
        instructions(4);
        work(CpuOperation::repeat_copy_dwords, half_columns);
        instructions(6);
    }
    instructions(6);
    for (unsigned row = 0; row < copy_rows; ++row) {
        if (row != 0) { instructions(3); }
        work(CpuOperation::repeat_copy_dwords, columns / 4);
    }
    instructions(3);
}

void present(ExecutionClock& clock) {
    const auto instructions = [&](unsigned count) { clock.execute(CpuOperation::instructions, count); };
    const auto poll = [&](bool wait_while_retrace) {
        auto remaining = std::numeric_limits<std::uint32_t>::max();
        bool retrace;
        do {
            retrace = clock.read_retrace();
            instructions(2);
        } while (--remaining != 0 && retrace == wait_while_retrace);
    };
    // 16648..166e3, including RET; its caller owns the CALL and page mutation.
    instructions(2);
    poll(true);
    clock.execute(CpuOperation::disable_interrupts, 1);
    clock.sample_irq_ticks();
    instructions(2);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(12);
    clock.execute(CpuOperation::port_write_word, 1);
    instructions(4);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(2);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(3);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(2);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(3);
    poll(false);
    instructions(7);
}
}
