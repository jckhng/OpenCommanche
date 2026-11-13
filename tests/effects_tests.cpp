#include "comanchecd/effects.hpp"
#include "loading_clock.hpp"
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
        GameplaySetup setup;
        setup.lights = {{0, 10, 0}, {5, 19, 1}};
        Palette palette{};
        TerrainTables tables;
        for (std::size_t index = 0; index < palette.size(); ++index) {
            palette[index] = {std::uint8_t(index % 64), std::uint8_t(index / 4), 0};
        }
        for (std::size_t index = 0; index < tables.transfer.size(); ++index) {
            tables.transfer[index] = static_cast<std::uint32_t>(index * index);
        }
        EffectsState entry;
        entry.gust_phase = 623;
        entry.drift[1] = 9;
        entry.particles[0] = {80, 90, 14, 2};
        ScriptedEffects carried(setup, palette, tables, entry);
        require(carried.state().particles[0].x == 4 && carried.state().particles[0].y == 4 &&
                carried.state().particles[0].speed == 14 && carried.state().particles[0].depth_level == 2 &&
                carried.state().gust_phase == 623 && carried.state().drift[1] == 9,
                "Mission entry must reset coordinates while retaining particle and wind carry");
        ScriptedEffects overlapping(setup, palette, tables);
        ScriptedEffects timed(setup, {}, tables);
        TerrainFrame timed_frame;
        timed_frame.pixels.fill(37);
        timed_frame.depth.resize(32 * scene_buffer_side);
        for (unsigned i = 0; i < timed_frame.depth.size(); ++i) {
            timed_frame.depth[i] = static_cast<std::uint8_t>((i * 13 + i / scene_buffer_side * 7) % 120);
        }
        SceneView timed_view;
        timed_view.pitch = timed_frame.camera.pitch = -62;
        timed_view.roll = timed_frame.camera.roll = -2;
        std::uint32_t timed_random = 0;
        LoadingClock particle_clock;
        timed.draw(timed_frame, timed_view, 0, -0x1234567, -0x1345678, timed_random,
                   EffectLayer::background, &particle_clock);
        require(particle_clock.accounting().dispatches == 8406,
                "Background particles must charge interval, rotation, respawn and drawing instructions");
        setup.lights.resize(1);
        ScriptedEffects first(setup, palette, tables);
        TerrainFrame a, b;
        a.depth.assign(256 * 32, 0);
        b.depth = a.depth;
        a.pixels.fill(32);
        b.pixels = a.pixels;
        std::uint32_t ra = 0x12345678, rb = ra;
        for (unsigned tick = 0; tick <= 10; ++tick) {
            for (const auto layer : {EffectLayer::background, EffectLayer::foreground}) {
                overlapping.draw(a, {}, tick, 0, 0, ra, layer);
                first.draw(b, {}, tick, 0, 0, rb, layer);
            }
        }
        require(a.pixels == b.pixels && ra == rb,
                "The first interval's AL overwrite must affect later interval comparisons");
        setup.waypoints = {{5, 6, 0, 50}};
        tables.gust.fill(4);
        tables.sine[0] = tables.sine[128] = 0x7fffffff;
        ScriptedEffects wind(setup, palette, tables, entry);
        std::int32_t forward = 0, side = 0;
        auto random = 1u;
        wind.wind(4, 0, 0, forward, side, random);
        require(random == 1 && forward == 0 && wind.state().gust_phase == 623,
                "Wind outside an interval must preserve RNG, velocity and gust phase");
        wind.wind(5, 0, 0, forward, side, random);
        require(wind.state().gust_phase == 0 && random != 1 && forward != 0 && side != 0 && wind.state().drift[1] == 9,
                "Wind begins inclusively, wraps the gust phase and retains untouched drift bytes");
        const auto phase = wind.state().gust_phase;
        const auto seed = random;
        wind.wind(7, 0, 0, forward, side, random);
        require(phase == wind.state().gust_phase && seed == random, "Wind must stop after its inclusive end");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
