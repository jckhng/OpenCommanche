#include "comanchecd/flight.hpp"
#include "comanchecd/execution_clock.hpp"
#include <iostream>
#include <stdexcept>

namespace {
class Clock final : public comanchecd::ExecutionClock {
public:
    std::function<void(std::uint64_t)> dispatch;
    void execute(comanchecd::CpuOperation operation, unsigned count) override {
        if (operation != comanchecd::CpuOperation::instructions) { throw std::runtime_error("Unexpected entity I/O"); }
        for (unsigned i = 0; i < count; ++i) { instruction([] {}); }
    }
    void instruction(const std::function<void()>& action) override {
        ++count_;
        if (dispatch) { dispatch(count_); }
        action();
    }
    void read_file(unsigned) override { throw std::runtime_error("Unexpected entity file read"); }
    bool read_retrace() override { throw std::runtime_error("Unexpected entity VGA poll"); }
    std::uint64_t cycles() const override { return count_; }
    std::uint64_t irq_ticks() const override { return 0; }
    std::uint64_t sample_irq_ticks() override { instruction([] {}); return 0; }
private:
    std::uint64_t count_ = 0;
};
std::uint32_t entity_random(std::uint32_t seed) {
    const auto negative = 0u - seed;
    const auto shifted = (negative >> 1) | (negative & 0x80000000u);
    return shifted ^ ((shifted & 0x80000000u) ? 0xbc9abb09u : 0);
}
}
int main() {
    try {
        using namespace comanchecd;
        constexpr std::uint32_t seed = 0x12345678;
        SceneAssets assets;
        assets.terrain.height.resize(terrain_side * terrain_side);
        for (const auto interrupt_at : {22u, 25u, 30u}) {
            MissionScene scene;
            scene.random_seed = seed;
            scene.entities[scene_slots - 1].active = true;
            FlightController controller({}, scene, {}, 20);
            Clock clock;
            clock.dispatch = [&](std::uint64_t instruction) {
                if (instruction == interrupt_at) { controller.timer_interrupt(); }
            };
            controller.update_entities(assets, &clock);
            IrqState irq{seed};
            irq.interrupt();
            auto expected = entity_random(seed);
            if (interrupt_at == 22) { expected = entity_random(irq.random_seed); }
            if (interrupt_at == 30) { irq.random_seed = expected; irq.interrupt(); expected = irq.random_seed; }
            if (clock.cycles() != 591 || controller.scene().random_seed != expected) {
                throw std::runtime_error("Entity RNG must preserve the original load/store boundary and IRQ ownership");
            }
        }
        {
            MissionScene scene;
            scene.entities[7].active = true;
            scene.entities[7].kind = 3;
            FlightController controller({}, scene, {}, 20);
            Clock clock;
            controller.update_entities(assets, &clock);
            if (clock.cycles() != 580) {
                throw std::runtime_error("An undamaged stationary entity must include its CMP/JNE/RET work");
            }
        }
        struct StationaryCase { std::uint8_t stage; unsigned near_work, far_work; };
        constexpr unsigned single_entity_dispatch_work = 577;
        constexpr unsigned stationary_slot = 47;
        constexpr std::uint32_t distant_position = 0x70000000;
        for (const auto test : {StationaryCase{0, 3, 3}, {1, 55, 48}, {2, 8, 8}, {3, 8, 8},
                                {4, 44, 37}, {5, 8, 8}, {254, 8, 8}, {255, 8, 8}}) {
            for (const auto distant : {false, true}) {
                MissionScene scene;
                auto& entity = scene.entities[stationary_slot];
                entity.active = true;
                entity.kind = 3;
                entity.damage_stage = test.stage;
                GameplaySetup setup;
                if (distant) { setup.x = setup.y = distant_position; }
                FlightController controller(setup, scene, {}, 20);
                Clock clock;
                controller.update_entities(assets, &clock);
                const auto& result = controller.scene().entities[stationary_slot];
                const auto expected_stage = test.stage == 0 ? 0 : static_cast<std::uint8_t>(test.stage + 1);
                if (clock.cycles() != single_entity_dispatch_work + (distant ? test.far_work : test.near_work) ||
                        result.damage_stage != expected_stage ||
                        (result.phase == EntityPhase::explosion) != (test.stage == 4)) {
                    throw std::runtime_error("Stationary destruction must retain its original stage and child work");
                }
            }
        }
        for (const auto occupied : {0u, 2u, 80u}) {
            for (const auto color : {0u, 3u}) {
                for (const auto armor : {0, -1}) {
                    for (const auto distant : {false, true}) {
                        MissionScene scene;
                        auto& entity = scene.entities[stationary_slot];
                        entity.active = true;
                        entity.kind = 3;
                        entity.damage_stage = 1;
                        entity.category_color = static_cast<std::uint8_t>(color);
                        for (unsigned slot = scene_slots - occupied; slot < scene_slots; ++slot) {
                            scene.entities[slot].active = true;
                            scene.entities[slot].phase = EntityPhase::explosion;
                            scene.entities[slot].animation_frame = 1;
                        }
                        GameplaySetup setup;
                        if (distant) { setup.x = setup.y = distant_position; }
                        setup.ammunition = static_cast<std::uint32_t>(armor);
                        FlightController controller(setup, scene, {}, 20);
                        Clock clock;
                        unsigned emitted = 0;
                        controller.update_entities(assets, &clock, [&](const SoundCue& cue) {
                            const auto expected = cue.bank == SampleBank::effects ? 429u : 435u;
                            const auto expected_at = expected + occupied * 19 -
                                unsigned(distant && cue.bank == SampleBank::speech) * 7;
                            if (clock.cycles() != expected_at || occupied == 80) {
                                throw std::runtime_error("Stationary sound calls must retain their original boundaries");
                            }
                            ++emitted;
                        });
                        auto work = 335u;
                        auto expected_cues = 0u;
                        if (occupied < 80) {
                            work = (distant ? 48u : 55u) + occupied * 4;
                            expected_cues = unsigned(!distant) + unsigned(color != 0 && armor >= 0);
                            if (color != 0) { work += 2 + unsigned(armor < 0) * 6; }
                        }
                        if (clock.cycles() != single_entity_dispatch_work + occupied * 15 + work ||
                                emitted != expected_cues) {
                            throw std::runtime_error("Stationary allocation and speech rejection must preserve original work");
                        }
                    }
                }
            }
        }
        for (const auto kind : {7u, 8u}) {
            MissionScene scene;
            auto& entity = scene.entities[7];
            entity.active = true;
            entity.kind = static_cast<std::uint8_t>(kind);
            entity.attribute10 = 2;
            entity.rotor_phase = 32;
            FlightController controller({}, scene, {}, 20);
            Clock clock;
            controller.update_entities(assets, &clock);
            const auto& result = controller.scene().entities[7];
            if (clock.cycles() != 591 || result.attribute10 != 1 || result.rotor_phase != 16 || result.random_phase != 1) {
                throw std::runtime_error("Both aircraft kinds must retain the shared 14-instruction cooldown path");
            }
        }
        for (const auto kind : {2u, 5u, 9u, 15u}) {
            MissionScene scene;
            scene.player_height = 129;
            auto& entity = scene.entities[60];
            entity.active = true;
            entity.kind = static_cast<std::uint8_t>(kind);
            entity.x = entity.y = 0x80000000;
            entity.height = 9;
            entity.attribute7 = 20;
            entity.attribute10 = 66;
            FlightController controller({}, scene, {}, 20);
            Clock clock;
            controller.update_entities(assets, &clock);
            const auto expected_work = kind == 9 ? 592u : 650u;
            if (clock.cycles() != expected_work || controller.scene().entities[60].attribute10 != 65) {
                throw std::runtime_error("Ground cooldown and turret range work must precede the entity RNG step");
            }
        }
        for (const auto stage : {1u, 14u}) {
            MissionScene scene;
            auto& entity = scene.entities[60];
            entity.active = true;
            entity.kind = 2;
            entity.damage_stage = static_cast<std::uint8_t>(stage);
            FlightController controller({}, scene, {}, 20);
            Clock clock;
            controller.update_entities(assets, &clock);
            const auto expected_work = stage == 1 ? 632u : 621u;
            if (clock.cycles() != expected_work) {
                throw std::runtime_error("Ground destruction must account for allocation, conversion and sound caller work");
            }
        }
        struct ProjectileCase { std::uint8_t kind; unsigned work; };
        for (const auto test : {ProjectileCase{guided_missile_kind, 32}, {rocket_kind, 274}, {cannon_round_kind, 40},
                                {stinger_kind, 46}, {enemy_rocket_kind, 31},
                                {climbing_missile_kind, 33}, {enemy_cannon_kind, 39}}) {
            MissionScene scene;
            scene.random_seed = seed;
            auto& entity = scene.entities[100];
            entity.active = true;
            entity.kind = test.kind;
            entity.x = entity.y = 0x80000000;
            entity.height = 100;
            entity.ballistic_height = 100u << 24;
            entity.animation_frame = 1;
            entity.homing_target = 60;
            scene.entities[60].height = 100;
            FlightController controller({}, scene, {}, 20);
            Clock clock;
            controller.update_entities(assets, &clock);
            if (clock.cycles() != single_entity_dispatch_work + test.work ||
                    controller.scene().random_seed != entity_random(seed)) {
                throw std::runtime_error("Projectile work must execute before the shared entity RNG tail");
            }
        }
        {
            MissionScene scene;
            auto& entity = scene.entities[100];
            entity.active = true;
            entity.kind = enemy_rocket_kind;
            entity.random_phase = 127;
            entity.x = entity.y = 0x80000000;
            GameplaySetup setup;
            setup.x = setup.y = entity.x;
            FlightController controller(setup, scene, {}, 20);
            Clock clock;
            unsigned emitted = 0;
            controller.update_entities(assets, &clock, [&](const SoundCue& cue) {
                if (clock.cycles() != 211 || cue.bank != SampleBank::effects || cue.sound != 1) {
                    throw std::runtime_error("Explosion audio must enter at the original sound CALL");
                }
                ++emitted;
                clock.execute(CpuOperation::instructions, 10);
            });
            if (emitted != 1 || controller.sounds().size() != 1 || clock.cycles() != 632) {
                throw std::runtime_error("Synchronous audio work must preserve retained cues and the entity continuation");
            }
        }
        {
            GameplaySetup setup;
            setup.waypoints = {{0, 1, 0, 1}};
            ScriptedEffects effects(setup, {}, {});
            std::int32_t forward = 0, side = 0;
            auto random = seed;
            Clock clock;
            clock.dispatch = [&](std::uint64_t instruction) {
                if (instruction == 29) { random = 0; }
            };
            effects.wind(0, 0, 0, forward, side, random, &clock);
            if (clock.cycles() != 70 || random != 0 || effects.state().gust_phase != (entity_random(seed) & 3)) {
                throw std::runtime_error("Wind must retain EAX for gust selection after an IRQ changes the stored RNG");
            }
        }
        struct ExplosionCase {
            unsigned slot, frame, target, remaining, exit, work;
        };
        for (const auto test : {ExplosionCase{7, 0, 1, 2, 0, 4}, {7, 18, 1, 2, 0, 4},
                                {7, 19, 0, 2, 0, 9}, {61, 19, 1, 2, 0, 7},
                                {7, 19, 1, 0, 0, 11}, {7, 19, 1, 2, 0, 13}}) {
            MissionScene scene;
            auto& entity = scene.entities[test.slot];
            entity.active = true;
            entity.phase = EntityPhase::explosion;
            entity.animation_frame = static_cast<std::uint8_t>(test.frame);
            entity.target = static_cast<std::uint8_t>(test.target);
            scene.remaining_targets = static_cast<std::uint8_t>(test.remaining);
            FlightController controller({}, scene, {}, 20);
            Clock clock;
            controller.update_entities(assets, &clock);
            if (clock.cycles() != 577 + test.work ||
                controller.scene().entities[test.slot].active != (test.frame < 19) ||
                controller.scene().remaining_targets != test.remaining -
                    unsigned(test.frame == 19 && test.slot <= 60 && test.target != 0 && test.remaining != 0)) {
                throw std::runtime_error("Explosion expiry must retain the original branch work and objective decrement");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
