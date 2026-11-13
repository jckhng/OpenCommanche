#pragma once

#include "comanchecd/pilot.hpp"
#include "comanchecd/playback.hpp"
#include "comanchecd/mission.hpp"
#include "comanchecd/gameplay.hpp"
#include "comanchecd/audio.hpp"
#include "comanchecd/world_renderer.hpp"
#include "comanchecd/cockpit.hpp"
#include "comanchecd/debrief.hpp"

#include <filesystem>
#include <string>

namespace comanchecd {

enum class FrontendScene { operations, campaign, debrief, debrief_success, credits, overview };
enum class PicturePresentation { pending, complete };

struct FrontendPicture {
    Palette palette;
    Background background;
    PicturePresentation presentation = PicturePresentation::pending;
};

struct DemoCinematics {
    KdvMovie animation, title, hangar;
    FrontendPicture backdrop, hangar_backdrop;
    std::vector<std::uint8_t> fanfare, canyon_music, hangar_music, sound_bank, cues;
};

struct FrontendAssets {
    PilotAssets pilot;
    std::vector<std::uint8_t> font;
    Transfer transfer;
    std::vector<std::uint8_t> pilot_record;
    std::vector<std::uint8_t> main_menu;
    std::vector<std::uint8_t> campaign_menu;
    std::uint8_t campaign_count;
    std::vector<std::uint8_t> mission_menu;
    std::vector<std::uint8_t> briefing_choices;
    std::vector<std::uint8_t> loading_prompt;
    std::vector<std::uint8_t> statistics;
    std::vector<std::uint8_t> locked_prompt;
    SettingsAssets settings;
    DebriefAssets debrief;
    MissionResources mission_resources;
    std::array<std::uint32_t, 6> gameplay_presets{};
};

FrontendPicture decode_frontend_picture(const std::vector<std::uint8_t>& file);

class Installation {
public:
    explicit Installation(std::filesystem::path directory, std::filesystem::path payload_directory = {});
    FrontendAssets load_frontend() const;
    MenuSettings load_menu_settings() const;
    MusicPlaylist load_menu_music() const;
    PilotMenuAssets load_pilot_menu() const;
    KdvMovie load_overview() const;
    DemoRecording load_demo(std::uint8_t index) const;
    DemoCinematics load_demo_cinematics() const;
    FanfareAssets load_fanfare() const;
    StartupMovieAssets load_startup_movie() const;
    MusicPlaylist load_debrief_music(const MissionResources& resources, const MissionResult& result) const;
    FrontendPicture load_debrief(const MissionResources& resources, FrontendScene scene, MissionAudio& audio) const;
    FrontendPicture load_picture(FrontendScene scene) const;
    FrontendPicture load_mission_picture(const MissionFilename& name) const;
    CampaignMissions load_missions(std::uint8_t campaign) const;
    CampaignMissions load_missions(std::uint8_t campaign, MissionAudio& audio, CampaignLoad phase) const;
    GameplayPayload load_payload(const std::string& name, PayloadKind kind) const;
    GameplayPayload load_payload(const std::string& name, PayloadKind kind, MissionAudio& audio, unsigned filename_bytes) const;
    FrontendPicture load_campaign_picture(MissionAudio& audio) const;
    GameplayPayload load_picture_payload(const std::string& name, ExecutionClock& clock) const;
    AudioTables load_audio() const;
    StartupFiles load_startup_files() const;
    SceneEntry load_scene_entry() const;
    CockpitTables load_cockpit_tables() const;
    TerrainTables load_terrain_tables(TerrainDetail detail = TerrainDetail::medium) const;
    void save_pilots(const std::vector<std::uint8_t>& record) const;

private:
    std::filesystem::path directory_;
    std::filesystem::path payload_directory_;
    std::vector<std::uint8_t> read(const std::string& name) const;
    StartupCampaign campaign_file(std::uint8_t campaign) const;
};

} // namespace comanchecd
