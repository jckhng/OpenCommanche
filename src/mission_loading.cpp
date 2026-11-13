#include "mission_loading.hpp"
#include "color_metric.hpp"
#include "menu_work.hpp"
#include <algorithm>

namespace comanchecd {
namespace {
constexpr unsigned palette_colors = 256;
constexpr unsigned mission_fixed_payloads = 13;
constexpr unsigned projection_scale = 0x03000000;
enum class ModelPalette { scenery, fixed };

void projection(const TerrainTables& tables, const CpuWorkSink& work) {
    work(CpuOperation::instructions, 8); // 165a9 with the allocated projection table.
    for (unsigned level = 0; level < tables.levels; ++level) {
        work(CpuOperation::instructions, 4);
        work(CpuOperation::string_bytes, 1);
        work(CpuOperation::instructions, 6);
        const auto scale = static_cast<std::int32_t>((std::uint64_t(tables.distance[2][level]) * projection_scale) >> 32);
        for (unsigned height = 0; height < palette_colors; ++height) {
            const auto relative = static_cast<std::int32_t>((height << 24) - 0x80000000u);
            auto projected = static_cast<std::int32_t>((std::int64_t(relative) * scale) >> 32) + tables.distance[3][level];
            work(CpuOperation::instructions, 5);
            if (static_cast<std::int32_t>(projected) < 0) {
                work(CpuOperation::instructions, 1);
                projected = 0;
            }
            work(CpuOperation::instructions, 2);
            if (projected > 0xff00) { work(CpuOperation::instructions, 1); }
            work(CpuOperation::instructions, 1);
            work(CpuOperation::string_bytes, 1);
            work(CpuOperation::instructions, 2);
        }
        work(CpuOperation::instructions, 3);
    }
    work(CpuOperation::instructions, 1);
}

Palette payload_palette(const std::vector<std::uint8_t>& bytes, std::size_t offset = 0) {
    Palette result{};
    for (auto& color : result) {
        for (auto& channel : color) { channel = bytes.at(offset++); }
    }
    return result;
}

std::uint32_t integer(const std::vector<std::uint8_t>& bytes, std::size_t offset, unsigned width = 4) {
    std::uint32_t result = 0;
    for (unsigned byte = 0; byte < width; ++byte) { result |= std::uint32_t(bytes.at(offset + byte)) << (byte * 8); }
    return result;
}

void overlay(const GameplayPayload& payload, const GameplaySetup& setup, std::size_t index,
             std::vector<std::uint8_t>& terrain, const CpuWorkSink& work) {
    const auto width = integer(payload.bytes, 8, 2);
    const auto height = integer(payload.bytes, 10, 2);
    const auto pixels = integer(payload.bytes, 12);
    const auto end = integer(payload.bytes, 4);
    const std::vector<std::uint8_t> fields(setup.overlay_fields.begin(), setup.overlay_fields.end());
    const auto x = integer(fields, index * 4);
    const auto y = integer(fields, 40 + index * 4);
    const auto last = (std::uint64_t(y) + height - 1) * terrain_side + x + width;
    if (width == 0 || height == 0 || last > terrain.size() || pixels > end ||
            std::uint64_t(width) * height > end - pixels || end > payload.bytes.size()) {
        throw std::invalid_argument("Overlay exceeds terrain or SG bounds");
    }
    work(CpuOperation::instructions, 15); // 6ad3 prepares the terrain and SG pointers.
    std::uint8_t level = 255;
    for (std::size_t row = 0; row < height; ++row) {
        work(CpuOperation::instructions, 2);
        for (std::size_t column = 0; column < width; ++column) {
            work(CpuOperation::instructions, 3);
            if (payload.bytes[pixels + row * width + column] != 0) {
                work(CpuOperation::instructions, 3);
                const auto position = (y + row) * terrain_side + x + column;
                if (level == 255) {
                    level = static_cast<std::uint8_t>(terrain[position] + fields[80 + index]);
                    work(CpuOperation::instructions, 2);
                }
                terrain[position] = level;
                work(CpuOperation::instructions, 1);
            }
            work(CpuOperation::instructions, 4);
        }
        work(CpuOperation::instructions, 4);
    }
    work(CpuOperation::instructions, 12); // Rewind the temporary allocation and advance the overlay loop.
}

void prepare_palette(Palette& palette, std::uint8_t preset, const CpuWorkSink& work) {
    if (preset != 1) {
        work(CpuOperation::instructions, 1);
        return;
    }
    work(CpuOperation::instructions, 3);
    for (unsigned index = 1; index < palette.size(); ++index) {
        auto& color = palette[index];
        const auto gray = static_cast<std::uint8_t>((color[1] * 5 + color[0] * 2 + color[2]) >> 3);
        work(CpuOperation::instructions, 24);
        if (gray + 10 > 63) { work(CpuOperation::instructions, 1); }
        color = {static_cast<std::uint8_t>(gray >> 2), static_cast<std::uint8_t>(std::min<unsigned>(gray + 10, 63)),
                 static_cast<std::uint8_t>((color[2] + gray) >> 1)};
    }
    work(CpuOperation::instructions, 2);
}

ColorMap remap(Palette source, const Palette& destination, const Transfer& transfer,
               unsigned last, unsigned source_last, std::size_t pixels, const CpuWorkSink& work) {
    work(CpuOperation::instructions, 8); // 2f36e / 2f37d table initialization.
    ColorMap map{};
    map[255] = 255;
    for (unsigned index = 1; index <= source_last; ++index) {
        work(CpuOperation::instructions, 10);
        map[index] = color_metric::nearest(destination, transfer, source[index], 1, last, work);
        work(CpuOperation::instructions, 7);
    }
    work(CpuOperation::instructions, 4);
    for (std::size_t pair = 0; pair < pixels / 2; ++pair) {
        work(CpuOperation::instructions, 4);
        work(CpuOperation::string_bytes, 1);
        work(CpuOperation::instructions, 1);
    }
    work(CpuOperation::instructions, 1);
    if (pixels & 1) {
        work(CpuOperation::instructions, 2);
        work(CpuOperation::string_bytes, 1);
    }
    work(CpuOperation::instructions, 1);
    return map;
}

ColorMap model(const GameplayPayload& payload, const GameplaySetup& setup, const Palette& destination,
               const Transfer& transfer, ModelPalette policy, const CpuWorkSink& work) {
    const auto fixed = policy == ModelPalette::fixed;
    const auto end = integer(payload.bytes, 4);
    const auto pixels = integer(payload.bytes, 12);
    auto colors = payload_palette(payload.bytes, end + 1);
    work(CpuOperation::instructions, 15);
    work(CpuOperation::repeat_copy_dwords, palette_colors * 3 / 4);
    work(CpuOperation::instructions, 1 + 6); // Shared metadata CALL and marker test.
    const auto metadata = end + 770;
    if (payload.bytes.size() >= metadata + 2 && payload.bytes[metadata] == '3' && payload.bytes[metadata + 1] == 'D') {
        work(CpuOperation::instructions, 1);
        work(CpuOperation::repeat_copy_bytes, 51);
    }
    work(CpuOperation::instructions, 4);
    if (fixed) {
        work(CpuOperation::instructions, 1);
    } else {
        work(CpuOperation::instructions, 2);
        prepare_palette(colors, setup.preset, work);
        work(CpuOperation::instructions, 2);
    }
    const unsigned last = !fixed && setup.terrain_variant ? 250 : 254;
    const unsigned source_last = !fixed && setup.terrain_variant ? 255 : 254;
    if (!fixed && setup.terrain_variant) { work(CpuOperation::instructions, 3); }
    return remap(colors, destination, transfer, last, source_last, end - pixels, work);
}
}

std::vector<GameplayPayload> load_mission_payloads(const Installation& installation, const GameplaySetup& setup,
    const MissionResources& resources, const Transfer& transfer, TerrainDetail detail, MissionAudio& audio) {
    const auto work = audio.cpu_work();
    audio.stop_mission_samples();
    work(CpuOperation::instructions, 375 + 4 * 20); // 5fdd..68ad, including four persistent high allocations.
    projection(installation.load_terrain_tables(detail), work);
    work(CpuOperation::instructions, 2); // JMP 694b and the first resource CALL.
    Palette terrain{};
    std::vector<std::uint8_t> overlay_heights;
    std::size_t index = 0;
    const auto optional = std::find_if(resources.begin() + 11, resources.end(), [](const auto& name) { return name[0] == '-'; }) - (resources.begin() + 11);
    const auto total = 4 + setup.overlays.size() + optional + mission_fixed_payloads;
    return load_gameplay(setup, resources, [&](const std::string& name, PayloadKind kind) {
        const auto position = index++;
        if (position == total - 1) { return installation.load_payload(name, kind); }
        const unsigned filename_bytes = position == 0 ? 9 : name == "HUDS.RLE" ? 9 : 13;
        const auto is_overlay = position >= 4 && position < 4 + setup.overlays.size();
        if (is_overlay) {
            work(CpuOperation::instructions, 27); // 6a65 saves the allocator and copies the filename.
        }
        auto payload = installation.load_payload(name, kind, audio, filename_bytes);
        if (position == 0) {
            work(CpuOperation::instructions, 2 + 4 + 2);
        } else if (position == 1) {
            terrain = payload_palette(payload.palette);
            work(CpuOperation::instructions, 1);
            menu_work::colors(terrain, transfer, work);
            work(CpuOperation::instructions, 2);
        } else if (position == 2) {
            if (!setup.overlays.empty()) {
                overlay_heights = expand_terrain(payload);
            }
            work(CpuOperation::instructions, 3);
            if (payload.height == terrain_side / 2) {
                work(CpuOperation::instructions, 3);
                if (payload.width == terrain_side / 2) {
                    constexpr unsigned half = terrain_side / 2;
                    work(CpuOperation::instructions, 6);
                    for (unsigned row = 0; row < half; ++row) { work(CpuOperation::instructions, 4 + 9 * half); }
                    work(CpuOperation::instructions, 6);
                    for (unsigned row = 0; row < terrain_side; ++row) { work(CpuOperation::instructions, 8 + 10 * half); }
                }
            }
            work(CpuOperation::instructions, 4 + 2);
        } else if (position == 3) {
            work(CpuOperation::instructions, 1 + 2);
            auto palette = payload_palette(payload.palette);
            prepare_palette(palette, setup.preset, work);
            work(CpuOperation::instructions, 2);
            if (setup.terrain_variant) { work(CpuOperation::instructions, 8); }
            remap(palette, terrain, transfer, setup.terrain_variant ? 250 : 254,
                  setup.terrain_variant ? 250 : 254, payload.bytes.size(), work);
            work(CpuOperation::instructions, 2 + 4 + 3);
        } else if (position == total - 2) {
            work(CpuOperation::instructions, 1); // 6f8c stores the speech pointer before scene construction.
        } else {
            work(CpuOperation::instructions, 3);
            if (payload.bytes.size() >= 2 && payload.bytes[0] == 'S' && payload.bytes[1] == 'G') {
                const auto policy = position >= total - 9 && position <= total - 6 ? ModelPalette::fixed : ModelPalette::scenery;
                model(payload, setup, terrain, transfer, policy, work);
            }
            if (is_overlay) {
                overlay(payload, setup, position - 4, overlay_heights, work);
            } else {
                work(CpuOperation::instructions, 3);
            }
        }
        payload.name = name;
        return payload;
    });
}
}
