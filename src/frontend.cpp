#include "comanchecd/frontend.hpp"
#include "pilot_presentation.hpp"
#include "menu_work.hpp"
#include "mission_work.hpp"
#include "comanchecd/execution_clock.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace comanchecd {
namespace {
constexpr std::size_t main_name_offset = 18;
constexpr std::uint8_t main_options = 7;
constexpr std::uint8_t scan_escape = 0x01;
constexpr std::uint8_t scan_tab = 0x0f;
constexpr std::uint8_t scan_enter = 0x1c;
constexpr std::uint8_t scan_space = 0x39;
constexpr std::uint8_t scan_up = 0x48;
constexpr std::uint8_t scan_left = 0x4b;
constexpr std::uint8_t scan_right = 0x4d;
constexpr std::uint8_t scan_down = 0x50;
enum class MainChoice : std::uint8_t { begin, statistics, pilot, overview, demo, credits, exit };

PilotController make_pilot(FrontendAssets& assets) {
    PilotStorage storage(std::move(assets.pilot_record));
    const auto selection = storage.normalize(storage.selected());
    return PilotController(std::move(storage), assets.pilot, selection);
}

MenuRenderer make_renderer(const FrontendAssets& assets, const FrontendPicture& picture,
                           PlanarSurface surface, std::size_t page, ExecutionClock* clock, const MenuInputReset& reset_input) {
    const auto colors = menu_work::colors(picture.palette, assets.transfer,
        clock ? CpuWorkSink([clock](CpuOperation op, unsigned n) { clock->execute(op, n); }) : CpuWorkSink{});
    return MenuRenderer(std::move(surface), Font(assets.font), colors.theme,
                        colors.remap, picture.background, page, 0, clock, reset_input);
}

MenuRenderer initial_renderer(const FrontendAssets& assets, const SceneLoader& loader, FrontendPage page,
                              ExecutionClock* clock, const MenuInputReset& reset_input) {
    if (!loader) {
        throw std::invalid_argument("Frontend scene loader is required");
    }
    const auto picture = loader(FrontendScene::operations);
    PlanarSurface surface(std::vector<std::uint8_t>(plane_count * plane_bytes, 0));
    surface.restore(0, picture.background);
    surface.restore(page_displacement, picture.background);
    return make_renderer(assets, picture, std::move(surface), page == FrontendPage::first ? 0 : page_displacement, clock, reset_input);
}
}

Frontend::Frontend(FrontendAssets assets, SceneLoader loader, PilotWriter writer, FrontendObserver observer,
                   MissionLoader mission_loader, FrontendPage initial_page, ExecutionClock* clock, MenuInputReset reset_input,
                   MissionLoader briefing_loader)
    : assets_(std::move(assets)), pilot_(make_pilot(assets_)), renderer_(initial_renderer(assets_, loader, initial_page, clock, reset_input)),
      loader_(std::move(loader)), writer_(std::move(writer)), observer_(std::move(observer)),
      mission_loader_(std::move(mission_loader)), briefing_loader_(std::move(briefing_loader)), clock_(clock),
      reset_input_(std::move(reset_input)) {
    if (!writer_ || assets_.campaign_count == 0 || assets_.main_menu.size() < main_name_offset + pilot_name_bytes) {
        throw std::invalid_argument("Incomplete frontend resources or persistence writer");
    }
    mission_.menu = assets_.mission_menu;
    mission_.resources = assets_.mission_resources;
}

FrontendState Frontend::state() const {
    return {mode_, mode_ == FrontendMode::pilot ? pilot_.selection() :
                   mode_ == FrontendMode::settings ? settings_menu_.row() : selection_, pilot_.storage().selected(),
            pilot_.mode(), pilot_.editor_length(), request_, pilot_.storage().campaign(pilot_.storage().selected()),
            pilot_.storage().committed_mission()};
}

const MissionPresentation& Frontend::mission_presentation() const { return mission_; }

const GameplaySetup& Frontend::gameplay_setup() const { return gameplay_; }

const MissionDefinition& Frontend::launch_definition() const {
    if (request_ != FrontendRequest::launch_mission) {
        throw std::logic_error("No accepted mission is ready for its caller");
    }
    return campaign_missions_.missions.at(pilot_.storage().committed_mission());
}

void Frontend::observe() const {
    if (observer_) {
        observer_(state(), pilot_, renderer_);
    }
}

void Frontend::draw() {
    if (mode_ == FrontendMode::locked) {
        renderer_.draw_locked_mission(assets_.locked_prompt);
        observe();
        renderer_.present();
        return;
    }
    if (mode_ == FrontendMode::settings) {
        renderer_.draw_settings(assets_.settings, *settings_, settings_menu_, settings_notice_);
        observe();
        renderer_.present();
        return;
    }
    if (mode_ == FrontendMode::statistics || mode_ == FrontendMode::credits || mode_ == FrontendMode::overview || mode_ == FrontendMode::demo) {
        if (mode_ == FrontendMode::statistics) {
            renderer_.draw_statistics(pilot_.storage().statistics(assets_.statistics));
        } else {
            renderer_.restore_background();
        }
        observe();
        renderer_.present();
        return;
    }
    if (mode_ == FrontendMode::debrief) {
        renderer_.draw_debrief(debrief_);
        observe();
        renderer_.present();
        return;
    }
    if (mode_ == FrontendMode::pilot) {
        detail::present_pilot(PilotUpdate::list, pilot_, renderer_, writer_,
                             [&](const auto&, const auto&) { observe(); });
        return;
    }
    if (mode_ == FrontendMode::briefing) {
        renderer_.draw_briefing(mission_.briefing, assets_.briefing_choices, selection_);
        observe();
        renderer_.present();
        return;
    }
    auto stream = mode_ == FrontendMode::main ? assets_.main_menu :
                  mode_ == FrontendMode::mission ? mission_.menu : assets_.campaign_menu;
    if (mode_ == FrontendMode::main) {
        const auto name = pilot_.storage().name(pilot_.selection());
        std::copy(name.begin(), name.end(), stream.begin() + main_name_offset);
    }
    renderer_.draw_list(stream, selection_, mode_ == FrontendMode::main ? ListLayout::main : ListLayout::standard);
    observe();
    renderer_.present();
}

void Frontend::start() { renderer_.begin_list(); draw(); }

void Frontend::enter_main() {
    renderer_.begin_list();
    mode_ = FrontendMode::main;
    selection_ = 0;
    draw();
}

void Frontend::load_scene(FrontendScene scene) {
    const auto picture = loader_(scene);
    if (picture.presentation == PicturePresentation::complete) {
        auto surface = renderer_.surface();
        surface.restore(renderer_.back_page(), picture.background);
        renderer_ = make_renderer(assets_, picture, std::move(surface), renderer_.back_page() ^ page_displacement,
                                  clock_, reset_input_);
        return;
    }
    renderer_ = make_renderer(assets_, picture, renderer_.surface(), renderer_.back_page(), clock_, reset_input_);
    renderer_.restore_background();
    renderer_.present();
    renderer_.restore_background();
}

void Frontend::consume_menu(PilotKey key) {
    const auto count = mode_ == FrontendMode::main ? main_options : mode_ == FrontendMode::mission ?
        static_cast<std::uint8_t>(campaign_missions_.missions.size()) : assets_.campaign_count;
    if (key.scan == scan_up || key.scan == scan_left || (key.scan == scan_tab && key.shift != PilotShift::none)) {
        selection_ = selection_ == 0 ? count - 1 : selection_ - 1;
        draw();
        return;
    }
    if (key.scan == scan_down || key.scan == scan_right || key.scan == scan_space || key.scan == scan_tab) {
        selection_ = static_cast<std::uint8_t>((selection_ + 1) % count);
        draw();
        return;
    }
    if (key.scan == scan_escape) {
        renderer_.restore_background();
        renderer_.present();
        if (mode_ == FrontendMode::campaign) {
            load_scene(FrontendScene::operations);
            enter_main();
            return;
        }
        if (mode_ == FrontendMode::mission) {
            mode_ = FrontendMode::campaign;
            selection_ = pilot_.storage().campaign(pilot_.selection());
            renderer_.begin_list();
            draw();
            return;
        }
        request_ = FrontendRequest::quit_prompt;
        settings_return_ = mode_;
        mode_ = FrontendMode::request;
        observe();
        return;
    }
    if (key.scan != scan_enter) {
        observe();
        return;
    }
    renderer_.restore_background();
    if (mode_ == FrontendMode::mission) {
        pilot_.choose_mission(selection_);
        if (mission_.colors[selection_] == 'o') {
            renderer_.present();
            request_ = FrontendRequest::mission_locked;
            mode_ = FrontendMode::request;
            observe();
            return;
        }
        enter_briefing();
        return;
    }
    renderer_.present();
    if (mode_ == FrontendMode::campaign) {
        if (mission_loader_) {
            pilot_.choose_campaign(selection_);
            campaign_missions_ = mission_loader_(selection_);
            populate_missions(mission_, campaign_missions_);
            enter_missions();
            return;
        }
        request_ = FrontendRequest::mission_selection;
        mode_ = FrontendMode::request;
        observe();
        return;
    }
    switch (static_cast<MainChoice>(selection_)) {
    case MainChoice::begin:
        load_scene(FrontendScene::campaign);
        mode_ = FrontendMode::campaign;
        selection_ = pilot_.storage().campaign(pilot_.selection());
        if (selection_ >= assets_.campaign_count) {
            selection_ = 0;
        }
        renderer_.begin_list();
        draw();
        return;
    case MainChoice::pilot:
        pilot_.resume();
        mode_ = FrontendMode::pilot;
        renderer_.begin_list();
        draw();
        return;
    case MainChoice::statistics: request_ = FrontendRequest::statistics; break;
    case MainChoice::overview: request_ = FrontendRequest::overview; break;
    case MainChoice::demo: request_ = FrontendRequest::demo; break;
    case MainChoice::credits: request_ = FrontendRequest::credits; break;
    case MainChoice::exit: request_ = FrontendRequest::exit; break;
    }
    mode_ = FrontendMode::request;
    observe();
}

FrontendRequest Frontend::consume(PilotKey key) {
    if (mode_ == FrontendMode::locked) {
        if (key.scan != 0) {
            enter_missions();
        }
        return request_;
    }
    if (mode_ == FrontendMode::settings) {
        consume_settings(key);
        return request_;
    }
    if (request_ == FrontendRequest::settings_closed) {
        request_ = FrontendRequest::none;
    }
    if (mode_ == FrontendMode::statistics || mode_ == FrontendMode::credits || mode_ == FrontendMode::overview || mode_ == FrontendMode::demo) {
        if (key.scan != 0) {
            if (mode_ == FrontendMode::credits || mode_ == FrontendMode::overview || mode_ == FrontendMode::demo) {
                load_scene(FrontendScene::operations);
            }
            enter_main();
        }
        return request_;
    }
    if (mode_ == FrontendMode::debrief) {
        if (key.scan == scan_escape) {
            load_scene(FrontendScene::operations);
            enter_main();
        }
        return request_;
    }
    if (mode_ == FrontendMode::request) {
        throw std::logic_error("Frontend request must be handled by its caller");
    }
    if (mode_ == FrontendMode::briefing) {
        consume_briefing(key);
        return request_;
    }
    if (mode_ != FrontendMode::pilot) {
        consume_menu(key);
        return request_;
    }
    const auto update = pilot_.consume(key);
    detail::present_pilot(update, pilot_, renderer_, writer_, [&](const auto&, const auto&) {
        if (update != PilotUpdate::accepted) {
            observe();
        }
    });
    if (update == PilotUpdate::accepted) {
        enter_main();
    } else if (update == PilotUpdate::exit_prompt) {
        request_ = FrontendRequest::quit_prompt;
        settings_return_ = FrontendMode::pilot;
        mode_ = FrontendMode::request;
    }
    return request_;
}

void Frontend::open_settings(MenuSettings settings) {
    if (request_ != FrontendRequest::quit_prompt) {
        throw std::logic_error("No settings menu requested");
    }
    settings_ = std::make_unique<MenuSettings>(std::move(settings));
    settings_menu_ = SettingsMenu{};
    settings_notice_ = SettingsNotice::none;
    request_ = FrontendRequest::none;
    mode_ = FrontendMode::settings;
    draw();
}

void Frontend::open_flight_settings(MenuSettings settings, const FrontendPicture& background) {
    if (!mission_active_) {
        throw std::logic_error("Flight settings require an active mission");
    }
    renderer_ = make_renderer(assets_, background, renderer_.surface(), renderer_.back_page(), clock_, reset_input_);
    settings_return_ = FrontendMode::request;
    request_ = FrontendRequest::quit_prompt;
    open_settings(std::move(settings));
}

const MenuSettings& Frontend::settings() const {
    if (!settings_) {
        throw std::logic_error("Settings have not been opened");
    }
    return *settings_;
}

void Frontend::consume_settings(PilotKey key) {
    request_ = FrontendRequest::none;
    if (key.scan == 0) {
        return;
    }
    if (settings_notice_ != SettingsNotice::none) {
        settings_notice_ = SettingsNotice::none;
        draw();
        return;
    }
    SettingsKey input;
    if (key.scan == scan_escape) { input = SettingsKey::escape; }
    else if (key.scan == scan_enter) { input = SettingsKey::enter; }
    else if (key.scan == scan_up) { input = SettingsKey::up; }
    else if (key.scan == scan_down || key.scan == scan_space) { input = SettingsKey::down; }
    else if (key.scan == scan_left || (key.scan == scan_tab && key.shift != PilotShift::none)) { input = SettingsKey::left; }
    else if (key.scan == scan_right || key.scan == scan_tab) { input = SettingsKey::right; }
    else { return; }
    auto action = settings_menu_.consume(input);
    if (action == SettingsAction::changed) {
        constexpr std::uint8_t control_category = 2, first_input_mode = 8, last_input_mode = 12;
        constexpr std::uint8_t first_peripheral_option = 14, last_peripheral_option = 15;
        const auto row = settings_menu_.row();
        if (settings_menu_.category() == control_category &&
            ((row >= first_input_mode && row <= last_input_mode) ||
             (row >= first_peripheral_option && row <= last_peripheral_option))) {
            request_ = FrontendRequest::input_mode_unavailable;
            draw();
            return;
        }
        action = settings_->activate(settings_menu_.category(), settings_menu_.row());
    }
    if (action == SettingsAction::close || action == SettingsAction::abort) {
        mode_ = settings_return_;
        request_ = action == SettingsAction::abort && mission_active_ ?
            FrontendRequest::settings_abort : FrontendRequest::settings_closed;
        if (mission_active_) {
            return;
        }
        if (mode_ == FrontendMode::pilot) {
            pilot_.resume();
        }
    } else if (action == SettingsAction::exit) {
        mode_ = FrontendMode::request;
        request_ = FrontendRequest::exit;
        return;
    } else if (action == SettingsAction::about) {
        settings_notice_ = SettingsNotice::about;
    } else if (action == SettingsAction::save) {
        request_ = FrontendRequest::settings_save;
        settings_notice_ = SettingsNotice::saved;
    } else if (action == SettingsAction::changed) {
        request_ = FrontendRequest::settings_changed;
    } else if (action == SettingsAction::calibrate) {
        request_ = FrontendRequest::calibration_unavailable;
    }
    draw();
}

void Frontend::open_requested_page() {
    if (mode_ != FrontendMode::request ||
            (request_ != FrontendRequest::statistics && request_ != FrontendRequest::credits &&
             request_ != FrontendRequest::mission_locked && request_ != FrontendRequest::overview && request_ != FrontendRequest::demo)) {
        throw std::logic_error("No supported information page requested");
    }
    if (request_ == FrontendRequest::demo) {
        mode_ = FrontendMode::demo;
    } else if (request_ == FrontendRequest::overview) {
        load_scene(FrontendScene::overview);
        mode_ = FrontendMode::overview;
    } else if (request_ == FrontendRequest::credits) {
        load_scene(FrontendScene::credits);
        mode_ = FrontendMode::credits;
    } else if (request_ == FrontendRequest::mission_locked) {
        mode_ = FrontendMode::locked;
    } else {
        mode_ = FrontendMode::statistics;
    }
    request_ = FrontendRequest::none;
    draw();
}

void Frontend::finish_mission(MissionResult result, const std::function<void()>& save_work) {
    if (!mission_active_) {
        throw std::logic_error("Mission return requires an accepted launch");
    }
    auto next_pilot = pilot_;
    next_pilot.record_result(result, campaign_missions_, clock_ ?
        CpuWorkSink([&](CpuOperation op, unsigned count) { clock_->execute(op, count); }) : CpuWorkSink{});
    auto text = debrief_text(assets_.debrief, result);
    if (save_work) { save_work(); }
    writer_(next_pilot.storage().record());
    const auto picture = loader_(result.survived && result.completed ? FrontendScene::debrief_success : FrontendScene::debrief);
    auto surface = renderer_.surface();
    auto page = renderer_.back_page();
    if (picture.presentation == PicturePresentation::complete) {
        surface.restore(page, picture.background);
        page ^= page_displacement;
        surface.restore(page, picture.background);
    }
    auto renderer = make_renderer(assets_, picture, std::move(surface), page, clock_, reset_input_);
    if (picture.presentation == PicturePresentation::pending) {
        renderer.restore_background();
        renderer.present();
        renderer.restore_background();
    }
    renderer.draw_debrief(text, picture.presentation == PicturePresentation::complete ?
        DebriefBackground::capture : DebriefBackground::restore);
    pilot_ = std::move(next_pilot);
    renderer_ = std::move(renderer);
    debrief_ = std::move(text);
    request_ = FrontendRequest::none;
    mode_ = FrontendMode::debrief;
    mission_active_ = false;
    observe();
    renderer_.present();
    if (clock_) { clock_->execute(CpuOperation::instructions, 1); }
    if (reset_input_) { reset_input_(); }
}

void Frontend::enter_missions() {
    renderer_.begin_list();
    const auto campaign = pilot_.storage().campaign(pilot_.selection());
    std::size_t completed = 0;
    for (std::size_t row = 0; row < campaign_missions_.missions.size(); ++row) {
        auto color = completed != row && mission_.restricted[row] != 0 ? 'o' : 'g';
        if (pilot_.storage().completed(pilot_.selection(), campaign, static_cast<std::uint8_t>(row))) {
            color = 'b';
            ++completed;
        }
        mission_.colors[row] = static_cast<std::uint8_t>(color);
        mission_.menu[mission_menu_header_bytes + row * mission_menu_row_bytes + 1] = static_cast<std::uint8_t>(color);
    }
    mode_ = FrontendMode::mission;
    selection_ = pilot_.storage().remembered_mission(pilot_.selection(), campaign);
    if (selection_ >= campaign_missions_.missions.size()) {
        selection_ = 0;
    }
    draw();
}

void Frontend::enter_briefing() {
    if (briefing_loader_) {
        campaign_missions_ = briefing_loader_(pilot_.storage().campaign(pilot_.selection()));
    }
    const auto& definition = campaign_missions_.missions.at(pilot_.storage().committed_mission());
    mission_.briefing = wrap_briefing(mission_.menu, pilot_.storage().committed_mission(), definition,
                                      assets_.pilot.glyph_widths);
    for (std::size_t index = 11; index < mission_.resources.size(); ++index) {
        mission_.resources[index][0] = '-';
    }
    std::copy(definition.resources.begin(), definition.resources.end(), mission_.resources.begin());
    gameplay_ = parse_gameplay(definition, mission_.resources, assets_.gameplay_presets, gameplay_);
    if (clock_) {
        mission_work::briefing(campaign_missions_, pilot_.storage().committed_mission(), assets_.pilot.glyph_widths,
            mission_.resources, [&](CpuOperation op, unsigned n) { clock_->execute(op, n); });
        clock_->execute(CpuOperation::instructions, 3);
    }
    renderer_.prepare_briefing();
    mode_ = FrontendMode::briefing;
    selection_ = 0;
    draw();
}

void Frontend::consume_briefing(PilotKey key) {
    if (key.scan == scan_up || key.scan == scan_left || key.scan == scan_down || key.scan == scan_right ||
            key.scan == scan_space || key.scan == scan_tab) {
        selection_ ^= 1;
        draw();
        return;
    }
    if (key.scan != scan_enter && key.scan != scan_escape) {
        observe();
        return;
    }
    renderer_.restore_background();
    renderer_.present();
    if (key.scan == scan_escape || selection_ == 1) {
        enter_missions();
        return;
    }
    renderer_.restore_background();
    renderer_.draw_loading(assets_.loading_prompt);
    renderer_.present();
    request_ = FrontendRequest::launch_mission;
    mission_active_ = true;
    mode_ = FrontendMode::request;
    observe();
}

} // namespace comanchecd
