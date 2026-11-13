#include "comanchecd/gameplay.hpp"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::size_t object_limit = 60;
constexpr std::size_t auxiliary_limit = 10;
constexpr std::size_t number_scan_limit = 79;
constexpr std::size_t number_bytes = 8;
constexpr std::size_t line_scan_limit = 160;
constexpr std::size_t picture_header_bytes = 128;
constexpr std::size_t maximum_payload_bytes = 4 * 1024 * 1024;
constexpr std::size_t resource_alignment = 4;

class SetupReader {
public:
    explicit SetupReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    std::uint8_t peek() const {
        if (position_ >= bytes_.size()) {
            throw std::invalid_argument("Truncated mission setup");
        }
        return bytes_[position_];
    }

    void advance(std::size_t count = 1) { position_ += count; }

    void line() {
        for (;;) {
            std::size_t scanned = 0;
            while (peek() != '\r') {
                advance();
                if (++scanned == line_scan_limit) {
                    throw std::invalid_argument("Mission setup line exceeds scan bound");
                }
            }
            advance();
            if (peek() == '\n') {
                advance();
            }
            if (peek() > '\r' && peek() != ';') {
                return;
            }
        }
    }

    std::uint32_t number() {
        std::size_t skipped = 0;
        while (peek() < '0' || peek() > '9') {
            advance();
            if (++skipped == number_scan_limit) {
                throw std::invalid_argument("Mission number exceeds scan bound");
            }
        }
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < number_bytes && peek() >= '0'; ++index) {
            value = value * 10 + (peek() & 15);
            advance();
        }
        return value;
    }

    MissionFilename filename() {
        MissionFilename result;
        result.fill(' ');
        std::size_t length = 0;
        while (peek() != ',') {
            if (length == result.size()) {
                throw std::invalid_argument("Overlay filename exceeds twelve bytes");
            }
            result[length++] = peek();
            advance();
        }
        return result;
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_ = 0;
};

void store(std::array<std::uint8_t, 90>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
    }
}

std::uint32_t word(const std::vector<std::uint8_t>& file, std::size_t offset) {
    if (offset + 2 > file.size()) {
        throw std::invalid_argument("Truncated payload header");
    }
    return file[offset] | (std::uint32_t(file[offset + 1]) << 8);
}

std::vector<std::uint8_t> expand(const std::vector<std::uint8_t>& file, std::size_t& cursor, std::size_t size,
                                PayloadPolicy policy) {
    if (size > maximum_payload_bytes) {
        throw std::invalid_argument("Payload exceeds supported allocation size");
    }
    std::vector<std::uint8_t> output;
    output.reserve(size);
    while (output.size() < size) {
        if (cursor == file.size()) {
            throw std::invalid_argument("Truncated payload run");
        }
        auto value = file[cursor++];
        std::size_t count = 1;
        if (value >= 0xc0) {
            count = value & 63;
            if (count == 0 && policy == PayloadPolicy::strict_frontend) {
                throw std::invalid_argument("Invalid frontend picture run");
            }
            if (cursor == file.size()) {
                throw std::invalid_argument("Missing payload run value");
            }
            value = file[cursor++];
        }
        if (count > size - output.size()) {
            throw std::invalid_argument("Payload run exceeds decoded allocation");
        }
        output.insert(output.end(), count, value);
    }
    return output;
}

std::string filename(const MissionFilename& bytes) {
    auto end = std::find(bytes.begin(), bytes.end(), ' ');
    return std::string(bytes.begin(), end);
}
}

GameplaySetup parse_gameplay(const MissionDefinition& definition, const MissionResources& resources,
                             const std::array<std::uint32_t, 6>& presets, const GameplaySetup& previous) {
    GameplaySetup result;
    result.overlay_fields = previous.overlay_fields;
    result.last_light_type = previous.last_light_type;
    SetupReader reader(definition.setup_text);
    reader.line();
    if (reader.peek() == 'A') {
        result.alternate_start = 1;
        reader.advance();
    }
    const auto preset = reader.number();
    if (preset >= presets.size()) {
        throw std::invalid_argument("Mission preset exceeds supported table");
    }
    result.preset = static_cast<std::uint8_t>(preset);
    result.preset_reference = presets[preset];
    reader.line();
    result.x = reader.number() << 22;
    result.y = reader.number() << 22;
    result.heading = reader.number() << 24;
    reader.line();
    for (auto& value : result.map_position) {
        value = static_cast<std::uint8_t>(reader.number());
    }
    reader.line();
    for (auto& value : result.vehicle_parameters) {
        value = reader.number();
    }
    reader.line();
    result.range = reader.number();
    reader.line();
    for (auto& value : result.weapons) {
        value = static_cast<std::uint8_t>(reader.number() - 1);
    }
    result.ammunition = reader.number();
    result.terrain_variant = resources[2][2] == 'w' || resources[2][2] == 'W' ||
                             resources[2][3] == 'w' || resources[2][3] == 'W';
    for (;;) {
        reader.line();
        auto code = reader.peek();
        if (code >= 0x60) {
            code = static_cast<std::uint8_t>(code - 0x20);
        }
        if (code == 'F') {
            const auto index = result.overlays.size();
            if (index == auxiliary_limit) {
                throw std::invalid_argument("Too many mission overlays");
            }
            reader.advance(2);
            result.overlays.push_back(reader.filename());
            const auto x = reader.number();
            const auto y = reader.number();
            result.overlay_fields[80 + index] = 0;
            store(result.overlay_fields, index * 4, x);
            store(result.overlay_fields, 40 + index * 4, y);
            if (reader.peek() == ',') {
                // The original optional byte aliases the preceding height/Y field.
                result.overlay_fields[79 + index] = static_cast<std::uint8_t>(reader.number());
            }
            continue;
        }
        if (code == 'W') {
            if (result.waypoints.size() == auxiliary_limit) {
                throw std::invalid_argument("Too many mission waypoints");
            }
            GameplayWaypoint point{reader.number(), reader.number(), reader.number() << 24, 50};
            if (reader.peek() == ',') {
                point.speed = reader.number();
            }
            result.waypoints.push_back(point);
            continue;
        }
        if (code == 'R' || code == 'S') {
            if (result.lights.size() == auxiliary_limit) {
                throw std::invalid_argument("Too many mission lights");
            }
            result.last_light_type = code == 'R' ? 1 : 0;
            result.lights.push_back({reader.number(), reader.number(), result.last_light_type});
            if (reader.peek() == ',') {
                auto& color = result.last_light_type == 1 ? result.secondary_color : result.light_color;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    color[channel] = static_cast<std::uint8_t>(reader.number());
                }
            }
            continue;
        }
        if (code != '*' && (code < '0' || code > '9')) {
            break;
        }
        if (result.objects.size() == object_limit) {
            throw std::invalid_argument("Too many mission objects");
        }
        GameplayObject object{};
        object.target = code == '*' ? 1 : 0;
        result.target_count += object.target;
        const auto type = reader.number();
        if (type >= 18) {
            throw std::invalid_argument("Unsupported mission object type");
        }
        object.type = static_cast<std::uint8_t>(type);
        object.category = static_cast<std::uint8_t>(reader.number());
        result.category_count += object.category == 2 || object.category == 3 ? 1 : 0;
        object.attribute2 = static_cast<std::uint8_t>(reader.number());
        object.attribute3 = static_cast<std::uint8_t>(reader.number());
        object.x = reader.number() << 22;
        object.y = reader.number() << 22;
        object.angle = static_cast<std::uint8_t>(reader.number());
        object.attribute7 = static_cast<std::uint8_t>(reader.number());
        object.attribute8 = static_cast<std::uint8_t>(reader.number());
        object.attribute9 = static_cast<std::uint8_t>(reader.number());
        object.attribute10 = reader.number();
        result.objects.push_back(object);
    }
    return result;
}

GameplayPayload decode_payload(const std::vector<std::uint8_t>& file, PayloadKind kind, PayloadPolicy policy) {
    GameplayPayload result;
    result.kind = kind;
    std::size_t cursor;
    std::size_t size;
    if (kind == PayloadKind::picture) {
        if (file.size() < picture_header_bytes) {
            throw std::invalid_argument("Truncated picture payload");
        }
        result.width = word(file, 8) + 1;
        result.height = word(file, 10) + 1;
        size = std::size_t(result.width) * result.height;
        cursor = picture_header_bytes;
    } else {
        if (file.size() < 8 || file[0] != 'K' || file[1] != 'R' || file[2] != 'L' ||
                (file[3] != '0' && file[3] != '1')) {
            throw std::invalid_argument("Unsupported resource container");
        }
        size = word(file, 4) | (word(file, 6) << 16);
        result.allocation_suffix.resize((resource_alignment - size % resource_alignment) % resource_alignment);
        cursor = 8;
        if (file[3] == '0') {
            if (size > maximum_payload_bytes || size > file.size() - cursor) {
                throw std::invalid_argument("Truncated raw resource");
            }
            result.bytes.assign(file.begin() + cursor, file.begin() + cursor + size);
            return result;
        }
    }
    result.bytes = expand(file, cursor, size, policy);
    if (policy == PayloadPolicy::strict_frontend && (cursor == file.size() || file[cursor] != 12)) {
        throw std::invalid_argument("Missing frontend picture palette");
    }
    if (kind == PayloadKind::picture && cursor < file.size() && file[cursor] == 12) {
        if (file.size() - ++cursor < 768) {
            throw std::invalid_argument("Truncated payload palette");
        }
        for (std::size_t channel = 0; channel < 768; ++channel) {
            result.palette.push_back(file[cursor++] >> 2);
        }
    }
    if (policy == PayloadPolicy::strict_frontend && cursor != file.size()) {
        throw std::invalid_argument("Unexpected frontend picture trailer");
    }
    return result;
}

std::vector<GameplayPayload> load_gameplay(const GameplaySetup& setup, const MissionResources& resources,
                                         const PayloadReader& reader) {
    if (!reader) {
        throw std::invalid_argument("Gameplay payload reader is required");
    }
    std::vector<GameplayPayload> payloads;
    const auto load = [&](const std::string& name, PayloadKind kind) {
        auto payload = reader(name, kind);
        payload.name = name;
        payloads.push_back(std::move(payload));
    };
    load("SND1.RLE", PayloadKind::resource);
    for (std::size_t index = 0; index < 3; ++index) {
        load(filename(resources[index]), PayloadKind::picture);
    }
    for (const auto& overlay : setup.overlays) {
        load(filename(overlay), PayloadKind::resource);
    }
    for (std::size_t index = 11; index < resources.size() && resources[index][0] != '-'; ++index) {
        load(filename(resources[index]), PayloadKind::resource);
    }
    for (const auto* name : {"CHAF.RLE", "STGR.RLE", "HELL.RLE", "ROCK.RLE", "BULL.RLE"}) {
        load(name, PayloadKind::resource);
    }
    load(filename(resources[4]), PayloadKind::resource);
    load("CONS.RLE", PayloadKind::resource);
    load("HUDS.RLE", PayloadKind::resource);
    load(filename(resources[6]), PayloadKind::resource);
    load("SPEAK0.RLE", PayloadKind::resource);
    load(filename(resources[3]), PayloadKind::resource);
    load(filename(resources[5]), PayloadKind::resource);
    load("CONSOL1S.DTA", PayloadKind::picture);
    return payloads;
}

} // namespace comanchecd
