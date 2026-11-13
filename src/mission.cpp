#include "comanchecd/mission.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace comanchecd {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t line_scan_limit = 160;
constexpr std::size_t briefing_width = 280;
constexpr std::size_t title_copy_bytes = 28;
constexpr std::size_t row_title_offset = 4;
constexpr std::size_t maximum_briefing_bytes = 2048;
constexpr std::uint8_t padding = 0xfc;

std::size_t find(const Bytes& bytes, std::size_t start, std::uint8_t value) {
    const auto result = std::find(bytes.begin() + start, bytes.end(), value);
    if (result == bytes.end()) {
        throw std::invalid_argument("Missing mission text delimiter");
    }
    return static_cast<std::size_t>(result - bytes.begin());
}

std::size_t next_line(const Bytes& bytes, std::size_t offset) {
    for (;;) {
        const auto end = find(bytes, offset, '\r');
        if (end - offset >= line_scan_limit) {
            throw std::invalid_argument("Mission line exceeds original scan bound");
        }
        offset = end + 1;
        if (offset < bytes.size() && bytes[offset] == '\n') {
            ++offset;
        }
        if (offset == bytes.size()) {
            throw std::invalid_argument("Truncated mission line");
        }
        if (bytes[offset] > '\r' && bytes[offset] != ';') {
            return offset;
        }
    }
}
}

CampaignMissions parse_campaign(const std::vector<std::uint8_t>& decoded) {
    if (decoded.empty()) {
        throw std::invalid_argument("Empty decoded campaign");
    }
    auto offset = find(decoded, 0, '}') + 1;
    CampaignMissions campaign;
    campaign.source = decoded;
    while (campaign.missions.size() < missions_per_campaign) {
        const auto open = std::find(decoded.begin() + offset, decoded.end(), '{');
        if (open == decoded.end()) {
            break;
        }
        const auto start = static_cast<std::size_t>(open - decoded.begin()) + 1;
        const auto end = find(decoded, start, '}');
        if (end - start > mission_menu_row_bytes - row_title_offset - 1) {
            throw std::invalid_argument("Mission title exceeds original row capacity");
        }
        MissionDefinition mission;
        mission.title.assign(decoded.begin() + start, decoded.begin() + end);
        const auto metadata = end + 1;
        mission.source_offset = metadata;
        if (metadata < decoded.size() && decoded[metadata] == '*') {
            mission.restricted = true;
            if (metadata + 1 < decoded.size() && decoded[metadata + 1] >= '0' && decoded[metadata + 1] <= '9') {
                mission.alternate_campaign = static_cast<std::uint8_t>(decoded[metadata + 1] - '0' + 1);
            }
        }
        const auto body = next_line(decoded, metadata);
        const auto delimiter = find(decoded, body, '<');
        mission.description.assign(decoded.begin() + body, decoded.begin() + delimiter);
        offset = delimiter + 1;
        for (;;) {
            offset = next_line(decoded, offset);
            if (decoded[offset] == '<') {
                break;
            }
            if (mission.resources.size() == mission_resource_count) {
                throw std::invalid_argument("Too many mission resources");
            }
            MissionFilename name;
            name.fill(' ');
            std::size_t length = 0;
            while (offset < decoded.size() && decoded[offset] > ' ' && decoded[offset] != ';' && length < name.size()) {
                name[length++] = decoded[offset++];
            }
            mission.resources.push_back(name);
        }
        const auto setup = offset + 1;
        const auto next = std::find(decoded.begin() + setup, decoded.end(), '{');
        // The original setup reader also accepts the following title's opening brace.
        mission.setup_text.assign(decoded.begin() + setup, next == decoded.end() ? next : next + 1);
        campaign.missions.push_back(std::move(mission));
        offset = static_cast<std::size_t>(next - decoded.begin());
    }
    if (campaign.missions.empty()) {
        throw std::invalid_argument("Campaign has no missions");
    }
    return campaign;
}

void populate_missions(MissionPresentation& presentation, const CampaignMissions& campaign) {
    constexpr auto required = mission_menu_header_bytes + missions_per_campaign * mission_menu_row_bytes + 3;
    if (presentation.menu.size() != required || campaign.missions.empty() || campaign.missions.size() > missions_per_campaign) {
        throw std::invalid_argument("Incomplete mission menu resources");
    }
    for (std::size_t row = 0; row <= campaign.missions.size(); ++row) {
        const auto offset = mission_menu_header_bytes + row * mission_menu_row_bytes;
        if (row == campaign.missions.size()) {
            std::copy_n(std::array<std::uint8_t, 3>{0, 0, 255}.begin(), 3, presentation.menu.begin() + offset);
            if (offset + 3 < presentation.menu.size()) {
                presentation.menu[offset + 3] = 0;
            }
            break;
        }
        const auto& definition = campaign.missions[row];
        if (definition.title.size() > mission_menu_row_bytes - row_title_offset - 1 ||
                definition.resources.size() > mission_resource_count) {
            throw std::invalid_argument("Mission definition exceeds its original storage bounds");
        }
        auto start = presentation.menu.begin() + offset;
        std::fill_n(start, mission_menu_row_bytes, padding);
        std::copy_n(std::array<std::uint8_t, 4>{'|', 'g', ' ', ' '}.begin(), 4, start);
        std::copy(definition.title.begin(), definition.title.end(), start + row_title_offset);
        start[mission_menu_row_bytes - 1] = 0;
        presentation.restricted[row] = definition.restricted ? 1 : 0;
        presentation.alternate[row] = definition.alternate_campaign;
    }
}

std::vector<std::uint8_t> wrap_briefing(const std::vector<std::uint8_t>& menu, std::uint8_t mission,
                                      const MissionDefinition& definition,
                                      const std::array<std::uint8_t, 256>& widths) {
    const auto title = mission_menu_header_bytes + mission * mission_menu_row_bytes + row_title_offset;
    if (title + title_copy_bytes > menu.size()) {
        throw std::invalid_argument("Missing briefing title row");
    }
    Bytes output{'|', 'b', 0};
    output.insert(output.end(), menu.begin() + title, menu.begin() + title + title_copy_bytes);
    output.insert(output.end(), {0, '|', 'g'});
    std::uint8_t previous = ' ';
    std::size_t width = 0;
    auto source_space = std::numeric_limits<std::size_t>::max();
    std::size_t destination_space = 0;
    for (std::size_t source = 0; source < definition.description.size();) {
        auto code = definition.description[source++];
        if (code == '\n') {
            continue;
        }
        if (code < ' ') {
            if (previous == ' ') {
                continue;
            }
            code = ' ';
        }
        if (code == ' ') {
            source_space = source;
            destination_space = output.size();
        }
        previous = code;
        if (code == '^') {
            code = 0;
            previous = ' ';
            width = 0;
            source_space = std::numeric_limits<std::size_t>::max();
        }
        output.push_back(code);
        width += widths[code];
        if (width > briefing_width) {
            width = 0;
            if (source_space != std::numeric_limits<std::size_t>::max()) {
                source = source_space;
                output.resize(destination_space);
            }
            source_space = std::numeric_limits<std::size_t>::max();
            output.push_back(0);
        }
        if (output.size() >= maximum_briefing_bytes) {
            throw std::invalid_argument("Briefing exceeds supported buffer");
        }
    }
    output.push_back(255);
    return output;
}

} // namespace comanchecd
