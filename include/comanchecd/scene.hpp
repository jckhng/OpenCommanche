#pragma once

#include "comanchecd/gameplay.hpp"
#include <limits>

namespace comanchecd {
class ExecutionClock;
struct IrqState;
constexpr std::size_t terrain_side = 1024;
constexpr std::size_t scene_slots = 141;
constexpr std::size_t player_slot = 61;
constexpr std::size_t missing_model = 15;
constexpr std::uint8_t guided_missile_kind = 18;
constexpr std::uint8_t rocket_kind = 19;
constexpr std::uint8_t cannon_round_kind = 20;
constexpr std::uint8_t stinger_kind = 21;
constexpr std::uint8_t enemy_rocket_kind = 22;
constexpr std::uint8_t climbing_missile_kind = 23;
constexpr std::uint8_t enemy_cannon_kind = 24;
constexpr std::uint8_t artillery_kind = 25;
constexpr std::uint8_t flare_kind = 26;
constexpr std::uint8_t chaff_kind = 27;

struct MaterializedModel {
    std::size_t payload;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> metadata;
};

struct Terrain {
    std::vector<std::uint8_t> height;
    std::vector<std::uint8_t> color;
    Palette palette;
};

struct SceneAssets {
    Terrain terrain;
    std::vector<MaterializedModel> models;
    std::array<std::size_t, 15> model_payloads;
};

std::vector<std::uint8_t> expand_terrain(const GameplayPayload& picture);
SceneAssets materialize_scene(const GameplaySetup& setup, const std::vector<GameplayPayload>& payloads,
                              const Transfer& transfer);

enum class EntityPhase { normal, explosion };

struct EntityEntry {
    std::uint32_t previous_x = 0, previous_y = 0;
    std::uint8_t previous_height = 0, animation_frame = 0;
    std::uint8_t rotor_phase = 0;
};

struct SceneEntry {
    std::array<EntityEntry, scene_slots> entities{};
};

struct SceneEntity {
    bool active = false;
    EntityPhase phase = EntityPhase::normal;
    std::uint8_t kind = 0;
    std::size_t primary_model = missing_model;
    std::size_t secondary_model = missing_model;
    std::size_t primary_payload = std::numeric_limits<std::size_t>::max();
    std::uint32_t x = 0, y = 0, speed = 0, attribute10 = 0;
    std::uint8_t angle = 0, height = 0, target = 0, attribute7 = 0, attribute9 = 0;
    std::uint8_t category_color = 0, random_phase = 0;
    std::uint32_t previous_x = 0, previous_y = 0;
    std::uint8_t previous_height = 0;
    std::uint8_t damage_stage = 0, animation_frame = 0;
    std::uint8_t rotor_phase = 0;
    std::uint32_t ballistic_height = 0;
    std::int32_t vertical_velocity = 0;
    std::uint8_t homing_target = 0, avoidance_turn = 0, projected_x = 0;
    std::int32_t projected_depth = 0;
};

struct MissionScene {
    std::array<SceneEntity, scene_slots> entities{};
    std::array<std::uint32_t, 6> vehicle_parameters{};
    std::uint32_t range = 0;
    std::uint32_t random_seed = 0;
    std::uint32_t wingman = 0;
    std::uint32_t camera_subject = 0;
    std::uint8_t player_height = 0;
    std::uint8_t remaining_targets = 0;
    std::uint8_t preset = 0, wingman_alert = 0;
    bool attack_allies = false, extended_enemy_range = false, wingman_lost = false;
};

MissionScene construct_scene(const GameplaySetup& setup, const SceneAssets& assets, std::uint32_t random_seed,
                             const SceneEntry& entry = {}, IrqState* live = nullptr, ExecutionClock* clock = nullptr);
} // namespace comanchecd
