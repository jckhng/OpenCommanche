#include "comanchecd/training.hpp"
#include "world_runtime_probe.hpp"

#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::invalid_argument("Usage: mission_catalog_probe INSTALLATION PAYLOAD_DIRECTORY TICKS OUTPUT");
        }
        const comanchecd::Installation installation(argv[1], argv[2]);
        const auto ticks = std::stoul(argv[3]);
        const std::filesystem::path output(argv[4]);
        if (ticks == 0 || ticks > 2000 || std::filesystem::exists(output)) {
            throw std::invalid_argument("Use 1..2000 ticks and a fresh output directory");
        }
        std::filesystem::create_directories(output);
        const auto frontend = installation.load_frontend();
        unsigned failures = 0, total = 0;
        std::ofstream report(output / "catalog.txt");
        for (unsigned campaign = 0; campaign < frontend.campaign_count; ++campaign) {
            const auto missions = installation.load_missions(static_cast<std::uint8_t>(campaign));
            for (std::size_t index = 0; index < missions.missions.size(); ++index) {
                ++total;
                const auto& mission = missions.missions[index];
                const auto identity = std::to_string(campaign) + "-" + std::to_string(index);
                report << identity << ' ' << std::string(mission.title.begin(), mission.title.end());
                try {
                    auto resources = frontend.mission_resources;
                    for (std::size_t slot = 11; slot < resources.size(); ++slot) {
                        resources[slot][0] = '-';
                    }
                    std::copy(mission.resources.begin(), mission.resources.end(), resources.begin());
                    const auto setup = comanchecd::parse_gameplay(mission, resources, frontend.gameplay_presets, {});
                    comanchecd::TrainingFlight flight(installation, {setup, resources});
                    const auto directory = output / identity;
                    std::filesystem::create_directories(directory);
                    world_output::bytes(directory / "entry.bin", flight.cockpit_image().pixels);
                    std::vector<std::int16_t> audio;
                    unsigned completed = 0;
                    for (; completed < ticks && !flight.state().ended; ++completed) {
                        comanchecd::FlightControls controls;
                        controls.throttle = 8;
                        controls.horizontal = (completed / 32) % 2 == 0 ? 100 : -100;
                        controls.cycle_target = completed % 24 == 0;
                        controls.fire = completed % 8 == 4;
                        controls.weapon = static_cast<comanchecd::WeaponSelection>((completed / 16) % 6);
                        controls.chaff = completed == 10;
                        controls.flare = completed == 20;
                        flight.advance(controls);
                        const auto samples = flight.render_audio(comanchecd::training_samples_per_tick);
                        audio.insert(audio.end(), samples.begin(), samples.end());
                    }
                    world_output::bytes(directory / "last.bin", flight.cockpit_image().pixels);
                    world_runtime_probe::flight_state(directory / "flight.txt", flight.state());
                    std::ofstream pcm(directory / "audio.s16le", std::ios::binary);
                    for (const auto sample : audio) {
                        pcm.put(static_cast<char>(sample & 255));
                        pcm.put(static_cast<char>((static_cast<std::uint16_t>(sample) >> 8) & 255));
                    }
                    if (!pcm) {
                        throw std::runtime_error("Cannot write catalog audio");
                    }
                    report << " passed ticks=" << completed << " objectives=" << unsigned(flight.objectives_remaining())
                           << " armor=" << flight.state().armor << '\n';
                } catch (const std::exception& error) {
                    ++failures;
                    report << " FAILED " << error.what() << '\n';
                }
                report.flush();
                std::cout << identity << '\n';
            }
        }
        report << "missions=" << total << " failures=" << failures << '\n';
        if (!report) {
            throw std::runtime_error("Cannot write catalog report");
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
