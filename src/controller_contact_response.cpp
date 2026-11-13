#include "controller_contact_response.hpp"

namespace comanchecd {
void respond_to_contact(FlightState& state, MissionScene& scene, const SceneView& previous,
                        std::uint8_t previous_height, std::vector<SoundCue>& cues,
                        const CpuWorkSink& work, ExecutionClock* clock, const SoundCueHandler& sound_handler) {
    const auto instructions = [&](unsigned n) { if (work) { work(CpuOperation::instructions, n); } };
    const auto instruction = [&](const std::function<void()>& action) {
        if (clock) { clock->instruction(action); }
        else { instructions(1); action(); }
    };
    instructions(6);
    state.view.x = previous.x;
    state.view.y = previous.y;
    state.view.height = previous_height;
    instructions(2);
    if (state.exit_requested != 0) { return; }
    constexpr unsigned contact_rise = 4, contact_shake_ticks = 10;
    instruction([&] { state.side_speed = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(state.side_speed)); });
    instruction([&] { state.forward_speed = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(state.forward_speed)); });
    instruction([&] { state.forward_speed >>= 1; });
    instruction([&] { state.shake_ticks = contact_shake_ticks; });
    instruction([&] { state.view.height = static_cast<std::uint8_t>(previous_height + contact_rise); });
    instructions(1);
    if (previous_height + contact_rise > 255) { instruction([&] { state.view.height = 255; }); }
    std::uint8_t sampled = 0;
    instruction([&] { sampled = static_cast<std::uint8_t>(scene.random_seed); });
    instructions(3);
    if ((sampled & 3) < 2) {
        instructions(2);
        cues.push_back({SampleBank::speech, static_cast<std::uint16_t>((sampled & 3) + 2)});
        if (sound_handler) { sound_handler(cues.back()); }
    }
    std::uint32_t x = 0;
    instruction([&] { x = state.view.x; });
    instruction([&] { scene.random_seed += x; });
    instruction([&] { sampled = static_cast<std::uint8_t>(scene.random_seed); });
    instructions(2);
    instruction([&] { --state.system_condition[sampled & 7]; });
    instructions(1);
    instruction([&] { --state.system_condition[sampled >> 5]; });
    instruction([&] { scene.random_seed = (scene.random_seed >> 8) | (scene.random_seed << 24); });
    instruction([&] { sampled = static_cast<std::uint8_t>(scene.random_seed); });
    instructions(1);
    instruction([&] {
        state.armor = static_cast<std::int32_t>(static_cast<std::uint32_t>(state.armor) - (sampled & 3));
    });
    instructions(1);
    if (state.armor < 0) { instruction([&] { state.exit_requested = 1; }); }
}
}
