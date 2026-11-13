#include "comanchecd/debrief.hpp"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::uint32_t objective_score = 100;
constexpr std::uint32_t completion_bonus = 1500;
constexpr std::uint32_t enemy_score = 32;
constexpr std::uint32_t armor_multiplier = 16;
constexpr std::uint32_t wingman_penalty = 1000;

std::uint32_t percentage(std::uint32_t remaining, std::uint32_t initial) {
    return initial == 0 ? 100 : (remaining * 100) / initial;
}

std::uint32_t weapons(const std::array<std::uint32_t, 6>& counts) {
    return (counts[0] >> 7) + (counts[1] >> 3) + counts[2] + counts[3] + counts[4] + counts[5];
}

void statistics_work(const GameplaySetup& initial, const FlightState& player, const MissionScene& scene,
                      const MissionResult& result, const CpuWorkSink& work) {
    if (!work) { return; }
    // 2b69d scans slots 60..1, formats five percentages, then copies the message.
    const auto armor = static_cast<std::int32_t>(std::uint32_t(player.armor) + 1);
    unsigned instructions = 11 + unsigned(static_cast<std::int32_t>(player.range) < 0) + unsigned(armor < 0);
    unsigned goals = 0, destroyed = 0, enemies = 0;
    for (std::size_t slot = player_slot - 1; slot != 0; --slot) {
        const auto& entity = scene.entities[slot];
        instructions += 5;
        if (entity.active) {
            for (const auto color : {9, 6, 3, 0}) {
                instructions += 2;
                if (entity.category_color == color) { break; }
            }
            if (entity.category_color != 9 && entity.category_color != 6 &&
                    entity.category_color != 3 && entity.category_color != 0) {
                ++instructions;
                ++enemies;
            }
        }
        if (entity.target != 0) {
            ++goals;
            instructions += 3;
            if (!entity.active) { instructions += 2; ++destroyed; }
        }
    }
    instructions += 6 + unsigned(result.completed);
    constexpr std::array<unsigned, 5> percentage_order{1, 0, 2, 3, 4};
    constexpr std::array<unsigned, 5> percentage_arguments{4, 4, 12, 30, 4};
    const std::array<std::uint32_t, 5> denominators{
        initial.ammunition + 1, goals, initial.category_count, weapons(initial.vehicle_parameters), initial.range};
    for (std::size_t call = 0; call < percentage_order.size(); ++call) {
        const auto row = percentage_order[call];
        const auto value = static_cast<std::uint8_t>(result.percentages[row]);
        instructions += percentage_arguments[call] + 15 + (denominators[row] != 0 ? 3 : 0) +
            (value >= 10 ? 6 : 0) + (value >= 100 ? 2 : 0);
    }
    instructions += 4;
    if (!result.survived) {
        instructions += 5 + unsigned(enemies <= 3);
        if (player.view.height <= 1) { instructions += 2 + unsigned(initial.terrain_variant != 0); }
    }
    instructions += 2;
    if (result.survived) {
        instructions += 5 + unsigned(static_cast<std::int32_t>(player.range) <= 0);
        if (result.completed) {
            instructions += 3;
            if (scene.wingman != 0 && (scene.wingman_lost || !scene.entities.at(scene.wingman).active)) {
                auto score = destroyed * objective_score + completion_bonus + result.percentages[0] * armor_multiplier +
                    result.enemies_destroyed * enemy_score;
                for (const auto value : result.percentages) { score += value; }
                instructions += 3 + unsigned(score < wingman_penalty);
            }
        }
    }
    work(CpuOperation::instructions, instructions);
    constexpr unsigned message_bytes = 47;
    work(CpuOperation::repeat_copy_bytes, message_bytes);
    // 2c0c7 writes at most eight decimal digits, including the zero score.
    instructions = 14 + unsigned(!result.survived);
    auto score = result.score;
    for (unsigned digit = 0; digit < 8; ++digit) {
        instructions += 6;
        score /= 10;
        if (score == 0) { break; }
        instructions += 2;
    }
    work(CpuOperation::instructions, instructions);
}
}

MissionResult evaluate_mission(const GameplaySetup& initial, const FlightState& player, const MissionScene& scene,
                               const CpuWorkSink& work) {
    MissionResult result;
    const auto armor = static_cast<std::int32_t>(std::uint32_t(player.armor) + 1);
    const auto surviving_armor = static_cast<std::uint32_t>(std::max(armor, 0));
    const auto fuel = static_cast<std::uint32_t>(std::max(static_cast<std::int32_t>(player.range), 0));
    unsigned goals = 0, destroyed = 0, enemies = 0;
    for (std::size_t slot = 1; slot < player_slot; ++slot) {
        const auto& entity = scene.entities[slot];
        if (entity.active && entity.category_color != 0 && entity.category_color != 3 &&
                entity.category_color != 6 && entity.category_color != 9) {
            ++enemies;
        }
        if (entity.target != 0) {
            ++goals;
            destroyed += !entity.active;
        }
    }
    result.survived = surviving_armor != 0;
    result.completed = goals == destroyed;
    result.enemies_destroyed = std::uint32_t(initial.category_count) - enemies;
    result.percentages = {percentage(surviving_armor, initial.ammunition + 1), percentage(destroyed, goals),
        percentage(result.enemies_destroyed, initial.category_count),
        percentage(weapons(player.ammunition), weapons(initial.vehicle_parameters)), percentage(fuel, initial.range)};
    result.score = destroyed * objective_score + (result.completed ? completion_bonus : 0) +
        result.percentages[0] * armor_multiplier + result.enemies_destroyed * enemy_score;
    for (const auto value : result.percentages) {
        result.score += value;
    }
    result.message = result.completed ? DebriefMessage::success : fuel == 0 ? DebriefMessage::fuel : DebriefMessage::aborted;
    if (result.completed && scene.wingman != 0 && (scene.wingman_lost || !scene.entities.at(scene.wingman).active)) {
        result.message = DebriefMessage::wingman;
        result.score = result.score < wingman_penalty ? 0 : result.score - wingman_penalty;
    }
    if (!result.survived) {
        result.score = 0;
        result.message = enemies <= 3 ? DebriefMessage::wrecked : DebriefMessage::defeated;
        if (player.view.height <= 1 && initial.terrain_variant != 0) {
            result.message = DebriefMessage::sunk;
        }
    }
    statistics_work(initial, player, scene, result, work);
    return result;
}

std::vector<std::uint8_t> debrief_text(const DebriefAssets& assets, const MissionResult& result) {
    constexpr std::size_t template_bytes = 255;
    constexpr std::size_t score_offset = 17;
    constexpr std::size_t score_digits = 8;
    constexpr std::size_t message_offset = 167;
    constexpr std::size_t message_bytes = 47;
    constexpr std::array<std::size_t, 5> percentage_offsets{31, 57, 89, 115, 141};
    if (assets.text.size() != template_bytes || assets.messages.at(unsigned(result.message)).size() != message_bytes) {
        throw std::invalid_argument("Invalid original debrief text resources");
    }
    auto text = assets.text;
    std::fill_n(text.begin() + score_offset, score_digits, 0xfc);
    auto score = result.score;
    for (std::size_t digit = 0; digit < score_digits; ++digit) {
        text[score_offset + score_digits - 1 - digit] = static_cast<std::uint8_t>('0' + score % 10);
        score /= 10;
        if (score == 0) {
            break;
        }
    }
    for (std::size_t row = 0; row < percentage_offsets.size(); ++row) {
        const auto offset = percentage_offsets[row];
        auto value = static_cast<std::uint8_t>(result.percentages[row]);
        text[offset] = text[offset + 1] = 0xfd;
        text[offset + 2] = static_cast<std::uint8_t>('0' + value % 10);
        value /= 10;
        if (value != 0) {
            text[offset + 1] = static_cast<std::uint8_t>('0' + value % 10);
            value /= 10;
            if (value != 0) {
                text[offset] = static_cast<std::uint8_t>('0' + value);
            }
        }
    }
    const auto& message = assets.messages[unsigned(result.message)];
    std::copy(message.begin(), message.end(), text.begin() + message_offset);
    return text;
}
} // namespace comanchecd
