#include "../src/controller_contacts.hpp"
#include "../src/controller_contact_response.hpp"
#include "loading_clock.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace { void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } } }
int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        const auto response = argc == 4 && std::string(argv[1]) == "--response";
        if (argc == 3 || response) {
            std::ifstream input(argv[response ? 2 : 1], std::ios::binary);input.exceptions(std::ios::failbit | std::ios::badbit);
            std::ofstream output(argv[response ? 3 : 2]);
            const auto integer = [&] { std::array<unsigned char,4> b{};input.read(reinterpret_cast<char*>(b.data()),b.size());return std::uint32_t(b[0])|std::uint32_t(b[1])<<8|std::uint32_t(b[2])<<16|std::uint32_t(b[3])<<24; };
            const auto count = integer();require(count < 10000, "Too many contact work cases");
            for (unsigned i = 0; i < count; ++i) {
                if (response) {
                    FlightState state;
                    state.view.x = integer(); state.view.y = integer();
                    state.view.height = static_cast<std::uint8_t>(integer());
                    state.side_speed = static_cast<std::int32_t>(integer());
                    state.forward_speed = static_cast<std::int32_t>(integer());
                    state.armor = static_cast<std::int32_t>(integer());
                    state.exit_requested = static_cast<std::uint8_t>(integer());
                    for (auto& value : state.system_condition) { value = static_cast<std::uint8_t>(integer()); }
                    MissionScene scene; scene.random_seed = integer();
                    SceneView previous; previous.x = integer(); previous.y = integer();
                    const auto height = static_cast<std::uint8_t>(integer());
                    std::vector<SoundCue> cues;
                    LoadingClock clock;
                    respond_to_contact(state, scene, previous, height, cues,
                        [&](CpuOperation op, unsigned n) { clock.execute(op, n); }, &clock);
                    const auto cost = clock.accounting();
                    output << cost.dispatches << ' ' << state.view.x << ' ' << state.view.y << ' '
                           << unsigned(state.view.height) << ' ' << state.side_speed << ' ' << state.forward_speed << ' '
                           << state.armor << ' ' << unsigned(state.exit_requested) << ' ' << scene.random_seed << ' '
                           << state.shake_ticks;
                    for (auto value : state.system_condition) { output << ' ' << unsigned(value); }
                    output << ' ' << cues.size() << '\n';
                    continue;
                }
                FlightState state;state.view.x=integer();state.view.y=integer();state.view.height=static_cast<std::uint8_t>(integer());
                MissionScene scene;
                for (std::size_t slot = 1; slot < player_slot; ++slot) {
                    auto& entity=scene.entities[slot];entity.active=integer()!=0;entity.phase=integer()!=0?EntityPhase::explosion:EntityPhase::normal;
                    entity.x=integer();entity.y=integer();entity.height=static_cast<std::uint8_t>(integer());
                }
                std::uint64_t work=0;const auto hit=scan_player_contacts(state,scene,[&](CpuOperation op,unsigned count){require(op==CpuOperation::instructions,"Contact scan has no I/O or strings");work+=count;});
                output<<work<<' '<<hit<<'\n';
            }
            require(bool(output),"Cannot write contact work");return 0;
        }
        require(argc==1,"Usage: controller_work_tests [INPUT OUTPUT]");
        FlightState state;MissionScene scene;std::uint64_t work=0;
        const CpuWorkSink sink=[&](CpuOperation,unsigned count){work+=count;};
        require(!scan_player_contacts(state,scene,sink)&&work==241,"Empty scan must visit all 60 slots");
        scene.entities[60].active=true;work=0;
        require(scan_player_contacts(state,scene,sink)&&work==25,"First overlapping object must branch before the loop decrement");
        scene.entities[60].phase=EntityPhase::explosion;work=0;
        require(!scan_player_contacts(state,scene,sink)&&work==262,"Explosion rejection must follow the three geometric checks");
        state.side_speed = 8; state.forward_speed = 9; state.armor = 1;
        scene.random_seed = 0x00000302;
        std::vector<SoundCue> cues;
        work = 0;
        respond_to_contact(state, scene, {}, 253, cues, sink);
        require(work == 33 && state.view.height == 255 && state.side_speed == -8 && state.forward_speed == -5 &&
                state.armor == -2 && state.exit_requested == 1 && cues.empty(),
                "Contact response must retain overflow, signed damping and rotated damage bits");
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
