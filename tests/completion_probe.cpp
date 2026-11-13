#include "comanchecd/frontend.hpp"
#include "comanchecd/training.hpp"
#include "comanchecd/session.hpp"
#include "profile.hpp"

#include <fstream>
#include <iostream>

namespace {
constexpr unsigned route_frame_limit = 4096;
void checkpoint(std::ostream& output, int frame, const comanchecd::FlightState& state,
                const comanchecd::TrainingFlight* flight = nullptr) {
    output << "{\"frame\":" << frame << ",\"state\":{";
    bool first = true;
    const auto field = [&](const std::string& name, std::int64_t value) {
        output << (first ? "" : ",") << '"' << name << "\":" << value;
        first = false;
    };
    field("x", state.view.x); field("y", state.view.y);
    field("heading", state.view.heading); field("height", state.view.height);
    field("pitch", state.view.pitch); field("roll", state.view.roll);
    field("forward_speed", state.forward_speed); field("side_speed", state.side_speed);
    field("yaw_rate", state.yaw_rate); field("range", state.range);
    field("control_power", state.control_power); field("previous_power", state.previous_power);
    field("engine_power", state.engine_power); field("height_gap", state.height_gap);
    field("previous_height_gap", state.previous_height_gap);
    for (std::size_t slot = 0; slot < state.ammunition.size(); ++slot) {
        field("ammunition_" + std::to_string(slot), state.ammunition[slot]);
    }
    for (std::size_t slot = 0; slot < state.system_condition.size(); ++slot) {
        field("system_" + std::to_string(slot), state.system_condition[slot]);
    }
    field("armor", state.armor); field("shake_ticks", state.shake_ticks);
    field("shake_offset", state.shake_offset); field("crash_rotation", state.crash_rotation);
    field("cockpit_mode", state.cockpit_mode); field("exit_countdown", state.exit_countdown);
    field("exit_requested", state.exit_requested); field("weapon", state.weapon);
    field("target_slot", state.target.slot);
    if (flight != nullptr) {
        field("objectives_remaining", flight->objectives_remaining());
        const auto objectives = flight->objectives();
        for (std::size_t index = 0; index < objectives.size(); ++index) {
            field("objective_" + std::to_string(index + 1), static_cast<unsigned>(objectives[index]));
        }
    }
    output << "}}\n";
}
void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        throw std::runtime_error("Cannot write mission completion checkpoint");
    }
}
}

int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        if (argc != 7) {
            throw std::invalid_argument("Usage: completion_probe INSTALLATION PAYLOAD_DIRECTORY SEED CONTROLS ACTIONS OUTPUT");
        }
        const Installation installation(argv[1], argv[2]);
        const std::filesystem::path output(argv[6]);
        if (std::filesystem::exists(output)) {
            throw std::invalid_argument("Use a fresh completion output directory");
        }
        std::filesystem::create_directories(output);
        const host::PilotProfile profile(output / "profile");
        const host::PilotProfile session_profile(output / "session-profile");
        unsigned session_writes = 0;
        GameSession session(installation, [&](const auto& record) {
            session_profile.write(record);
            ++session_writes;
        });
        std::vector<std::uint8_t> saved, observed;
        Palette palette{};
        unsigned writes = 0;
        auto assets = installation.load_frontend();
        std::unique_ptr<Frontend> frontend;
        frontend = std::make_unique<Frontend>(assets, [&](FrontendScene scene) {
            constexpr std::size_t success_picture = 7, failure_picture = 9;
            auto picture = scene == FrontendScene::debrief || scene == FrontendScene::debrief_success ?
                installation.load_mission_picture(frontend->mission_presentation().resources[
                    scene == FrontendScene::debrief_success ? success_picture : failure_picture]) :
                installation.load_picture(scene);
            palette = picture.palette;
            return picture;
        }, [&](const auto& record) {
            profile.write(record);
            saved = profile.read();
            if (saved != record) {
                throw std::runtime_error("Pilot save did not survive its storage round trip");
            }
            write(output / ("save-" + std::to_string(writes++) + ".bin"), saved);
        }, [&](const auto& state, const auto& pilot, const auto& renderer) {
            observed = pilot.storage().record();
            if (state.mode == FrontendMode::debrief) {
                write(output / "debrief-planes.bin", renderer.surface().bytes());
                std::ofstream(output / "page.txt") << renderer.back_page();
            }
        }, [&](std::uint8_t campaign) { return installation.load_missions(campaign); });
        frontend->start();
        std::ifstream actions(argv[5]);
        unsigned scan = 0;
        while (actions >> scan) {
            if (scan >= 128) {
                throw std::invalid_argument("Invalid frontend scan");
            }
            frontend->consume({static_cast<std::uint8_t>(scan)});
            session.consume({static_cast<std::uint8_t>(scan)});
        }
        if (!actions.eof() || frontend->state().request != FrontendRequest::launch_mission) {
            throw std::invalid_argument("The declared prefix must reach mission launch");
        }
        write(output / "pilots-entry.bin", observed);
        const auto selected = frontend->state();
        const PilotStorage entry_pilots(observed);
        const bool previously_completed = entry_pilots.completed(selected.selected_pilot, selected.campaign,
                                                               selected.committed_mission);
        FlightMission mission{frontend->gameplay_setup(), frontend->mission_presentation().resources,
                              installation.load_menu_settings().record()};
        TrainingFlight flight(installation, std::move(mission), std::stoul(argv[3], nullptr, 0));
        std::ofstream(output / "mission.json") << "{\"campaign\":" << unsigned(selected.campaign)
            << ",\"mission\":" << unsigned(selected.committed_mission) << "}\n";
        std::ofstream states(output / "checkpoints.jsonl");
        checkpoint(states, -1, flight.state(), &flight);
        std::ofstream session_states(output / "session-checkpoints.jsonl");
        std::ofstream session_clock(output / "session-clock.jsonl");
        const auto clock_checkpoint = [&](int frame) {
            session_clock << "{\"frame\":" << frame << ",\"cycle\":" << session.cpu_cycles()
                          << ",\"irq\":" << session.irq_ticks() << ",\"state\":{\"random_seed\":"
                          << session.random_seed() << "}}\n";
        };
        checkpoint(session_states, -1, session.flight_state());
        clock_checkpoint(-1);
        std::ifstream controls(argv[4]);
        FlightControls input;
        unsigned throttle, fire, weapon, target;
        unsigned frame = 0;
        while (controls >> input.horizontal) {
            if (!(controls >> input.vertical >> throttle >> fire >> weapon >> target) ||
                throttle > 15 || fire > 1 || target > 1 || weapon < static_cast<unsigned>(WeaponSelection::previous) ||
                weapon > static_cast<unsigned>(WeaponSelection::unchanged) || frame >= route_frame_limit) {
                throw std::invalid_argument("Invalid complete-mission control stream");
            }
            input.throttle = static_cast<std::uint8_t>(throttle);
            input.fire = fire != 0;
            input.weapon = static_cast<WeaponSelection>(weapon);
            input.cycle_target = target != 0;
            flight.advance(input);
            flight.render_audio(training_samples_per_tick);
            session.advance(input, {});
            session.render_audio(training_samples_per_tick);
            checkpoint(states, static_cast<int>(frame), flight.state(), &flight);
            checkpoint(session_states, static_cast<int>(frame), session.flight_state());
            clock_checkpoint(static_cast<int>(frame++));
            if (flight.state().ended) {
                break;
            }
        }
        if (!controls.eof() && !flight.state().ended) {
            throw std::invalid_argument("Truncated mission controls");
        }
        std::cout << "Frames " << frame << "; ended " << flight.state().ended
                  << "; objectives " << unsigned(flight.objectives_remaining()) << '\n';
        if (!flight.state().ended) {
            return 2;
        }
        const auto result = flight.result();
        frontend->finish_mission(result);
        std::ofstream summary(output / "summary.txt");
        summary << result.survived << ' ' << result.completed << ' ' << result.score << ' ' << result.enemies_destroyed;
        for (const auto value : result.percentages) {
            summary << ' ' << value;
        }
        summary << ' ' << unsigned(result.message) << '\n';
        write(output / "debrief-text.bin", debrief_text(assets.debrief, result));
        write(output / "pilots-debrief.bin", saved);
        std::vector<std::uint8_t> palette_bytes;
        for (const auto& color : palette) {
            palette_bytes.insert(palette_bytes.end(), color.begin(), color.end());
        }
        write(output / "debrief-palette.bin", palette_bytes);
        frontend->consume({0x01});
        if (frontend->state().mode != FrontendMode::main || profile.read() != saved) {
            throw std::runtime_error("Debrief return changed the saved pilot or failed to reach operations");
        }
        write(output / "pilots-return.bin", observed);
        std::ofstream(output / "writes.txt") << writes;
        if (session.flying() || session.menu_state().mode != FrontendMode::debrief) {
            throw std::runtime_error("GameSession did not reach debrief with the retained control route");
        }
        write(output / "session-debrief-indexed.bin", {session.image().pixels.begin(), session.image().pixels.end()});
        std::vector<std::uint8_t> session_palette;
        for (const auto& color : session.image().palette) {
            session_palette.insert(session_palette.end(), color.begin(), color.end());
        }
        write(output / "session-debrief-palette.bin", session_palette);
        session.consume({0x01});
        if (session.menu_state().mode != FrontendMode::main || session_writes != 1 || session_profile.read() != saved) {
            throw std::runtime_error("GameSession return/save differs from the composed frontend/flight route");
        }
        GameSession reloaded(installation, [](const auto&) {}, session_profile.read());
        const PilotStorage persisted(session_profile.read());
        const bool completed = persisted.completed(persisted.selected(), selected.campaign, selected.committed_mission);
        if (reloaded.menu_state().selected_pilot != session.menu_state().selected_pilot ||
            completed != (previously_completed || (result.survived && result.completed))) {
            throw std::runtime_error("A new GameSession did not retain the saved selection and mission completion");
        }
        std::ofstream(output / "session-writes.txt") << session_writes;
        std::ofstream(output / "session-reload.json") << "{\"selected_pilot\":"
            << unsigned(reloaded.menu_state().selected_pilot) << ",\"campaign\":" << unsigned(selected.campaign)
            << ",\"mission\":" << unsigned(selected.committed_mission)
            << ",\"mission_completed\":" << (completed ? "true" : "false") << ",\"reload_equal\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
