#include "mission_work.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace comanchecd::mission_work {
namespace {
constexpr unsigned line_scan_limit = 160;
constexpr unsigned number_scan_limit = 79;
constexpr unsigned number_digits = 8;
constexpr unsigned briefing_width = 280;
constexpr unsigned briefing_title_bytes = 28;
constexpr unsigned title_padding_bytes = mission_menu_row_bytes - 5;

class Reader {
public:
    Reader(const std::vector<std::uint8_t>& bytes, std::size_t position, const CpuWorkSink& work)
        : position(position), bytes_(bytes), work_(work) {}

    std::uint8_t peek() const { return bytes_.at(position); }
    void instructions(unsigned count) const { work_(CpuOperation::instructions, count); }
    void string() const { work_(CpuOperation::string_bytes, 1); }
    void scan(std::uint8_t value, unsigned limit) {
        const auto first = position;
        while (position < bytes_.size() && position - first < limit) {
            if (bytes_[position++] == value) {
                work_(CpuOperation::repeat_scan_bytes, static_cast<unsigned>(position - first));
                return;
            }
        }
        throw std::invalid_argument("Mission work scan exceeded source bounds");
    }
    void line() {
        // 2da8a..2daa7, including its calling CALL.
        instructions(2);
        for (;;) {
            instructions(2);
            scan('\r', line_scan_limit);
            instructions(3);
            if (peek() == '\n') { ++position; instructions(1); }
            instructions(2);
            if (peek() <= '\r') { continue; }
            instructions(2);
            if (peek() == ';') { continue; }
            instructions(2);
            return;
        }
    }
    std::uint32_t number() {
        // 2daaf..2dae7, including its calling CALL.
        instructions(6);
        for (unsigned skipped = 0;; ++skipped) {
            if (skipped == number_scan_limit) { throw std::invalid_argument("Mission work number scan exceeded bounds"); }
            instructions(6);
            if (peek() < '0') { ++position; continue; }
            instructions(2);
            if (peek() <= '9') { break; }
            ++position;
        }
        instructions(2);
        std::uint32_t value = 0;
        for (unsigned digit = 0; digit < number_digits; ++digit) {
            instructions(3);
            if (peek() < '0') { break; }
            value = value * 10 + (peek() & 15);
            ++position;
            instructions(5);
        }
        instructions(3);
        return value;
    }

    std::size_t position;
private:
    const std::vector<std::uint8_t>& bytes_;
    const CpuWorkSink& work_;
};
}

void catalog(const CampaignMissions& campaign, const CpuWorkSink& work) {
    if (!work || campaign.source.empty()) { return; }
    Reader reader(campaign.source, 0, work);
    const auto instructions = [&](unsigned n) { reader.instructions(n); };
    // 2d9d6..2da89 after the selected-MIS read and decode.
    instructions(6);
    reader.scan('}', static_cast<unsigned>(campaign.source.size()));
    instructions(3);
    for (unsigned index = 0; index < missions_per_campaign; ++index) {
        instructions(3);
        const auto open = std::find(campaign.source.begin() + reader.position, campaign.source.end(), '{');
        if (open == campaign.source.end()) {
            const auto remaining = static_cast<unsigned>(campaign.source.size() - reader.position);
            if (remaining) { work(CpuOperation::repeat_scan_bytes, remaining); }
            instructions(2);
            return;
        }
        reader.scan('{', static_cast<unsigned>(campaign.source.size() - reader.position));
        instructions(3);
        const auto title = reader.position;
        reader.scan('}', static_cast<unsigned>(campaign.source.size() - reader.position));
        instructions(6);
        if (reader.peek() == '*') {
            instructions(2);
            if (campaign.source.at(reader.position + 1) >= '0') {
                instructions(2);
                if (campaign.source.at(reader.position + 1) <= '9') { instructions(4); }
            }
        }
        instructions(11);
        const auto length = static_cast<unsigned>(reader.position - title - 1);
        work(CpuOperation::repeat_copy_bytes, length);
        instructions(5);
        const auto padding = title_padding_bytes - length;
        if (padding) {
            instructions(2);
            work(CpuOperation::repeat_store_bytes, padding);
            instructions(1);
            reader.string();
        }
        instructions(3);
    }
    instructions(1);
}

void briefing(const CampaignMissions& campaign, unsigned mission,
    const std::array<std::uint8_t, 256>& widths, const MissionResources& resources, const CpuWorkSink& work) {
    if (!work || campaign.source.empty()) { return; }
    Reader reader(campaign.source, campaign.missions.at(mission).source_offset, work);
    const auto instructions = [&](unsigned n) { reader.instructions(n); };
    // 2c5ba..2cb56. Typed parsing owns state, this pass owns the original CPU work.
    instructions(2); reader.string(); instructions(1); reader.string(); instructions(6);
    work(CpuOperation::repeat_copy_bytes, briefing_title_bytes);
    instructions(1); reader.string(); instructions(1); reader.string(); instructions(3);
    reader.line();
    instructions(5);
    unsigned width = 0;
    std::uint8_t previous = ' ';
    auto space = std::numeric_limits<std::size_t>::max();
    for (;;) {
        auto code = reader.peek(); ++reader.position; reader.string(); instructions(2);
        if (code == '<') { break; }
        instructions(2);
        if (code == '\n') { continue; }
        instructions(2);
        if (code < ' ') {
            instructions(2);
            if (previous == ' ') { continue; }
            code = ' '; instructions(1);
        }
        instructions(2);
        if (code == ' ') { space = reader.position; instructions(2); }
        previous = code; instructions(3);
        if (code == '^') {
            code = 0; previous = ' '; width = 0;
            space = std::numeric_limits<std::size_t>::max(); instructions(4);
        }
        reader.string(); instructions(6);
        width += widths[code];
        if (width <= briefing_width) { continue; }
        width = 0; instructions(3);
        if (space != std::numeric_limits<std::size_t>::max()) { reader.position = space; instructions(2); }
        space = std::numeric_limits<std::size_t>::max(); instructions(2); reader.string(); instructions(1);
    }
    instructions(1); reader.string(); instructions(4 + 15 * 5 + 3);
    for (unsigned resource = 0; resource < mission_resource_count; ++resource) {
        instructions(1); reader.line(); instructions(3);
        if (reader.peek() == '<') { break; }
        instructions(2);
        unsigned remaining = mission_filename_bytes;
        while (remaining) {
            instructions(3);
            if (reader.peek() <= ' ') { break; }
            instructions(2);
            if (reader.peek() == ';') { break; }
            ++reader.position; --remaining; instructions(4);
        }
        instructions(3 + remaining * 3 + 3);
        if (resource + 1 == mission_resource_count) { instructions(1); }
    }
    instructions(2); reader.line(); instructions(2);
    if (reader.peek() == 'A') { ++reader.position; instructions(2); }
    reader.number(); instructions(5);
    reader.line(); reader.number(); instructions(3); reader.number(); instructions(3); reader.number(); instructions(2);
    reader.line(); reader.number(); instructions(1); reader.number(); instructions(1);
    reader.line(); instructions(2);
    for (unsigned value = 0; value < 6; ++value) { reader.number(); instructions(3); }
    reader.line(); reader.number(); instructions(1);
    reader.line(); instructions(2);
    for (unsigned value = 0; value < 8; ++value) { reader.number(); instructions(4); }
    reader.number(); instructions(11 + 16);
    const auto& terrain = resources[2];
    if (terrain[2] == 'w' || terrain[2] == 'W' || terrain[3] == 'w' || terrain[3] == 'W') { instructions(1); }
    instructions(7);
    for (;;) {
        reader.line();
        const auto raw = reader.peek();
        const auto code = raw >= 0x60 ? static_cast<std::uint8_t>(raw - 0x20) : raw;
        instructions(7);
        if (code == 'F') {
            instructions(11); reader.position += 2;
            for (unsigned length = 0;; ++length) {
                instructions(3);
                if (reader.peek() == ',') { break; }
                if (length == mission_filename_bytes) { throw std::invalid_argument("Mission work overlay exceeds filename bounds"); }
                ++reader.position; instructions(5);
            }
            reader.number(); instructions(3); reader.number(); instructions(6);
            if (reader.peek() == ',') { reader.number(); instructions(3); }
            continue;
        }
        instructions(2);
        if (code == 'W') {
            reader.number(); instructions(4); reader.number(); instructions(2); reader.number(); instructions(8);
            if (reader.peek() == ',') { reader.number(); instructions(3); }
            continue;
        }
        instructions(3);
        if (code != 'R') { instructions(2); }
        if (code == 'R' || code == 'S') {
            if (code == 'S') { instructions(1); }
            instructions(5); reader.number(); instructions(2); reader.number(); instructions(8);
            if (reader.peek() == ',') {
                instructions(2);
                for (unsigned channel = 0; channel < 3; ++channel) { reader.number(); instructions(1); }
                instructions(1);
            }
            continue;
        }
        instructions(2);
        if (code == '*') { instructions(4); }
        else {
            instructions(3);
            if (code < '0') { return; }
            instructions(2);
            if (code > '9') { return; }
        }
        instructions(2); reader.number(); instructions(4); reader.number(); instructions(7);
        reader.number(); instructions(1); reader.number(); instructions(1);
        reader.number(); instructions(2); reader.number(); instructions(2);
        for (unsigned field = 0; field < 5; ++field) { reader.number(); instructions(1); }
        instructions(2);
    }
}
}
