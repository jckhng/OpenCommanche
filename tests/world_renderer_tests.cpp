#include "comanchecd/world_renderer.hpp"
#include "comanchecd/flight.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <memory>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        comanchecd::TerrainTables tables;
        tables.levels = 1;
        tables.sine[128] = 0x7fffffff;
        tables.distance[3][0] = 128 << 8;
        const comanchecd::TerrainRenderer renderer(tables);
        require(renderer.projection() == std::vector<std::uint8_t>(256, 128),
                "Projection preserves the DTL bias before byte selection");
        comanchecd::SceneView view;
        const auto camera = renderer.camera(view);
        require(camera.sine == 0 && camera.cosine == 0x7ffffffe,
                "Camera interpolation preserves separate signed high products");
        view.pitch = 128;
        view.roll = 5;
        const auto banked = renderer.camera(view);
        require(banked.roll == 1 && banked.horizon[95] == 0 && banked.horizon[96] == 1 &&
                banked.horizon[97] == 2, "Banked horizon preserves byte rounding and roll clamp");
        comanchecd::SceneAssets assets;
        assets.terrain.height.assign(1024 * 1024, 0);
        assets.terrain.color.assign(1024 * 1024, 73);
        comanchecd::MaterializedModel sky{};
        sky.bytes.assign(16 + 512 * 150, 42);
        sky.bytes[12] = 16;
        sky.bytes[13] = sky.bytes[14] = sky.bytes[15] = 0;
        assets.models.push_back(std::move(sky));
        const auto frame = renderer.render(assets, {});
        comanchecd::FlightController projection_owner({}, {}, tables, 20);
        comanchecd::TargetingFrame projected;
        projected.entities[140].depth = 0x01000000;
        projection_owner.observe_projection(projected);
        auto retained_projection = frame;
        renderer.draw_models(retained_projection, assets, projection_owner.scene(), {});
        require(retained_projection.targeting.entities[140].depth == projected.entities[140].depth &&
                !projection_owner.scene().entities[140].active,
                "Inactive model slots must retain the last committed depth for the shared sort table");
        require(frame.pixels[16] == 73 && frame.pixels[128 * 256 + 16] == 42,
                "Terrain spans and panoramic sky share the wrapped scene canvas");
        require(std::count_if(frame.write_mask.begin(), frame.write_mask.end(), [](auto value) { return value != 0; }) ==
                192 * 256, "Synthetic sky and terrain cover every active scene column");
        require(frame.depth[31] == 0 && frame.depth[32] == 128 && frame.depth[223] == 128 && frame.depth[224] == 0,
                "Depth rows retain the original 32-byte origin and 192-column width");
        const comanchecd::TerrainRenderer flat_renderer(tables, 0x03000000, {comanchecd::CloudMode::flat, false});
        auto flat = flat_renderer.render(assets, {});
        require(flat.sky[16] == 1 && flat.sky[255 * 256 + 239] == 1 && flat.sky[15] == 0 &&
                std::count(flat.sky.begin(), flat.sky.end(), 1) == 192 * 256,
                "Clouds Off fills every row in the original interleaved columns using the nearest non-fog color");
        const auto unshaded = flat.pixels;
        flat_renderer.shade_distance(flat, assets, 0);
        require(flat.pixels == unshaded, "Haze Off bypasses distance shading");
        const auto flat_monitor = flat_renderer.render_monitor(assets, {}, flat);
        require(flat_monitor.pixels[200 * 256 + 44] == 1 && flat_monitor.pixels[255 * 256 + 83] == 1 &&
                flat_monitor.pixels[200 * 256 + 43] == flat.pixels[200 * 256 + 43],
                "Clouds Off clears the complete monitor strip while preserving neighboring columns");
        const comanchecd::TerrainRenderer coarse_renderer(tables, 0x03000000,
            {comanchecd::CloudMode::panoramic, false, comanchecd::PixelSize::large});
        const auto coarse = coarse_renderer.render(assets, view);
        for (std::size_t row = 0; row < 256; ++row) {
            require(std::equal(coarse.pixels.begin() + row * 256 + 16, coarse.pixels.begin() + row * 256 + 112,
                               coarse.pixels.begin() + row * 256 + 144),
                    "Large terrain pixels duplicate both planar halves before models are drawn");
        }
        const comanchecd::WaterReflections water(assets.terrain.palette, tables.transfer);
        auto reflected = flat;
        reflected.pixels.fill(73);
        const auto dry_pixels = reflected.pixels;
        comanchecd::ReflectionState reflection_state{1};
        unsigned reflection_instructions = 0;
        water.draw(reflected, reflection_state, [&](comanchecd::CpuOperation operation, unsigned count) {
            require(operation == comanchecd::CpuOperation::instructions, "Reflection scanning uses ordinary CPU work");
            reflection_instructions += count;
        });
        require(reflection_instructions == 132495,
                "A clear reflection scan must account for all 192 columns and their 135 row tests");
        require(reflected.pixels == dry_pixels && reflection_state.random == 0x436544f7,
                "A dry frame leaves pixels intact but advances the separate reflection generator");
        reflected.camera.horizon.fill(250);
        reflected.pixels.fill(251);
        water.draw(reflected, reflection_state);
        require(reflected.pixels[250 * 256 + 16] == 251 && reflected.pixels[251 * 256 + 16] == 1 &&
                reflected.pixels[129 * 256 + 16] == 1 && reflected.pixels[130 * 256 + 16] == 251,
                "Reflection spans wrap at byte boundaries and exclude the horizon row");
        reflected.pixels.fill(255);
        const auto before_invalid = reflection_state.random;
        bool reflection_rejected = false;
        try {
            water.draw(reflected, reflection_state);
        } catch (const std::invalid_argument&) {
            reflection_rejected = true;
        }
        require(reflection_rejected && reflection_state.random == before_invalid,
                "Unrecovered adjacent-memory reflection reads reject without committing RNG state");
        tables.distance[0][0] = 0x00200000;
        tables.distance[4][0] = 0xffffffff;
        tables.sprite_steps.assign(256, 1);
        const comanchecd::TerrainRenderer model_renderer(tables);
        comanchecd::MaterializedModel model{};
        model.payload = 1;
        model.bytes.assign(20, 0);
        model.bytes[2] = 1;
        model.bytes[8] = model.bytes[9] = 2;
        model.bytes[12] = 16;
        model.bytes[16] = 3;
        model.bytes[18] = 4;
        model.bytes[19] = 5;
        model.metadata.assign(51, 0);
        std::fill(model.metadata.begin() + 2, model.metadata.begin() + 34, 1);
        assets.models.insert(assets.models.begin(), std::move(model));
        assets.model_payloads[0] = 1;
        comanchecd::MissionScene scene;
        scene.entities[60].active = true;
        scene.entities[60].kind = 3;
        scene.entities[60].primary_model = 0;
        scene.entities[60].y = 0xc0000000;
        auto models = model_renderer.render(assets, {});
        model_renderer.draw_models(models, assets, scene, {});
        require(models.pixels[128 * 256 + 64] == 3 && models.pixels[129 * 256 + 64] == 42 &&
                models.pixels[128 * 256 + 192] == 4 && models.pixels[129 * 256 + 192] == 5,
                "Directional SG frames preserve column-major pixels and transparent zero");
        tables.distance[3][0] = 127 << 8;
        const comanchecd::TerrainRenderer top_renderer(tables);
        assets.models.front().bytes[11] = 127;
        std::unique_ptr<comanchecd::TerrainFrame> clipped_top(new comanchecd::TerrainFrame(top_renderer.render(assets, {})));
        std::fill(clipped_top->depth.begin(), clipped_top->depth.end(), 0);
        const std::vector<std::uint8_t> before_top(clipped_top->pixels.begin(), clipped_top->pixels.end());
        top_renderer.draw_models(*clipped_top, assets, scene, {});
        require(std::equal(clipped_top->pixels.begin(), clipped_top->pixels.end(), before_top.begin()),
                "A positive SG origin landing on row zero must take the original carry-reject path");
        assets.models.front().bytes[11] = 0;
        tables.distance[3][0] = 128 << 8;
        tables.depth_padding.assign(comanchecd::scene_buffer_side - comanchecd::scene_columns, 255);
        const comanchecd::TerrainRenderer padded_renderer(tables);
        scene.entities[60].x = 0x06800000;
        auto padded = padded_renderer.render(assets, {});
        padded_renderer.draw_models(padded, assets, scene, {});
        require(padded.depth[232] == 255 && padded.pixels[128 * 256 + 116] == 0,
                "Declared depth allocation padding clips sprites outside the active terrain columns");
        tables.depth_padding.pop_back();
        bool padding_rejected = false;
        try {
            const comanchecd::TerrainRenderer invalid_padding(tables);
        } catch (const std::invalid_argument&) {
            padding_rejected = true;
        }
        require(padding_rejected, "Depth padding must cover both complete allocation margins");
        assets.terrain.height.clear();
        bool rejected = false;
        try {
            renderer.render(assets, {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Incomplete terrain must reject before sampling");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
