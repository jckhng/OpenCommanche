#include "comanchecd/scene.hpp"
#include "color_metric.hpp"
#include "comanchecd/execution_clock.hpp"
#include "comanchecd/timing.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::size_t terrain_bytes = terrain_side * terrain_side;
constexpr std::size_t sg_palette_bytes = 769;
constexpr std::size_t model_metadata_bytes = 51;
constexpr std::size_t object_slots = 60;
constexpr std::size_t fixed_payloads = 13;
constexpr std::size_t unshaded_first_from_end = 9;
constexpr std::size_t unshaded_last_from_end = 6;
enum class ColorPolicy { terrain, model, fixed };
constexpr std::array<std::uint8_t, 6> category_colors{9, 6, 7, 8, 3, 0};

std::uint32_t integer(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t width) {
    if (offset > bytes.size() || width > bytes.size() - offset) {
        throw std::invalid_argument("Truncated SG resource");
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value |= std::uint32_t(bytes[offset + index]) << (index * 8);
    }
    return value;
}

Palette palette(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    Palette result{};
    for (auto& color : result) {
        for (auto& channel : color) {
            channel = static_cast<std::uint8_t>(integer(bytes, offset++, 1));
        }
    }
    return result;
}

void apply_preset(Palette& colors, std::uint8_t preset) {
    if (preset != 1) {
        return;
    }
    for (std::size_t index = 1; index < colors.size(); ++index) {
        auto& color = colors[index];
        const auto gray = static_cast<std::uint8_t>((color[1] * 5 + color[0] * 2 + color[2]) >> 3);
        color[2] = static_cast<std::uint8_t>((color[2] + gray) >> 1);
        color[0] = gray >> 2;
        color[1] = std::min<unsigned>(gray + 10, 63);
    }
}

ColorMap remap(const Palette& destination, Palette source, const Transfer& transfer,
               const GameplaySetup& setup, ColorPolicy policy) {
    if (policy != ColorPolicy::fixed) {
        apply_preset(source, setup.preset);
    }
    ColorMap result{};
    result[255] = 255;
    const std::size_t last = policy != ColorPolicy::fixed && setup.terrain_variant ? 250 : 254;
    const auto source_last = policy == ColorPolicy::model && setup.terrain_variant ? 255 : last;
    for (std::size_t index = 1; index <= source_last; ++index) {
        result[index] = color_metric::nearest(destination, transfer, source[index], 1, last);
    }
    if (last == 250 && policy == ColorPolicy::terrain) {
        for (std::size_t index = 251; index < 255; ++index) {
            result[index] = static_cast<std::uint8_t>(index);
        }
    }
    return result;
}

MaterializedModel materialize(const GameplayPayload& payload, std::size_t index, const Palette& destination,
                              const Transfer& transfer, const GameplaySetup& setup, ColorPolicy policy) {
    const auto& input = payload.bytes;
    if (integer(input, 0, 2) != 0x4753) {
        throw std::invalid_argument("Expected SG model resource");
    }
    const auto pixels = integer(input, 12, 4);
    const auto end = integer(input, 4, 4);
    if (pixels > end || end > input.size() || input.size() - end < sg_palette_bytes || input[end] != 12) {
        throw std::invalid_argument("Invalid SG pixel/palette ranges");
    }
    MaterializedModel result{index, input, {}};
    const auto map = remap(destination, palette(input, end + 1), transfer, setup, policy);
    for (std::size_t pixel = pixels; pixel < end; ++pixel) {
        result.bytes[pixel] = map[input[pixel]];
    }
    const auto metadata = end + sg_palette_bytes + 1;
    if (metadata + 2 <= input.size() && integer(input, metadata, 2) == 0x4433) {
        const auto defined = std::min(model_metadata_bytes, input.size() - metadata);
        const auto carried = std::min(model_metadata_bytes - defined, payload.allocation_suffix.size());
        // Direction/elevation lookup reads 50 bytes; DOS also copies an unused 51st byte.
        if (defined + carried < model_metadata_bytes - 1) {
            throw std::invalid_argument("Missing SG direction/elevation data");
        }
        result.metadata.assign(input.begin() + metadata, input.begin() + metadata + defined);
        result.metadata.insert(result.metadata.end(), payload.allocation_suffix.begin(), payload.allocation_suffix.begin() + carried);
        std::copy(result.metadata.begin(), result.metadata.end(), result.bytes.begin() + end);
    }
    result.bytes.resize(end + result.metadata.size());
    return result;
}

std::uint8_t sample_height(const Terrain& terrain, std::uint32_t x, std::uint32_t y, unsigned offset) {
    const auto value = static_cast<std::uint8_t>(terrain.height[(y >> 22) * terrain_side + (x >> 22)] << 1);
    return static_cast<std::uint8_t>(std::min<unsigned>(value + offset, 255));
}

std::uint32_t next_random(std::uint32_t seed) {
    const auto negative = 0u - seed;
    const auto shifted = (negative >> 1) | (negative & 0x80000000u);
    return shifted ^ ((shifted & 0x80000000u) ? 0xbc9abb09u : 0);
}
}

std::vector<std::uint8_t> expand_terrain(const GameplayPayload& picture) {
    if (picture.width == terrain_side && picture.height == terrain_side && picture.bytes.size() == terrain_bytes) {
        return picture.bytes;
    }
    constexpr std::size_t half = terrain_side / 2;
    if (picture.width != half || picture.height != half || picture.bytes.size() != half * half) {
        throw std::invalid_argument("Expected 512 or 1024 square height map");
    }
    std::vector<std::uint8_t> horizontal(half * terrain_side);
    for (std::size_t y = 0; y < half; ++y) {
        for (std::size_t x = 0; x < half; ++x) {
            const auto first = picture.bytes[y * half + x];
            const auto second = picture.bytes[y * half + (x + 1) % half];
            horizontal[y * terrain_side + x * 2] = first;
            horizontal[y * terrain_side + x * 2 + 1] = static_cast<std::uint8_t>(first + second) >> 1;
        }
    }
    std::vector<std::uint8_t> result(terrain_bytes);
    for (std::size_t y = 0; y < half; ++y) {
        for (std::size_t x = 0; x < terrain_side; ++x) {
            const auto first = horizontal[y * terrain_side + x];
            const auto second = horizontal[((y + 1) % half) * terrain_side + x];
            result[y * 2 * terrain_side + x] = first;
            result[(y * 2 + 1) * terrain_side + x] = static_cast<std::uint8_t>(first + second) >> 1;
        }
    }
    return result;
}

SceneAssets materialize_scene(const GameplaySetup& setup, const std::vector<GameplayPayload>& payloads,
                              const Transfer& transfer) {
    if (payloads.size() < 4 + fixed_payloads) {
        throw std::invalid_argument("Missing terrain payloads");
    }
    SceneAssets result{};
    result.model_payloads.fill(std::numeric_limits<std::size_t>::max());
    result.terrain.height = expand_terrain(payloads[2]);
    result.terrain.palette = palette(payloads[1].palette, 0);
    if (payloads[3].bytes.size() != terrain_bytes) {
        throw std::invalid_argument("Expected 1024 square terrain color map");
    }
    result.terrain.color = payloads[3].bytes;
    const auto color_map = remap(result.terrain.palette, palette(payloads[3].palette, 0), transfer, setup, ColorPolicy::terrain);
    for (auto& color : result.terrain.color) {
        color = color_map[color];
    }
    for (std::size_t index = 4; index < payloads.size(); ++index) {
        const auto& payload = payloads[index];
        if (payload.kind != PayloadKind::resource || payload.bytes.size() < 2 ||
                payload.bytes[0] != 'S' || payload.bytes[1] != 'G') {
            continue;
        }
        const auto policy = index >= payloads.size() - unshaded_first_from_end && index <= payloads.size() - unshaded_last_from_end ?
            ColorPolicy::fixed : ColorPolicy::model;
        result.models.push_back(materialize(payload, index, result.terrain.palette, transfer, setup, policy));
    }
    if (4 + setup.overlays.size() > payloads.size() - fixed_payloads) {
        throw std::invalid_argument("Missing overlay/model payloads");
    }
    for (std::size_t index = 0; index < setup.overlays.size(); ++index) {
        const auto payload_index = 4 + index;
        const auto model = std::find_if(result.models.begin(), result.models.end(), [&](const auto& item) {
            return item.payload == payload_index;
        });
        if (model == result.models.end()) {
            throw std::invalid_argument("Overlay is not an SG resource");
        }
        const auto width = integer(model->bytes, 8, 2);
        const auto height = integer(model->bytes, 10, 2);
        const auto pixels = integer(model->bytes, 12, 4);
        const auto end = integer(model->bytes, 4, 4);
        const std::vector<std::uint8_t> fields(setup.overlay_fields.begin(), setup.overlay_fields.end());
        const auto x = integer(fields, index * 4, 4);
        const auto y = integer(fields, 40 + index * 4, 4);
        const auto last = (std::uint64_t(y) + height - 1) * terrain_side + x + width;
        if (width == 0 || height == 0 || last > terrain_bytes ||
                std::uint64_t(width) * height > end - pixels) {
            throw std::invalid_argument("Overlay exceeds terrain or SG bounds: x=" + std::to_string(x) +
                " y=" + std::to_string(y) + " width=" + std::to_string(width) + " height=" + std::to_string(height));
        }
        std::uint8_t level = 255;
        for (std::size_t row = 0; row < height; ++row) {
            for (std::size_t column = 0; column < width; ++column) {
                const auto color = model->bytes[pixels + row * width + column];
                if (color == 0) {
                    continue;
                }
                const auto position = (y + row) * terrain_side + x + column;
                result.terrain.color[position] = color;
                if (level == 255) {
                    level = static_cast<std::uint8_t>(result.terrain.height[position] + setup.overlay_fields[80 + index]);
                }
                result.terrain.height[position] = level;
            }
        }
    }
    const auto first_model = 4 + setup.overlays.size();
    result.models.erase(std::remove_if(result.models.begin(), result.models.end(), [&](const auto& model) {
        return model.payload < first_model;
    }), result.models.end());
    const auto optional_models = payloads.size() - fixed_payloads - first_model;
    if (optional_models > result.model_payloads.size()) {
        throw std::invalid_argument("Too many optional scene models");
    }
    for (std::size_t index = 0; index < optional_models; ++index) {
        result.model_payloads[index] = first_model + index;
    }
    return result;
}

MissionScene construct_scene(const GameplaySetup& setup, const SceneAssets& assets, std::uint32_t random_seed,
                             const SceneEntry& entry, IrqState* live, ExecutionClock* clock) {
    if (assets.terrain.height.size() != terrain_bytes) {
        throw std::invalid_argument("Scene construction requires materialized terrain");
    }
    MissionScene result{};
    const auto instructions = [&](unsigned count) { if (clock) { clock->execute(CpuOperation::instructions, count); } };
    if (clock && !live) { throw std::invalid_argument("Clocked scene construction requires live IRQ state"); }
    result.vehicle_parameters = setup.vehicle_parameters;
    result.range = setup.range;
    result.random_seed = live ? live->random_seed : random_seed;
    result.remaining_targets = setup.target_count;
    result.preset = setup.preset;
    result.attack_allies = setup.alternate_start != 0;
    for (std::size_t slot = 0; slot < scene_slots; ++slot) {
        auto& entity = result.entities[slot];
        entity.previous_x = entry.entities[slot].previous_x;
        entity.previous_y = entry.entities[slot].previous_y;
        entity.previous_height = entry.entities[slot].previous_height;
        entity.animation_frame = entry.entities[slot].animation_frame;
        entity.rotor_phase = entry.entities[slot].rotor_phase;
    }
    std::size_t next_slot = object_slots;
    instructions(6 + 6 * 5 + (setup.objects.empty() ? 3 : 4)); // 6f91..6fd8.
    for (const auto& object : setup.objects) {
        if (object.type >= 18 || object.attribute2 >= missing_model || object.attribute3 >= missing_model ||
                object.category >= category_colors.size()) {
            throw std::invalid_argument("Unsupported scene object attributes");
        }
        const auto slot = object.type == 0 ? player_slot : next_slot;
        if (slot == 0) {
            continue;
        }
        instructions(29); // Load the constructor arguments and indirect CALL.
        auto& entity = result.entities[slot];
        entity.active = true;
        entity.kind = object.type;
        entity.primary_model = object.attribute2;
        entity.secondary_model = object.attribute3;
        if (object.type == 0) {
            instructions(10 + 5);
            continue;
        }
        --next_slot;
        if (clock) {
            if (object.type != 7) { instructions(1); }
            instructions(3 + 3 * (object_slots - slot));
            std::uint32_t value = 0;
            clock->instruction([&] { value = live->random_seed; });
            instructions(5);
            value = next_random(value);
            clock->instruction([&] { live->random_seed = value; });
            clock->instruction([&] { entity.random_phase = static_cast<std::uint8_t>(value); });
        } else {
            result.random_seed = next_random(result.random_seed);
            entity.random_phase = static_cast<std::uint8_t>(result.random_seed);
        }
        entity.x = object.x;
        entity.y = object.y;
        entity.speed = std::uint32_t(object.attribute8) << 21;
        entity.angle = object.angle;
        entity.target = object.target;
        entity.attribute7 = object.attribute7;
        entity.attribute9 = object.attribute9;
        entity.attribute10 = object.attribute10;
        entity.category_color = category_colors[object.category];
        entity.height = sample_height(assets.terrain, object.x, object.y, 9);
        constexpr std::array<unsigned, 18> constructor_tail{0, 2, 3, 3, 3, 3, 3, 0, 2, 3, 3, 3, 4, 4, 4, 3, 3, 3};
        const auto ground = static_cast<std::uint8_t>(assets.terrain.height[(object.y >> 22) * terrain_side + (object.x >> 22)] << 1);
        instructions(41 + (unsigned(ground) + 9 > 255 ? 1 : 0) + constructor_tail[object.type] + 5);
        if (object.type == 4) {
            result.wingman = static_cast<std::uint32_t>(slot);
        }
    }
    result.player_height = sample_height(assets.terrain, setup.x, setup.y, 5);
    const auto ground = static_cast<std::uint8_t>(assets.terrain.height[(setup.y >> 22) * terrain_side + (setup.x >> 22)] << 1);
    instructions(13 + (unsigned(ground) + 5 > 255 ? 1 : 0));
    if (live) { result.random_seed = live->random_seed; }
    if (result.entities[1].active) {
        result.entities[player_slot].rotor_phase = result.entities[1].attribute9;
    } else {
        result.entities[1].attribute9 = result.entities[player_slot].rotor_phase;
    }
    return result;
}
} // namespace comanchecd
