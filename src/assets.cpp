#include "comanchecd/assets.hpp"
#include "loading_work.hpp"
#include "mission_work.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace comanchecd {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t packed_bytes = 67221;
constexpr std::size_t image_bytes = 404738;
constexpr std::size_t protected_image = 0x1800;
constexpr std::size_t picture_header_bytes = 128;
constexpr std::size_t picture_width = line_stride * plane_count;
constexpr std::size_t picture_height = 240;
constexpr std::size_t campaign_header_bytes = 23;
constexpr std::size_t campaign_row_bytes = 32;
constexpr std::size_t campaign_limit = 10;
constexpr std::size_t maximum_file_bytes = 16 * 1024 * 1024;
constexpr std::uint64_t supported_executable_identity = 0xca98bc78eef27ed8ull;
constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

std::string mission_filename(const MissionFilename& field) {
    const auto end = std::find(field.begin(), field.end(), 0);
    auto name = std::string(field.begin(), end);
    name.erase(name.find_last_not_of(' ') + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char code) {
        return static_cast<char>(std::toupper(code));
    });
    return name;
}

std::uint16_t word(const Bytes& bytes, std::size_t offset) {
    if (offset >= bytes.size() || bytes.size() - offset < 2) {
        throw std::invalid_argument("Truncated resource word");
    }
    return bytes[offset] | (std::uint16_t(bytes[offset + 1]) << 8);
}

std::uint32_t integer(const Bytes& bytes, std::size_t offset) {
    return std::uint32_t(word(bytes, offset)) | (std::uint32_t(word(bytes, offset + 2)) << 16);
}

Bytes slice(const Bytes& bytes, std::size_t offset, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        throw std::invalid_argument("Truncated embedded resource");
    }
    return Bytes(bytes.begin() + offset, bytes.begin() + offset + count);
}

Bytes table(const Bytes& bytes, std::size_t offset) {
    if (offset >= bytes.size()) {
        throw std::invalid_argument("Missing menu table");
    }
    const auto end = std::find(bytes.begin() + offset, bytes.end(), 255);
    if (end == bytes.end()) {
        throw std::invalid_argument("Unterminated menu table");
    }
    return Bytes(bytes.begin() + offset, end + 1);
}

class PackedReader {
public:
    PackedReader(const Bytes& file, std::size_t offset) : file_(file), offset_(offset), bits_(word(file, offset)) {
        offset_ += 2;
    }

    std::uint8_t byte() {
        if (offset_ >= file_.size()) {
            throw std::invalid_argument("Truncated LZ91 stream");
        }
        return file_[offset_++];
    }

    unsigned bit() {
        const auto value = bits_ & 1;
        bits_ >>= 1;
        if (--remaining_ == 0) {
            bits_ = word(file_, offset_);
            offset_ += 2;
            remaining_ = 16;
        }
        return value;
    }

private:
    const Bytes& file_;
    std::size_t offset_;
    std::uint16_t bits_;
    unsigned remaining_ = 16;
};

Bytes unpack(const Bytes& file) {
    // Version guard: FNV-1a identifies the supported packed CD executable.
    std::uint64_t identity = fnv_offset_basis;
    for (const auto byte : file) {
        identity = (identity ^ byte) * fnv_prime;
    }
    if (file.size() != packed_bytes || identity != supported_executable_identity) {
        throw std::invalid_argument("Unsupported COMANCHE.EXE version");
    }
    const auto code = std::size_t(word(file, 8) + word(file, 22)) * 16;
    const auto compressed_paragraphs = word(file, code + 8);
    if (code < std::size_t(compressed_paragraphs) * 16) {
        throw std::invalid_argument("Invalid LZ91 stream offset");
    }
    PackedReader input(file, code - std::size_t(compressed_paragraphs) * 16);
    Bytes output;
    output.reserve(image_bytes);
    while (output.size() <= image_bytes) {
        if (input.bit() != 0) {
            output.push_back(input.byte());
            continue;
        }
        unsigned length;
        int displacement;
        if (input.bit() == 0) {
            const auto high = input.bit();
            length = (high << 1) + input.bit() + 2;
            displacement = int(input.byte()) - 256;
        } else {
            const auto low = input.byte();
            const auto high = input.byte();
            displacement = int(low | ((high & 0xf8) << 5) | 0xe000) - 65536;
            length = (high & 7) + 2;
            if (length == 2) {
                length = input.byte();
                if (length == 0) {
                    if (output.size() != image_bytes) {
                        throw std::invalid_argument("Unexpected unpacked image size");
                    }
                    return output;
                }
                if (length == 1) {
                    continue;
                }
                ++length;
            }
        }
        const auto distance = std::size_t(-displacement);
        if (distance > output.size() || length > image_bytes - output.size()) {
            throw std::invalid_argument("Invalid LZ91 back reference");
        }
        for (unsigned index = 0; index < length; ++index) {
            output.push_back(output[output.size() - distance]);
        }
    }
    throw std::invalid_argument("Unterminated LZ91 image");
}

Bytes decode_mission(Bytes bytes) {
    if (bytes.empty()) {
        throw std::invalid_argument("Empty mission file");
    }
    if (bytes.front() == '{') {
        return bytes;
    }
    const std::array<std::uint32_t, 2> keys = bytes.front() == 'x' ?
        std::array<std::uint32_t, 2>{0x11120603, 0x11120603} :
        std::array<std::uint32_t, 2>{0x3aa2a6a3, 0x7eaa3d8a};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] ^= static_cast<std::uint8_t>(keys[(index / 4) % 2] >> ((index % 4) * 8));
    }
    return bytes;
}
}

FrontendPicture decode_frontend_picture(const std::vector<std::uint8_t>& file) {
    constexpr unsigned credits_height = 245;
    if (file.size() < picture_header_bytes || word(file, 8) + 1 != picture_width ||
            (word(file, 10) + 1 != picture_height && word(file, 10) + 1 != credits_height)) {
        throw std::invalid_argument("Unsupported frontend picture dimensions");
    }
    const auto decoded = decode_payload(file, PayloadKind::picture, PayloadPolicy::strict_frontend);
    FrontendPicture result{};
    std::size_t offset = 0;
    for (auto& color : result.palette) {
        for (auto& channel : color) {
            channel = decoded.palette[offset++];
        }
    }
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        result.background[plane].resize(background_bytes, 0);
        for (std::size_t pixel = plane; pixel < std::min(decoded.bytes.size(), background_bytes * plane_count); pixel += plane_count) {
            result.background[plane][pixel / plane_count] = decoded.bytes[pixel];
        }
    }
    return result;
}

Installation::Installation(std::filesystem::path directory, std::filesystem::path payload_directory)
    : directory_(std::move(directory)), payload_directory_(std::move(payload_directory)) {}

std::vector<std::uint8_t> Installation::read(const std::string& name) const {
    auto normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char code) {
        return static_cast<char>(std::toupper(code));
    });
    if (normalized.empty() || normalized.find_first_of("/\\:") != std::string::npos || normalized.find("..") != std::string::npos) {
        throw std::invalid_argument("Expected an installation resource filename");
    }
    auto path = directory_ / normalized;
    if (!std::filesystem::exists(path) && !payload_directory_.empty()) {
        path = payload_directory_ / normalized;
    }
    if (std::filesystem::file_size(path) > maximum_file_bytes) {
        throw std::invalid_argument("Resource exceeds supported file size: " + name);
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    return Bytes(std::istreambuf_iterator<char>(file), {});
}

FrontendAssets Installation::load_frontend() const {
    const auto image = unpack(read("COMANCHE.EXE"));
    const auto resource = [&](std::size_t offset, std::size_t count) {
        return slice(image, protected_image + offset, count);
    };
    FrontendAssets assets{};
    assets.font = resource(0x2a47d, 0x2b1e5 - 0x2a47d);
    assets.pilot.menu_header = resource(0x2cd5c, 0x12);
    assets.pilot.editor_prompt = resource(0x2b411, 54);
    const auto copy_resource = [&](std::size_t offset, auto& target) {
        const auto bytes = resource(offset, target.size());
        std::copy(bytes.begin(), bytes.end(), target.begin());
    };
    copy_resource(0x2b3fc, assets.pilot.row_prefix);
    copy_resource(0x2b405, assets.pilot.row_tail);
    copy_resource(0x2a485, assets.pilot.glyph_widths);
    copy_resource(0x19fd, assets.pilot.normal_keys);
    copy_resource(0x197d, assets.pilot.shifted_keys);
    copy_resource(0x2f160, assets.transfer);
    assets.pilot_record = read("COMANCHE.NAM");
    PilotStorage checked(assets.pilot_record);
    assets.main_menu = table(image, protected_image + 0x2d31e);
    assets.mission_menu = table(image, protected_image + 0x2d56d);
    assets.briefing_choices = table(image, protected_image + 0x2d53d);
    assets.loading_prompt = table(image, protected_image + 0x2d6c5);
    assets.statistics = resource(0x2bf57, 130);
    assets.locked_prompt = table(image, protected_image + 0x29a9a);
    assets.settings.header = table(image, protected_image + 0x29590);
    constexpr std::array<std::size_t, 4> settings_menus{0x295d7, 0x2963e, 0x2969f, 0x297af};
    for (std::size_t category = 0; category < settings_menus.size(); ++category) {
        auto& menu = assets.settings.menus[category];
        menu = table(image, protected_image + settings_menus[category]);
        unsigned width = 0, maximum = 0;
        // 29998 skips the initial marker before measuring the first row.
        for (std::size_t index = 1; index < menu.size(); ++index) {
            const auto code = menu[index];
            if (code == 0 || code == 255) {
                maximum = std::max(maximum, width);
                width = 0;
                ++assets.settings.rows[category];
            } else {
                width += assets.pilot.glyph_widths[code];
            }
        }
        assets.settings.widths[category] = static_cast<std::uint8_t>((maximum + 18) >> 2);
    }
    assets.settings.about = table(image, protected_image + 0x299e2);
    assets.settings.saved = table(image, protected_image + 0x29a81);
    assets.debrief.text = resource(0x2ba85, 255);
    for (std::size_t index = 0; index < assets.debrief.messages.size(); ++index) {
        assets.debrief.messages[index] = resource(0x2b90d + index * 47, 47);
    }
    for (std::size_t index = 0; index < assets.gameplay_presets.size(); ++index) {
        const auto offset = protected_image + 0x2e294 + index * 4;
        assets.gameplay_presets[index] = word(image, offset) | (std::uint32_t(word(image, offset + 2)) << 16);
    }
    for (std::size_t index = 0; index < assets.mission_resources.size(); ++index) {
        const auto pointer = protected_image + 0x2db34 + index * 4;
        const std::size_t address = word(image, pointer) | (std::uint32_t(word(image, pointer + 2)) << 16);
        copy_resource(address, assets.mission_resources[index]);
    }
    const auto campaign = table(image, protected_image + 0x2d3d8);
    assets.campaign_menu = slice(campaign, 0, campaign_header_bytes);
    for (std::size_t index = 0; index < campaign_limit; ++index) {
        const auto name = std::to_string(index) + ".MIS";
        if (!std::filesystem::exists(directory_ / name)) {
            break;
        }
        const auto mission = decode_mission(read(name));
        const auto start = std::find(mission.begin(), mission.end(), '{');
        const auto end = std::find(start, mission.end(), '}');
        if (start == mission.end() || end == mission.end() || end - start - 1 > 29) {
            throw std::invalid_argument("Invalid campaign title: " + name);
        }
        auto row = slice(campaign, campaign_header_bytes + index * campaign_row_bytes, campaign_row_bytes);
        std::copy(start + 1, end, row.begin() + 2);
        assets.campaign_menu.insert(assets.campaign_menu.end(), row.begin(), row.end());
        ++assets.campaign_count;
    }
    if (assets.campaign_count == 0) {
        throw std::invalid_argument("No supported campaigns found");
    }
    assets.campaign_menu.insert(assets.campaign_menu.end(), {0, 0, 255});
    return assets;
}

MenuSettings Installation::load_menu_settings() const {
    if (std::filesystem::exists(directory_ / "COMANCHE.MNU")) {
        return MenuSettings(read("COMANCHE.MNU"));
    }
    const auto image = unpack(read("COMANCHE.EXE"));
    return MenuSettings(slice(image, protected_image + 0x28d24, MenuSettings::record_bytes));
}

KdvMovie Installation::load_overview() const {
    return KdvMovie(load_payload("HANGER.KDV", PayloadKind::resource).bytes);
}

DemoRecording Installation::load_demo(std::uint8_t index) const {
    constexpr std::uint8_t demo_count = 4;
    if (index >= demo_count) {
        throw std::invalid_argument("Demo index exceeds the original attract cycle");
    }
    return DemoRecording(read("DEMO" + std::to_string(index) + ".SCR"));
}

MusicPlaylist Installation::load_menu_music() const {
    return {read("MENUMUS1.MS3"), read("MENUMUS2.MS3")};
}

PilotMenuAssets Installation::load_pilot_menu() const {
    const auto resource = [&](const char* name) {
        return StartupResource{read(name), std::filesystem::exists(directory_ / name) ?
            StartupLocation::local : StartupLocation::cdrom};
    };
    return {{resource("MENUMUS1.MS3"), resource("MENUMUS2.MS3")}, resource("RAH66.DTA")};
}

DemoCinematics Installation::load_demo_cinematics() const {
    const auto payload = [&](const char* name) { return load_payload(name, PayloadKind::resource).bytes; };
    return {KdvMovie(payload("TANIM.KDV")), KdvMovie(payload("TITLE.KDV")), load_overview(),
            decode_frontend_picture(read("BACK.DTA")), decode_frontend_picture(read("HANGER.DTA")),
            payload("FANFARE.RLE"), payload("M_CANYON.RLE"), read("MENUMUS2.MS3"),
            payload("SND1.RLE"), read("TANIM.SND")};
}

FanfareAssets Installation::load_fanfare() const {
    FanfareAssets result;
    result.score = read("FANFARE.RLE");
    result.picture = read("BACK.DTA");
    result.score_location = std::filesystem::exists(directory_ / "FANFARE.RLE") ? StartupLocation::local : StartupLocation::cdrom;
    result.picture_location = std::filesystem::exists(directory_ / "BACK.DTA") ? StartupLocation::local : StartupLocation::cdrom;
    const auto image = unpack(read("COMANCHE.EXE"));
    const auto transfer = slice(image, protected_image + 0x2f160, result.transfer.size());
    std::copy(transfer.begin(), transfer.end(), result.transfer.begin());
    return result;
}

StartupMovieAssets Installation::load_startup_movie() const {
    const auto resource = [&](const char* name) {
        return StartupResource{read(name), std::filesystem::exists(directory_ / name) ?
            StartupLocation::local : StartupLocation::cdrom};
    };
    return {resource("TANIM.SND"), resource("M_CANYON.RLE"), resource("SND1.RLE"), resource("TANIM.KDV")};
}

MusicPlaylist Installation::load_debrief_music(const MissionResources& resources, const MissionResult& result) const {
    constexpr std::size_t success_music = 8, failure_music = 10;
    const auto& field = resources[result.survived && result.completed ? success_music : failure_music];
    return {{}, load_payload(mission_filename(field), PayloadKind::resource).bytes};
}

FrontendPicture Installation::load_mission_picture(const MissionFilename& field) const {
    return decode_frontend_picture(read(mission_filename(field)));
}

FrontendPicture Installation::load_debrief(const MissionResources& resources, FrontendScene scene, MissionAudio& audio) const {
    if (scene != FrontendScene::debrief && scene != FrontendScene::debrief_success) {
        throw std::invalid_argument("Debrief resources require a mission result scene");
    }
    constexpr unsigned filename_bytes = 13;
    const auto picture_slot = scene == FrontendScene::debrief_success ? 7u : 9u;
    const auto name = mission_filename(resources[picture_slot]);
    const auto encoded = read(name);
    auto picture = decode_frontend_picture(encoded);
    auto& clock = audio.execution_clock();
    clock.execute(CpuOperation::instructions, 2); // Picture destination and CALL.
    const auto payload = audio.load_mission_payload({encoded, std::filesystem::exists(directory_ / name) ?
        StartupLocation::local : StartupLocation::cdrom}, PayloadKind::picture, filename_bytes);
    clock.execute(CpuOperation::instructions, 1);
    auto score = load_payload(mission_filename(resources[picture_slot + 1]), PayloadKind::resource, audio, filename_bytes);
    clock.execute(CpuOperation::instructions, scene == FrontendScene::debrief_success ? 1 : 2);
    audio.present_debrief({{}, std::move(score.bytes)}, payload.height);
    picture.presentation = PicturePresentation::complete;
    return picture;
}

FrontendPicture Installation::load_picture(FrontendScene scene) const {
    return decode_frontend_picture(read(scene == FrontendScene::operations ? "RAH66.DTA" :
        scene == FrontendScene::campaign ? "MISSION.DTA" : scene == FrontendScene::credits ? "CREDIT1.DTA" :
        scene == FrontendScene::overview ? "HANGER.DTA" : "LOSE4.DTA"));
}

StartupCampaign Installation::campaign_file(std::uint8_t campaign) const {
    if (campaign >= campaign_limit) {
        throw std::invalid_argument("Campaign file index out of range");
    }
    const auto name = std::to_string(campaign) + ".MIS";
    StartupCampaign file;
    file.encoded = read(name);
    file.decoded = decode_mission(file.encoded);
    file.find_location = !payload_directory_.empty() && std::filesystem::exists(payload_directory_ / name) ?
        StartupLocation::cdrom : StartupLocation::local;
    file.read_location = std::filesystem::exists(directory_ / name) ? StartupLocation::local : StartupLocation::cdrom;
    return file;
}

CampaignMissions Installation::load_missions(std::uint8_t campaign) const {
    return parse_campaign(campaign_file(campaign).decoded);
}

CampaignMissions Installation::load_missions(std::uint8_t campaign, MissionAudio& audio, CampaignLoad phase) const {
    const auto file = campaign_file(campaign);
    auto result = parse_campaign(file.decoded);
    audio.load_campaign(file);
    if (phase == CampaignLoad::catalog) {
        mission_work::catalog(result, [&](CpuOperation op, unsigned n) { audio.execution_clock().execute(op, n); });
    }
    return result;
}

GameplayPayload Installation::load_payload(const std::string& name, PayloadKind kind) const {
    return decode_payload(read(name), kind);
}

GameplayPayload Installation::load_payload(const std::string& name, PayloadKind kind, MissionAudio& audio, unsigned filename_bytes) const {
    return audio.load_mission_payload({read(name), std::filesystem::exists(directory_ / name) ?
        StartupLocation::local : StartupLocation::cdrom}, kind, filename_bytes);
}

FrontendPicture Installation::load_campaign_picture(MissionAudio& audio) const {
    const auto encoded = read("MISSION.DTA");
    auto result = decode_frontend_picture(encoded);
    audio.execution_clock().execute(CpuOperation::instructions, 2); // Destination and 2c422 CALL.
    audio.load_menu_picture({encoded, std::filesystem::exists(directory_ / "MISSION.DTA") ?
        StartupLocation::local : StartupLocation::cdrom}, 12);
    result.presentation = PicturePresentation::complete;
    return result;
}

GameplayPayload Installation::load_picture_payload(const std::string& name, ExecutionClock& clock) const {
    return loading_work::picture(read(name), clock);
}

SceneEntry Installation::load_scene_entry() const {
    const auto image = unpack(read("COMANCHE.EXE"));
    SceneEntry result;
    for (std::size_t slot = 0; slot < scene_slots; ++slot) {
        auto& entry = result.entities[slot];
        entry.previous_x = integer(image, protected_image + 0x21ebc + slot * 4);
        entry.previous_y = integer(image, protected_image + 0x220ec + slot * 4);
        entry.previous_height = image.at(protected_image + 0x229af + slot);
        entry.animation_frame = image.at(protected_image + 0x22897 + slot);
        entry.rotor_phase = image.at(protected_image + 0x22e87 + slot);
    }
    return result;
}

CockpitTables Installation::load_cockpit_tables() const {
    const auto image = unpack(read("COMANCHE.EXE"));
    CockpitTables result;
    for (std::size_t index = 0; index < result.panel_y.size(); ++index) {
        result.panel_y[index] = image.at(protected_image + 0x7b14 + index);
        result.left_panel_x[index] = image.at(protected_image + 0x7b1a + index);
        result.right_panel_x[index] = image.at(protected_image + 0x7b20 + index);
        result.weapon_flash[index] = image.at(protected_image + 0x7c01 + index);
    }
    for (std::size_t index = 0; index < result.shake_displacement.size(); ++index) {
        result.shake_displacement[index] = integer(image, protected_image + 0x16630 + index * 4);
    }
    result.map_scale = integer(image, protected_image + 0x1a918);
    result.chaff = image.at(protected_image + 0x1aa29);
    result.flares = image.at(protected_image + 0x1aa2a);
    result.center_marker = image.at(protected_image + 0x297af) != 0x7b;
    result.heading_tape = image.at(protected_image + 0x297d5) != 0x7b;
    result.flight_hud = image.at(protected_image + 0x297c4) != 0x7b;
    return result;
}

TerrainTables Installation::load_terrain_tables(TerrainDetail detail) const {
    const auto image = unpack(read("COMANCHE.EXE"));
    const std::array<const char*, 3> names{"HIGH.DTL", "MEDIUM.DTL", "LOW.DTL"};
    const auto index = static_cast<std::size_t>(detail);
    if (index >= names.size()) {
        throw std::invalid_argument("Unsupported terrain detail setting");
    }
    const auto distances = read(names[index]);
    constexpr std::size_t detail_bytes = 4 + 5 * terrain_distance_capacity * 4;
    if (distances.size() != detail_bytes) {
        throw std::invalid_argument("Invalid terrain detail resource size");
    }
    TerrainTables result;
    result.levels = integer(distances, 0);
    for (std::size_t sample = 0; sample < result.sine.size(); ++sample) {
        result.sine[sample] = static_cast<std::int32_t>(integer(image, protected_image + 0x35f10 + sample * 4));
    }
    for (std::size_t table = 0; table < result.distance.size(); ++table) {
        for (std::size_t level = 0; level < terrain_distance_capacity; ++level) {
            result.distance[table][level] = integer(distances, 4 + (table * terrain_distance_capacity + level) * 4);
        }
    }
    for (std::size_t octant = 0; octant < result.angle_octants.size(); ++octant) {
        result.angle_octants[octant] = integer(image, protected_image + 0x236b1 + octant * 4);
    }
    for (std::size_t ratio = 0; ratio < result.angle_ratio.size(); ++ratio) {
        result.angle_ratio[ratio] = image.at(protected_image + 0x236d1 + ratio);
    }
    for (std::size_t scale = 0; scale < result.sprite_offsets.size(); ++scale) {
        result.sprite_offsets[scale] = integer(image, protected_image + 0x1e49c + scale * 4);
    }
    result.sprite_steps = slice(image, protected_image + 0x1e89c, 0x2041);
    result.initial_height_gap = image.at(protected_image + 0x1aa99);
    for (std::size_t index = 0; index < result.gust.size(); ++index) {
        result.gust[index] = static_cast<std::int8_t>(image.at(protected_image + 0x28058 + index));
    }
    for (std::size_t index = 0; index < result.transfer.size(); ++index) {
        result.transfer[index] = image.at(protected_image + 0x2f160 + index);
    }
    return result;
}

StartupFiles Installation::load_startup_files() const {
    StartupFiles result;
    if (std::filesystem::exists(directory_ / "COMANCHE.CDR")) {
        result.cdrom = read("COMANCHE.CDR");
    }
    result.configuration = read("COMANCHE.CFG");
    result.campaigns.emplace();
    for (unsigned index = 0; index < campaign_limit; ++index) {
        const auto name = std::to_string(index) + ".MIS";
        const bool local = std::filesystem::exists(directory_ / name);
        const bool cdrom = !payload_directory_.empty() && std::filesystem::exists(payload_directory_ / name);
        if (!local && !cdrom) {
            result.campaigns->emplace_back();
            continue;
        }
        result.campaigns->push_back(campaign_file(static_cast<std::uint8_t>(index)));
    }
    result.loading.emplace();
    auto& loading = *result.loading;
    if (std::filesystem::exists(directory_ / "COMANCHE.NAM")) { loading.pilots = read("COMANCHE.NAM"); }
    if (std::filesystem::exists(directory_ / "COMANCHE.MNU")) { loading.settings = read("COMANCHE.MNU"); }
    const auto image = unpack(read("COMANCHE.EXE"));
    loading.active_settings = loading.settings ? *loading.settings : slice(image, protected_image + 0x28d24, MenuSettings::record_bytes);
    const MenuSettings settings(loading.active_settings);
    constexpr std::array<const char*, 3> detail_names{"HIGH.DTL", "MEDIUM.DTL", "LOW.DTL"};
    const auto name = detail_names.at(settings.detail());
    loading.detail = read(name);
    loading.detail_location = std::filesystem::exists(directory_ / name) ? StartupLocation::local : StartupLocation::cdrom;
    constexpr std::array<std::size_t, 5> menu_addresses{0x295d7, 0x2963e, 0x2969f, 0x297af, 0x29856};
    constexpr std::array<std::size_t, 15> flag_addresses{0x29763, 0x2977d, 0x29670, 0x29679, 0x29688, 0x29691,
        0x297af, 0x297c4, 0x297d5, 0x297e3, 0x297f1, 0x2980b, 0x29821, 0x2983c, 0x29849};
    constexpr std::array<unsigned, 15> flag_fields{41, 42, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};
    auto menu_image = image;
    for (std::size_t index = 0; index < flag_fields.size(); ++index) {
        menu_image[protected_image + flag_addresses[index]] = loading.active_settings[flag_fields[index]];
    }
    for (std::size_t index = 0; index < menu_addresses.size(); ++index) {
        loading.menus[index] = table(menu_image, protected_image + menu_addresses[index]);
    }
    const auto widths = slice(image, protected_image + 0x2a485, loading.glyph_widths.size());
    std::copy(widths.begin(), widths.end(), loading.glyph_widths.begin());
    return result;
}

AudioTables Installation::load_audio() const {
    const auto image = unpack(read("COMANCHE.EXE"));
    AudioTables result;
    for (std::size_t index = 0; index < result.instruments.size(); ++index) {
        const auto data = slice(image, protected_image + 0x30139 + index * 16, 16);
        std::copy(data.begin(), data.end(), result.instruments[index].begin());
        result.pitch[index] = image.at(protected_image + 0x30efc + index);
        result.volume[index] = image.at(protected_image + 0x30f7c + index);
        const auto pointer = protected_image + 0x31012 + index * 4;
        auto position = protected_image + (word(image, pointer) | (std::uint32_t(word(image, pointer + 2)) << 16));
        for (std::size_t count = 0; count < 512; ++count) {
            const auto value = image.at(position++);
            result.envelopes[index].push_back(value);
            if (value == 0x80 || value == 0x81) {
                break;
            }
        }
        if (result.envelopes[index].back() != 0x80 && result.envelopes[index].back() != 0x81) {
            throw std::invalid_argument("Unterminated audio envelope");
        }
    }
    for (std::size_t index = 0; index < result.frequencies.size(); ++index) {
        result.frequencies[index] = word(image, protected_image + 0x30939 + index * 2);
    }
    const auto conversion = slice(image, protected_image + 0x4df9, result.sample_conversion.size());
    std::copy(conversion.begin(), conversion.end(), result.sample_conversion.begin());
    const auto soft_conversion = slice(image, protected_image + 0x4bf9, result.soft_sample_conversion.size());
    std::copy(soft_conversion.begin(), soft_conversion.end(), result.soft_sample_conversion.begin());
    return result;
}

void Installation::save_pilots(const std::vector<std::uint8_t>& record) const {
    if (record.size() != pilot_record_bytes) {
        throw std::invalid_argument("Expected complete pilot persistence record");
    }
    std::ofstream file(directory_ / "COMANCHE.NAM", std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
    if (!file) {
        throw std::runtime_error("Cannot save COMANCHE.NAM");
    }
}

} // namespace comanchecd
