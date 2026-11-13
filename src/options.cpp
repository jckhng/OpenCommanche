#include "comanchecd/options.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace comanchecd {
namespace {
constexpr std::uint8_t enabled_marker = 125;
constexpr std::uint8_t toggle_mask = 6;
constexpr std::size_t detail_offset = 24, sound_offset = 25, music_offset = 26, control_offset = 27;
constexpr std::size_t scenery_offset = 28, display_offset = 32, peripheral_offset = 41;
constexpr std::size_t rotor_offset = 36, altitude_offset = 37;
constexpr std::size_t stealth_offset = 38, chaff_offset = 39, flares_offset = 40;
constexpr std::uint8_t minimum_flare_height = 10, minimum_chaff_height = 20;
}

MenuSettings::MenuSettings(std::vector<std::uint8_t> record) : record_(std::move(record)) {
    if (record_.size() != record_bytes) {
        throw std::invalid_argument("Expected 43-byte COMANCHE.MNU settings");
    }
}

bool MenuSettings::alternate_keys() const { return record_[peripheral_offset] == enabled_marker; }

FlightPolicy MenuSettings::flight_policy() const {
    return {
        record_[chaff_offset] == enabled_marker ? DefenseMode::automatic : DefenseMode::manual,
        record_[flares_offset] == enabled_marker ? DefenseMode::automatic : DefenseMode::manual,
        record_[stealth_offset] == enabled_marker ? EnemyRange::stealth : EnemyRange::extended,
        record_[rotor_offset] == enabled_marker ? RotorControl::mixed : RotorControl::independent,
        record_[altitude_offset] == enabled_marker ? AltitudeControl::stabilized : AltitudeControl::manual};
}

MusicLevel MenuSettings::music_level() const {
    return record_[music_offset] == '1' ? MusicLevel::soft :
           record_[music_offset] == '2' ? MusicLevel::off : MusicLevel::loud;
}

SceneryPolicy MenuSettings::scenery_policy() const {
    constexpr std::size_t clouds_offset = 28, pixels_offset = 29, haze_offset = 30, reflections_offset = 31;
    return {record_[clouds_offset] == enabled_marker ? CloudMode::panoramic : CloudMode::flat,
            record_[haze_offset] == enabled_marker,
            record_[pixels_offset] == enabled_marker ? PixelSize::small : PixelSize::large,
            record_[reflections_offset] == enabled_marker};
}

SoundLevel MenuSettings::sound_level() const {
    return record_[sound_offset] == '1' ? SoundLevel::soft :
           record_[sound_offset] == '2' ? SoundLevel::off : SoundLevel::loud;
}

std::uint8_t MenuSettings::detail() const {
    const auto value = record_[detail_offset];
    if (value < '0' || value > '2') {
        throw std::invalid_argument("Invalid terrain detail setting");
    }
    return value - '0';
}

bool MenuSettings::display(DisplayOption option) const {
    return record_.at(display_offset + static_cast<std::size_t>(option)) != 123;
}

void MenuSettings::toggle(DefenseOption option) {
    const auto offset = option == DefenseOption::stealth ? stealth_offset :
                        option == DefenseOption::chaff ? chaff_offset : flares_offset;
    record_[offset] ^= toggle_mask;
}

const std::vector<std::uint8_t>& MenuSettings::record() const { return record_; }

SettingsAction MenuSettings::activate(std::uint8_t category, std::uint8_t row) {
    constexpr std::array<SettingsAction, 5> game{
        SettingsAction::about, SettingsAction::close, SettingsAction::save, SettingsAction::abort, SettingsAction::exit};
    if (category == 0 && row < game.size()) {
        return game[row];
    }
    if (category == 1 && row < 3) {
        record_[detail_offset] = '0' + row;
    } else if (category == 1 && row >= 4 && row <= 7) {
        record_[scenery_offset + row - 4] ^= toggle_mask;
    } else if (category == 2 && row < 3) {
        record_[sound_offset] = '0' + row;
    } else if (category == 2 && row >= 4 && row <= 6) {
        record_[music_offset] = '0' + row - 4;
    } else if (category == 2 && row >= 8 && row <= 12) {
        record_[control_offset] = '0' + row - 8;
    } else if (category == 2 && row >= 14 && row <= 15) {
        record_[peripheral_offset + row - 14] ^= toggle_mask;
    } else if (category == 2 && row == 16) {
        return SettingsAction::calibrate;
    } else if (category == 3 && row < 9) {
        record_[display_offset + row] ^= toggle_mask;
    } else {
        return SettingsAction::none;
    }
    return SettingsAction::changed;
}

std::vector<std::uint8_t> MenuSettings::menu(const SettingsAssets& assets, std::uint8_t category) const {
    auto result = assets.menus.at(category);
    std::size_t row = 0;
    for (std::size_t position = 0; position < result.size(); ++position) {
        if (position != 0 && result[position - 1] != 0) {
            continue;
        }
        if (category == 1 && row < 3) {
            result[position] = record_[detail_offset] == '0' + row ? '`' : ' ';
        } else if (category == 1 && row >= 4 && row <= 7) {
            // Reflections has a color command before its checkbox.
            result[position + (row == 7 ? 2 : 0)] = record_[scenery_offset + row - 4];
        } else if (category == 2 && row < 3) {
            result[position] = record_[sound_offset] == '0' + row ? '`' : ' ';
        } else if (category == 2 && row >= 4 && row <= 6) {
            result[position] = record_[music_offset] == '0' + row - 4 ? '`' : ' ';
        } else if (category == 2 && row >= 8 && row <= 12) {
            result[position] = record_[control_offset] == '0' + row - 8 ? '`' : ' ';
        } else if (category == 2 && row >= 14 && row <= 15) {
            result[position] = record_[peripheral_offset + row - 14];
        } else if (category == 3 && row < 9) {
            result[position] = record_[display_offset + row];
        }
        ++row;
    }
    return result;
}

SettingsAction SettingsMenu::consume(SettingsKey key) {
    constexpr std::array<std::uint8_t, 4> counts{5, 8, 17, 9};
    if (key == SettingsKey::escape) {
        return SettingsAction::close;
    }
    if (key == SettingsKey::enter) {
        return SettingsAction::changed;
    }
    if (key == SettingsKey::left || key == SettingsKey::right) {
        category_ = (category_ + (key == SettingsKey::left ? 3 : 1)) % counts.size();
        row_ = 0;
        return SettingsAction::none;
    }
    const int direction = key == SettingsKey::up ? -1 : 1;
    do {
        row_ = static_cast<std::uint8_t>((row_ + direction + counts[category_]) % counts[category_]);
    } while ((category_ == 1 && row_ == 3) ||
             (category_ == 2 && (row_ == 3 || row_ == 7 || row_ == 13)));
    return SettingsAction::none;
}

std::uint8_t SettingsMenu::category() const { return category_; }
std::uint8_t SettingsMenu::row() const { return row_; }

AltitudeResponse altitude_response(AltitudeControl control, std::uint8_t power, std::int32_t forward_speed,
                                   std::int32_t pitch, std::int32_t roll) {
    if (control == AltitudeControl::stabilized) {
        return {power, 3};
    }
    int result = std::max(int(power) - 5, 0);
    // Original CBW/XOR uses ones-complement magnitude, including its negative-unit bias.
    const auto speed = static_cast<std::int8_t>(forward_speed >> 21);
    const auto magnitude = static_cast<std::uint8_t>(speed < 0 ? ~speed : speed);
    result = std::min(result + magnitude, 255);
    auto bank = static_cast<std::int8_t>(-static_cast<std::uint8_t>((roll * 3) >> 3));
    int penalty = bank < 0 ? static_cast<std::uint8_t>(-bank) : bank;
    if (bank < 0 && speed < 0) {
        penalty -= magnitude;
    }
    if (penalty >= 0) {
        result = std::max(result - penalty, 0);
    }
    const auto tilt = static_cast<std::uint8_t>(std::uint32_t(pitch < 0 ? ~pitch : pitch) >> 4);
    result = std::max(result - tilt, 0);
    return {static_cast<std::uint8_t>(std::min(result + 5, 255)), 2};
}

DefenseAction automatic_defense(const FlightPolicy& policy, const DefenseStatus& status) {
    // The tracking branch precedes incoming-hit, even when chaff cannot deploy.
    if (status.proximity != ThreatProximity::nearby && status.tracking) {
        return policy.chaff == DefenseMode::automatic && status.height > minimum_chaff_height &&
               status.active_chaff == 0 ? DefenseAction::chaff : DefenseAction::none;
    }
    if (status.proximity == ThreatProximity::nearby || status.incoming_hit) {
        return policy.flares == DefenseMode::automatic && status.height > minimum_flare_height &&
               status.active_flares == 0 ? DefenseAction::flare : DefenseAction::none;
    }
    return DefenseAction::none;
}
} // namespace comanchecd
