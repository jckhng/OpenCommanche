#include "comanchecd/assets.hpp"
#include "audio_output.hpp"

#include <algorithm>
#include <iostream>
#ifdef COMANCHECD_SDL2_AUDIO
#include "sdl_audio.hpp"
#endif

int main(int argc, char** argv) {
    try {
        if (argc < 6 || argc > 8) {
            throw std::invalid_argument("Usage: audio_probe INSTALLATION PAYLOAD_DIRECTORY CAMPAIGN|menu|carry MISSION OUTPUT [SECONDS] [--play]");
        }
        const auto carry = std::string(argv[3]) == "carry";
        const auto menu = carry || std::string(argv[3]) == "menu";
        const auto campaign = menu ? 0 : std::stoul(argv[3]);
        const auto selected = std::stoul(argv[4]);
        const auto seconds = argc >= 7 ? std::stoul(argv[6]) : 5;
        if (campaign > 9 || selected > 9 || seconds == 0 || seconds > 60 ||
                (argc == 8 && std::string(argv[7]) != "--play")) {
            throw std::invalid_argument("Unsupported audio audition arguments");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        auto assets = installation.load_frontend();
        auto resources = assets.mission_resources;
        const auto missions = installation.load_missions(static_cast<std::uint8_t>(campaign));
        const auto& mission = missions.missions.at(selected);
        for (std::size_t index = 11; index < resources.size(); ++index) {
            resources[index][0] = '-';
        }
        std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
        const auto setup = comanchecd::parse_gameplay(mission, resources, assets.gameplay_presets, {});
        const auto payloads = comanchecd::load_gameplay(setup, resources,
            [&](const auto& name, auto kind) { return installation.load_payload(name, kind); });
        const std::filesystem::path output(argv[5]);
        if (std::filesystem::exists(output)) {
            throw std::invalid_argument("Use a fresh audio output directory");
        }
        auto audio = menu ? std::make_unique<comanchecd::MissionAudio>(installation.load_audio(), installation.load_menu_music()) :
                            std::make_unique<comanchecd::MissionAudio>(installation.load_audio(), payloads);
        std::vector<std::int16_t> samples;
        if (carry) {
            const auto append = [&](std::size_t count) {
                const auto block = audio->render(count);
                samples.insert(samples.end(), block.begin(), block.end());
            };
            append(35119);
            audio->enter_mission(payloads, 15);
            append(57831);
            audio->suspend_engine();
            audio->set_music_level(comanchecd::MusicLevel::off);
            append(21207);
            audio->set_music_level(comanchecd::MusicLevel::loud, comanchecd::MusicChange::loaded);
            audio->resume_engine(15);
            append(69017);
            audio->leave_mission(installation.load_menu_music());
            append(87221);
        } else {
            samples = audio->render(seconds * comanchecd::MissionAudio::sample_rate);
        }
        audio_output::write_samples(output, *audio, samples);
        if (carry) {
            std::ofstream music_file(output / "mission-music.bin", std::ios::binary);
            const auto& bytes = payloads[payloads.size() - 5].bytes;
            music_file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!music_file) {
                throw std::runtime_error("Cannot write transition music fixture");
            }
        }
        if (argc == 8) {
#ifdef COMANCHECD_SDL2_AUDIO
            comanchecd::host::SdlAudioOutput device;
            device.play_blocking(samples);
#else
            throw std::runtime_error("Live audition requires COMANCHECD_BUILD_SDL2_HOST=ON");
#endif
        }
        std::cout << "Rendered " << samples.size() << " samples of audio\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
