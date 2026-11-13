#pragma once

#include "comanchecd/assets.hpp"

namespace comanchecd {

enum class FrontendMode { pilot, main, campaign, request, mission, briefing, debrief, statistics, credits, settings, locked, overview, demo, startup };
enum class FrontendPage { first, second };
enum class FrontendRequest { none, statistics, overview, demo, credits, exit, mission_selection, quit_prompt,
                             launch_mission, mission_locked, settings_changed, settings_save, settings_closed, settings_abort,
                             calibration_unavailable, input_mode_unavailable };

struct FrontendState {
    FrontendMode mode;
    std::uint8_t selection;
    std::uint8_t selected_pilot;
    PilotMode pilot_mode;
    std::uint8_t editor_length;
    FrontendRequest request;
    std::uint8_t campaign;
    std::uint8_t committed_mission;
};

using SceneLoader = std::function<FrontendPicture(FrontendScene)>;
using MissionLoader = std::function<CampaignMissions(std::uint8_t)>;
using FrontendObserver = std::function<void(const FrontendState&, const PilotController&, const MenuRenderer&)>;

class Frontend {
public:
    Frontend(FrontendAssets assets, SceneLoader loader, PilotWriter writer, FrontendObserver observer = {},
             MissionLoader mission_loader = {}, FrontendPage initial_page = FrontendPage::first,
             ExecutionClock* clock = nullptr, MenuInputReset reset_input = {}, MissionLoader briefing_loader = {});
    void start();
    FrontendRequest consume(PilotKey key);
    FrontendState state() const;
    const MissionPresentation& mission_presentation() const;
    const MissionDefinition& launch_definition() const;
    const GameplaySetup& gameplay_setup() const;
    void finish_mission(MissionResult result, const std::function<void()>& save_work = {});
    void open_requested_page();
    void open_settings(MenuSettings settings);
    void open_flight_settings(MenuSettings settings, const FrontendPicture& background);
    const MenuSettings& settings() const;

private:
    FrontendAssets assets_;
    PilotController pilot_;
    MenuRenderer renderer_;
    SceneLoader loader_;
    PilotWriter writer_;
    FrontendObserver observer_;
    MissionLoader mission_loader_;
    MissionLoader briefing_loader_;
    ExecutionClock* clock_;
    MenuInputReset reset_input_;
    CampaignMissions campaign_missions_;
    MissionPresentation mission_;
    GameplaySetup gameplay_;
    FrontendMode mode_ = FrontendMode::pilot;
    FrontendRequest request_ = FrontendRequest::none;
    std::uint8_t selection_ = 0;
    std::vector<std::uint8_t> debrief_;
    std::unique_ptr<MenuSettings> settings_;
    SettingsMenu settings_menu_;
    SettingsNotice settings_notice_ = SettingsNotice::none;
    bool mission_active_ = false;
    FrontendMode settings_return_ = FrontendMode::main;
    void consume_settings(PilotKey key);
    void observe() const;
    void draw();
    void enter_main();
    void load_scene(FrontendScene scene);
    void consume_menu(PilotKey key);
    void enter_missions();
    void enter_briefing();
    void consume_briefing(PilotKey key);
};

} // namespace comanchecd
