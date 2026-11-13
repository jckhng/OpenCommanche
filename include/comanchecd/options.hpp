#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>

namespace comanchecd {
enum class DefenseMode { manual, automatic };
enum class RotorControl { mixed, independent };
enum class AltitudeControl { stabilized, manual };
enum class EnemyRange { stealth, extended };
enum class ThreatProximity { clear, nearby };
enum class DefenseAction { none, chaff, flare };

struct FlightPolicy {
    DefenseMode chaff = DefenseMode::manual;
    DefenseMode flares = DefenseMode::manual;
    EnemyRange enemy_range = EnemyRange::stealth;
    RotorControl rotor = RotorControl::mixed;
    AltitudeControl altitude = AltitudeControl::stabilized;
};

struct DefenseStatus {
    std::uint8_t height = 0, active_chaff = 0, active_flares = 0;
    ThreatProximity proximity = ThreatProximity::clear;
    bool tracking = false, incoming_hit = false;
};

struct AltitudeResponse {
    std::uint8_t power, descent_shift;
};
AltitudeResponse altitude_response(AltitudeControl control, std::uint8_t power, std::int32_t forward_speed,
                                   std::int32_t pitch, std::int32_t roll);

DefenseAction automatic_defense(const FlightPolicy& policy, const DefenseStatus& status);

enum class DefenseOption { stealth, chaff, flares };
enum class MusicLevel { loud, soft, off };
enum class SoundLevel { loud, soft, off };
enum class MusicChange { loaded, selected };
enum class DisplayOption { artificial_horizon, indicators, compass, missile_camera };
enum class CloudMode { panoramic, flat };
enum class PixelSize { small, large };
struct SceneryPolicy {
    CloudMode clouds = CloudMode::panoramic;
    bool haze = true;
    PixelSize pixels = PixelSize::small;
    bool reflections = false;
};
enum class SettingsAction { none, changed, about, close, save, abort, exit, calibrate };
enum class SettingsKey { left, right, up, down, enter, escape };
enum class SettingsNotice { none, about, saved };
struct SettingsAssets {
    std::vector<std::uint8_t> header;
    std::array<std::vector<std::uint8_t>, 4> menus;
    std::array<std::uint8_t, 4> widths{}, rows{};
    std::vector<std::uint8_t> about, saved;
};

class MenuSettings {
public:
    static constexpr std::size_t record_bytes = 43;
    explicit MenuSettings(std::vector<std::uint8_t> record);
    FlightPolicy flight_policy() const;
    SceneryPolicy scenery_policy() const;
    MusicLevel music_level() const;
    SoundLevel sound_level() const;
    std::uint8_t detail() const;
    bool alternate_keys() const;
    bool display(DisplayOption option) const;
    void toggle(DefenseOption option);
    SettingsAction activate(std::uint8_t category, std::uint8_t row);
    std::vector<std::uint8_t> menu(const SettingsAssets& assets, std::uint8_t category) const;
    const std::vector<std::uint8_t>& record() const;

private:
    std::vector<std::uint8_t> record_;
};

class SettingsMenu {
public:
    SettingsAction consume(SettingsKey key);
    std::uint8_t category() const;
    std::uint8_t row() const;
private:
    std::uint8_t category_ = 0, row_ = 0;
};
} // namespace comanchecd
