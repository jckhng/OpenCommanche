#pragma once

#include "comanchecd/flight.hpp"
#include "audio_output.hpp"
#include "scene_output.hpp"
#include "world_output.hpp"

namespace world_runtime_probe {
inline void entity_state(const std::filesystem::path& path, const comanchecd::MissionScene& scene,
                         const comanchecd::SceneAssets& assets) {
    std::ofstream file(path);
    for (std::size_t slot = 1; slot < scene.entities.size(); ++slot) {
        const auto& entity = scene.entities[slot];
        if (!entity.active) {
            continue;
        }
        const auto payload = entity.primary_payload == std::numeric_limits<std::size_t>::max() ?
            assets.model_payloads.at(entity.primary_model) : entity.primary_payload;
        file << slot << ' ' << unsigned(entity.kind) << ' ' << payload << ' ' << entity.x << ' ' << entity.y << ' '
             << entity.speed << ' ' << unsigned(entity.angle) << ' ' << unsigned(entity.height) << ' '
             << unsigned(entity.random_phase) << ' ' << unsigned(entity.category_color) << ' '
             << entity.previous_x << ' ' << entity.previous_y << ' ' << unsigned(entity.previous_height) << '\n';
    }
    if (!file) {
        throw std::runtime_error("Cannot write simulated entity state");
    }
}

inline void flight_state(const std::filesystem::path& path, const comanchecd::FlightState& state) {
    std::ofstream file(path);
    file << state.view.x << ' ' << state.view.y << ' ' << state.view.heading << ' ' << unsigned(state.view.height) << ' '
         << state.view.pitch << ' ' << state.view.roll << ' ' << state.forward_speed << ' ' << state.side_speed << ' '
         << state.yaw_rate << ' ' << state.range << ' ' << unsigned(state.control_power) << ' '
         << unsigned(state.previous_power) << ' ' << unsigned(state.engine_power) << ' '
         << unsigned(state.height_gap) << ' ' << unsigned(state.previous_height_gap);
    for (const auto count : state.ammunition) {
        file << ' ' << count;
    }
    file << '\n';
    if (!file) {
        throw std::runtime_error("Cannot write flight state");
    }
}

inline void run(const std::filesystem::path& movie, const std::filesystem::path& output,
                const comanchecd::GameplaySetup& setup, const comanchecd::SceneAssets& assets,
                const comanchecd::MissionScene& scene, comanchecd::TerrainTables tables,
                std::size_t payload_count, comanchecd::MissionAudio& audio) {
    std::ifstream input(movie);
    if (!input) {
        throw std::runtime_error("Cannot read gameplay control movie");
    }
    world_output::read_depth_padding(input, tables);
    comanchecd::FlightController flight(setup, scene, tables, payload_count);
    const comanchecd::TerrainRenderer renderer(std::move(tables));
    flight.update_entities(assets);
    flight.step({}, assets);
    std::size_t frame = 0;
    comanchecd::FlightControls controls;
    unsigned throttle, fire;
    std::uint64_t sample;
    std::vector<std::int16_t> waveform;
    while (input >> controls.horizontal) {
        if (!(input >> controls.vertical >> throttle >> fire >> sample) || throttle > 15 || fire > 1 || frame >= 64) {
            throw std::invalid_argument("Invalid gameplay control movie");
        }
        controls.throttle = static_cast<std::uint8_t>(throttle);
        controls.fire = fire != 0;
        const auto directory = output / "world" / ("frame-" + std::to_string(frame++));
        auto image = renderer.render(assets, flight.state().view);
        world_output::write(directory, renderer, image);
        renderer.shade_distance(image, assets, setup.terrain_variant);
        world_output::bytes(directory / "fog.bin", image.pixels);
        flight.update_entities(assets);
        for (const auto& cue : flight.sounds()) {
            audio.play_cue(cue);
        }
        renderer.draw_models(image, assets, flight.scene(), flight.state().view);
        world_output::bytes(directory / "models.bin", image.pixels);
        world_output::bytes(directory / "model-coverage.bin", image.write_mask);
        entity_state(directory / "entities.txt", flight.scene(), assets);
        flight_state(directory / "before.txt", flight.state());
        const auto host_sample = (sample * comanchecd::MissionAudio::sample_rate +
            comanchecd::Sb16Playback::source_rate - 1) / comanchecd::Sb16Playback::source_rate;
        if (host_sample < waveform.size()) {
            throw std::invalid_argument("Gameplay DSP clock moved backward");
        }
        auto generated = audio.render(static_cast<std::size_t>(host_sample - waveform.size()));
        waveform.insert(waveform.end(), generated.begin(), generated.end());
        if (audio.digital_pcm().size() != sample) {
            throw std::runtime_error("Gameplay audio did not reach its declared producer cursor");
        }
        flight.step(controls, assets);
        audio.set_engine_power(flight.state().engine_power);
        for (const auto& cue : flight.sounds()) {
            audio.play_cue(cue);
        }
        flight_state(directory / "after.txt", flight.state());
    }
    if (!input.eof() || frame == 0) {
        throw std::invalid_argument("Truncated or empty gameplay control movie");
    }
    const auto host_end = std::max<std::size_t>(waveform.size() + comanchecd::MissionAudio::sample_rate / 4,
                                              2 * comanchecd::MissionAudio::sample_rate);
    auto tail = audio.render(host_end - waveform.size());
    waveform.insert(waveform.end(), tail.begin(), tail.end());
    audio_output::write_samples(output / "audio", audio, waveform);
    std::ofstream requests(output / "world/sound-requests.txt");
    for (const auto& request : audio.sample_requests()) {
        requests << request.sample << ' ' << unsigned(request.bank) << ' ' << request.sound << ' '
                 << unsigned(request.voice) << ' ' << unsigned(request.attenuation) << ' ' << request.step << ' '
                 << unsigned(request.mode) << '\n';
    }
    if (!requests) {
        throw std::runtime_error("Cannot write native sound requests");
    }
}
}
