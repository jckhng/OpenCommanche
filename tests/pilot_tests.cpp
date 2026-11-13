#include "comanchecd/pilot.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

comanchecd::PilotAssets assets() {
    comanchecd::PilotAssets data{};
    data.editor_prompt.resize(54, ' ');
    data.editor_prompt.back() = 255;
    data.glyph_widths.fill(2);
    data.glyph_widths['W'] = 10;
    data.glyph_widths['M'] = 8;
    data.normal_keys[0x17] = data.shifted_keys[0x17] = 'i';
    data.normal_keys[0x2d] = data.shifted_keys[0x2d] = 'x';
    data.normal_keys[0x1e] = 'a';
    data.shifted_keys[0x1e] = 'A';
    data.normal_keys[0x11] = 'w';
    data.shifted_keys[0x11] = 'W';
    data.normal_keys[0x32] = 'm';
    data.shifted_keys[0x32] = 'M';
    data.row_prefix.fill(0xfc);
    data.row_tail.fill(0xfc);
    data.row_tail.back() = 0;
    return data;
}

std::vector<std::uint8_t> record() {
    std::vector<std::uint8_t> data(comanchecd::pilot_record_bytes);
    for (std::size_t slot = 0; slot < comanchecd::pilot_count; ++slot) {
        data[slot * 43 + 9] = ' ';
    }
    return data;
}
}

int main() {
    try {
        const auto baseline = record();
        comanchecd::PilotStorage ranked(baseline);
        unsigned ranking_instructions = 0, ranking_stores = 0;
        ranked.normalize(0, [&](comanchecd::CpuOperation operation, unsigned count) {
            if (operation == comanchecd::CpuOperation::instructions) { ranking_instructions += count; }
            else if (operation == comanchecd::CpuOperation::string_bytes) { ranking_stores += count; }
            else { throw std::runtime_error("Unexpected ranking operation"); }
        });
        require(ranking_instructions == 1813 && ranking_stores == 245,
                "Ranking must retain seven swaps and format all eight score rows");
        comanchecd::PilotController cancelled(comanchecd::PilotStorage(baseline), assets(), 3);
        require(cancelled.consume({0x01}) == comanchecd::PilotUpdate::exit_prompt,
                "Escape must return the pilot list to its settings caller");
        cancelled.resume();
        require(cancelled.mode() == comanchecd::PilotMode::list && cancelled.selection() == 3 &&
                cancelled.storage().record() == baseline,
                "Closing pilot settings must restore the pending selection without committing it");
        comanchecd::PilotController controller(comanchecd::PilotStorage(baseline), assets(), 2);
        controller.consume({0x48});
        controller.consume({0x48});
        controller.consume({0x48});
        require(controller.selection() == 7, "Up must wrap zero to seven");
        controller.consume({0x0f});
        require(controller.selection() == 0, "Tab must move to the next pilot");
        controller.consume({0x0f, comanchecd::PilotShift::left});
        require(controller.selection() == 7, "Shift Tab must move to the previous pilot");
        controller.consume({0x50});
        require(controller.selection() == 0, "Down must wrap seven to zero");
        require(controller.consume({0x1c}) == comanchecd::PilotUpdate::editor_open,
                "Enter on a leading-space name must open the editor");
        require(controller.consume({0x0e}) == comanchecd::PilotUpdate::ignored,
                "Backspace at zero must not redraw");
        controller.consume({0x1e, comanchecd::PilotShift::left});
        require(controller.editor_buffer()[0] == 'A', "Left Shift must select the shifted table");
        controller.consume({0x01});
        require(controller.storage().record() == baseline, "Escape must preserve every pilot byte");
        controller.consume({0x53});
        controller.consume({0x1c});
        require(controller.storage().record() == baseline, "Empty Enter must cancel without clearing progress");
        controller.consume({0x53});
        for (unsigned count = 0; count < 22; ++count) {
            controller.consume({0x17});
        }
        controller.consume({0x2d});
        require(controller.editor_length() == 22 && controller.editor_buffer()[22] == '_',
                "Capacity rejection must redraw the unchanged cursor");
        controller.consume({0x1c});
        require(controller.storage().record()[9] == 'i' && controller.storage().record()[30] == 'i',
                "Commit must copy all twenty-two name bytes");
        controller.consume({0x53});
        for (unsigned count = 0; count < 11; ++count) {
            controller.consume({0x11, comanchecd::PilotShift::right});
        }
        require(controller.editor_length() == 10, "Names at or above 108 pixels must reject the candidate");
        controller.consume({0x32, comanchecd::PilotShift::left});
        require(controller.editor_length() == 10, "Exactly 108 pixels must reject, not only larger widths");
        controller.consume({0x0e});
        require(controller.editor_length() == 9 && controller.editor_buffer()[9] == '_' &&
                controller.editor_buffer()[10] == 0xfc, "Backspace must remove the old cursor as well as the character");

        auto tied = record();
        tied[0x1da] = 1;
        tied[0x1de] = 5;
        tied[40] = 0xa1;
        tied[43 + 40] = 0xb2;
        comanchecd::PilotStorage storage(tied);
        const auto selected = storage.normalize(1);
        require(selected == 0 && storage.record()[0x17f] == 0,
                "Sorting must carry the highlighted pilot to the new slot");
        require(storage.record()[40] == 0xa1 && storage.record()[43 + 40] == 0xb2,
                "Sort must preserve the unswapped three-byte row tails");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
