#include "../src/mission_work.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::vector<std::uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }

comanchecd::CampaignMissions synthetic_campaign() {
        const auto source = bytes("{Synthetic}\r\n{ABCDEFGHIJKLMNOPQRSTUVWXYZA}*1\r\n; comment\r\n\r\n" +
            std::string(141, 'A') + "^Word word\r\n<\r\nTERRAIN.DTA ; comment\r\n<\r\n"
            "A0\r\n12345678,2,3\r\n4,5\r\n0,1,2,3,4,5\r\n6\r\n1,1,1,1,1,1,1,1,30\r\n"
            "F,OVERLAY.DTA,1,2,3\r\nW1,2,3,4\r\nR1,2,3,4,5\r\nS1,2\r\n"
            "*3,2,0,0,1,2,3,4,5,6,7\r\n<\r\n"
            "{Second}*X\r\nB\r\n<\r\n<\r\n0\r\n1,2,3\r\n4,5\r\n0,0,0,0,0,0\r\n6\r\n"
            "1,1,1,1,1,1,1,1,30\r\nW1,2,3\r\nR1,2\r\nS1,2,3,4,5\r\n<\r\n");
        return comanchecd::parse_campaign(source);
}

std::uint64_t fingerprint(const std::function<void(const comanchecd::CpuWorkSink&)>& run) {
    std::uint64_t hash = 14695981039346656037ull;
    const auto value = [&](unsigned item) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash = (hash ^ ((item >> shift) & 255)) * 1099511628211ull;
        }
    };
    run([&](comanchecd::CpuOperation operation, unsigned count) {
        if (operation == comanchecd::CpuOperation::instructions) {
            for (unsigned index = 0; index < count; ++index) { value(unsigned(operation)); value(1); }
        } else { value(unsigned(operation)); value(count); }
    });
    return hash;
}
}

int main() {
    try {
        using namespace comanchecd;
        const auto campaign = synthetic_campaign();
        std::array<std::uint8_t, 256> widths;
        widths.fill(2);
        widths[0] = 0;
        MissionResources resources{};
        resources[2][2] = 'W';
        const auto catalog = fingerprint([&](const auto& work) { mission_work::catalog(campaign, work); });
        const auto first = fingerprint([&](const auto& work) { mission_work::briefing(campaign, 0, widths, resources, work); });
        const auto second = fingerprint([&](const auto& work) { mission_work::briefing(campaign, 1, widths, resources, work); });
        // Ordered operation fingerprints from bounded original CPU calls on this synthetic source.
        constexpr std::uint64_t original_catalog = 4812276759055078964ull,
            original_first = 1176478685488656370ull, original_second = 11760350893147078509ull;
        if (catalog != original_catalog || first != original_first || second != original_second) {
            throw std::runtime_error("Campaign and briefing work must retain original ordered CPU operations");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
