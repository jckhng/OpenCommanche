#include "controller_contacts.hpp"

namespace comanchecd {
bool scan_player_contacts(const FlightState& state, const MissionScene& scene, const CpuWorkSink& work) {
    const auto instructions = [&](unsigned count) { if (work) { work(CpuOperation::instructions, count); } };
    const auto magnitude = [](std::uint32_t value) { return value ^ std::uint32_t(static_cast<std::int32_t>(value) >> 31); };
    constexpr std::uint32_t contact_radius = 0x01000000;
    constexpr unsigned maximum_height_difference = 12;
    instructions(1);
    for (std::size_t slot = player_slot - 1; slot != 0; --slot) {
        const auto& entity = scene.entities[slot];
        instructions(2);
        if (!entity.active) { instructions(2); continue; }
        const auto half_height = (int(state.view.height) - int(entity.height)) >> 1;
        const auto height = unsigned(half_height ^ (half_height >> 31));
        instructions(7);
        if (height > maximum_height_difference) { instructions(2); continue; }
        instructions(6);
        if (magnitude(state.view.x - entity.x) > contact_radius) { instructions(2); continue; }
        instructions(6);
        if (magnitude(state.view.y - entity.y) > contact_radius) { instructions(2); continue; }
        instructions(2);
        if (entity.phase != EntityPhase::explosion) { instructions(1); return true; }
        instructions(2);
    }
    return false;
}
}
