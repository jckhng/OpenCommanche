#include "comanchecd/flight.hpp"
#include "flight_work.hpp"
#include "controller_contacts.hpp"
#include "controller_contact_response.hpp"
#include "comanchecd/execution_clock.hpp"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::int32_t keyboard_control = 350;
constexpr std::int32_t side_limit = 0x00800000;
constexpr std::int32_t yaw_limit = 0x04000000;
constexpr std::uint8_t missile_weapon = 2;
constexpr std::uint8_t cannon_weapon = 0, rocket_weapon = 1;
constexpr std::uint8_t weapon_count = 6;
constexpr std::uint32_t ballistic_gravity = 0x00100000;
constexpr std::uint32_t cannon_contact_distance = 0x01c00000;
constexpr std::uint8_t cannon_end_frame = 7;
constexpr std::uint16_t missile_sound = 4;
constexpr std::size_t object_slots = 60;
constexpr std::uint8_t explosion_end_frame = 20;
constexpr std::uint8_t chaff_animation_frames = 8;
constexpr std::uint8_t missile_damage = 80;
constexpr std::uint32_t missile_contact_distance = 0x01400000;
constexpr std::uint32_t hit_shake_ticks = 10;
constexpr std::uint32_t presentation_reset_ticks = 1;
constexpr std::uint32_t crash_rotation_step = 82;
constexpr std::uint32_t crash_rotation_limit = 3690;
constexpr int missile_turn_limit = 20;
constexpr std::uint8_t missile_descent_age = 6;
constexpr std::uint32_t fuel_exit_threshold = static_cast<std::uint32_t>(-270);

struct SoundBatch {
    std::vector<SoundCue> values;
    SoundCueHandler handler;

    void push_back(const SoundCue& cue) {
        values.push_back(cue);
        if (handler) { handler(cue); }
    }
};

class InterruptTarget {
public:
    InterruptTarget(MissionScene*& scene, FlightState*& state, MissionScene& next_scene, FlightState& next_state)
        : scene_(scene), state_(state), previous_scene_(scene), previous_state_(state) {
        scene_ = &next_scene;
        state_ = &next_state;
    }
    ~InterruptTarget() { scene_ = previous_scene_; state_ = previous_state_; }
    InterruptTarget(const InterruptTarget&) = delete;
    InterruptTarget& operator=(const InterruptTarget&) = delete;
private:
    MissionScene*& scene_;
    FlightState*& state_;
    MissionScene* previous_scene_;
    FlightState* previous_state_;
};

bool release_countermeasure(FlightState& state, MissionScene& scene, const SceneView& pose,
                            DefenseAction action, std::size_t cannon_payload, std::size_t missile_payload,
                            std::size_t reserved = 0, const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    if (action != DefenseAction::chaff && action != DefenseAction::flare) {
        throw std::invalid_argument("Invalid manual countermeasure");
    }
    const auto flare = action == DefenseAction::flare;
    auto& count = flare ? state.flares : state.chaff;
    instructions(2);
    if (count == 0) { instructions(1); return false; }
    instructions(2);
    for (std::size_t slot = scene_slots - 1; slot >= player_slot; --slot) {
        auto& entity = scene.entities[slot];
        instructions(2);
        if (entity.active || slot == reserved) { instructions(2); continue; }
        instructions(flare ? 19 : 20);
        --count;
        auto& active = flare ? state.active_flares : state.active_chaff;
        ++active;
        entity.active = true;
        entity.phase = EntityPhase::normal;
        entity.kind = flare ? flare_kind : chaff_kind;
        entity.random_phase = flare ? 20 : 30;
        entity.animation_frame = 1;
        entity.category_color = 0;
        entity.primary_payload = flare ? cannon_payload : missile_payload - 2;
        entity.x = pose.x;
        entity.y = pose.y;
        entity.height = pose.height;
        return true;
    }
    instructions(1);
    return false;
}


void finish_flight(FlightState& state) {
    state.ended = true;
    state.shake_ticks = presentation_reset_ticks;
    state.shake_offset = 0;
    state.crash_rotation = 0;
}

std::int32_t damp(std::int32_t value, unsigned shift) {
    const auto decrement = (value >> shift) | std::int32_t(value > 0);
    return static_cast<std::int32_t>(std::uint32_t(value) - std::uint32_t(decrement));
}

std::int32_t multiply_high(std::int32_t first, std::int32_t second) {
    return static_cast<std::int32_t>((std::int64_t(first) * second) >> 32);
}

std::uint32_t magnitude(std::uint32_t difference) {
    const auto signed_value = static_cast<std::int32_t>(difference);
    return difference ^ static_cast<std::uint32_t>(signed_value >> 31);
}

unsigned height_distance(std::uint8_t first, std::uint8_t second) {
    const auto half = (int(first) - int(second)) >> 1;
    return static_cast<unsigned>(half ^ (half >> 31));
}

void guide_missile(SceneEntity& missile, const SceneEntity& target, const TerrainTables& tables, const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(5 + 16);
    const auto dx = static_cast<std::int32_t>(missile.x - target.x);
    const auto dy = static_cast<std::int32_t>(missile.y - target.y);
    auto major = magnitude(static_cast<std::uint32_t>(dx));
    auto minor = magnitude(static_cast<std::uint32_t>(dy));
    auto octant = unsigned(dy < 0) * 4 + unsigned(dx < 0) * 2;
    if (major <= minor) {
        instructions(2);
        ++octant;
        std::swap(major, minor);
    }
    const auto distance = major + (minor >> 1);
    auto angle = tables.angle_octants[octant];
    instructions(6);
    if ((major >> 10) != 0) {
        instructions(4);
        angle ^= tables.angle_ratio.at(minor / (major >> 10));
    }
    const auto bearing = static_cast<std::uint8_t>(~((angle + 1) >> 1));
    const auto difference = static_cast<std::uint8_t>(bearing - missile.angle);
    instructions(6 + 2);
    if (difference != 0) {
        const auto half = static_cast<std::uint8_t>((difference >> 1) | (bearing < missile.angle ? 128 : 0));
        // RCR preserves SF from SUB. The branch uses the unhalved byte's sign.
        const auto negative = (difference & 128) != 0;
        const auto amount = negative ? static_cast<std::uint8_t>(-half) : half;
        instructions(2 + (negative ? 5 : 3) + unsigned(amount > missile_turn_limit));
        const auto turn = std::min<int>(amount, missile_turn_limit);
        missile.angle = static_cast<std::uint8_t>(missile.angle + (negative ? -turn : turn));
    }
    instructions(3);
    if (missile.height < 250) {
        instructions(1);
        missile.height = static_cast<std::uint8_t>(missile.height + 3);
    }
    instructions(3);
    if (target.height >= missile.height) {
        instructions(2);
        if (target.height - missile.height > 4) {
            instructions(2 + (missile.height > 247 ? 2 : 0));
            missile.height = static_cast<std::uint8_t>(std::min<unsigned>(missile.height + 8, 255));
        }
        return;
    }
    instructions(4);
    if (missile.random_phase < missile_descent_age) {
        return;
    }
    if (missile.speed == 0) {
        throw std::invalid_argument("Guided missile has zero speed");
    }
    instructions(7);
    const auto duration = (distance << 1) / missile.speed;
    auto descent = unsigned(missile.height - target.height - 1);
    if (duration != 0) {
        instructions(2);
        descent /= duration;
    }
    descent = static_cast<std::uint8_t>(descent + 3);
    instructions(3 + unsigned(missile.height < descent));
    missile.height = static_cast<std::uint8_t>(missile.height < descent ? 0 : missile.height - descent);
}

void explosion_sound(const SceneEntity& entity, const FlightState& state, SoundBatch& sounds,
                     const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    auto x = magnitude(state.view.x - entity.x);
    auto y = magnitude(state.view.y - entity.y);
    instructions(16 + unsigned(x >= y));
    if (x < y) {
        std::swap(x, y);
    }
    const auto attenuation = (x + (y >> 1)) >> 29;
    if (attenuation <= 4) {
        instructions(6);
        sounds.push_back({SampleBank::effects, 1, static_cast<std::uint8_t>(attenuation)});
        instructions(1);
    }
    instructions(1);
}

void begin_explosion(SceneEntity& entity, std::size_t payload, const FlightState& state, SoundBatch& sounds,
                     const CpuWorkSink& work = {}) {
    if (work) { work(CpuOperation::instructions, 12); }
    entity.phase = EntityPhase::explosion;
    entity.primary_payload = payload;
    entity.category_color = 0;
    entity.animation_frame = 1;
    entity.x = entity.previous_x;
    entity.y = entity.previous_y;
    entity.height = entity.previous_height;
    explosion_sound(entity, state, sounds, work);
}

void cannon_impact(SceneEntity& entity, const CpuWorkSink& work = {}) {
    if (work) { work(CpuOperation::instructions, 8); }
    ++entity.animation_frame;
    entity.x = entity.previous_x;
    entity.y = entity.previous_y;
    entity.height = entity.previous_height;
}

void hit_player(SceneEntity& entity, MissionScene& scene, FlightState& player,
                std::size_t explosion_payload, SoundBatch& sounds, ExecutionClock* clock = nullptr);

void update_ballistic(SceneEntity& entity, MissionScene& scene, FlightState& player,
                      const SceneAssets& assets, const TerrainTables& tables,
                      std::size_t explosion_payload, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const auto work = clock ? CpuWorkSink([&](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto cannon = entity.kind == cannon_round_kind;
    if (cannon) { instructions(2); }
    if (cannon && entity.animation_frame > 1) {
        instructions(3);
        if (++entity.animation_frame > cannon_end_frame) {
            instructions(1);
            entity.active = false;
        }
        instructions(1);
        return;
    }
    instructions(17 + unsigned(cannon));
    entity.previous_x = entity.x;
    entity.previous_y = entity.y;
    entity.previous_height = entity.height;
    if (cannon) {
        ++entity.random_phase;
    }
    entity.y -= multiply_high(static_cast<std::int32_t>(entity.speed), tables.sine[entity.angle * 2 + 128]);
    entity.x += multiply_high(static_cast<std::int32_t>(entity.speed), tables.sine[entity.angle * 2]);
    entity.vertical_velocity = static_cast<std::int32_t>(std::uint32_t(entity.vertical_velocity) - ballistic_gravity);
    const auto old_height = entity.ballistic_height;
    entity.ballistic_height += static_cast<std::uint32_t>(entity.vertical_velocity);
    const auto carry = entity.ballistic_height < old_height;
    instructions(2);
    if (entity.vertical_velocity >= 0 && carry) {
        instructions(2);
        entity.active = false;
        return;
    }
    const auto impact = [&] {
        if (cannon) {
            cannon_impact(entity, work);
        } else {
            begin_explosion(entity, explosion_payload, player, sounds, work);
        }
    };
    if (entity.vertical_velocity < 0 && !carry) {
        if (!cannon) { instructions(1); }
        impact();
        return;
    }
    if (entity.vertical_velocity >= 0) { instructions(1); }
    instructions(13);
    entity.height = static_cast<std::uint8_t>(entity.ballistic_height >> 24);
    const auto position = std::size_t(entity.y >> 22) * terrain_side + (entity.x >> 22);
    const auto ground = static_cast<std::uint8_t>((assets.terrain.height[position] << 1) + 1);
    if (entity.height <= ground) {
        if (cannon) {
            instructions(2);
            entity.animation_frame = 3;
        }
        impact();
        return;
    }
    if (cannon) { instructions(2); }
    if (cannon && (entity.random_phase & 128) != 0) {
        impact();
        return;
    }
    if (cannon) { instructions(2); }
    if (cannon && entity.random_phase <= 1) {
        instructions(1);
        return;
    }
    instructions(1);
    const auto radius = cannon ? cannon_contact_distance : missile_contact_distance;
    const auto height_radius = cannon ? 20u : 15u;
    for (std::size_t slot = object_slots; slot > 0; --slot) {
        auto& target = scene.entities[slot];
        instructions(2);
        if (!target.active) { instructions(2); continue; }
        instructions(7);
        if (height_distance(entity.height, target.height) > height_radius) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.x - target.x) > radius) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.y - target.y) > radius) { instructions(2); continue; }
        auto damage = 1u;
        if (!cannon) {
            instructions(1);
            auto random = scene.random_seed;
            if (clock) { clock->instruction([&] { random = scene.random_seed; }); }
            instructions(1);
            damage = (random & 0xc0) != 0 ? 6u : 5u;
            if (damage == 5) { instructions(3); }
        }
        instructions(2);
        if (target.attribute7 < damage) {
            instructions(1);
            target.damage_stage |= 1;
        }
        target.attribute7 = static_cast<std::uint8_t>(target.attribute7 - damage);
        if (!cannon) { instructions(1); }
        impact();
        return;
    }
    if (!cannon) { instructions(1); return; }
    instructions(7);
    if (height_distance(entity.height, player.view.height) > 12) { instructions(1); return; }
    instructions(6);
    if (magnitude(entity.x - player.view.x) > missile_contact_distance) { instructions(1); return; }
    instructions(6);
    if (magnitude(entity.y - player.view.y) > missile_contact_distance) { instructions(1); return; }
    instructions(2);
    ++entity.animation_frame;
    hit_player(entity, scene, player, explosion_payload, sounds, clock);
}

bool spawn_explosion(MissionScene& scene, std::size_t source, std::size_t payload,
                     const FlightState& state, SoundBatch& sounds, const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(3);
    for (std::size_t slot = scene_slots - 1; slot >= player_slot; --slot) {
        auto& entity = scene.entities[slot];
        instructions(2);
        if (entity.active) {
            instructions(2);
            continue;
        }
        instructions(14);
        entity.active = true;
        entity.phase = EntityPhase::explosion;
        entity.primary_payload = payload;
        entity.category_color = 0;
        entity.animation_frame = 1;
        entity.x = scene.entities[source].x;
        entity.y = scene.entities[source].y;
        entity.height = scene.entities[source].height;
        explosion_sound(scene.entities[source], state, sounds, work);
        return true;
    }
    instructions(1);
    return false;
}

struct Bearing {
    std::uint8_t angle;
    std::uint32_t distance;
};

Bearing bearing_to(const SceneEntity& entity, std::uint32_t x, std::uint32_t y, const TerrainTables& tables,
                   const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(16);
    const auto dx = static_cast<std::int32_t>(entity.x - x);
    const auto dy = static_cast<std::int32_t>(entity.y - y);
    auto major = magnitude(std::uint32_t(dx)), minor = magnitude(std::uint32_t(dy));
    auto octant = unsigned(dy < 0) * 4 + unsigned(dx < 0) * 2;
    if (major <= minor) {
        instructions(2);
        ++octant;
        std::swap(major, minor);
    }
    const auto distance = major + (minor >> 1);
    auto angle = tables.angle_octants[octant];
    instructions(6);
    if ((major >> 10) != 0) {
        instructions(4);
        angle ^= tables.angle_ratio.at(minor / (major >> 10));
    }
    instructions(6);
    return {static_cast<std::uint8_t>(~((angle + 1) >> 1)), distance};
}

void save_pose(SceneEntity& entity) {
    entity.previous_x = entity.x;
    entity.previous_y = entity.y;
    entity.previous_height = entity.height;
}

void move_entity(SceneEntity& entity, const TerrainTables& tables) {
    entity.y -= multiply_high(static_cast<std::int32_t>(entity.speed), tables.sine[entity.angle * 2 + 128]);
    entity.x += multiply_high(static_cast<std::int32_t>(entity.speed), tables.sine[entity.angle * 2]);
}

std::uint8_t terrain_height(const SceneEntity& entity, const SceneAssets& assets) {
    return static_cast<std::uint8_t>(assets.terrain.height[std::size_t(entity.y >> 22) * terrain_side + (entity.x >> 22)] << 1);
}

void hit_player(SceneEntity& entity, MissionScene& scene, FlightState& player,
                std::size_t explosion_payload, SoundBatch& sounds, ExecutionClock* clock) {
    const auto work = clock ? CpuWorkSink([&](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto instruction = [&](const std::function<void()>& action) {
        if (clock) { clock->instruction(action); }
        else { action(); }
    };
    bool flare_test = false;
    instruction([&] { flare_test = (scene.random_seed & 12) != 0; });
    instructions(1);
    if (flare_test) { instructions(2); }
    if (flare_test && player.active_flares != 0) {
        instructions(3);
        entity.homing_target = 0;
        return;
    }
    instructions(2);
    if (player.exit_requested != 0) {
        instructions(1);
        return;
    }
    instructions(8); // 2663a..26657 through the sound CALL.
    player.incoming_hit = true;
    player.shake_ticks = hit_shake_ticks;
    sounds.push_back({SampleBank::effects, 5});
    instructions(2);
    instruction([&] { scene.random_seed += player.view.x; });
    std::uint32_t random = 0;
    instruction([&] { random = scene.random_seed & 255; });
    instructions(5);
    --player.system_condition[random & 7];
    --player.system_condition[random >> 5];
    instruction([&] { scene.random_seed = (scene.random_seed >> 8) | (scene.random_seed << 24); });
    instruction([&] { random = scene.random_seed; });
    instructions(4);
    player.armor = static_cast<std::int32_t>(std::uint32_t(player.armor) - (random & 3) - 2);
    if (player.armor < 0) {
        instructions(2);
        player.exit_requested = 1;
    }
    begin_explosion(entity, explosion_payload, player, sounds, work);
}

void missile_contacts(SceneEntity& entity, MissionScene& scene, FlightState& player,
                      std::size_t explosion_payload, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const auto work = clock ? CpuWorkSink([&](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(2);
    if (entity.random_phase < 3) { instructions(1); return; }
    instructions(1);
    for (std::size_t slot = object_slots; slot > 0; --slot) {
        auto& target = scene.entities[slot];
        instructions(2);
        if (!target.active) { instructions(2); continue; }
        instructions(7);
        if (height_distance(entity.height, target.height) > 20) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.x - target.x) > missile_contact_distance) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.y - target.y) > missile_contact_distance) { instructions(2); continue; }
        instructions(1);
        auto random = scene.random_seed;
        if (clock) { clock->instruction([&] { random = scene.random_seed; }); }
        instructions(1);
        const auto damage = (random & 0xc0) != 0 ? 8u : 6u;
        if (damage == 6) { instructions(3); }
        instructions(2);
        if (target.attribute7 < damage) {
            instructions(1);
            target.damage_stage |= 1;
        }
        target.attribute7 = static_cast<std::uint8_t>(target.attribute7 - damage);
        entity.previous_height = target.height;
        instructions(3);
        begin_explosion(entity, explosion_payload, player, sounds, work);
        return;
    }
    instructions(7);
    if (height_distance(entity.height, player.view.height) > 15) { instructions(1); return; }
    instructions(6);
    if (magnitude(entity.x - player.view.x) > missile_contact_distance) { instructions(1); return; }
    instructions(6);
    if (magnitude(entity.y - player.view.y) > missile_contact_distance) { instructions(1); return; }
    hit_player(entity, scene, player, explosion_payload, sounds, clock);
}

void update_guided_missile(SceneEntity& entity, MissionScene& scene, FlightState& player,
                           const SceneAssets& assets, const TerrainTables& tables,
                           std::size_t explosion_payload, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const auto work = clock ? CpuWorkSink([&](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(8);
    save_pose(entity);
    ++entity.random_phase;
    if ((entity.random_phase & 128) != 0) {
        begin_explosion(entity, explosion_payload, player, sounds, work);
        return;
    }
    instructions(3);
    if (player.target.slot != 0) {
        guide_missile(entity, scene.entities.at(player.target.slot), tables, work);
    }
    instructions(7 + 11);
    move_entity(entity, tables);
    const auto ground = static_cast<std::uint8_t>(terrain_height(entity, assets) + 1);
    if (entity.height <= ground) {
        begin_explosion(entity, explosion_payload, player, sounds, work);
        return;
    }
    instructions(2);
    if (entity.random_phase <= 3) { instructions(1); return; }
    instructions(1);
    for (std::size_t slot = object_slots; slot > 0; --slot) {
        auto& target = scene.entities[slot];
        instructions(2);
        if (!target.active) { instructions(2); continue; }
        instructions(7);
        if (height_distance(entity.height, target.height) > 15) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.x - target.x) > missile_contact_distance) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.y - target.y) > missile_contact_distance) { instructions(2); continue; }
        instructions(2);
        if (target.attribute7 < missile_damage) {
            instructions(1);
            target.damage_stage |= 1;
        }
        target.attribute7 = static_cast<std::uint8_t>(target.attribute7 - missile_damage);
        entity.previous_height = target.height;
        instructions(3);
        begin_explosion(entity, explosion_payload, player, sounds, work);
        return;
    }
    instructions(1);
}

void update_enemy_missile(SceneEntity& entity, MissionScene& scene, FlightState& player,
                          const SceneAssets& assets, const TerrainTables& tables,
                          std::size_t explosion_payload, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const auto work = clock ? CpuWorkSink([&](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(8);
    save_pose(entity);
    if ((++entity.random_phase & 128) != 0) {
        begin_explosion(entity, explosion_payload, player, sounds, work);
        return;
    }
    if (entity.kind == stinger_kind) { instructions(3 + (entity.homing_target != 0 ? 2 : 0)); }
    if (entity.kind == stinger_kind && entity.homing_target != 0 && scene.entities.at(entity.homing_target).active) {
        const auto& target = scene.entities[entity.homing_target];
        instructions(5);
        const auto bearing = bearing_to(entity, target.x, target.y, tables, work).angle;
        const auto difference = static_cast<std::uint8_t>(bearing - entity.angle);
        instructions(2);
        if (difference != 0) {
            const auto half = static_cast<std::uint8_t>((difference >> 1) | (bearing < entity.angle ? 128 : 0));
            const auto negative = (difference & 128) != 0;
            const auto amount = negative ? static_cast<std::uint8_t>(-half) : half;
            instructions(2 + (negative ? 5 : 3) + unsigned(amount > 25));
            const auto turn = std::min<unsigned>(amount, 25);
            entity.angle = static_cast<std::uint8_t>(entity.angle + (negative ? -int(turn) : int(turn)));
        }
    }
    instructions(7);
    move_entity(entity, tables);
    const auto ground = terrain_height(entity, assets);
    if (entity.kind == enemy_cannon_kind) {
        instructions(5);
        const auto old_height = entity.ballistic_height;
        entity.ballistic_height += static_cast<std::uint32_t>(entity.vertical_velocity);
        if (entity.vertical_velocity < 0 && entity.ballistic_height >= old_height) {
            begin_explosion(entity, explosion_payload, player, sounds, work);
            return;
        }
        if (entity.vertical_velocity >= 0 && entity.ballistic_height < old_height) {
            instructions(1);
            entity.ballistic_height = 0xffffffff;
        }
        if (entity.vertical_velocity < 0) { instructions(1); }
        instructions(15);
        entity.height = static_cast<std::uint8_t>(entity.ballistic_height >> 24);
        entity.vertical_velocity = static_cast<std::int32_t>(std::uint32_t(entity.vertical_velocity) - 0x02000000);
        if (entity.height <= static_cast<std::uint8_t>(ground + 1)) {
            begin_explosion(entity, explosion_payload, player, sounds, work);
            return;
        }
        instructions(1);
    } else if (entity.kind == enemy_rocket_kind) {
        instructions(11);
        if (entity.height <= static_cast<std::uint8_t>(ground + 1)) {
            begin_explosion(entity, explosion_payload, player, sounds, work);
            return;
        }
        instructions(2);
        --entity.height;
    } else if (entity.kind == climbing_missile_kind) {
        instructions(4 + unsigned(entity.height != scene.entities.at(entity.homing_target).height));
        const auto target_height = scene.entities.at(entity.homing_target).height;
        if (entity.height > target_height) {
            instructions(2);
            const auto underflow = entity.height < 5;
            entity.height = static_cast<std::uint8_t>(entity.height - 5);
            if (underflow) {
                begin_explosion(entity, explosion_payload, player, sounds, work);
                return;
            }
            instructions(1);
        } else if (entity.height < target_height) {
            instructions(2);
            const auto overflow = entity.height > 237;
            entity.height = static_cast<std::uint8_t>(entity.height + 18);
            if (overflow) {
                instructions(3);
                entity.height = 254;
                entity.random_phase = static_cast<std::uint8_t>(entity.random_phase + 10);
                if ((entity.random_phase & 128) != 0) {
                    begin_explosion(entity, explosion_payload, player, sounds, work);
                    return;
                }
            }
        }
        instructions(10);
        if (entity.height < ground) {
            begin_explosion(entity, explosion_payload, player, sounds, work);
            return;
        }
        instructions(1);
    } else {
        instructions(10 + unsigned(ground + 20 > 255));
        auto desired = std::min<unsigned>(ground + 20, 255);
        instructions(3);
        if (entity.homing_target != 0) {
            instructions(2 + unsigned(desired <= scene.entities.at(entity.homing_target).height));
            desired = std::max<unsigned>(desired, scene.entities.at(entity.homing_target).height);
        }
        instructions(2);
        if (desired != entity.height) { instructions(1); }
        if (desired > entity.height) {
            instructions(2);
            if (desired - entity.height > 10) {
                instructions(2);
                const auto overflow = entity.height > 239;
                if (overflow) { instructions(2); }
                entity.height = static_cast<std::uint8_t>(std::min<unsigned>(entity.height + 16, 255));
            }
        } else if (desired < entity.height) {
            instructions(3);
            if (entity.height - desired > 8) {
                instructions(2);
                const auto underflow = entity.height < 16;
                entity.height = static_cast<std::uint8_t>(entity.height - 16);
                if (underflow) {
                    begin_explosion(entity, explosion_payload, player, sounds, work);
                    return;
                }
            }
        }
        instructions(5);
        if (entity.height <= static_cast<std::uint8_t>(ground + 1)) {
            begin_explosion(entity, explosion_payload, player, sounds, work);
            return;
        }
    }
    missile_contacts(entity, scene, player, explosion_payload, sounds, clock);
}

void enemy_launch(MissionScene& scene, const SceneEntity& source, std::uint8_t target,
                  std::size_t payload_count, const TerrainTables& tables, const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(3);
    for (std::size_t slot = scene_slots - 1; slot >= player_slot; --slot) {
        auto& projectile = scene.entities[slot];
        instructions(2);
        if (projectile.active) {
            instructions(2);
            continue;
        }
        instructions(21);
        projectile.active = true;
        projectile.phase = EntityPhase::normal;
        projectile.random_phase = 0;
        projectile.category_color = 8;
        if (source.kind != 10 && source.kind != 14 && source.kind != 15 && source.kind != 16) {
            projectile.homing_target = target;
        }
        projectile.kind = source.kind == 2 || source.kind == 13 ? climbing_missile_kind :
            source.kind == 5 || source.kind == 6 || source.kind == 8 || source.kind == 11 || source.kind == 12 || source.kind == 17 ?
            stinger_kind : enemy_rocket_kind;
        projectile.primary_payload = payload_count - (projectile.kind == climbing_missile_kind ? 12 :
                                                    projectile.kind == stinger_kind ? 11 : 10);
        projectile.speed = 0x04000000;
        projectile.x = source.x;
        projectile.y = source.y;
        projectile.height = source.height;
        projectile.angle = source.angle;
        if (source.kind == 10 || source.kind == 14 || source.kind == 15 || source.kind == 16) {
            projectile.kind = source.kind == 10 ? cannon_round_kind : enemy_cannon_kind;
            projectile.primary_payload = payload_count - 9;
            projectile.animation_frame = 1;
            projectile.ballistic_height = std::uint32_t(source.height) << 24;
            if (source.kind == 10) {
                instructions(3);
                projectile.category_color = 0;
                projectile.vertical_velocity = 0x06000000;
            } else {
                instructions(24);
                const auto& victim = scene.entities.at(target);
                const auto bearing = bearing_to(source, victim.x, victim.y, tables, work);
                projectile.angle = bearing.angle;
                projectile.speed = 0x03800000;
                const auto distance = bearing.distance >> 1;
                if (distance == 0) {
                    throw std::invalid_argument("Enemy cannon target has zero firing distance");
                }
                const auto reciprocal = static_cast<std::int32_t>(0xffffffffu / distance);
                const auto height_delta = static_cast<std::int32_t>(
                    std::uint32_t((int(victim.height) - int(source.height)) >> 1) << 24);
                projectile.vertical_velocity = static_cast<std::int32_t>(
                    distance + std::uint32_t((std::int64_t(height_delta) * reciprocal) >> 7));
            }
        }
        return;
    }
    instructions(2);
}

void attack_allies(MissionScene& scene, const SceneEntity& source, const TerrainTables& tables, std::size_t payload_count,
                   const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(3); // CALL and the enable check.
    if (!scene.attack_allies) {
        instructions(1);
        return;
    }
    instructions(3);
    if ((source.random_phase & source.attribute9) != 0) {
        instructions(1);
        return;
    }
    instructions(1);
    for (std::size_t slot = object_slots; slot > 0; --slot) {
        const auto& target = scene.entities[slot];
        instructions(2);
        if (!target.active) {
            instructions(2);
            continue;
        }
        instructions(2);
        if (target.category_color != 9) {
            instructions(2);
            if (target.category_color != 6) {
                instructions(2);
                continue;
            }
        }
        instructions(6);
        const auto bearing = bearing_to(source, target.x, target.y, tables, work);
        instructions(3);
        if (bearing.distance >= 0x2bc00000) {
            instructions(2);
            continue;
        }
        instructions(2);
        if (bearing.distance <= 0x0a000000) {
            instructions(2);
            continue;
        }
        const auto difference = static_cast<std::uint8_t>(bearing.angle - source.angle);
        const auto half = static_cast<std::int8_t>((difference >> 1) | (bearing.angle < source.angle ? 128 : 0));
        instructions(6);
        if (static_cast<std::uint8_t>(half ^ (half >> 7)) >= 16) {
            instructions(2);
            continue;
        }
        instructions(2);
        if (slot == scene.wingman) {
            instructions(2);
            if (scene.wingman_alert == 0) {
                instructions(2);
                scene.wingman_alert = 20;
                return;
            }
        }
        instructions(3);
        enemy_launch(scene, source, static_cast<std::uint8_t>(slot), payload_count, tables, work);
        instructions(1);
        return;
    }
    instructions(1);
}

void ground_movement(SceneEntity& entity, const SceneAssets& assets, const TerrainTables& tables, unsigned turn_extra,
                     const CpuWorkSink& work) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto original_angle = entity.angle;
    unsigned attempts = 0;
    const auto attempt = [&](std::uint8_t angle) {
        entity.x = entity.previous_x;
        entity.y = entity.previous_y;
        entity.angle = angle;
        move_entity(entity, tables);
        const auto ground = unsigned(terrain_height(entity, assets)) + 9;
        instructions((attempts++ == 0 ? 19 : 23) + unsigned(ground > 255));
        return entity.height == std::min<unsigned>(ground, 255);
    };
    if (attempt(original_angle)) {
        instructions(turn_extra != 0 ? 2 : 1);
        entity.avoidance_turn = 0;
        return;
    }
    instructions(4);
    bool turn_right = (entity.avoidance_turn & 128) == 0;
    if (turn_right) {
        instructions(1);
        if (entity.avoidance_turn == 0) {
            instructions(3);
            turn_right = attempt(static_cast<std::uint8_t>(original_angle + 32));
            if (!turn_right) {
                instructions(3);
                const auto left = attempt(static_cast<std::uint8_t>(original_angle - 32));
                if (!left) {
                    instructions(3);
                    turn_right = attempt(static_cast<std::uint8_t>(original_angle + 64));
                }
            }
        }
    }
    instructions(6 + turn_extra + unsigned(!turn_right) + 5);
    const auto amount = (entity.speed >> 21) + turn_extra;
    entity.angle = static_cast<std::uint8_t>(original_angle + (turn_right ? int(amount) : -int(amount)));
    entity.avoidance_turn = turn_right ? 1 : 255;
    entity.x = entity.previous_x;
    entity.y = entity.previous_y;
}

void update_ground(SceneEntity& entity, std::size_t slot, MissionScene& scene, FlightState& player,
                   const SceneAssets& assets, const TerrainTables& tables,
                   std::size_t payload_count, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const CpuWorkSink work = clock ? CpuWorkSink([clock](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto turret = entity.kind == 2 || entity.kind == 5 || entity.kind == 15;
    instructions(turret ? 2 : 4);
    if (entity.damage_stage != 0) {
        instructions(2);
        if (entity.damage_stage == 1) {
            instructions(3);
            ++entity.damage_stage;
            spawn_explosion(scene, slot, payload_count - 8, player, sounds, work);
            if (player.armor >= 0) {
                sounds.push_back({SampleBank::speech, 1});
            }
            instructions(4);
        } else {
            instructions(3);
            if (++entity.damage_stage == 15) {
                begin_explosion(entity, payload_count - 8, player, sounds, work);
            } else {
                instructions(1);
            }
        }
        return;
    }
    ++entity.random_phase;
    save_pose(entity);
    instructions(7 + 2);
    if (!turret && entity.attribute10 != 0) {
        instructions(1);
        --entity.attribute10;
    } else if (entity.attribute7 > (turret ? 6 : 2)) {
        instructions(2);
        if (entity.attribute10 != 0) {
            instructions(1);
            --entity.attribute10;
        } else {
            ground_movement(entity, assets, tables, turret ? 1 : 0, work);
        }
    } else if (!turret) {
        instructions(2);
    }
    if (!turret) {
        instructions(1);
        return;
    }
    instructions(3);
    entity.angle = static_cast<std::uint8_t>(entity.angle + entity.rotor_phase);
    entity.height = static_cast<std::uint8_t>(entity.height + 6);
    attack_allies(scene, entity, tables, payload_count, work);
    entity.angle = static_cast<std::uint8_t>(entity.angle - entity.rotor_phase);
    entity.height = static_cast<std::uint8_t>(entity.height - 6);
    instructions(8);
    const auto bearing = bearing_to(entity, player.view.x, player.view.y, tables, work);
    auto range = (std::uint32_t(player.view.height) << 21) + 0x25800000;
    if (entity.projected_x != 0) {
        range += 0x11800000;
    }
    if (scene.extended_enemy_range) {
        range += 0x1b800000;
    }
    instructions(9 + unsigned(entity.projected_x != 0) + unsigned(scene.extended_enemy_range));
    if (bearing.distance >= range) {
        instructions(1);
        return;
    }
    bool chaff_gate = false;
    if (clock) { clock->instruction([&] { chaff_gate = (scene.random_seed & 12) != 0; }); }
    else { chaff_gate = (scene.random_seed & 12) != 0; }
    instructions(1);
    if (chaff_gate) {
        instructions(2);
        if (player.active_chaff != 0) { instructions(1); return; }
    }
    instructions(10);
    player.enemy_tracking = true;
    const auto difference = static_cast<std::int8_t>(bearing.angle - static_cast<std::uint8_t>(entity.angle + entity.rotor_phase));
    const auto error = static_cast<std::uint8_t>(difference ^ (difference >> 7));
    if (error < 13) {
        instructions(3);
        if ((entity.random_phase & entity.attribute9) == 0) {
            instructions(8);
            entity.angle = static_cast<std::uint8_t>(entity.angle + entity.rotor_phase);
            entity.height = static_cast<std::uint8_t>(entity.height + 6);
            enemy_launch(scene, entity, player_slot, payload_count, tables, work);
            entity.angle = static_cast<std::uint8_t>(entity.angle - entity.rotor_phase);
            entity.height = static_cast<std::uint8_t>(entity.height - 6);
            instructions(4);
        }
    }
    instructions(2);
    if (difference != 0) {
        instructions(4);
        const auto turn = error < 13 ? 1 : 2;
        entity.rotor_phase = static_cast<std::uint8_t>(entity.rotor_phase + (difference < 0 ? -turn : turn));
    } else {
        instructions(1);
    }
}

void update_aircraft(SceneEntity& entity, std::size_t slot, MissionScene& scene, FlightState& player,
                     const SceneAssets& assets, const TerrainTables& tables,
                     std::size_t payload_count, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const CpuWorkSink work = clock ? CpuWorkSink([clock](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(10);
    entity.rotor_phase = static_cast<std::uint8_t>(entity.rotor_phase - 16);
    save_pose(entity);
    ++entity.random_phase;
    if (entity.damage_stage != 0) {
        instructions(2); // 25bc2 damage-stage gate.
        if (entity.damage_stage == 1) {
            instructions(3);
            ++entity.damage_stage;
            if (player.armor >= 0) {
                sounds.push_back({SampleBank::speech, 1});
            } else {
                instructions(6); // 501d..5030 suppresses the announcement after player death.
            }
        }
        entity.angle = static_cast<std::uint8_t>(entity.angle + 16);
        const auto underflow = entity.height < 4;
        entity.height = static_cast<std::uint8_t>(entity.height - 4);
        instructions(3);
        if (underflow) {
            instructions(1);
            begin_explosion(entity, payload_count - 8, player, sounds, work);
            return;
        }
        const auto ground = terrain_height(entity, assets);
        instructions(11); // 25be8..25c12 terrain lookup and impact gate.
        if (ground >= entity.height) {
            instructions(1);
            begin_explosion(entity, payload_count - 8, player, sounds, work);
            return;
        }
        instructions(2);
        if (entity.damage_stage <= 2) {
            instructions(3);
            if (static_cast<std::uint8_t>(ground + 15) >= entity.height) {
                instructions(2);
                ++entity.damage_stage;
                spawn_explosion(scene, slot, payload_count - 8, player, sounds, work);
                return;
            }
        }
        instructions(1);
        return;
    }
    instructions(2);
    if (entity.attribute10 != 0) {
        instructions(2);
        --entity.attribute10;
        return;
    }
    instructions(19);
    move_entity(entity, tables);
    const auto clearance = terrain_height(entity, assets) + 36u;
    const auto desired = std::min<unsigned>(clearance, 255);
    if (clearance > 255) { instructions(1); }
    if (desired > entity.height) {
        instructions(3);
        const auto difference = desired - entity.height;
        if (difference > 20) {
            instructions(4);
            entity.x = entity.previous_x;
            entity.y = entity.previous_y;
        }
        instructions(2);
        if (difference > 4) {
            instructions(2);
            if (entity.height > 247) { instructions(2); }
            entity.height = static_cast<std::uint8_t>(std::min<unsigned>(entity.height + 8, 255));
        }
    } else if (desired < entity.height) {
        instructions(4);
        if (entity.height - desired > 4) {
            instructions(1);
            entity.height = static_cast<std::uint8_t>(entity.height - 8);
        }
    }
    attack_allies(scene, entity, tables, payload_count, work);
    instructions(5);
    const auto bearing = bearing_to(entity, player.view.x, player.view.y, tables, work);
    const auto range = (((std::uint32_t(scene.preset) ^ 1) << 6) + 185 +
                        (scene.extended_enemy_range ? 110 : 0)) << 22;
    const auto difference = static_cast<std::int8_t>(bearing.angle - entity.angle);
    const auto error = static_cast<std::uint8_t>(difference ^ (difference >> 7));
    instructions(9 + unsigned(scene.extended_enemy_range));
    const auto seed = [&] {
        auto value = scene.random_seed;
        if (clock) { clock->instruction([&] { value = scene.random_seed; }); }
        return value;
    };
    if (bearing.distance >= range) {
        const auto value = seed();
        instructions(4);
        entity.angle = static_cast<std::uint8_t>(entity.angle + (value & 3) - 2);
        return;
    }
    instructions(7);
    if (error < 14) {
        instructions(3);
        if ((entity.random_phase & entity.attribute9) == 0) {
            instructions(4);
            enemy_launch(scene, entity, player_slot, payload_count, tables, work);
        }
        instructions(3 + unsigned(player.view.height < 10));
        const auto minimum = player.view.height < 10 ? 10 : player.view.height - 10;
        instructions(2);
        if (entity.height <= minimum) {
            instructions(4);
            entity.height = static_cast<std::uint8_t>(entity.height > 239 ? 254 : entity.height + 10);
        } else {
            instructions(1);
        }
        return;
    }
    instructions(2);
    if (bearing.distance <= 0x05000000) {
        const auto value = seed();
        instructions(3);
        entity.angle = static_cast<std::uint8_t>(entity.angle + (value & 7));
        return;
    }
    instructions(2);
    if (bearing.distance < 0x19000000) {
        instructions(2);
        if (error < 70) {
            instructions(2);
            if (difference == 0) { instructions(1); return; }
            instructions(4);
            entity.angle = static_cast<std::uint8_t>(entity.angle + (difference < 0 ? -12 : 12));
            return;
        }
    }
    const auto value = seed();
    instructions(4);
    entity.angle = static_cast<std::uint8_t>(entity.angle + (value & 3) - 2);
}

void friendly_altitude(SceneEntity& entity, const SceneAssets& assets, unsigned clearance, const CpuWorkSink& work) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto height = terrain_height(entity, assets) + clearance;
    const auto desired = std::min<unsigned>(height, 255);
    instructions(10 + unsigned(height > 255));
    instructions(2);
    if (desired == entity.height) {
        return;
    }
    instructions(1);
    if (desired > entity.height) {
        instructions(2);
        if (desired - entity.height > 15) {
            instructions(4);
            entity.x = entity.previous_x;
            entity.y = entity.previous_y;
        }
        instructions(2);
        if (desired - entity.height > 4) {
            instructions(2 + (entity.height > 247 ? 2 : 0));
            entity.height = static_cast<std::uint8_t>(std::min<unsigned>(entity.height + 8, 255));
        }
    } else {
        instructions(2);
        if (static_cast<std::int8_t>(desired - entity.height) < -4) {
            instructions(1);
            entity.height = static_cast<std::uint8_t>(entity.height - 8);
        }
    }
}

bool launch_wingman(MissionScene& scene, FlightState& player, std::size_t payload,
                    SoundBatch& sounds, const std::function<void(const FlightState&)>& stores,
                    const CpuWorkSink& work) {
    constexpr auto weapon = static_cast<unsigned>(WeaponSelection::wingman);
    const auto instructions = [&](unsigned count) { if (work) { work(CpuOperation::instructions, count); } };
    const auto empty_stores = [&] {
        instructions(3); // 245a0 clears ammunition, sets the redraw flag and calls 7c10.
        player.ammunition[weapon] = 0;
        if (stores) { stores(player); }
        instructions(1);
    };
    instructions(2);
    if (player.ammunition[weapon] == 0) {
        empty_stores();
        return false;
    }
    instructions(2);
    std::size_t slot = scene_slots - 1;
    for (; slot >= player_slot; --slot) {
        instructions(2);
        if (!scene.entities[slot].active) { break; }
        instructions(2);
    }
    if (slot < player_slot) {
        instructions(1);
        return false;
    }
    instructions(5);
    --player.ammunition[weapon];
    if (scene.wingman != 0) { instructions(2); }
    if (scene.wingman == 0 || !scene.entities.at(scene.wingman).active) {
        empty_stores();
        return false;
    }
    instructions(3);
    if (player.weapon == weapon) {
        instructions(2);
        if (stores) { stores(player); }
    }
    instructions(5);
    sounds.push_back({SampleBank::effects, missile_sound, 1});
    const auto& wingman = scene.entities[scene.wingman];
    auto& missile = scene.entities[slot];
    missile.active = true;
    missile.phase = EntityPhase::normal;
    missile.kind = guided_missile_kind;
    missile.random_phase = 0;
    missile.category_color = 0;
    missile.primary_payload = payload;
    missile.homing_target = static_cast<std::uint8_t>(player.target.slot);
    missile.speed = 0x04000000;
    missile.x = wingman.x;
    missile.y = wingman.y;
    missile.height = wingman.height;
    missile.angle = wingman.angle;
    instructions(22); // 245f8..24681 copies the wingman's launch pose and returns.
    return true;
}

bool acknowledge_wingman(SceneEntity& entity, const MissionScene& scene, const FlightState& player,
                         SoundBatch& sounds, const CpuWorkSink& work) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    instructions(2);
    if (--entity.random_phase != 0) {
        return false;
    }
    instructions(3);
    if (player.target.slot == scene.wingman) {
        instructions(2);
        sounds.push_back({SampleBank::speech, 12});
        instructions(1);
        return true;
    }
    instructions(2);
    if (player.target.slot == 0) {
        return false;
    }
    instructions(2);
    if (player.weapon != 5) {
        return false;
    }
    instructions(2);
    sounds.push_back({SampleBank::speech, 10});
    return true;
}

void update_friendly_aircraft(SceneEntity& entity, std::size_t slot, MissionScene& scene, FlightState& player,
                              const SceneAssets& assets, const TerrainTables& tables,
                              const std::array<std::int32_t, 2>& movement_basis,
                              std::size_t payload_count, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const CpuWorkSink work = clock ? CpuWorkSink([clock](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto wingman = entity.kind == 4;
    const auto autonomous = !wingman || scene.wingman_alert != 0;
    if (wingman) {
        instructions(2 + (entity.damage_stage == 0 ? 2 : 0));
    }
    if (!wingman || entity.damage_stage == 0) {
        if (wingman && autonomous) {
            instructions(1);
            --scene.wingman_alert;
        }
        instructions(7 + (autonomous ? 2 : 0));
        entity.rotor_phase = static_cast<std::uint8_t>(entity.rotor_phase - 16);
        save_pose(entity);
    }
    if (entity.damage_stage != 0) {
        instructions(2);
        if (entity.damage_stage == 1) {
            instructions(1);
            ++entity.damage_stage;
            if (wingman) {
                instructions(3);
                scene.wingman_lost = true;
                player.ammunition[5] = 0;
                sounds.push_back({SampleBank::speech, 5});
                instructions(1);
            }
        }
        instructions(3);
        entity.angle = static_cast<std::uint8_t>(entity.angle + 16);
        const auto underflow = entity.height < 4;
        entity.height = static_cast<std::uint8_t>(entity.height - 4);
        if (underflow) {
            instructions(1);
            begin_explosion(entity, payload_count - 8, player, sounds, work);
            return;
        }
        const auto ground = terrain_height(entity, assets);
        instructions(11);
        if (ground >= entity.height) {
            instructions(1);
            begin_explosion(entity, payload_count - 8, player, sounds, work);
            return;
        }
        instructions(2);
        if (entity.damage_stage > 2) {
            instructions(1);
            return;
        }
        instructions(3);
        if (static_cast<std::uint8_t>(ground + 15) >= entity.height) {
            instructions(2);
            ++entity.damage_stage;
            spawn_explosion(scene, slot, payload_count - 8, player, sounds, work);
            return;
        }
        instructions(1);
        return;
    }
    auto x = player.view.x, y = player.view.y;
    if (!autonomous) {
        instructions(6);
        if (player.weapon == 5) {
            instructions(3);
        }
        if (player.weapon == 5 && player.target.slot != 0) {
            instructions(6 + unsigned(entity.height > 246));
            const auto& target = scene.entities.at(player.target.slot);
            x = target.x;
            y = target.y;
            entity.height = static_cast<std::uint8_t>(std::min<unsigned>(entity.height + 9, 255));
        }
        constexpr auto formation_offset = static_cast<std::int32_t>(0xee800000);
        // The initial draw precedes the first controller update of 1a954/1a958.
        x += multiply_high(formation_offset, movement_basis[1]);
        y += multiply_high(formation_offset, movement_basis[0]);
        instructions(7);
        const auto bearing = bearing_to(entity, x, y, tables, work);
        const auto difference = static_cast<std::int8_t>(bearing.angle - entity.angle);
        bool acknowledged = false;
        instructions(3 + (difference != 0 ? 3 : 0));
        if (difference > 4 || difference < -4) {
            instructions(3);
            entity.angle = static_cast<std::uint8_t>(entity.angle + (difference < 0 ? -8 : 8));
            entity.random_phase = 15;
        } else {
            acknowledged = acknowledge_wingman(entity, scene, player, sounds, work);
        }
        // 501d replaces ESI with the speech bank before the caller's distance test.
        instructions(2);
        if (!acknowledged && bearing.distance > 0x19000000) {
            instructions(7);
            move_entity(entity, tables);
        }
        friendly_altitude(entity, assets, 34, work);
        instructions(1);
        return;
    }
    instructions(2);
    if (entity.attribute10 != 0) {
        instructions(2);
        --entity.attribute10;
        return;
    }
    instructions(5);
    const auto bearing = bearing_to(entity, x, y, tables, work);
    const auto difference = static_cast<std::int8_t>(bearing.angle - entity.angle);
    instructions(2);
    if (bearing.distance > 0x32000000) {
        instructions(3 + (difference != 0 ? 3 : 0));
        if (difference > 16) {
            instructions(2);
            entity.angle = static_cast<std::uint8_t>(entity.angle + 2);
        } else if (difference < 0 && difference >= -16) {
            // The original branch enters the wingman's acknowledgement/altitude tail.
            const auto acknowledged = acknowledge_wingman(entity, scene, player, sounds, work);
            instructions(2);
            if (!acknowledged) {
                instructions(7);
                move_entity(entity, tables);
            }
            friendly_altitude(entity, assets, 34, work);
            instructions(1);
            return;
        } else if (difference < -16) {
            instructions(1);
            entity.angle = static_cast<std::uint8_t>(entity.angle - 2);
        }
    }
    instructions(7);
    move_entity(entity, tables);
    friendly_altitude(entity, assets, 50, work);
    instructions(4);
    if ((++entity.random_phase & entity.attribute9) != 0) {
        instructions(1);
        return;
    }
    instructions(1);
    for (std::size_t target_slot = object_slots; target_slot > 0; --target_slot) {
        const auto& target = scene.entities[target_slot];
        instructions(2);
        if (!target.active) {
            instructions(2);
            continue;
        }
        instructions(2 + (target.category_color != 7 ? 2 : 0));
        if (target.category_color != 7 && target.category_color != 8) {
            instructions(2);
            continue;
        }
        instructions(6);
        const auto aim = bearing_to(entity, target.x, target.y, tables, work);
        instructions(3 + (aim.distance < 0x28000000 ? 2 : 0));
        if (aim.distance <= 0x0c800000 || aim.distance >= 0x28000000) {
            instructions(2);
            continue;
        }
        const auto delta = static_cast<std::uint8_t>(aim.angle - entity.angle);
        const auto half = static_cast<std::int8_t>((delta >> 1) | (aim.angle < entity.angle ? 128 : 0));
        instructions(6);
        if (static_cast<std::uint8_t>(half ^ (half >> 7)) < 16) {
            instructions(2);
            auto launcher = entity;
            launcher.kind = 11;
            enemy_launch(scene, launcher, static_cast<std::uint8_t>(target_slot), payload_count, tables, work);
            instructions(1);
            return;
        }
        instructions(2);
    }
    instructions(1);
}

void update_support_vehicle(SceneEntity& entity, std::size_t slot, MissionScene& scene, FlightState& player,
                            const SceneAssets& assets, const TerrainTables& tables,
                            std::size_t payload_count, SoundBatch& sounds, ExecutionClock* clock = nullptr) {
    const CpuWorkSink work = clock ? CpuWorkSink([clock](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{};
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto original_angle = entity.angle;
    instructions(1); // Shared 25580 movement/damage CALL.
    update_ground(entity, slot, scene, player, assets, tables, payload_count, sounds, clock);
    instructions(2);
    if (entity.damage_stage != 0) {
        if (entity.kind >= 12 && entity.kind <= 14) {
            instructions(2);
            if (entity.height != 0) {
                instructions(1);
                --entity.height;
            }
        }
        instructions(1);
        return;
    }
    if (entity.kind == 10) {
        instructions(5);
        const auto bearing = bearing_to(entity, player.view.x, player.view.y, tables, work);
        auto seed = scene.random_seed;
        if (clock) { clock->instruction([&] { seed = scene.random_seed; }); }
        instructions(4);
        if (bearing.distance >= (seed & 0x1fc00000) + 0x05000000) {
            instructions(1);
            return;
        }
        instructions(3);
        if ((entity.random_phase & entity.attribute9) != 0) {
            instructions(1);
            return;
        }
        instructions(4);
        const auto angle = entity.angle;
        entity.angle = bearing.angle;
        enemy_launch(scene, entity, player_slot, payload_count, tables, work);
        entity.angle = angle;
        instructions(3);
        return;
    }
    if (entity.kind >= 16) {
        instructions(3);
        if ((entity.random_phase & entity.attribute9) != 0) {
            instructions(1);
            return;
        }
        instructions(1);
        for (std::size_t target_slot = object_slots; target_slot > 0; --target_slot) {
            const auto& target = scene.entities[target_slot];
            instructions(2);
            if (!target.active) {
                instructions(2);
                continue;
            }
            instructions(2 + (target.category_color != 7 ? 2 : 0));
            if (target.category_color != 7 && target.category_color != 8) {
                instructions(2);
                continue;
            }
            instructions(6);
            const auto aim = bearing_to(entity, target.x, target.y, tables, work);
            instructions(3 + (aim.distance < 0x28000000 ? 2 : 0));
            if (aim.distance <= 0x0c800000 || aim.distance >= 0x28000000) {
                instructions(2);
                continue;
            }
            const auto angle = static_cast<std::uint8_t>(entity.angle - entity.rotor_phase);
            const auto delta = static_cast<std::uint8_t>(aim.angle - angle);
            const auto half = static_cast<std::int8_t>((delta >> 1) | (aim.angle < angle ? 128 : 0));
            const auto error = static_cast<std::uint8_t>(half ^ (half >> 7));
            instructions(8);
            if (error >= 16) {
                instructions(2);
                continue;
            }
            instructions(2);
            if (error != 0) {
                instructions(4);
                entity.rotor_phase = static_cast<std::uint8_t>(entity.rotor_phase + (half < 0 ? -1 : 1));
            }
            instructions(8);
            entity.attribute10 = 5;
            entity.angle = static_cast<std::uint8_t>(entity.angle + entity.rotor_phase);
            entity.height = static_cast<std::uint8_t>(entity.height + 6);
            enemy_launch(scene, entity, static_cast<std::uint8_t>(target_slot), payload_count, tables, work);
            entity.angle = static_cast<std::uint8_t>(entity.angle - entity.rotor_phase);
            entity.height = static_cast<std::uint8_t>(entity.height - 6);
            instructions(5);
            return;
        }
        instructions(1);
        return;
    }
    instructions(1); // attack_allies owns its CALL.
    entity.height = static_cast<std::uint8_t>(entity.height + 8);
    attack_allies(scene, entity, tables, payload_count, work);
    entity.height = static_cast<std::uint8_t>(entity.height - 8);
    instructions(6);
    const auto bearing = bearing_to(entity, player.view.x, player.view.y, tables, work);
    const auto range = (((std::uint32_t(scene.preset) ^ 1) << 6) + 185 +
                        (scene.extended_enemy_range ? 110 : 0)) << 22;
    instructions(9 + unsigned(scene.extended_enemy_range));
    if (bearing.distance >= range) {
        instructions(1);
        return;
    }
    instructions(2);
    if (bearing.distance > 0x05000000) {
        instructions(8);
        player.enemy_tracking = true;
        const auto difference = static_cast<std::int8_t>(bearing.angle - entity.angle);
        if (static_cast<std::uint8_t>(difference ^ (difference >> 7)) < 10) {
            instructions(3);
            if ((entity.random_phase & entity.attribute9) == 0) {
                instructions(5);
                entity.height = static_cast<std::uint8_t>(entity.height + 8);
                enemy_launch(scene, entity, player_slot, payload_count, tables, work);
                entity.height = static_cast<std::uint8_t>(entity.height - 8);
                entity.attribute10 = 5;
                instructions(2);
            }
            instructions(1);
            return;
        }
    }
    instructions(2);
    if (entity.avoidance_turn == 0) {
        instructions(5);
        entity.angle = static_cast<std::uint8_t>(original_angle - (entity.speed >> 21));
    }
    instructions(1);
}

void update_artillery(SceneEntity& entity, MissionScene& scene, FlightState& player,
                      std::size_t explosion_payload, SoundBatch& sounds, const CpuWorkSink& work = {}) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    ++entity.random_phase;
    instructions(4); // 25d90 age increment and first cue gate.
    if (entity.random_phase == 100) {
        instructions(5);
        sounds.push_back({SampleBank::effects, 2});
        return;
    }
    instructions(2);
    if (entity.random_phase == 70) {
        instructions(2);
        sounds.push_back({SampleBank::speech, 9});
        return;
    }
    instructions(2);
    if (entity.random_phase < 120) {
        instructions(1);
        return;
    }
    instructions(6);
    sounds.push_back({SampleBank::effects, 6});
    player.shake_ticks = 5;
    entity.phase = EntityPhase::explosion;
    entity.category_color = 0;
    entity.primary_payload = explosion_payload;
    entity.animation_frame = 1;
    instructions(9);
    constexpr std::uint32_t blast_radius = 0x0c800000;
    for (std::size_t slot = object_slots; slot > 0; --slot) {
        auto& target = scene.entities[slot];
        instructions(2);
        if (!target.active) { instructions(2); continue; }
        instructions(7);
        if (height_distance(entity.height, target.height) > 50) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.x - target.x) > blast_radius) { instructions(2); continue; }
        instructions(6);
        if (magnitude(entity.y - target.y) > blast_radius) { instructions(2); continue; }
        constexpr unsigned damage = 100;
        instructions(2);
        if (target.attribute7 < damage) {
            instructions(1);
            target.damage_stage |= 1;
        }
        target.attribute7 = static_cast<std::uint8_t>(target.attribute7 - damage);
        instructions(4);
        spawn_explosion(scene, slot, explosion_payload, player, sounds, work);
        instructions(4); // Restore registers, decrement and loop.
    }
    instructions(7);
    if (height_distance(entity.height, player.view.height) > 50) { instructions(1); return; }
    instructions(6);
    if (magnitude(entity.x - player.view.x) > blast_radius) { instructions(1); return; }
    instructions(6);
    if (magnitude(entity.y - player.view.y) > blast_radius) { instructions(1); return; }
    player.armor = -1;
    player.exit_requested = 1;
    player.shake_ticks = hit_shake_ticks;
    instructions(9);
    sounds.push_back({SampleBank::effects, 5});
    instructions(2);
}
}

void FlightController::begin_settings() {
    state_.shake_ticks = presentation_reset_ticks;
    state_.shake_offset = 0;
}

void FlightController::timer_interrupt(const CpuWorkSink& shake_work) {
    auto& scene = interrupt_scene_ ? *interrupt_scene_ : scene_;
    auto& state = interrupt_state_ ? *interrupt_state_ : state_;
    IrqState irq{scene.random_seed, state.shake_ticks, state.shake_offset};
    irq.interrupt(shake_work);
    scene.random_seed = irq.random_seed;
    state.shake_ticks = irq.shake_ticks;
    state.shake_offset = irq.shake_offset;
}

FlightController::FlightController(const GameplaySetup& setup, MissionScene scene, TerrainTables tables, std::size_t payload_count)
    : tables_(std::move(tables)), scene_(std::move(scene)), missile_payload_(payload_count - 11),
      explosion_payload_(payload_count - 8), cannon_payload_(payload_count - 9), rocket_payload_(payload_count - 10),
      terrain_variant_(setup.terrain_variant) {
    if (payload_count < 17) {
        throw std::invalid_argument("Missing mission projectile resources");
    }
    state_.view.x = setup.x;
    state_.view.y = setup.y;
    state_.view.heading = setup.heading;
    state_.view.height = scene_.player_height;
    state_.range = scene_.range;
    state_.ammunition = scene_.vehicle_parameters;
    state_.height_gap = tables_.initial_height_gap;
    state_.system_condition = setup.weapons;
    state_.armor = static_cast<std::int32_t>(setup.ammunition);
    state_.chaff = setup.map_position[0];
    state_.flares = setup.map_position[1];
}

const FlightState& FlightController::state() const {
    return state_;
}

const MissionScene& FlightController::scene() const {
    return scene_;
}

const std::vector<SoundCue>& FlightController::sounds() const {
    return sounds_;
}

void FlightController::draw_effects(ScriptedEffects& effects, TerrainFrame& frame, const SceneView& view,
                                    std::uint32_t tick, EffectLayer layer, ExecutionClock* clock) {
    effects.draw(frame, view, tick, state_.yaw_rate, state_.side_speed, scene_.random_seed, layer, clock);
}

void FlightController::apply_wind(ScriptedEffects& effects, std::uint32_t tick, std::uint32_t camera_heading,
                                  ExecutionClock* clock) {
    effects.wind(tick, state_.view.heading, camera_heading, state_.forward_speed, state_.side_speed, scene_.random_seed, clock);
}

SceneView FlightController::render_view(const SceneAssets& assets) const {
    auto view = state_.view;
    view.vertical_pan = static_cast<std::int32_t>(state_.shake_offset);
    if (state_.cockpit_mode == 7) {
        view = fixed_view_;
        view.vertical_pan = static_cast<std::int32_t>(state_.shake_offset);
        const auto bearing = bearing_to(scene_.entities[player_slot], view.x, view.y, tables_).angle;
        view.heading = std::uint32_t(std::uint8_t(bearing + 128)) << 24;
        return view;
    }
    if (state_.cockpit_mode != 6) {
        return view;
    }
    view.mode = SceneViewMode::chase;
    view.body_roll = view.roll;
    view.roll = 0;
    view.heading -= std::uint32_t(state_.yaw_rate) * 4;
    if (state_.exit_requested != 0) {
        view.heading -= 0xa0000000u;
    }
    const auto angle = view.heading >> 23;
    view.y += multiply_high(0x0c800000, tables_.sine[angle + 128]);
    view.x -= multiply_high(0x0c800000, tables_.sine[angle]);
    if (assets.terrain.height.size() != terrain_side * terrain_side) {
        throw std::invalid_argument("Chase camera requires materialized terrain");
    }
    const auto position = std::size_t(view.y >> 22) * terrain_side + (view.x >> 22);
    const auto ground = static_cast<std::uint8_t>(assets.terrain.height[position] << 1);
    view.height = std::max<unsigned>(view.height, std::min<unsigned>(ground + 12, 255));
    return view;
}

void FlightController::enter_recording(const DemoPose& pose) {
    state_.view.x = pose.x;
    state_.view.y = pose.y;
    state_.view.heading = pose.heading;
    state_.view.height = pose.height;
    state_.view.pitch = state_.view.roll = state_.yaw_rate = 0;
    state_.forward_speed = state_.side_speed = 0;
    state_.target = {};
}

void FlightController::select_weapon(std::uint8_t weapon) {
    if (weapon >= weapon_count) { throw std::invalid_argument("Invalid weapon selection"); }
    state_.weapon = weapon;
    if (state_.cockpit_mode == 1) {
        if ((state_.system_condition[5] & 128) != 0) { state_.ammunition[cannon_weapon] = 0; }
        if ((state_.system_condition[3] & 128) != 0) { state_.ammunition[rocket_weapon] = 0; }
    }
}

bool FlightController::deploy_countermeasure(DefenseAction action, const CpuWorkSink& work) {
    return release_countermeasure(state_, scene_, state_.view, action, cannon_payload_, missile_payload_, 0, work);
}

RecordedAction FlightController::recorded_key(std::uint8_t scan, const SceneAssets& assets) {
    constexpr std::uint8_t previous_weapon = 0x1a, next_weapon = 0x1b;
    if (scan == previous_weapon || scan == next_weapon) {
        select_weapon((state_.weapon + (scan == previous_weapon ? weapon_count - 1 : 1)) % weapon_count);
        return RecordedAction::stores;
    }
    if (scan == 0 || scan == 0x18) {
        // O is a disabled recording-editor command in the retail attract streams.
        return RecordedAction::none;
    }
    if (scan != 2 && scan != 7 && scan != 8) {
        throw std::invalid_argument("Unsupported recorded key");
    }
    const auto mode = std::uint8_t(scan - 1);
    if (mode == state_.cockpit_mode && mode != 7) {
        return RecordedAction::none;
    }
    if (mode == 7) {
        const auto position = std::size_t(state_.view.y >> 22) * terrain_side + (state_.view.x >> 22);
        const auto ground = std::uint8_t(assets.terrain.height.at(position) << 1);
        const auto angle = state_.view.heading >> 23;
        fixed_view_ = {};
        fixed_view_.mode = SceneViewMode::fixed;
        fixed_view_.height = std::min<unsigned>(ground + 10, 255);
        fixed_view_.x = state_.view.x - multiply_high(0x02800000, tables_.sine[angle]);
        fixed_view_.y = state_.view.y + multiply_high(0x02800000, tables_.sine[angle + 128]);
    }
    state_.cockpit_mode = mode;
    return RecordedAction::view;
}

void FlightController::update_entities(const SceneAssets& assets, ExecutionClock* clock, const SoundCueHandler& sound_handler) {
    if (assets.terrain.height.size() != terrain_side * terrain_side) {
        throw std::invalid_argument("Entity updates require materialized terrain");
    }
    auto next = scene_;
    auto next_state = state_;
    InterruptTarget interrupt_target(interrupt_scene_, interrupt_state_, next, next_state);
    const auto instructions = [&](unsigned n) { if (clock) { clock->execute(CpuOperation::instructions, n); } };
    instructions(3);
    next_state.enemy_tracking = false;
    next_state.incoming_hit = false;
    SoundBatch sounds{{}, sound_handler};
    for (std::size_t reverse = scene_slots; reverse > 1; --reverse) {
        auto& entity = next.entities[reverse - 1];
        instructions(2);
        if (!entity.active) {
            instructions(2);
            continue;
        }
        instructions(3);
        if (entity.phase == EntityPhase::explosion) {
            instructions(3);
            entity.animation_frame = static_cast<std::uint8_t>(entity.animation_frame + 1);
            if (entity.animation_frame >= explosion_end_frame) {
                instructions(3);
                entity.active = false;
                if (reverse - 1 <= object_slots) {
                    instructions(2);
                    if (entity.target != 0) {
                        instructions(2);
                        if (next.remaining_targets != 0) {
                            instructions(2);
                            --next.remaining_targets;
                            if (next.remaining_targets == 0) {
                                instructions(2);
                                if (next_state.exit_requested == 0) {
                                    instructions(3);
                                    next_state.exit_requested = 1;
                                    sounds.push_back({SampleBank::speech, 7});
                                }
                            }
                        }
                    }
                }
            }
            instructions(1);
        } else if (entity.kind == 0) {
            instructions(14 + unsigned(state_.view.height == 255));
            entity.rotor_phase = static_cast<std::uint8_t>(entity.rotor_phase - 16);
            // DOS rotor[61] aliases the first object's firing mask.
            next.entities[1].attribute9 = entity.rotor_phase;
            entity.x = state_.view.x;
            entity.y = state_.view.y;
            entity.height = static_cast<std::uint8_t>(std::min<unsigned>(state_.view.height + 1, 255));
            entity.angle = static_cast<std::uint8_t>((state_.view.heading + 0x00800000u) >> 24);
        } else if (entity.kind == 2 || entity.kind == 5 || entity.kind == 9 || entity.kind == 15) {
            update_ground(entity, reverse - 1, next, next_state, assets, tables_, explosion_payload_ + 8, sounds, clock);
        } else if (entity.kind == 1 || entity.kind == 6 || entity.kind == 7 || entity.kind == 8) {
            update_aircraft(entity, reverse - 1, next, next_state, assets, tables_, explosion_payload_ + 8, sounds, clock);
        } else if (entity.kind == 4 || entity.kind == 11) {
            update_friendly_aircraft(entity, reverse - 1, next, next_state, assets, tables_, movement_basis_,
                                     explosion_payload_ + 8, sounds, clock);
        } else if (entity.kind == 10 || (entity.kind >= 12 && entity.kind <= 17)) {
            update_support_vehicle(entity, reverse - 1, next, next_state, assets, tables_, explosion_payload_ + 8, sounds, clock);
        } else if (entity.kind == stinger_kind || entity.kind == enemy_rocket_kind ||
                   entity.kind == climbing_missile_kind || entity.kind == enemy_cannon_kind) {
            update_enemy_missile(entity, next, next_state, assets, tables_, explosion_payload_, sounds, clock);
        } else if (entity.kind == artillery_kind) {
            update_artillery(entity, next, next_state, explosion_payload_, sounds,
                clock ? CpuWorkSink([clock](CpuOperation operation, unsigned count) { clock->execute(operation, count); }) : CpuWorkSink{});
        } else if (entity.kind == flare_kind || entity.kind == chaff_kind) {
            instructions(2);
            if (entity.height != 1) { instructions(2); }
            if (entity.height == 1 || entity.random_phase == 1) {
                instructions(3);
            } else {
                instructions(entity.kind == chaff_kind ? 4 +
                    unsigned(static_cast<std::uint8_t>(entity.animation_frame + 1) > chaff_animation_frames) : 1);
            }
            if (--entity.height == 0 || --entity.random_phase == 0) {
                entity.active = false;
                auto& active = entity.kind == flare_kind ? next_state.active_flares : next_state.active_chaff;
                --active;
            } else if (entity.kind == chaff_kind && ++entity.animation_frame > chaff_animation_frames) {
                entity.animation_frame = 1;
            }
        } else if (entity.kind == rocket_kind || entity.kind == cannon_round_kind) {
            update_ballistic(entity, next, next_state, assets, tables_, explosion_payload_, sounds, clock);
        } else if (entity.kind == guided_missile_kind) {
            update_guided_missile(entity, next, next_state, assets, tables_, explosion_payload_, sounds, clock);
        } else if (entity.kind == 3) {
            const auto work = clock ? CpuWorkSink([clock](CpuOperation operation, unsigned count) {
                clock->execute(operation, count);
            }) : CpuWorkSink{};
            instructions(2); // 254b0 damage-stage gate.
            if (entity.damage_stage != 0) {
                const auto stage = entity.damage_stage;
                instructions(5);
                entity.damage_stage = static_cast<std::uint8_t>(stage + 1);
                if (stage == 1) {
                    const auto allocated = spawn_explosion(next, reverse - 1, explosion_payload_, next_state, sounds, work);
                    // A full 23aeb allocation leaves ECX at zero until the caller's POP.
                    const auto color = allocated ? entity.category_color : next.entities[0].category_color;
                    instructions(2);
                    if (color != 0) {
                        instructions(2);
                        if (next_state.armor >= 0) {
                            sounds.push_back({SampleBank::speech, 1});
                        } else {
                            instructions(6); // 501d rejects speech 1 after the player has crashed.
                        }
                    }
                    instructions(2);
                } else if (entity.damage_stage == 5) {
                    begin_explosion(entity, explosion_payload_, next_state, sounds, work);
                } else {
                    instructions(1);
                }
            } else {
                instructions(1);
            }
        } else {
            throw std::invalid_argument("Entity simulation policy has not been recovered for this kind");
        }
        auto seed = next.random_seed;
        if (clock) { clock->instruction([&] { seed = next.random_seed; }); }
        const auto negative = 0u - seed;
        const auto shifted = (negative >> 1) | (negative & 0x80000000u);
        const auto advanced = shifted ^ ((shifted & 0x80000000u) ? 0xbc9abb09u : 0);
        instructions(5);
        if (clock) { clock->instruction([&] { next.random_seed = advanced; }); }
        else { next.random_seed = advanced; }
        instructions(3);
    }
    instructions(3);
    if (next.camera_subject != 0 && next.entities.at(next.camera_subject).phase == EntityPhase::explosion) {
        instructions(2);
        next.camera_subject = 0;
    }
    scene_ = std::move(next);
    state_ = next_state;
    sounds_ = std::move(sounds.values);
}

std::vector<std::uint16_t> FlightController::step(const FlightControls& controls, const SceneAssets& assets,
    const std::function<void(FlightController&)>& view_transition, const TargetingFrame* targeting,
    ThreatProximity proximity, const CpuWorkSink& work, ExecutionClock* clock, const SoundCueHandler& sound_handler,
    const FlightStatusUpdates& status_updates) {
    if (state_.ended) {
        sounds_.clear();
        return {};
    }
    const auto recorded = controls.source == ControlSource::recording;
    const auto control_limit = recorded ? 32768 : keyboard_control;
    if (controls.horizontal < -control_limit || controls.horizontal >= control_limit + (recorded ? 0 : 1) ||
            controls.vertical < -control_limit || controls.vertical >= control_limit + (recorded ? 0 : 1) || controls.throttle > 15 ||
            assets.terrain.height.size() != terrain_side * terrain_side) {
        throw std::invalid_argument("Unsupported normal-flight controls or terrain");
    }
    auto next = state_;
    auto next_scene = scene_;
    InterruptTarget interrupt_target(interrupt_scene_, interrupt_state_, next_scene, next);
    auto next_launch_offset = launch_offset_;
    auto next_trigger_held = recorded ? false : trigger_held_;
    auto next_target_held = recorded ? false : target_held_;
    auto previous_height = next.view.height;
    auto horizontal = controls.horizontal;
    auto vertical_control = controls.vertical;
    SoundBatch cues{{}, sound_handler};
    const auto direction_keys = static_cast<std::uint8_t>((controls.horizontal < 0 ? 16 : 0) |
        (controls.horizontal > 0 ? 32 : 0) | (controls.vertical < 0 ? 64 : 0) | (controls.vertical > 0 ? 128 : 0));
    flight_work::polling(next, controls.source, direction_keys, [&] { return next_scene.random_seed; }, work);
    next.previous_power = next.control_power;
    next.control_power = recorded ? controls.recorded_power : static_cast<std::uint8_t>(controls.throttle * 16 + 5);
    if (static_cast<std::int32_t>(next.range) < 0) {
        if (next.range == fuel_exit_threshold) {
            finish_flight(next);
            state_ = next;
            sounds_.clear();
            return {};
        }
        next.control_power = next.previous_power == 0 ? 0 : next.previous_power - 1;
    }
    --next.range;
    if (next.exit_requested != 0) {
        next.exit_countdown = static_cast<std::uint8_t>(next.exit_countdown - next.exit_requested);
        if (next.exit_countdown == 0) {
            finish_flight(next);
            state_ = next;
            sounds_.clear();
            return {};
        }
        horizontal = vertical_control = 0;
        if (next.armor < 0) {
            next.control_power = 0;
            next.shake_ticks = hit_shake_ticks;
            if (next.cockpit_mode == 1) {
                next.crash_rotation += crash_rotation_step;
                if (next.crash_rotation >= crash_rotation_limit) {
                    next.cockpit_mode = 6;
                    next.crash_rotation = 0;
                    proximity = ThreatProximity::clear; // 1a311 clears radar before the nested view returns.
                    if (view_transition) {
                        auto nested = *this;
                        nested.interrupt_scene_ = nullptr;
                        nested.interrupt_state_ = nullptr;
                        nested.state_ = next;
                        nested.scene_ = next_scene;
                        view_transition(nested);
                        next = nested.state_;
                        next_scene = std::move(nested.scene_);
                        next_launch_offset = nested.launch_offset_;
                        next_trigger_held = nested.trigger_held_;
                        next_target_held = nested.target_held_;
                        previous_height = next.view.height;
                    }
                    cues.push_back({SampleBank::speech, 11});
                }
            }
            if ((next_scene.random_seed & 15) == 0) {
                spawn_explosion(next_scene, player_slot, explosion_payload_, next, cues, work);
            }
        } else {
            next.control_power = 254;
            if (next.view.height <= 250) {
                next.view.height = static_cast<std::uint8_t>(next.view.height + 4);
            }
            if (next.view.height >= 230) {
                vertical_control = -keyboard_control;
            }
        }
    }
    flight_work::dynamics(next, policy_, horizontal, vertical_control, work);
    const auto vertical = vertical_control >> 5;
    const auto power = unsigned(vertical ^ (vertical >> 31)) + unsigned(next.control_power) * 3;
    next.engine_power = static_cast<std::uint8_t>(std::min(power, 200u));
    const auto damaged_engine = (next.system_condition[1] & 128) != 0;
    if (damaged_engine) {
        next.forward_limit = 0x00800000;
        next.engine_power >>= 1;
    }
    const unsigned damping = next.control_power <= 16 ? 3 : 5;
    next.forward_speed = damp(next.forward_speed, damping);
    next.side_speed = damp(next.side_speed, damping);
    next.yaw_rate = damp(next.yaw_rate, damping - 1);
    next.view.pitch = damp(next.view.pitch, damping - 1);
    next.view.roll = damp(next.view.roll, damping - 2);
    if ((next.system_condition[0] & 128) != 0 && next.armor >= 0) {
        next.yaw_rate += 0x00600000;
    }
    if (policy_.rotor == RotorControl::mixed) {
        next.yaw_rate += horizontal * 0x4000;
    }
    next.yaw_rate = std::clamp(next.yaw_rate, -yaw_limit, yaw_limit);
    const auto lateral_limit = policy_.rotor == RotorControl::mixed ? side_limit : side_limit * 2;
    next.view.pitch = std::clamp(next.view.pitch + ((horizontal + 8) >> 4), -133, 133);
    next.side_speed = std::clamp(next.side_speed + horizontal * 0x800, -lateral_limit, lateral_limit);
    next.view.roll = std::clamp(next.view.roll + ((16 - vertical_control) >> 5), -33, 33);
    const auto speed_limit = next.forward_limit;
    next.forward_speed = std::clamp(next.forward_speed - vertical_control * 0x800, -(speed_limit >> 1), speed_limit);

    const auto refresh_stores = [&] {
        if (next.cockpit_mode == 1) {
            if ((next.system_condition[5] & 128) != 0) {
                next.ammunition[cannon_weapon] = 0;
            }
            if ((next.system_condition[3] & 128) != 0) {
                next.ammunition[rocket_weapon] = 0;
            }
        }
    };
    if (controls.weapon != WeaponSelection::unchanged) {
        switch (controls.weapon) {
        case WeaponSelection::previous: next.weapon = (next.weapon + weapon_count - 1) % weapon_count; break;
        case WeaponSelection::next: next.weapon = (next.weapon + 1) % weapon_count; break;
        case WeaponSelection::cannon: case WeaponSelection::rockets: case WeaponSelection::hellfire: case WeaponSelection::stinger:
        case WeaponSelection::artillery: case WeaponSelection::wingman:
            next.weapon = static_cast<std::uint8_t>(controls.weapon);
            break;
        default: throw std::invalid_argument("Invalid weapon selection");
        }
        refresh_stores();
    }
    std::vector<std::uint16_t> sounds;
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto cannon = next.weapon == cannon_weapon;
    const auto rocket = next.weapon == rocket_weapon;
    const auto stinger = next.weapon == static_cast<std::uint8_t>(WeaponSelection::stinger);
    const auto guided_launcher = stinger || next.weapon == missile_weapon;
    const auto wingman_weapon = next.weapon == static_cast<std::uint8_t>(WeaponSelection::wingman);
    const auto artillery_weapon = next.weapon == static_cast<std::uint8_t>(WeaponSelection::artillery);
    next.firing = controls.fire && (cannon || !next_trigger_held);
    instructions(3); // 19d12 firing flag and weapon gate.
    instructions(cannon ? (next.firing ? 3 : 8) : (next.firing ? 2 : 6));
    if (next.firing) { instructions(6); } // Callback setup and CALL. Its body owns its work.
    if (guided_launcher && next.firing) {
        instructions(next.ammunition[next.weapon] == 0 ? 3 : 4); // 24320/24450 ammunition and allocator gates.
    }
    if (artillery_weapon && next.firing) {
        instructions(2);
        if (next.ammunition[next.weapon] == 0) {
            instructions(1);
        } else {
            instructions(2);
            instructions(next.target.slot == 0 ? 1 : 2);
        }
    }
    std::size_t missile_slot = 0;
    SceneEntity missile;
    if (next.firing && wingman_weapon) {
        const auto stores = [&](const FlightState&) {
            refresh_stores();
            if (status_updates.stores) { status_updates.stores(next); }
        };
        if (launch_wingman(next_scene, next, missile_payload_, cues, stores, work)) {
            sounds.push_back(missile_sound);
        }
    }
    if (next.firing && (cannon || rocket)) {
        instructions(2); // 23bb0/23fb0 ammunition gate.
        if (next.ammunition[next.weapon] != 0) { instructions(2); }
        const auto damaged = (next.system_condition[cannon ? 5 : 3] & 128) != 0;
        if (damaged || next.ammunition[next.weapon] == 0) {
            instructions(3);
            next.ammunition[next.weapon] = 0;
            refresh_stores();
            if (status_updates.stores) { status_updates.stores(next); }
            instructions(1);
        } else {
            instructions(2); // Allocator setup.
        }
    }
    if (next.firing && !wingman_weapon && next.ammunition[next.weapon] != 0 &&
            (next.weapon != unsigned(WeaponSelection::artillery) || next.target.slot != 0)) {
        for (std::size_t slot = scene_slots - 1; slot >= player_slot; --slot) {
            if (guided_launcher || artillery_weapon || cannon || rocket) { instructions(next_scene.entities[slot].active ? 4 : 2); }
            if (!next_scene.entities[slot].active) {
                missile_slot = slot;
                missile = next_scene.entities[slot];
                break;
            }
        }
        if (missile_slot == 0 && (guided_launcher || artillery_weapon || cannon || rocket)) { instructions(1); }
        if (missile_slot != 0) {
            if (next.weapon == missile_weapon || next.weapon == unsigned(WeaponSelection::stinger)) {
                next_scene.camera_subject = static_cast<std::uint32_t>(missile_slot);
            }
            const auto cost = cannon ? 2u : 1u;
            const auto ammunition_before = next.ammunition[next.weapon];
            next.ammunition[next.weapon] -= std::min(next.ammunition[next.weapon], cost);
            if (!artillery_weapon) { refresh_stores(); }
            if (guided_launcher || cannon || rocket) {
                if (guided_launcher) { instructions(8); } // 24340/24470 through the shared stores CALL.
                if (cannon) { instructions(6 + unsigned(ammunition_before < 2)); }
                if (rocket) { instructions(7); }
                if (status_updates.stores) { status_updates.stores(next); }
                instructions(5); // Sound arguments and CALL 5094.
            }
            if (artillery_weapon) {
                instructions(5); // 246c2..246cd selects the slot, decrements ammunition and calls 5015.
                cues.push_back({SampleBank::announcements, 1});
                instructions(2);
                refresh_stores();
                if (status_updates.stores) { status_updates.stores(next); }
                missile.active = true;
                missile.phase = EntityPhase::normal;
                missile.kind = artillery_kind;
                missile.random_phase = 0;
                missile.category_color = 0;
                missile.primary_payload = missile_payload_;
                const auto& target = next_scene.entities.at(next.target.slot);
                missile.x = target.x;
                missile.y = target.y;
                const auto height = unsigned(terrain_height(missile, assets)) + 11;
                missile.height = static_cast<std::uint8_t>(std::min<unsigned>(height, 255));
                instructions(25 + unsigned(height > 255)); // 246de..24766 publishes the terrain-relative strike.
            } else {
                const auto sound = static_cast<std::uint16_t>(cannon ? 7 : missile_sound);
                sounds.push_back(sound);
                cues.push_back({SampleBank::effects, sound, 0,
                               cannon ? EffectPolicy::restart_matching : EffectPolicy::available_first,
                               rocket ? 0x20000u : 0u});
                missile.active = true;
                missile.phase = EntityPhase::normal;
                if (guided_launcher) { instructions(36); } // 24374/244a4 launch pose through RET.
                if (cannon) { instructions(30 + 15 * unsigned(next.target.slot != 0)); }
                if (rocket) { instructions(43 + 13 * unsigned(next.target.slot != 0)); }
                missile.kind = cannon ? cannon_round_kind : rocket ? rocket_kind : stinger ? stinger_kind : guided_missile_kind;
                if (stinger) {
                    missile.homing_target = static_cast<std::uint8_t>(next.target.slot);
                }
                if (!rocket) {
                    missile.random_phase = cannon ? 1 : 0;
                }
                missile.category_color = 0;
                missile.primary_payload = cannon ? cannon_payload_ : rocket ? rocket_payload_ : stinger ? missile_payload_ - 1 : missile_payload_;
                missile.speed = cannon ? 0x08000000 : 0x04000000;
                missile.angle = static_cast<std::uint8_t>((next.view.heading + (rocket ? 0x00800000u : 0u)) >> 24);
                missile.height = next.view.height;
                missile.x = next.view.x;
                missile.y = next.view.y;
                if (!cannon) {
                    const auto angle = next.view.heading >> 23;
                    const auto sine = tables_.sine[angle];
                    const auto cosine = tables_.sine[angle + 128];
                    const auto rear_offset = rocket ? 0x01000000 : 0x03000000;
                    missile.x += multiply_high(-next_launch_offset, cosine) - multiply_high(rear_offset, sine);
                    missile.y += multiply_high(-next_launch_offset, sine) + multiply_high(rear_offset, cosine);
                    next_launch_offset = -next_launch_offset;
                }
                if (cannon || rocket) {
                    if (cannon) {
                        missile.animation_frame = 1;
                    }
                    missile.ballistic_height = std::uint32_t(missile.height) << 24;
                    missile.vertical_velocity = static_cast<std::int32_t>((0u - std::uint32_t(next.view.roll)) << 22);
                    if (next.target.slot != 0) {
                        const auto& projection = targeting == nullptr ? next.target.projection : targeting->entities.at(next.target.slot);
                        const auto& target = next_scene.entities.at(next.target.slot);
                        if (cannon) {
                            missile.angle = projection.bearing;
                        }
                        const auto depth = static_cast<std::uint32_t>(projection.depth);
                        const auto divisor = (depth >> (cannon ? 25 : 24)) + 1;
                        const auto delta = static_cast<std::int32_t>(std::uint32_t(int(target.height) - int(missile.height)) << 23);
                        missile.vertical_velocity = static_cast<std::int32_t>(std::uint32_t(delta / static_cast<std::int32_t>(divisor)) +
                                                                              (depth >> (cannon ? 5 : 4)));
                    }
                }
            }
        }
    }

    instructions(3); // 19d8c pending target-key test.
    const auto cycle_target = targeting != nullptr && controls.cycle_target && !next_target_held;
    if (!cycle_target) { instructions(2); }
    if (cycle_target) {
        instructions(2);
        if ((next.system_condition[2] & 128) == 0) {
            instructions(2);
        }
    }
    if (cycle_target && (next.system_condition[2] & 128) == 0 && next.cockpit_mode <= 5) {
        instructions(4);
        if (next.target.slot == 0) {
            instructions(4);
            next.target.slot = targeting->candidate;
        } else {
            instructions(1);
            const auto previous = next.target.slot;
            do {
                instructions(3 + unsigned(next.target.slot == object_slots));
                next.target.slot = next.target.slot % object_slots + 1;
                instructions(2);
                if (next.target.slot == previous) { break; }
                instructions(2);
                if (targeting->entities.at(next.target.slot).x != 0) { break; }
                instructions(1);
            } while (true);
            if (next.target.slot == previous) {
                instructions(1);
                next.target.slot = 0;
            }
            instructions(2);
        }
        next_scene.camera_subject = 0;
    }
    instructions(6); // Released yaw keys and the damaged-targeting gate at 19e43.
    if ((next.system_condition[4] & 128) != 0) {
        auto seed = next_scene.random_seed;
        if (clock) { clock->instruction([&] { seed = next_scene.random_seed; }); }
        else { instructions(1); seed = next_scene.random_seed; }
        instructions(1);
        if ((seed & 0x1c) == 0) {
            instructions(1);
            next.target.slot = 0;
        }
    }

    bool contact = scan_player_contacts(next, next_scene, work);
    const auto position = std::size_t(next.view.y >> 22) * terrain_side + (next.view.x >> 22);
    const auto ground = static_cast<std::uint8_t>((assets.terrain.height[position] << 1) + 1);
    if (!contact && work) {
        const auto color = position < assets.terrain.color.size() ? assets.terrain.color[position] : 0;
        flight_work::ground_response(next, policy_, ground, color, previous_height, terrain_variant_, work);
    }
    if (!contact && next.view.height <= ground) {
        const auto overlap = unsigned(ground) - next.view.height;
        if (overlap > 2) {
            contact = true;
        } else {
            next.view.height = static_cast<std::uint8_t>(next.view.height + overlap + 1);
            previous_height = next.view.height;
        }
    } else if (!contact) {
        const auto gap = static_cast<std::uint8_t>(next.view.height - ground);
        next.previous_height_gap = next.height_gap;
        if (gap < 7 && next.control_power <= 6) {
            next.yaw_rate = next.view.pitch = next.view.roll = 0;
            next.forward_speed = next.side_speed = 0;
            next.height_gap = gap;
            if (terrain_variant_ != 0) {
                if (assets.terrain.color.size() != terrain_side * terrain_side) {
                    throw std::invalid_argument("Water contact requires materialized terrain colors");
                }
                if (assets.terrain.color[position] >= 251) {
                    if (previous_height != 0) {
                        --previous_height;
                        next.view.height = previous_height;
                    }
                    contact = previous_height <= 3;
                }
            }
        } else {
            if (gap < 7 && next.view.height != 255) {
                ++next.view.height;
            }
            const auto altitude = altitude_response(policy_.altitude, next.control_power, next.forward_speed,
                                                    next.view.pitch, next.view.roll);
            const auto average = (unsigned(gap) + next.height_gap + 1) >> 1;
            next.height_gap = gap;
            if (average < altitude.power) {
                const auto rise = ((altitude.power - average) >> 3) + 2;
                next.view.height = static_cast<std::uint8_t>(std::min<unsigned>(next.view.height + rise, 255));
            } else if (average > altitude.power) {
                const auto fall = ((average - altitude.power) >> altitude.descent_shift) + 3;
                next.view.height = static_cast<std::uint8_t>(next.view.height < fall ? 0 : next.view.height - fall);
            }
        }
    }
    if (contact) {
        respond_to_contact(next, next_scene, state_.view, previous_height, cues.values, work, clock, sound_handler);
    }
    const auto average_height = static_cast<std::uint8_t>((unsigned(next.view.height) + previous_height) >> 1);
    flight_work::motion(policy_, {average_height, next.active_chaff, next.active_flares, proximity,
                                 next.enemy_tracking, next.incoming_hit}, work);
    next.view.height = average_height;
    next.view.heading += next.yaw_rate;
    const auto angle = next.view.heading >> 23;
    const auto sine = tables_.sine[angle];
    const auto cosine = tables_.sine[angle + 128];
    movement_basis_ = {sine, cosine};
    next.view.y -= multiply_high(next.forward_speed, cosine);
    next.view.x += multiply_high(next.forward_speed, sine);
    next.view.x += multiply_high(next.side_speed, cosine);
    next.view.y += multiply_high(next.side_speed, sine);
    const auto deploy = [&](std::uint8_t kind) {
        auto pose = state_.view;
        // 23f3c reads the height backup updated by shallow terrain and water response.
        pose.height = previous_height;
        const auto deployed = release_countermeasure(next, next_scene, pose,
            kind == flare_kind ? DefenseAction::flare : DefenseAction::chaff,
            cannon_payload_, missile_payload_, missile_slot, work);
        if (deployed && status_updates.countermeasures) { status_updates.countermeasures(next); }
    };
    const auto defense = automatic_defense(policy_, {next.view.height, next.active_chaff, next.active_flares,
                                                   proximity, next.enemy_tracking, next.incoming_hit});
    if (defense == DefenseAction::chaff) {
        deploy(chaff_kind);
    } else if (defense == DefenseAction::flare) {
        deploy(flare_kind);
    }
    if (controls.chaff && !chaff_held_) {
        deploy(chaff_kind);
    }
    if (controls.flare && !flare_held_) {
        deploy(flare_kind);
    }
    state_ = next;
    trigger_held_ = controls.fire;
    target_held_ = controls.cycle_target;
    chaff_held_ = controls.chaff;
    flare_held_ = controls.flare;
    launch_offset_ = next_launch_offset;
    if (missile_slot != 0) {
        next_scene.entities[missile_slot] = missile;
    }
    scene_ = std::move(next_scene);
    sounds_ = std::move(cues.values);
    return sounds;
}

void FlightController::refresh_target(const TargetingFrame& targeting) {
    observe_projection(targeting);
    if (state_.cockpit_mode > 5 || state_.target.slot == 0) {
        return;
    }
    state_.target.projection = targeting.entities.at(state_.target.slot);
    if (state_.target.projection.x == 0) {
        state_.target.slot = 0;
    }
}

void FlightController::observe_projection(const TargetingFrame& targeting) {
    for (std::size_t slot = 1; slot < scene_slots; ++slot) {
        scene_.entities[slot].projected_x = targeting.entities[slot].x;
        scene_.entities[slot].projected_depth = targeting.entities[slot].depth;
    }
}

void FlightController::abort() {
    finish_flight(state_);
    sounds_.clear();
}

void FlightController::set_policy(FlightPolicy policy) {
    policy_ = policy;
    scene_.extended_enemy_range = policy.enemy_range == EnemyRange::extended;
}
} // namespace comanchecd
