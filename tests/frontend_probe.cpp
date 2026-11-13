#include "comanchecd/frontend.hpp"
#include "setup_output.hpp"
#include "scene_output.hpp"
#include "world_output.hpp"
#include "world_runtime_probe.hpp"
#include "world_view_probe.hpp"
#include "audio_output.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Cannot write native checkpoint");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 12) {
            throw std::invalid_argument("Usage: frontend_probe INSTALLATION ACTIONS OUTPUT [first|second] [PAYLOAD_DIRECTORY] [SCENE_SEED] [AUDIO_CACHE_HEX] [ALLOCATION_SUFFIXES] [SAMPLE_REQUESTS] [WORLD_MOVIE] [WORLD_VIEWS]");
        }
        const std::string phase = argc >= 5 ? argv[4] : "first";
        if (phase != "first" && phase != "second") {
            throw std::invalid_argument("Unknown initial page phase");
        }
        const auto page = phase == "first" ? comanchecd::FrontendPage::first : comanchecd::FrontendPage::second;
        const comanchecd::Installation installation(argv[1], argc >= 6 ? argv[5] : std::filesystem::path{});
        const std::filesystem::path output(argv[3]);
        if (std::filesystem::exists(output)) {
            throw std::invalid_argument("Use a fresh native output directory");
        }
        std::filesystem::create_directories(output);
        const comanchecd::Installation profile(output);
        std::size_t checkpoint = 0, writes = 0;
        const auto frontend_assets = installation.load_frontend();
        comanchecd::Frontend frontend(frontend_assets,
            [&](comanchecd::FrontendScene scene) { return installation.load_picture(scene); },
            [&](const auto& record) {
                profile.save_pilots(record);
                write(output / ("save-" + std::to_string(writes++) + ".nam"), record);
            },
            [&](const auto& state, const auto& pilot, const auto& renderer) {
                const auto directory = output / ("step-" + std::to_string(checkpoint++));
                std::filesystem::create_directories(directory);
                write(directory / "planes.bin", renderer.surface().bytes());
                write(directory / "record.bin", pilot.storage().record());
                write(directory / "editor.bin", {pilot.editor_buffer().begin(), pilot.editor_buffer().end()});
                setup_output::write(directory / "setup", frontend.gameplay_setup());
                const auto& mission = frontend.mission_presentation();
                write(directory / "mission-menu.bin", mission.menu);
                write(directory / "briefing.bin", mission.briefing);
                std::vector<std::uint8_t> resources, metadata;
                for (const auto& name : mission.resources) {
                    resources.insert(resources.end(), name.begin(), name.end());
                }
                metadata.insert(metadata.end(), mission.restricted.begin(), mission.restricted.end());
                metadata.insert(metadata.end(), mission.colors.begin(), mission.colors.end());
                metadata.insert(metadata.end(), mission.alternate.begin(), mission.alternate.end());
                write(directory / "resources.bin", resources);
                write(directory / "mission-metadata.bin", metadata);
                std::ofstream(directory / "mission-state.txt") << unsigned(state.campaign) << ' '
                    << unsigned(state.committed_mission) << '\n';
                std::ofstream(directory / "state.txt") << unsigned(state.mode) << ' ' << unsigned(state.selection) << ' '
                    << unsigned(state.selected_pilot) << ' ' << unsigned(state.pilot_mode) << ' '
                    << unsigned(state.editor_length) << ' ' << unsigned(state.request) << ' '
                    << renderer.back_page() << '\n';
            }, [&](std::uint8_t campaign) { return installation.load_missions(campaign); }, page);
        frontend.start();
        std::ifstream actions(argv[2]);
        if (!actions) {
            throw std::runtime_error("Cannot read scan events");
        }
        unsigned scan, left, right;
        while (actions >> scan >> left >> right) {
            if (scan >= 128 || left > 1 || right > 1) {
                throw std::invalid_argument("Invalid scan event");
            }
            frontend.consume({static_cast<std::uint8_t>(scan), static_cast<comanchecd::PilotShift>(left | (right << 1))});
        }
        if (!actions.eof()) {
            throw std::invalid_argument("Malformed scan events");
        }
        std::ofstream(output / "writes.txt") << writes << '\n';
        if (frontend.state().request == comanchecd::FrontendRequest::launch_mission) {
            write(output / "launch-setup.bin", frontend.launch_definition().setup_text);
            if (argc >= 6) {
                auto payloads = comanchecd::load_gameplay(frontend.gameplay_setup(), frontend.mission_presentation().resources,
                    [&](const auto& name, auto kind) { return installation.load_payload(name, kind); });
                if (argc >= 9 && std::string(argv[8]) != "-") {
                    std::istringstream entries(argv[8]);
                    std::string entry;
                    std::vector<bool> supplied(payloads.size(), false);
                    while (std::getline(entries, entry, ',')) {
                        const auto separator = entry.find(':');
                        std::size_t consumed = 0;
                        const auto payload = std::stoul(entry.substr(0, separator), &consumed);
                        if (separator == std::string::npos || consumed != separator || payload >= payloads.size() || supplied[payload]) {
                            throw std::invalid_argument("Invalid allocation-tail payload index");
                        }
                        auto& suffix = payloads[payload].allocation_suffix;
                        const auto encoded = entry.substr(separator + 1);
                        if (suffix.empty() || encoded.size() != suffix.size() * 2 ||
                                encoded.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                            throw std::invalid_argument("Invalid allocation-tail byte count");
                        }
                        for (std::size_t byte = 0; byte < suffix.size(); ++byte) {
                            suffix[byte] = static_cast<std::uint8_t>(std::stoul(encoded.substr(byte * 2, 2), nullptr, 16));
                        }
                        supplied[payload] = true;
                    }
                }
                std::ofstream index(output / "payloads.txt");
                for (std::size_t payload = 0; payload < payloads.size(); ++payload) {
                    const auto& entry = payloads[payload];
                    const auto directory = output / "payloads" / std::to_string(payload);
                    std::filesystem::create_directories(directory);
                    write(directory / "bytes.bin", entry.bytes);
                    if (!entry.palette.empty()) {
                        write(directory / "palette.bin", entry.palette);
                    }
                    index << payload << ' ' << entry.name << ' ' << unsigned(entry.kind) << ' '
                          << entry.width << ' ' << entry.height << ' ' << entry.bytes.size() << '\n';
                }
                const auto seed = argc >= 7 ? std::stoull(argv[6], nullptr, 0) : 0x12345678ull;
                if (seed > 0xffffffffu) {
                    throw std::invalid_argument("Scene seed exceeds 32 bits");
                }
                const auto assets = comanchecd::materialize_scene(frontend.gameplay_setup(), payloads, frontend_assets.transfer);
                const auto scene = comanchecd::construct_scene(frontend.gameplay_setup(), assets, static_cast<std::uint32_t>(seed), installation.load_scene_entry());
                scene_output::write(output / "scene", assets, scene);
                const comanchecd::TerrainRenderer terrain_renderer(installation.load_terrain_tables());
                comanchecd::SceneView view;
                view.x = frontend.gameplay_setup().x;
                view.y = frontend.gameplay_setup().y;
                view.heading = frontend.gameplay_setup().heading;
                view.height = scene.player_height;
                auto terrain_frame = terrain_renderer.render(assets, view);
                world_output::write(output / "world", terrain_renderer, terrain_frame);
                if (argc == 11) {
                    terrain_renderer.shade_distance(terrain_frame, assets, frontend.gameplay_setup().terrain_variant);
                    world_output::bytes(output / "world/fog.bin", terrain_frame.pixels);
                    terrain_renderer.draw_models(terrain_frame, assets, scene, view);
                    world_output::bytes(output / "world/models.bin", terrain_frame.pixels);
                }
                comanchecd::AudioEntry audio_entry;
                if (argc >= 8) {
                    const std::string cache = argv[7];
                    constexpr std::size_t full_entry_bytes = 47;
                    if ((cache.size() != audio_entry.frequency_high.size() * 2 && cache.size() != full_entry_bytes * 2) ||
                            cache.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                        throw std::invalid_argument("Expected nine hexadecimal audio cache bytes");
                    }
                    for (std::size_t index = 0; index < audio_entry.frequency_high.size(); ++index) {
                        audio_entry.frequency_high[index] = static_cast<std::uint8_t>(std::stoul(cache.substr(index * 2, 2), nullptr, 16));
                    }
                    if (cache.size() == full_entry_bytes * 2) {
                        std::size_t offset = audio_entry.frequency_high.size();
                        for (auto& value : audio_entry.echo) {
                            value = static_cast<std::uint8_t>(std::stoul(cache.substr(offset++ * 2, 2), nullptr, 16));
                        }
                        for (std::size_t byte = 0; byte < 4; ++byte) {
                            audio_entry.echo_cursor |= std::uint32_t(std::stoul(cache.substr(offset++ * 2, 2), nullptr, 16)) << (byte * 8);
                        }
                        audio_entry.engine_power = static_cast<std::uint8_t>(std::stoul(cache.substr(offset++ * 2, 2), nullptr, 16));
                        audio_entry.music_volume = static_cast<std::uint8_t>(std::stoul(cache.substr(offset * 2, 2), nullptr, 16));
                    }
                }
                std::vector<comanchecd::SampleRequest> requests;
                if (argc >= 10 && std::string(argv[9]).size() != 0 && std::string(argv[9]) != "-") {
                    std::ifstream input(argv[9]);
                    if (!input) {
                        throw std::runtime_error("Cannot read sampled sound requests");
                    }
                    std::uint64_t sample;
                    unsigned bank, sound, voice, attenuation, mode;
                    std::uint32_t step;
                    while (input >> sample) {
                        if (!(input >> bank >> sound >> voice >> attenuation >> step >> mode)) {
                            throw std::invalid_argument("Truncated sampled sound request");
                        }
                        if (bank > 3 || sound > 65535 || voice > 2 || attenuation > 7 || mode > 1) {
                            throw std::invalid_argument("Invalid sampled sound request");
                        }
                        requests.push_back({sample, static_cast<comanchecd::SampleBank>(bank), static_cast<std::uint16_t>(sound),
                            static_cast<std::uint8_t>(voice), static_cast<std::uint8_t>(attenuation), step,
                            static_cast<comanchecd::SampleMode>(mode)});
                    }
                    if (!input.eof()) {
                        throw std::invalid_argument("Truncated sampled sound request");
                    }
                }
                comanchecd::MissionAudio audio(installation.load_audio(), payloads, audio_entry, std::move(requests));
                if (argc == 12) {
                    if (std::string(argv[10]) != "-") {
                        throw std::invalid_argument("Controlled views require an absent gameplay movie");
                    }
                    world_view_probe::run(argv[11], output, frontend.gameplay_setup(), assets, scene,
                                          installation.load_terrain_tables());
                } else if (argc == 11) {
                    world_runtime_probe::run(argv[10], output, frontend.gameplay_setup(), assets, scene,
                                             installation.load_terrain_tables(), payloads.size(), audio);
                } else {
                    audio_output::write(output / "audio", audio, 2);
                }
            }
        }
        std::cout << "Frontend checkpoints " << checkpoint << ", saves " << writes << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
