#include "comanchecd/effects.hpp"
#include "comanchecd/execution_clock.hpp"
#include "color_metric.hpp"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::array<std::size_t, 3> first_particle{0, 96, 224};
constexpr std::array<std::size_t, 3> particle_count{96, 128, 64};
constexpr std::array<std::array<unsigned, 8>, 3> depth_divisor{{
    {3, 3, 3, 3, 3, 2, 2, 2}, {8, 7, 6, 5, 4, 3, 3, 3}, {14, 12, 10, 9}}};
constexpr std::array<std::array<unsigned, 8>, 3> fall_speed{{
    {5, 5, 5, 5, 5, 4, 4, 3}, {12, 11, 9, 7, 6, 5, 5, 5}, {19, 18, 15, 12}}};

unsigned rotate(std::uint32_t& random, ExecutionClock* clock) {
    unsigned carry = 0;
    const auto action = [&] {
        carry = random & 1;
        random = (random >> 1) | (random << 31);
    };
    if (clock) { clock->instruction(action); }
    else { action(); }
    return carry;
}

void advance(std::uint32_t& random, std::uint32_t feedback) {
    const auto shifted = static_cast<std::int32_t>(0u - random) >> 1;
    random = std::uint32_t(shifted) ^ (shifted < 0 ? feedback : 0);
}

std::int32_t high(std::int32_t first, std::int32_t second) {
    return static_cast<std::int32_t>((std::int64_t(first) * second) >> 32);
}
}

EffectsState::EffectsState() {
    for (std::size_t index = 0; index < particles.size(); ++index) {
        particles[index].speed = static_cast<std::uint8_t>(index % 8 + 1);
    }
}

ScriptedEffects::ScriptedEffects(const GameplaySetup& setup, const Palette& palette, const TerrainTables& tables,
                               EffectsState entry)
    : setup_(setup), tables_(tables), state_(std::move(entry)), type_(setup.last_light_type) {
    const auto last = setup.terrain_variant == 0 ? 255u : 250u;
    const std::array<std::uint8_t, 3> solid{setup.light_color[0], setup.light_color[1], setup.light_color[2]};
    solid_color_ = color_metric::nearest(palette, tables.transfer, solid, 1, last);
    for (std::size_t type = 0; type < colors_.size(); ++type) {
        const auto& tint = type == 0 ? setup.light_color : setup.secondary_color;
        for (std::size_t index = 0; index < 256; ++index) {
            colors_[type][index] = static_cast<std::uint8_t>(index);
            if (index >= last) {
                continue;
            }
            auto color = palette[index];
            for (std::size_t channel = 0; channel < color.size(); ++channel) {
                color[channel] = static_cast<std::uint8_t>((color[channel] + tint[channel] + 1) >> 1);
            }
            colors_[type][index] = color_metric::nearest(palette, tables.transfer, color, 1, last);
        }
    }
    for (std::size_t index = 0; index < state_.particles.size(); ++index) {
        auto& particle = state_.particles[index];
        particle.x = particle.y = static_cast<std::uint8_t>(4 - index % 4);
    }
}

const EffectsState& ScriptedEffects::state() const { return state_; }

void ScriptedEffects::draw(TerrainFrame& frame, const SceneView& view, std::uint32_t tick, std::int32_t yaw,
                          std::int32_t side_speed, std::uint32_t& random, EffectLayer layer, ExecutionClock* clock) {
    if (setup_.lights.empty()) {
        return;
    }
    if (layer == EffectLayer::foreground) {
        particles(frame, view, (yaw >> 23) + (side_speed >> 20), random, 2, clock);
        return;
    }
    const auto instructions = [&](unsigned count) { if (clock) { clock->execute(CpuOperation::instructions, count); } };
    instructions(4 + 2); // 278fe resets activity and loads the first interval.
    active_ = false;
    auto comparison_tick = tick;
    for (const auto& interval : setup_.lights) {
        instructions(2);
        if (comparison_tick >= interval.x) {
            instructions(2);
            if (comparison_tick <= interval.y) {
                instructions(3);
                active_ = true;
                type_ = interval.type;
                // The original reuses AL for the selected type before testing subsequent intervals.
                comparison_tick = (comparison_tick & 0xffffff00u) | type_;
            }
        }
        instructions(2);
    }
    instructions(1);
    particles(frame, view, yaw >> 23, random, 0, clock);
    particles(frame, view, yaw >> 23, random, 1, clock);
}

void ScriptedEffects::particles(TerrainFrame& frame, const SceneView& view, std::int32_t motion,
                               std::uint32_t& random, std::size_t group, ExecutionClock* clock) {
    const auto instructions = [&](unsigned count) { if (clock) { clock->execute(CpuOperation::instructions, count); } };
    instructions((group == 2 ? 11 : 8) + unsigned(type_ != 0));
    const auto levels = frame.depth.size() / scene_buffer_side;
    for (std::size_t index = first_particle[group]; index < first_particle[group] + particle_count[group]; ++index) {
        auto& particle = state_.particles[index];
        instructions(6); // Load coordinates, pointer and speed before the three ROR/SBB pairs.
        auto x = static_cast<std::uint8_t>(particle.x - motion - rotate(random, clock));
        instructions(1);
        x = static_cast<std::uint8_t>(x + rotate(random, clock));
        instructions(1);
        const auto descent = particle.speed + rotate(random, clock);
        instructions(2);
        if (particle.y < descent) {
            instructions(2);
            if (active_) {
                auto value = random;
                if (clock) { clock->instruction([&] { value = random; }); }
                instructions(5);
                advance(value, group == 2 ? 0xbc9abb01 : 0xbc9abb09);
                if (clock) { clock->instruction([&] { random = value; }); }
                else { random = value; }
                const auto bucket = (value >> 8) & (group == 2 ? 3 : 7);
                particle.x = static_cast<std::uint8_t>(value);
                particle.y = 255;
                instructions(4);
                auto low = random;
                if (clock) { clock->instruction([&] { low = random; }); }
                instructions(10);
                particle.speed = static_cast<std::uint8_t>(fall_speed[group][bucket] + (low & 1));
                particle.depth_level = static_cast<std::int32_t>(levels / depth_divisor[group][bucket]);
            }
            instructions(3);
            continue;
        }
        x = static_cast<std::uint8_t>(x + state_.drift[group == 2 ? 0 : particle.speed + 1]);
        particle.x = x;
        particle.y = static_cast<std::uint8_t>(particle.y - descent);
        const auto depth = particle.depth_level < 0 ? 0 : frame.depth.at(std::size_t(particle.depth_level) * 256 + x);
        instructions(6);
        if (particle.y < depth) {
            instructions(3);
            continue;
        }
        instructions(5);
        const auto offset = int(x) - 128;
        const auto slope = (frame.camera.pitch * std::abs(offset) + 128) >> 8;
        const auto horizon = frame.camera.roll - view.vertical_pan + (offset < 0 ? -slope : slope);
        auto row = static_cast<std::uint8_t>(particle.y + horizon);
        const auto column = (x >> 1) | ((x & 1) << 7);
        for (unsigned pixel = 0; pixel < (group == 2 ? 2u : 1u); ++pixel, ++row) {
            const auto destination = std::size_t(row) * 256 + column;
            const auto old = frame.pixels[destination];
            frame.pixels[destination] = type_ == 0 && group != 0 ? solid_color_ : colors_.at(type_)[old];
            frame.write_mask[destination] |= 4;
        }
        const auto pixel_work = group == 2 ? (type_ == 0 ? 4 : 7) : group == 1 && type_ == 0 ? 2 : 3;
        instructions(pixel_work + 3);
    }
    instructions(1);
}

void ScriptedEffects::wind(std::uint32_t tick, std::uint32_t heading, std::uint32_t camera_heading,
                          std::int32_t& forward, std::int32_t& side, std::uint32_t& random, ExecutionClock* clock) {
    const auto instructions = [&](unsigned n) { if (clock) { clock->execute(CpuOperation::instructions, n); } };
    instructions(3);
    if (setup_.waypoints.empty()) {
        instructions(1);
        return;
    }
    instructions(2);
    for (const auto& interval : setup_.waypoints) {
        instructions(2);
        if (tick < interval.x) { instructions(2); continue; }
        instructions(2);
        if (tick > interval.y) { instructions(2); continue; }
        instructions(12);
        const auto angle = (interval.altitude - heading) >> 23;
        const auto component = [&](std::size_t offset) {
            return static_cast<std::int32_t>((std::int64_t(tables_.sine[angle + offset]) *
                                            static_cast<std::int32_t>(interval.speed)) >> 7);
        };
        const auto horizontal = component(0), vertical = component(128);
        auto seed = random;
        if (clock) { clock->instruction([&] { seed = random; }); }
        instructions(5);
        advance(seed, 0xbc9abb09);
        if (clock) { clock->instruction([&] { random = seed; }); }
        else { random = seed; }
        instructions(6);
        state_.gust_phase += seed & 3;
        auto index = state_.gust_phase >> 2;
        if (index >= tables_.gust.size()) {
            instructions(2);
            index = state_.gust_phase = 0;
        }
        instructions(36);
        const auto gust = std::uint32_t(static_cast<std::int32_t>(tables_.gust[index])) << 18;
        forward = static_cast<std::int32_t>(std::uint32_t(forward) + high(static_cast<std::int32_t>(gust), vertical));
        side = static_cast<std::int32_t>(std::uint32_t(side) + high(static_cast<std::int32_t>(gust), horizontal));
        const auto drift = high(tables_.sine[(interval.altitude - camera_heading) >> 23], gust >> 16);
        const auto half = [](std::uint8_t value) { return static_cast<std::uint8_t>(static_cast<std::int8_t>(value) >> 1); };
        auto scaled = half(static_cast<std::uint8_t>(drift * 3));
        state_.drift[0] = scaled;
        for (const auto slot : {11, 12, 13}) { state_.drift[slot] = static_cast<std::uint8_t>(drift); }
        scaled = half(scaled);
        for (const auto slot : {8, 9, 10}) { state_.drift[slot] = scaled; }
        scaled = half(scaled);
        for (const auto slot : {2, 3, 4}) { state_.drift[slot] = scaled; }
        for (const auto slot : {5, 6, 7}) { state_.drift[slot] = half(static_cast<std::uint8_t>(drift)); }
        return;
    }
    instructions(1);
}
}
