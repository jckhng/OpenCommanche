#include "comanchecd/frontend.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

comanchecd::FrontendAssets assets() {
    comanchecd::FrontendAssets data{};
    data.font.resize(780);
    data.pilot.menu_header.resize(4, 0);
    data.pilot.editor_prompt.resize(54, 0xfc);
    data.pilot.editor_prompt.back() = 255;
    data.pilot_record.resize(comanchecd::pilot_record_bytes, 0xfc);
    std::fill(data.pilot_record.begin() + 0x17f, data.pilot_record.end(), 0);
    for (std::size_t slot = 0; slot < 8; ++slot) {
        data.pilot_record[slot * 43 + 9] = static_cast<std::uint8_t>('A' + slot);
        data.pilot_record[slot * 43 + 42] = 0;
    }
    data.pilot_record[344] = 0;
    data.pilot_record[382] = 255;
    data.pilot_record[0x181] = 9;
    data.main_menu.resize(60, 0xfc);
    std::fill_n(data.main_menu.begin(), 14, 0);
    data.main_menu.back() = 255;
    data.campaign_menu.resize(30, 0xfc);
    std::fill_n(data.campaign_menu.begin(), 15, 0);
    data.campaign_menu.back() = 255;
    data.campaign_count = 10;
    return data;
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
        const auto writer = [](const auto&) { throw std::runtime_error("Navigation must not save NAM"); };
        comanchecd::Frontend frontend(assets(), picture, writer);
        frontend.start();
        frontend.consume({0x1c});
        require(frontend.state().mode == comanchecd::FrontendMode::main, "Pilot acceptance must enter main");
        frontend.consume({0x48});
        require(frontend.state().selection == 6, "Main must wrap across seven options");
        frontend.consume({0x0f});
        frontend.consume({0x50});
        frontend.consume({0x50});
        frontend.consume({0x1c});
        require(frontend.state().mode == comanchecd::FrontendMode::pilot, "Choose Another Pilot must reopen retained storage");
        frontend.consume({0x50});
        frontend.consume({0x1c});
        require(frontend.state().selected_pilot == 1, "Returning pilot selection must commit the new identity");
        frontend.consume({0x1c});
        require(frontend.state().mode == comanchecd::FrontendMode::campaign && frontend.state().selection == 9,
                "Campaign selection must use its ten-row bound, not the main menu bound");
        frontend.consume({0x50});
        require(frontend.state().selection == 0, "Campaign selection must wrap ten options");
        frontend.consume({0x01});
        require(frontend.state().mode == comanchecd::FrontendMode::main && frontend.state().selection == 0,
                "Campaign Escape must return to a fresh main selection");
        for (const auto choice : {1, 3, 4, 5, 6}) {
            comanchecd::Frontend dispatch(assets(), picture, writer);
            dispatch.start();
            dispatch.consume({0x1c});
            for (int row = 0; row < choice; ++row) {
                dispatch.consume({0x50});
            }
            const auto request = dispatch.consume({0x1c});
            const auto expected = choice == 1 ? comanchecd::FrontendRequest::statistics :
                choice == 3 ? comanchecd::FrontendRequest::overview : choice == 4 ? comanchecd::FrontendRequest::demo :
                choice == 5 ? comanchecd::FrontendRequest::credits : comanchecd::FrontendRequest::exit;
            require(dispatch.state().mode == comanchecd::FrontendMode::request && request == expected,
                    "Main action must leave an explicit request for its external screen");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
