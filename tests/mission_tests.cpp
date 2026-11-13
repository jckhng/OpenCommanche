#include "comanchecd/frontend.hpp"
#include "comanchecd/execution_clock.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Bytes = std::vector<std::uint8_t>;
class PageClock final : public comanchecd::ExecutionClock {
public:
    std::vector<std::pair<comanchecd::CpuOperation, unsigned>> work;
    std::vector<std::size_t> waits;
    void execute(comanchecd::CpuOperation operation, unsigned count) override { work.emplace_back(operation, count); }
    void instruction(const std::function<void()>& action) override {
        execute(comanchecd::CpuOperation::instructions, 1);
        action();
    }
    void read_file(unsigned) override {}
    bool read_retrace() override {
        waits.push_back(work.size());
        return waits.size() % 2 != 0;
    }
    std::uint64_t cycles() const override { return 0; }
    std::uint64_t irq_ticks() const override { return 0; }
    std::uint64_t sample_irq_ticks() override { return 0; }
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Bytes bytes(const std::string& value) { return Bytes(value.begin(), value.end()); }

comanchecd::FrontendAssets assets() {
    comanchecd::FrontendAssets result{};
    result.locked_prompt = {255};
    result.font.resize(780);
    result.pilot.menu_header.resize(4, 0);
    result.pilot.editor_prompt.resize(54, 0xfc);
    result.pilot.editor_prompt.back() = 255;
    result.pilot_record.resize(comanchecd::pilot_record_bytes, 0xfc);
    std::fill(result.pilot_record.begin() + 0x17f, result.pilot_record.end(), 0);
    for (std::size_t slot = 0; slot < 8; ++slot) {
        result.pilot_record[slot * 43 + 9] = 'A';
        result.pilot_record[slot * 43 + 42] = 0;
    }
    result.pilot_record[344] = 0;
    result.pilot_record[382] = 255;
    result.main_menu.resize(60, 0xfc);
    std::fill_n(result.main_menu.begin(), 14, 0);
    result.main_menu.back() = 255;
    result.campaign_menu.resize(30, 0xfc);
    std::fill_n(result.campaign_menu.begin(), 7, 0);
    result.campaign_menu.back() = 255;
    result.campaign_count = 2;
    result.mission_menu.resize(343, 0xfc);
    std::fill_n(result.mission_menu.begin(), 3, 0);
    result.mission_menu.back() = 255;
    result.briefing_choices = {'A', 0, 'B', 255};
    result.loading_prompt = {'L', 255};
    result.pilot.glyph_widths.fill(2);
    return result;
}

comanchecd::FrontendPicture picture(comanchecd::FrontendScene) {
    comanchecd::FrontendPicture result{};
    for (auto& plane : result.background) {
        plane.resize(comanchecd::background_bytes, 0);
    }
    return result;
}
}

int main() {
    try {
        const auto campaign = comanchecd::parse_campaign(bytes(
            "{Test}\r\n{First}\r\n;comment\r\n\r\nA^\r\nB\r\n<\r\np1.dta ; terrain\r\n<\r\n"
            "0\r\n1,2,3\r\n4,5\r\n0,0,0,0,0,0\r\n6\r\n1,1,1,1,1,1,1,1,0\r\n<\r\n"
            "{Second}*1\r\nSecond briefing\r\n<\r\np2.dta\r\n<\r\n"
            "0\r\n1,2,3\r\n4,5\r\n0,0,0,0,0,0\r\n6\r\n1,1,1,1,1,1,1,1,0\r\n<\r\n"));
        require(campaign.missions.size() == 2 && campaign.missions[1].restricted &&
                campaign.missions[1].alternate_campaign == 2, "Mission metadata and title scan");
        require(campaign.missions[0].description == bytes("A^\r\nB\r\n"), "Initial comments/blank lines must be skipped");
        require(campaign.missions[0].resources[0] == comanchecd::MissionFilename{'p', '1', '.', 'd', 't', 'a', ' ', ' ', ' ', ' ', ' ', ' '},
                "Resource names must stop before whitespace/comment and pad twelve bytes");
        require(!campaign.missions[0].setup_text.empty(), "Gameplay setup text must remain available to the launch caller");
        const auto adjacent = comanchecd::parse_campaign(bytes(
            "{Test}\r\n{First}\r\nBriefing\r\n<\r\np1.dta\r\n<\r\n"
            "0\r\n1,2,3\r\n4,5\r\n0,0,0,0,0,0\r\n6\r\n1,1,1,1,1,1,1,1,0\r\n\r\n"
            "{Second}\r\nBriefing\r\n<\r\np2.dta\r\n<\r\n"
            "0\r\n1,2,3\r\n4,5\r\n0,0,0,0,0,0\r\n6\r\n1,1,1,1,1,1,1,1,0\r\n<\r\n"));
        require(comanchecd::parse_gameplay(adjacent.missions[0], {}, {}, {}).objects.empty(),
                "The next mission title terminates setup without a separate angle-bracket marker");
        comanchecd::MissionPresentation presentation{};
        presentation.menu = assets().mission_menu;
        comanchecd::populate_missions(presentation, campaign);
        require(presentation.menu[comanchecd::mission_menu_header_bytes + 2 * comanchecd::mission_menu_row_bytes + 3] == 0,
                "Shorter catalogs must clear all four original footer bytes");
        std::array<std::uint8_t, 256> widths;
        widths.fill(2);
        widths[0] = 0;
        auto definition = campaign.missions[0];
        definition.description = bytes(std::string(140, 'A'));
        auto stream = comanchecd::wrap_briefing(presentation.menu, 0, definition, widths);
        require(stream.size() == 34 + 140 + 1, "Exactly 280 pixels must remain on the current line");
        definition.description.push_back('B');
        stream = comanchecd::wrap_briefing(presentation.menu, 0, definition, widths);
        require(stream[34 + 141] == 0, "A long word wraps after the first character beyond 280 pixels");
        definition.description = bytes(std::string(100, 'A') + " " + std::string(50, 'B'));
        stream = comanchecd::wrap_briefing(presentation.menu, 0, definition, widths);
        const auto expected = bytes(std::string(100, 'A') + std::string(1, '\0') + std::string(50, 'B'));
        require(std::equal(expected.begin(), expected.end(), stream.begin() + 34), "Overflow must rewind to the last space");
        stream = comanchecd::wrap_briefing(presentation.menu, 0, campaign.missions[0], widths);
        require(stream[34] == 'A' && stream[35] == 0 && stream[36] == 'B', "Caret forces a line break and resets whitespace state");

        auto unlocked = campaign;
        unlocked.missions[1].restricted = false;
        const auto writer = [](const auto&) { throw std::runtime_error("Mission selection must not write NAM"); };
        comanchecd::Frontend frontend(assets(), picture, writer, {}, [&](std::uint8_t) { return unlocked; });
        frontend.start();
        frontend.consume({0x1c});
        frontend.consume({0x1c});
        frontend.consume({0x1c});
        require(frontend.state().mode == comanchecd::FrontendMode::mission, "Campaign acceptance must enter missions");
        frontend.consume({0x50});
        require(frontend.state().committed_mission == 0, "Highlight movement must preserve the committed mission");
        frontend.consume({0x1c});
        require(frontend.state().mode == comanchecd::FrontendMode::briefing && frontend.state().committed_mission == 1,
                "Mission acceptance must commit before briefing");
        frontend.consume({0x01});
        require(frontend.state().mode == comanchecd::FrontendMode::mission && frontend.state().selection == 1,
                "Briefing Escape must retain the remembered mission");
        frontend.consume({0x01});
        frontend.consume({0x50});
        frontend.consume({0x1c});
        require(frontend.state().campaign == 1 && frontend.state().selection == 0,
                "Each campaign must have a separate remembered mission");
        frontend.consume({0x1c});
        require(frontend.consume({0x1c}) == comanchecd::FrontendRequest::launch_mission,
                "Accepting briefing row zero must render loading and return a launch request");
        require(frontend.launch_definition().title == bytes("First"), "Launch must carry the accepted definition");

        PageClock page_clock;
        comanchecd::Frontend clocked(assets(), picture, writer, {}, [&](std::uint8_t) { return unlocked; },
            comanchecd::FrontendPage::first, &page_clock);
        clocked.start();
        for (unsigned enter = 0; enter < 3; ++enter) { clocked.consume({0x1c}); }
        page_clock.work.clear();
        page_clock.waits.clear();
        clocked.consume({0x1c});
        require(!page_clock.waits.empty(), "Briefing entry must present its prepared page");
        const auto first_wait = page_clock.work.begin() + page_clock.waits.front();
        require(std::count_if(page_clock.work.begin(), first_wait, [](const auto& item) {
            return item.first == comanchecd::CpuOperation::repeat_copy_dwords &&
                item.second == comanchecd::background_bytes / 4;
        }) == 8, "Briefing must restore and save all four background planes before its first presentation");
        require(std::count_if(page_clock.work.begin(), page_clock.work.end(), [](const auto& item) {
            return item.first == comanchecd::CpuOperation::repeat_copy_bytes &&
                item.second == comanchecd::line_stride * comanchecd::briefing_page_height;
        }) == 1, "Briefing entry must latch-copy the presented 240-row page before drawing");

        auto progress = assets();
        progress.pilot_record[0x23a] = 1;
        comanchecd::Frontend completed(progress, picture, writer, {}, [&](std::uint8_t) { return campaign; });
        completed.start();
        completed.consume({0x1c});
        completed.consume({0x1c});
        completed.consume({0x1c});
        require(completed.mission_presentation().colors[0] == 'b' && completed.mission_presentation().colors[1] == 'g',
                "Completion must turn a mission blue and unlock the restricted frontier");

        std::size_t observed_page = 0;
        comanchecd::Frontend second_page(assets(), picture, writer,
            [&](const auto&, const auto&, const auto& renderer) { observed_page = renderer.back_page(); },
            {}, comanchecd::FrontendPage::second);
        second_page.start();
        require(observed_page == comanchecd::page_displacement, "Entry phase must select the second physical page");

        comanchecd::Frontend locked(assets(), picture, writer, {}, [&](std::uint8_t) { return campaign; });
        locked.start();
        locked.consume({0x1c});
        locked.consume({0x1c});
        locked.consume({0x1c});
        require(locked.mission_presentation().colors[0] == 'g' && locked.mission_presentation().colors[1] == 'o',
                "Restricted missions beyond the completion frontier must use orange");
        locked.consume({0x50});
        require(locked.consume({0x1c}) == comanchecd::FrontendRequest::mission_locked && locked.state().committed_mission == 1,
                "Locked acceptance still commits before requesting its modal dialog");
        locked.open_requested_page();
        require(locked.state().mode == comanchecd::FrontendMode::locked,
                "The locked mission request must open its modal dialog");
        locked.consume({0x1c});
        require(locked.state().mode == comanchecd::FrontendMode::mission && locked.state().selection == 1,
                "Dismissing the lock dialog must retain the selected mission");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
