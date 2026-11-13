#include "comanchecd/cockpit.hpp"
#include "color_metric.hpp"

#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr std::size_t scene_copy_rows = 182;
constexpr std::uint8_t transparent_picture = 255;

void instructions(const CpuWorkSink& work, unsigned count) {
    if (work) { work(CpuOperation::instructions, count); }
}

void copy_span(const CpuWorkSink& work, std::size_t destination, unsigned length, bool leading, bool trailing) {
    if (!work) { return; }
    // 83cc/8461 load one span and select its optional partial-plane edges.
    instructions(work, 8);
    if (leading) {
        instructions(work, 3);
        work(CpuOperation::port_write_word, 1);
        instructions(work, 3);
        work(CpuOperation::port_write_word, 1);
        instructions(work, 1);
    }
    instructions(work, 2);
    if ((destination & 1) != 0) {
        work(CpuOperation::string_bytes, 1);
        instructions(work, 2);
        --length;
    }
    if (length != 0) {
        instructions(work, 2);
        if (length / 2 != 0) {
            work(CpuOperation::repeat_copy_words, length / 2);
            instructions(work, 1);
        }
        if ((length & 1) != 0) { work(CpuOperation::string_bytes, 1); }
    }
    instructions(work, 2);
    if (trailing) {
        instructions(work, 3);
        work(CpuOperation::port_write_word, 1);
        work(CpuOperation::string_bytes, 1);
        instructions(work, 1);
        work(CpuOperation::port_write_word, 1);
        instructions(work, 1);
    }
    instructions(work, 4);
}

std::uint32_t integer(const std::vector<std::uint8_t>& bytes, std::size_t position, std::size_t count) {
    if (position > bytes.size() || count > bytes.size() - position) {
        throw std::invalid_argument("Truncated cockpit sprite");
    }
    std::uint32_t result = 0;
    for (std::size_t byte = 0; byte < count; ++byte) {
        result |= std::uint32_t(bytes[position + byte]) << (byte * 8);
    }
    return result;
}

std::int32_t multiply_high(std::int32_t first, std::int32_t second) {
    return static_cast<std::int32_t>((std::int64_t(first) * second) >> 32);
}

std::uint32_t magnitude(std::int32_t value) {
    return std::uint32_t(value) ^ std::uint32_t(value >> 31);
}

struct Sprite {
    std::size_t width, height, source;
    std::int32_t x_origin, y_origin;
};

Sprite sprite(const std::vector<std::uint8_t>& bytes, std::uint8_t number) {
    if (number == 0 || bytes.size() < 8 || bytes[0] != 'S' || bytes[1] != 'G' || number > bytes[2]) {
        throw std::invalid_argument("Invalid cockpit sprite frame");
    }
    const auto entry = std::size_t(number) * 8;
    Sprite result{integer(bytes, entry, 1), integer(bytes, entry + 1, 1), integer(bytes, entry + 4, 4),
        static_cast<std::int8_t>(integer(bytes, entry + 2, 1)),
        static_cast<std::int8_t>(integer(bytes, entry + 3, 1))};
    if (result.width == 0 || result.height == 0 || result.source > bytes.size() ||
            result.width * result.height > bytes.size() - result.source) {
        throw std::invalid_argument("Invalid cockpit sprite extent");
    }
    return result;
}

std::vector<std::uint8_t> picture(const GameplayPayload& payload, const Palette& destination, const Transfer& transfer) {
    if (payload.width != cockpit_backing_width || payload.height < cockpit_height || payload.height > 290 ||
            payload.bytes.size() != payload.width * payload.height || payload.palette.size() != 768) {
        throw std::invalid_argument("Unsupported cockpit picture");
    }
    ColorMap mapping{};
    mapping[255] = 255;
    for (std::size_t index = 1; index < 255; ++index) {
        const std::array<std::uint8_t, 3> color{payload.palette[index * 3], payload.palette[index * 3 + 1], payload.palette[index * 3 + 2]};
        mapping[index] = color_metric::nearest(destination, transfer, color, 1, 254);
    }
    auto result = payload.bytes;
    for (auto& pixel : result) {
        pixel = mapping[pixel];
    }
    return result;
}
}

CockpitAssets materialize_cockpit(const GameplayPayload& normal, const GameplayPayload& crash,
                                const SceneAssets& scene, const Transfer& transfer) {
    CockpitAssets result;
    result.palette = scene.terrain.palette;
    result.theme = build_theme(result.palette, transfer);
    result.normal_picture = picture(normal, result.palette, transfer);
    result.crash_picture = picture(crash, result.palette, transfer);
    if (scene.models.size() < 3) {
        throw std::invalid_argument("Missing cockpit sprite resources");
    }
    result.console = scene.models[scene.models.size() - 3].bytes;
    result.hud = scene.models[scene.models.size() - 2].bytes;
    sprite(result.console, 57);
    sprite(result.hud, 33);
    return result;
}

CockpitRenderer::CockpitRenderer(CockpitAssets assets, CockpitTables tables, const FlightState& player, CockpitEntry entry)
    : assets_(std::move(assets)), tables_(std::move(tables)), state_(std::move(entry)),
      surface_(state_.planes.empty() ? std::vector<std::uint8_t>(plane_count * plane_bytes) : state_.planes) {
    if (state_.back_page != 0 && state_.back_page != page_displacement) {
        throw std::invalid_argument("Invalid cockpit back page");
    }
    const auto fresh = state_.planes.empty();
    std::vector<std::uint8_t>().swap(state_.planes);
    if (fresh) {
        load_picture(player.cockpit_mode);
        stores(player);
        lamps();
    }
}

const PlanarSurface& CockpitRenderer::surface() const { return surface_; }
const CockpitEntry& CockpitRenderer::state() const { return state_; }
void CockpitRenderer::refresh_stores(const FlightState& player, const CpuWorkSink& work) {
    ammunition(player, work);
    stores_pending_ = true;
    if (player.cockpit_mode == 1) { countermeasures(player); }
}
void CockpitRenderer::refresh_countermeasures(const FlightState& player, const CpuWorkSink& work) {
    countermeasures(player, work);
    countermeasures_pending_ = true;
}
void CockpitRenderer::update_pending(const FlightState& player, const CpuWorkSink& work) {
    instructions(work, 4); // 72ae..72ca, lamp and stores guards.
    if (stores_pending_) {
        stores_pending_ = false;
        instructions(work, 2); // DEC and CALL 7c10.
        ammunition(player, work);
    }
    instructions(work, 2);
    if (countermeasures_pending_) {
        countermeasures_pending_ = false;
        instructions(work, 2); // DEC and CALL 7d00.
        countermeasures(player, work);
    }
}
void CockpitRenderer::select_display(InstrumentPanel panel, InstrumentDisplay display, const CpuWorkSink& work) {
    if (display > InstrumentDisplay::disabled) { throw std::invalid_argument("Invalid instrument display"); }
    auto& current = panel == InstrumentPanel::left ? state_.controls.left : state_.controls.right;
    auto& previous = panel == InstrumentPanel::left ? state_.previous_controls.left : state_.previous_controls.right;
    previous = current;
    current = display;
    lamps(work);
}
void CockpitRenderer::set_zoom(MapZoom zoom) {
    constexpr std::int32_t zoom_step = 0x00800000;
    state_.zoom_step = zoom == MapZoom::in ? -zoom_step : zoom == MapZoom::out ? zoom_step : 0;
}
void CockpitRenderer::restore_console(const FlightState& player) {
    if (player.cockpit_mode == 1) {
        stores(player);
        lamps();
    }
}

void CockpitRenderer::load_picture(std::uint8_t mode) {
    if (mode != 1 && mode != 6 && mode != 7) {
        throw std::invalid_argument("Unsupported cockpit view mode");
    }
    state_.mode = mode;
    state_.nearby = 0; // 1a311 clears radar proximity when the view picture changes.
    const auto& pixels = mode == 1 ? assets_.normal_picture : assets_.crash_picture;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        surface_.write(state_.back_page + index / plane_count, std::uint8_t(1u << (index % plane_count)), pixels[index]);
    }
}

void CockpitRenderer::pixel(std::int32_t x, std::int32_t y, std::uint8_t color) {
    const auto offset = std::int64_t(state_.back_page + panel_offset_) + std::int64_t(y) * line_stride + (x >> 2);
    if (offset < 0 || offset >= plane_bytes) {
        throw std::invalid_argument("Cockpit drawing exceeds planar memory");
    }
    surface_.write(static_cast<std::size_t>(offset), std::uint8_t(1u << (x & 3)), color);
}

void CockpitRenderer::console(std::uint8_t number, std::int32_t x, std::uint8_t y, const CpuWorkSink& work) {
    const auto item = sprite(assets_.console, number);
    const auto width = item.width >> 2;
    const auto origin = std::int64_t(state_.back_page + panel_offset_) + ((x - item.x_origin) >> 2) +
                        (std::int32_t(y) + item.y_origin) * line_stride;
    auto source = item.source;
    instructions(work, 21);
    for (std::size_t row = 0; row < item.height; ++row) {
        for (unsigned plane = 0; plane < plane_count; ++plane) {
            instructions(work, plane == plane_count - 1 ? 2 : 1);
            if (work) { work(CpuOperation::port_write_word, 1); }
            instructions(work, plane == plane_count - 1 ? 1 : 2);
            if (work) { work(CpuOperation::repeat_copy_bytes, width); }
            instructions(work, 1);
            for (std::size_t column = 0; column < width; ++column) {
                const auto target = origin - std::int64_t(row * line_stride) + std::int64_t(column);
                if (target < 0 || target >= plane_bytes) {
                    throw std::invalid_argument("Console sprite exceeds planar memory");
                }
                surface_.write(static_cast<std::size_t>(target), std::uint8_t(1u << plane), assets_.console.at(source++));
            }
        }
        instructions(work, 3);
    }
    instructions(work, 1);
}

void CockpitRenderer::overlay(std::uint8_t number, std::int32_t x, std::uint8_t y, const CpuWorkSink& work) {
    const auto item = sprite(assets_.console, number);
    auto source = item.source;
    const auto origin_x = ((x - item.x_origin) >> 2) * 4;
    instructions(work, 21);
    for (std::size_t row = 0; row < item.height; ++row) {
        for (unsigned plane = 0; plane < plane_count; ++plane) {
            instructions(work, plane == plane_count - 1 ? 2 : 1);
            if (work) { work(CpuOperation::port_write_word, 1); }
            instructions(work, plane == plane_count - 1 ? 1 : 2);
            for (std::size_t column = 0; column < (item.width >> 2); ++column) {
                if (work) { work(CpuOperation::string_bytes, 1); }
                instructions(work, 2);
                const auto color = assets_.console.at(source++);
                if (color != 0) {
                    instructions(work, 1);
                    pixel(origin_x + static_cast<std::int32_t>(column * 4 + plane),
                          std::int32_t(y) + item.y_origin - static_cast<std::int32_t>(row), color);
                }
                instructions(work, 2);
            }
            instructions(work, 1);
        }
        instructions(work, 3);
    }
    instructions(work, 1);
}

void CockpitRenderer::glyph(std::uint8_t number, std::int32_t x, std::uint8_t y, const TerrainFrame* warp,
                           const CpuWorkSink& work) {
    const auto item = sprite(assets_.hud, number);
    auto source = item.source;
    x -= std::uint8_t(item.x_origin);
    const auto origin_y = static_cast<std::uint8_t>(y + item.y_origin);
    instructions(work, 24);
    for (std::size_t column = 0; column < item.width; ++column) {
        instructions(work, 4);
        if (work) { work(CpuOperation::port_write_word, 1); }
        instructions(work, 2);
        const auto screen_x = x + static_cast<std::int32_t>(column);
        auto screen_y = std::int32_t(origin_y);
        if (warp != nullptr) {
            instructions(work, 7);
            const auto horizon = (screen_x >> 1) + 14;
            if (horizon < 0 || horizon >= scene_columns) {
                throw std::invalid_argument("HUD glyph exceeds camera horizon");
            }
            screen_y -= static_cast<std::int8_t>(warp->camera.horizon[horizon]);
        } else {
            instructions(work, 1);
        }
        for (std::size_t row = 0; row < item.height; ++row) {
            if (work) { work(CpuOperation::string_bytes, 1); }
            instructions(work, 2);
            const auto color = assets_.hud.at(source++);
            if (color != 0) {
                instructions(work, 1);
                pixel(screen_x, screen_y - static_cast<std::int32_t>(row), color);
            }
            instructions(work, 2);
        }
        instructions(work, 4);
    }
    instructions(work, 1);
}

void CockpitRenderer::bar(std::int32_t x, std::uint8_t y, unsigned height, std::uint8_t color, const CpuWorkSink& work) {
    if (height == 0) {
        throw std::invalid_argument("Original cockpit gauge rejects zero height");
    }
    instructions(work, 2);
    if (work) { work(CpuOperation::port_write_word, 1); }
    instructions(work, 12);
    for (unsigned row = 0; row < height; ++row) {
        instructions(work, 3);
        const auto offset = state_.back_page + panel_offset_ + std::size_t(y) * line_stride + (x >> 2) - row * line_stride;
        surface_.write(offset, 3, color);
    }
    instructions(work, 1);
}

void CockpitRenderer::digits(std::uint32_t value, std::int32_t x, std::uint8_t y, unsigned count, std::uint8_t first,
                            const CpuWorkSink& work) {
    const auto spacing = first == 10 ? 5 : 4;
    for (unsigned digit = 0; digit < count; ++digit) {
        instructions(work, 8);
        glyph(static_cast<std::uint8_t>(first + value % 10), x, y, nullptr, work);
        value /= 10;
        x -= spacing;
        instructions(work, 4);
    }
}

void CockpitRenderer::number(std::uint32_t value, std::int32_t x, std::uint8_t y, const CpuWorkSink& work) {
    instructions(work, 8);
    glyph(static_cast<std::uint8_t>(22 + value % 10), x + 8, y, nullptr, work);
    instructions(work, 9);
    glyph(static_cast<std::uint8_t>(22 + value / 10 % 10), x + 4, y, nullptr, work);
    const auto hundreds = static_cast<std::uint8_t>(value / 100);
    instructions(work, 3);
    if (hundreds <= 9) {
        instructions(work, 4);
        glyph(static_cast<std::uint8_t>(hundreds + 22), x, y, nullptr, work);
    }
    instructions(work, 1);
}

void CockpitRenderer::lamps(const CpuWorkSink& work) {
    instructions(work, 2);
    if (state_.mode != 1) {
        instructions(work, 1);
        return;
    }
    const auto left = static_cast<unsigned>(state_.controls.left);
    const auto right = static_cast<unsigned>(state_.controls.right);
    if (left >= tables_.panel_y.size() || right >= tables_.panel_y.size()) {
        throw std::invalid_argument("Invalid instrument display selection");
    }
    const auto old_left = static_cast<unsigned>(state_.previous_controls.left);
    const auto old_right = static_cast<unsigned>(state_.previous_controls.right);
    if (old_left >= tables_.panel_y.size() || old_right >= tables_.panel_y.size()) {
        throw std::invalid_argument("Invalid previous instrument display selection");
    }
    instructions(work, 12);
    console(static_cast<std::uint8_t>(13 + old_left * 2), tables_.left_panel_x[old_left] * 4, tables_.panel_y[old_left], work);
    instructions(work, 12);
    console(static_cast<std::uint8_t>(14 + left * 2), tables_.left_panel_x[left] * 4, tables_.panel_y[left], work);
    instructions(work, 12);
    console(static_cast<std::uint8_t>(25 + old_right * 2), tables_.right_panel_x[old_right] * 4, tables_.panel_y[old_right], work);
    instructions(work, 12);
    console(static_cast<std::uint8_t>(26 + right * 2), tables_.right_panel_x[right] * 4, tables_.panel_y[right], work);
    instructions(work, 1);
}

void CockpitRenderer::stores(const FlightState& player) {
    ammunition(player);
    countermeasures(player);
}

void CockpitRenderer::ammunition(const FlightState& player, const CpuWorkSink& work) {
    instructions(work, 2);
    if (state_.mode != 1) {
        instructions(work, 1);
        return;
    }
    instructions(work, 4);
    if ((player.system_condition[5] & 128) != 0) { instructions(work, 1); }
    if ((player.system_condition[3] & 128) != 0) { instructions(work, 1); }
    const auto selected_weapon = player.weapon;
    instructions(work, 6);
    console(7 + selected_weapon, 16, 234, work);
    instructions(work, 4);
    console(37, 12, 201, work);
    instructions(work, 6);
    bar(12 + selected_weapon * 8, 201, 1, assets_.theme[6], work);
    instructions(work, 7);
    digits(player.ammunition[selected_weapon], 39, 214, 4, 10, work);
    instructions(work, 1);
}

void CockpitRenderer::countermeasures(const FlightState& player, const CpuWorkSink& work) {
    instructions(work, 2);
    if (state_.mode != 1) {
        instructions(work, 1);
        return;
    }
    instructions(work, 4 + 1);
    digits(player.chaff, 307, 184, 2, 10, work);
    instructions(work, 1 + 4 + 1);
    digits(player.flares, 307, 193, 2, 10, work);
    instructions(work, 1 + 1);
}

void CockpitRenderer::initialize_displays(const FlightState& player, const CpuWorkSink& work) {
    // 71a0 calls 7b04, then repeats 7b2b and 7c10 explicitly.
    instructions(work, 1 + 1);
    ammunition(player, work);
    instructions(work, 1);
    countermeasures(player, work);
    instructions(work, 1);
    lamps(work);
    instructions(work, 1 + 1);
    lamps(work);
    instructions(work, 1);
    ammunition(player, work);
}

void CockpitRenderer::restore_marker(const CpuWorkSink& work) {
    auto& marker = markers_[state_.back_page / page_displacement];
    instructions(work, 2);
    if (state_.mode <= 5) {
        instructions(work, 4);
        std::size_t source = 0;
        if (marker.height != 0) { instructions(work, 6 + 21); }
        for (std::size_t row = 0; row < marker.height; ++row) {
            for (unsigned plane = 0; plane < plane_count; ++plane) {
                instructions(work, 1);
                if (work) { work(CpuOperation::port_write_word, 1); }
                instructions(work, 2);
                if (work) { work(CpuOperation::repeat_copy_bytes, static_cast<unsigned>(marker.width)); }
                instructions(work, 1);
                for (std::size_t column = 0; column < marker.width; ++column) {
                    surface_.write(marker.origin - row * line_stride + column, std::uint8_t(1u << plane),
                                   marker.pixels.at(source++));
                }
            }
            instructions(work, 3);
        }
        if (marker.height != 0) { instructions(work, 1); }
    }
    instructions(work, 1);
    marker = {};
}

void CockpitRenderer::prepare_frame(const CpuWorkSink& work) {
    instructions(work, 1); // CALL b8a0.
    restore_marker(work);
    frame_prepared_ = true;
}

void CockpitRenderer::begin_frame(const TerrainFrame& frame, std::uint8_t mode, const CockpitControls& controls,
                                 const CpuWorkSink& work) {
    if (!frame_prepared_) { restore_marker({}); }
    frame_prepared_ = false;
    if (state_.mode != mode) {
        load_picture(mode);
    }
    if (state_.controls.left != controls.left) {
        state_.previous_controls.left = state_.controls.left;
    }
    if (state_.controls.right != controls.right) {
        state_.previous_controls.right = state_.controls.right;
    }
    state_.controls = controls;
    lamps();
    const auto& mask = mode == 1 ? assets_.normal_picture : assets_.crash_picture;
    for (unsigned pair = 0; pair < 2; ++pair) {
        instructions(work, 2);
        if (work) { work(CpuOperation::port_write_word, 1); }
        instructions(work, pair == 0 ? 4 : 3);
        if (state_.back_page != 0) { instructions(work, 1); }
        for (std::size_t row = 0; row < scene_copy_rows; ++row) {
            const auto delta = std::int32_t(row) - 94;
            const auto slope = (frame.camera.pitch * std::abs(delta) + 128) >> 8;
            const auto horizontal = static_cast<std::uint8_t>(48 + (delta < 0 ? -slope : slope));
            const auto shift = std::uint8_t(static_cast<std::int8_t>(horizontal) >> 3);
            const auto half = ((horizontal >> 2) & 1) << 7;
            const auto source_shift = shift + (pair == 0 ? half : (half ^ 128) + int((half ^ 128) < 128));
            const auto visible_row = scene_copy_rows - 1 - row;
            const auto transparent = [&](std::size_t group, unsigned plane) {
                return mask.at(visible_row * cockpit_backing_width + group * 4 + pair * 2 + plane) == transparent_picture;
            };
            std::size_t group = 0;
            while (group < line_stride) {
                if (!transparent(group, 0) || !transparent(group, 1)) {
                    ++group;
                    continue;
                }
                const auto copy = [&](std::int32_t target, std::uint8_t plane_mask) {
                    const auto source = (row + 32) * scene_buffer_side + 17 + target + source_shift;
                    surface_.write(state_.back_page + visible_row * line_stride + target, plane_mask, frame.pixels.at(source));
                };
                const auto previous = visible_row * cockpit_backing_width + group * 4 + pair * 2;
                const auto start = group;
                const auto leading = previous >= 3 && mask.at(previous - 3) == transparent_picture;
                if (leading) {
                    // The original span builder tests the preceding byte across row boundaries.
                    copy(static_cast<std::int32_t>(group) - 1, static_cast<std::uint8_t>(2u << (pair * 2)));
                }
                do {
                    copy(group, static_cast<std::uint8_t>(3u << (pair * 2)));
                    ++group;
                } while (group < line_stride && transparent(group, 0) && transparent(group, 1));
                const auto trailing = group < line_stride && transparent(group, 0);
                if (trailing) {
                    copy(group, static_cast<std::uint8_t>(1u << (pair * 2)));
                }
                copy_span(work, state_.back_page + visible_row * line_stride + start, group - start, leading, trailing);
            }
        }
    }
    instructions(work, 1);
}

void CockpitRenderer::copy_front_page() {
    const auto height = (state_.mode == 1 ? assets_.normal_picture : assets_.crash_picture).size() / cockpit_backing_width;
    for (unsigned plane = 0; plane < plane_count; ++plane) {
        for (std::size_t offset = 0; offset < height * line_stride; ++offset) {
            surface_.write(state_.back_page + offset, std::uint8_t(1u << plane),
                           surface_.read(plane, (state_.back_page ^ page_displacement) + offset));
        }
    }
}

CockpitImage CockpitRenderer::present(const FlightState& player) {
    if (player.shake_offset >= tables_.shake_displacement.size()) {
        throw std::invalid_argument("Invalid cockpit shake displacement");
    }
    CockpitImage image;
    image.palette = assets_.palette;
    const auto origin = state_.back_page + 1 + tables_.shake_displacement[player.shake_offset] + player.crash_rotation;
    for (std::size_t y = 0; y < cockpit_height; ++y) {
        for (std::size_t x = 0; x < cockpit_width; ++x) {
            const auto scan_x = x + player.shake_offset;
            const auto offset = (origin + y * line_stride + scan_x / plane_count) & (plane_bytes - 1);
            image.pixels[y * cockpit_width + x] = surface_.read(scan_x % plane_count, offset);
        }
    }
    state_.back_page ^= page_displacement;
    return image;
}
void CockpitRenderer::heading(std::uint32_t angle, const CpuWorkSink& work) {
    const auto item = sprite(assets_.hud, 1);
    auto source = item.source + ((angle >> 25) * item.height & 0xffff);
    const auto origin = state_.back_page + 33 - std::uint8_t(item.x_origin) +
                        static_cast<std::uint8_t>(5 + item.y_origin) * line_stride;
    instructions(work, 30);
    for (unsigned group = 0; group < 16; ++group) {
        for (unsigned plane = 0; plane < plane_count; ++plane) {
            instructions(work, 1);
            if (work) { work(CpuOperation::port_write_word, 1); }
            instructions(work, 2);
            for (std::size_t row = 0; row < item.height; ++row) {
                if (work) { work(CpuOperation::string_bytes, 1); }
                instructions(work, 2);
                const auto color = assets_.hud.at(source++);
                if (color != 0) {
                    instructions(work, 1);
                    surface_.write(origin + group - row * line_stride, std::uint8_t(1u << plane), color);
                }
                instructions(work, 2);
            }
        }
        instructions(work, 3);
    }
    instructions(work, 1);
}

void CockpitRenderer::vector_line(std::int32_t x, std::uint8_t y, const CpuWorkSink& work) {
    const auto dx = std::abs(x - 165);
    const auto dy = std::abs(std::int32_t(y) - 90);
    const auto horizontal = x < 165 ? -1 : 1;
    const auto vertical = y < 90 ? -1 : 1;
    auto px = 165;
    auto py = 90;
    unsigned fraction = 128;
    instructions(work, 14 + unsigned(y < 90) * 2 + unsigned(x < 165) * 2);
    if (dx < dy) {
        instructions(work, 15);
        if (work) { work(CpuOperation::port_write_word, 1); }
        instructions(work, 2);
        const auto step = static_cast<std::uint8_t>(((dx << 16) / (dy + 1) + 256) >> 8);
        for (int count = 0; count <= dy; ++count) {
            instructions(work, 5);
            pixel(px, py, assets_.theme[6]);
            py += vertical;
            if (count == dy) { instructions(work, 1); break; }
            instructions(work, 2);
            fraction += step;
            if (fraction >= 256) {
                instructions(work, 5);
                if (work) { work(CpuOperation::port_write_word, 1); }
                instructions(work, 3);
                fraction -= 256;
                px += horizontal;
            }
        }
        return;
    }
    instructions(work, 10);
    const auto step = static_cast<std::uint8_t>(((dy << 16) / (dx + 1) + 256) >> 8);
    for (int count = 0; count <= dx; ++count) {
        instructions(work, 4);
        if (work) { work(CpuOperation::port_write_word, 1); }
        instructions(work, 7);
        pixel(px, py, assets_.theme[6]);
        px += horizontal;
        if (count == dx) { instructions(work, 1); break; }
        instructions(work, 2);
        fraction += step;
        if (fraction >= 256) {
            instructions(work, 2);
            fraction -= 256;
            py += vertical;
        }
    }
}

void CockpitRenderer::camera(const TerrainFrame& frame, const CpuWorkSink& work) {
    constexpr std::array<std::size_t, 4> origins{0xa22c, 0xa2ac, 0xa22d, 0xa2ad};
    for (unsigned plane = 0; plane < plane_count; ++plane) {
        instructions(work, 2);
        if (work) { work(CpuOperation::port_write_word, 1); }
        instructions(work, 3 + 5);
        for (unsigned row = 0; row < 44; ++row) {
            instructions(work, 1);
            for (unsigned column = 0; column < 20; ++column) {
                instructions(work, 2);
                const auto source = origins[plane] - row * 512 + column * 2;
                surface_.write(state_.back_page + panel_offset_ + 0x37ce + row * line_stride + column,
                               std::uint8_t(1u << plane), frame.pixels.at(source));
            }
            instructions(work, 4);
        }
        instructions(work, 1);
    }
    instructions(work, 1);
}

void CockpitRenderer::map(const TerrainFrame& frame, const FlightState& player, const Terrain& terrain,
                         const CpuWorkSink& work) {
    if (terrain.color.size() != terrain_side * terrain_side || tables_.map_scale < 32) {
        throw std::invalid_argument("Invalid cockpit terrain map or scale");
    }
    auto dx = multiply_high(static_cast<std::int32_t>(tables_.map_scale), frame.camera.sine);
    auto dy = multiply_high(static_cast<std::int32_t>(tables_.map_scale), frame.camera.cosine);
    auto origin_x = player.view.x + std::uint32_t(dx) * 8;
    auto origin_y = player.view.y - std::uint32_t(dy) * 8;
    instructions(work, 3 + 16);
    for (unsigned plane = 0; plane < plane_count; ++plane) {
        instructions(work, plane == 3 ? 3 : 6);
        if (work) { work(CpuOperation::port_write_word, 1); }
        instructions(work, 2 + 1);
        auto x = origin_x;
        auto y = origin_y;
        for (unsigned row = 0; row < 44; ++row) {
            // 163ef..165a2 contains twenty unrolled samples and its row loop.
            instructions(work, 148 + 12);
            for (int column = 0; column < 20; ++column) {
                const auto sample_x = x + std::uint32_t(dy) * std::uint32_t(column - 10);
                const auto sample_y = y + std::uint32_t(dx) * std::uint32_t(column - 10);
                const auto color = terrain.color[(sample_y >> 22) * terrain_side + (sample_x >> 22)];
                surface_.write(state_.back_page + panel_offset_ + 0x37ce + row * line_stride + column,
                               std::uint8_t(1u << plane), color);
            }
            dx = static_cast<std::int32_t>(std::uint32_t(dx >> 2) << 2);
            dy = static_cast<std::int32_t>(std::uint32_t(dy >> 2) << 2);
            x -= std::uint32_t(dx >> 2);
            y += std::uint32_t(dy >> 2);
        }
        origin_y += std::uint32_t(dx >> 2);
        origin_x += std::uint32_t(dy >> 2);
        instructions(work, 1);
        if (plane != 3) { instructions(work, 9); }
    }
    instructions(work, 1 + 4);
    overlay(3, 112, 206, work);
    instructions(work, 1);
}

void CockpitRenderer::panel(InstrumentDisplay display, const TerrainFrame& frame, const SceneView& rendered_view,
    const FlightState& player, const MissionScene& scene, const Terrain& terrain, std::uint32_t tick,
    const InstrumentCamera*& instrument_camera, const CpuWorkSink& work, const InstrumentCameraProvider& camera_provider) {
    if ((player.system_condition[6 + unsigned(panel_offset_ != 0)] & 128) != 0) {
        console(50, 72, 174, work);
        return;
    }
    if (display == InstrumentDisplay::disabled) {
        console(51, 72, 174, work);
        return;
    }
    if (display == InstrumentDisplay::systems) {
        console(42, 72, 174, work);
        if ((tick & 3) != 0) {
            for (unsigned system = 0; system < 6; ++system) {
                if ((player.system_condition[system] & 128) != 0) {
                    overlay(static_cast<std::uint8_t>(43 + system), 72, 174, work);
                }
            }
        }
        return;
    }
    if (display == InstrumentDisplay::objectives) {
        console(56, 72, 174, work);
        digits(tables_.category_count, 142, 198, 2, 10, work);
        digits(scene.remaining_targets, 142, 208, 2, 10, work);
        return;
    }
    if (display == InstrumentDisplay::camera) {
        const auto missile = scene.camera_subject != 0 && tables_.missile_camera;
        const auto rendered = missile || player.target.slot != 0;
        instructions(work, 3 + (scene.camera_subject != 0 ? 2 : 0) + (missile ? 0 : 3));
        if (rendered) { instructions(work, missile ? 20 : 26); }
        const InstrumentCamera* selected_camera = nullptr;
        if (camera_provider) {
            instrument_camera = camera_provider(panel_offset_ == 0 ? InstrumentPanel::left : InstrumentPanel::right);
            selected_camera = instrument_camera;
        } else {
            selected_camera = panel_offset_ != 0 && instrument_camera != nullptr && instrument_camera->second ?
                instrument_camera->second.get() : instrument_camera;
        }
        if (rendered) { instructions(work, camera_provider ? 9 : 12); }
        camera(selected_camera == nullptr ? frame : selected_camera->frame, work);
        if ((scene.camera_subject == 0 || !tables_.missile_camera) && player.target.slot != 0) {
            instructions(work, 4);
            glyph(32, 117, 215, nullptr, work);
            instructions(work, 3);
            number(static_cast<std::uint32_t>(player.target.projection.depth >> 20), 117, 215, work);
        }
        return;
    }
    if (display == InstrumentDisplay::map) {
        map(frame, player, terrain, work);
    } else if (display == InstrumentDisplay::radar) {
        instructions(work, 4);
        console(49, 72, 174, work);
    } else {
        throw std::invalid_argument("Unsupported cockpit instrument display");
    }
    instructions(work, display == InstrumentDisplay::map ? 8 : 2);
    const auto last_slot = display == InstrumentDisplay::map ? 60u : unsigned(scene_slots - 1);
    const auto scale = tables_.map_scale >> 5;
    const auto ratio = static_cast<std::int32_t>(0xffffffffu / scale);
    // A left camera refresh replaces the shared projection arrays before the right display runs.
    const auto refreshed = panel_offset_ != 0 && state_.controls.left == InstrumentDisplay::camera &&
                           (player.system_condition[6] & 128) == 0 && instrument_camera != nullptr;
    const auto& projection = refreshed ? instrument_camera->frame.camera : frame.camera;
    const auto& entity_view = refreshed ? instrument_camera->view : rendered_view;
    for (unsigned slot = last_slot; slot > (display == InstrumentDisplay::map ? 0u : 60u); --slot) {
        const auto& entity = scene.entities[slot];
        const auto loop_tail = display == InstrumentDisplay::map ? 2u : 3u;
        instructions(work, 2);
        if (!entity.active) {
            instructions(work, loop_tail);
            continue;
        }
        instructions(work, 3);
        if (entity.category_color == 0) {
            instructions(work, loop_tail);
            continue;
        }
        const auto x = static_cast<std::int32_t>(entity.x - entity_view.x) >> 1;
        const auto y = static_cast<std::int32_t>(entity.y - entity_view.y) >> 1;
        const auto lateral = static_cast<std::int32_t>(std::uint32_t(multiply_high(projection.cosine, x)) +
            std::uint32_t(multiply_high(projection.sine, y)));
        const auto depth = static_cast<std::int32_t>(std::uint32_t(multiply_high(projection.sine, x)) -
            std::uint32_t(multiply_high(projection.cosine, y)));
        const auto px = display == InstrumentDisplay::map ? multiply_high(lateral, ratio) : lateral >> 23;
        const auto py = display == InstrumentDisplay::map ? multiply_high(-depth, ratio) : -depth >> 23;
        const auto center_y = display == InstrumentDisplay::map ? 206 : 196;
        instructions(work, display == InstrumentDisplay::map ? 6 : 5);
        if (px <= (display == InstrumentDisplay::map ? -38 : -40)) { instructions(work, loop_tail); continue; }
        instructions(work, 2);
        if (px >= (display == InstrumentDisplay::map ? 38 : 40)) { instructions(work, loop_tail); continue; }
        instructions(work, display == InstrumentDisplay::map ? 7 : 5);
        const auto map_y = static_cast<std::uint32_t>(py + center_y);
        if (display == InstrumentDisplay::map ? map_y <= 174 : py <= -22) { instructions(work, loop_tail); continue; }
        instructions(work, 2);
        if (display == InstrumentDisplay::map ? map_y >= 217 : py >= 22) { instructions(work, loop_tail); continue; }
        instructions(work, display == InstrumentDisplay::map ? 6 : 7);
        glyph(entity.category_color, px + 111, static_cast<std::uint8_t>(py + center_y), nullptr, work);
        instructions(work, 2);
        if (display == InstrumentDisplay::map) {
            instructions(work, 2 + ((tick & 3) == 0 ? 2 : 0));
        }
        if (display == InstrumentDisplay::map && (tick & 3) == 0 && entity.target != 0) {
            instructions(work, 4);
            glyph(20, px + 111, static_cast<std::uint8_t>(py + center_y), nullptr, work);
            instructions(work, 2);
        }
        instructions(work, loop_tail);
    }
    instructions(work, 1);
}

std::vector<SoundCue> CockpitRenderer::draw_instruments(const TerrainFrame& frame, const SceneView& rendered_view,
    const FlightState& player, const MissionScene& scene, const SceneAssets& assets, std::uint32_t tick,
    const CockpitControls& controls, const InstrumentCamera* instrument_camera, const CpuWorkSink& work,
    const InstrumentCameraProvider& camera_provider, const SoundCueHandler& sound_handler) {
    std::vector<SoundCue> cues;
    state_.controls = controls;
    if (state_.mode > 5) {
        instructions(work, 11);
        return cues;
    }
    instructions(work, state_.mode == 5 ? 4 : 6);
    if (tables_.center_marker) {
        instructions(work, 4);
        glyph(5, 165, 90, &frame, work);
    }
    instructions(work, state_.mode == 5 ? 2 : 4);
    if (state_.mode == 1) {
        stores(player);
        instructions(work, 5);
        bar(272, 188, 22, 0, work);
        instructions(work, 6);
        if (static_cast<std::int32_t>(player.range) >= 0 || (tick & 1) != 0) {
            const auto attitude = static_cast<std::int32_t>(player.range + std::uint32_t(player.view.roll) * 4);
            const auto height = static_cast<std::uint8_t>((std::uint32_t(std::max(attitude, 0)) >> 8) + 1);
            instructions(work, 11 + unsigned(attitude < 0) + unsigned(height > 22));
            bar(272, 188, std::min<unsigned>(height, 22), assets_.theme[8], work);
        }
        instructions(work, 5);
        bar(48, 188, 22, 0, work);
        instructions(work, 8 + unsigned((player.control_power >> 3) + 1 > 22));
        bar(48, 188, std::min<unsigned>((player.control_power >> 3) + 1, 22), assets_.theme[8], work);
        instructions(work, 5);
        bar(56, 188, 22, 0, work);
        instructions(work, 8 + unsigned((player.engine_power >> 3) + 1 > 22));
        bar(56, 188, std::min<unsigned>((player.engine_power >> 3) + 1, 22), assets_.theme[5], work);
        const auto flash_active = static_cast<std::uint8_t>(state_.flash_frame + tables_.weapon_flash.at(player.weapon)) != 0;
        instructions(work, 4);
        if (flash_active) {
            instructions(work, 3);
            state_.flash_frame = static_cast<std::uint8_t>(state_.flash_frame + 1);
            if (state_.flash_frame > 3) {
                instructions(work, 1);
                state_.flash_frame = 0;
            }
        } else {
            instructions(work, 1);
            state_.flash_frame = 0;
        }
        instructions(work, 6);
        console(38 + state_.flash_frame, 36, 168, work);
        instructions(work, 2);
        console(38 + state_.flash_frame, 280, 168, work);
        instructions(work, 7);
        console(52 + unsigned(state_.nearby != 0), 140, 153, work);
        instructions(work, 7);
        console(54 + unsigned(player.enemy_tracking), 187, 153, work);
        instructions(work, 5); // BBA0 CALL and 7fe0 console arguments.
        console(57, 272, 203, work);
        instructions(work, 5);
        const auto was_nearby = state_.nearby;
        state_.nearby = 0;
        for (std::size_t slot = scene_slots - 1; slot > 60; --slot) {
            const auto& entity = scene.entities[slot];
            instructions(work, 2);
            if (!entity.active) {
                instructions(work, 3);
                continue;
            }
            instructions(work, 3);
            if (entity.category_color == 0) {
                instructions(work, 3);
                continue;
            }
            const auto x = static_cast<std::int32_t>(entity.x - rendered_view.x) >> 1;
            const auto y = static_cast<std::int32_t>(entity.y - rendered_view.y) >> 1;
            const auto lateral = static_cast<std::int32_t>(std::uint32_t(multiply_high(frame.camera.cosine, x)) +
                std::uint32_t(multiply_high(frame.camera.sine, y)));
            const auto depth = static_cast<std::int32_t>(std::uint32_t(multiply_high(frame.camera.sine, x)) -
                std::uint32_t(multiply_high(frame.camera.cosine, y)));
            const auto px = lateral >> 24;
            const auto py = -depth >> 24;
            instructions(work, 5);
            if (px <= -20) { instructions(work, 3); continue; }
            instructions(work, 2);
            if (px >= 20) { instructions(work, 3); continue; }
            instructions(work, 5);
            if (py <= -15) { instructions(work, 3); continue; }
            instructions(work, 2);
            if (px > -20 && px < 20 && py > -15 && py < 15) {
                instructions(work, 7);
                glyph(entity.category_color, px + 294, static_cast<std::uint8_t>(py + 218), nullptr, work);
                instructions(work, 3);
                state_.nearby = 1;
            }
            instructions(work, 3);
        }
        instructions(work, 2 + (state_.nearby != 0 ? 2 : 0));
        if (state_.nearby != 0 && was_nearby == 0) {
            instructions(work, 2);
            cues.push_back({SampleBank::announcements, 2});
            if (sound_handler) { sound_handler(cues.back()); }
        }
        instructions(work, 1 + 4); // Threat RET and released zoom-key gates.
        const auto zoom = tables_.map_scale + static_cast<std::uint32_t>(state_.zoom_step);
        instructions(work, 4);
        if (zoom < 0x10000000) {
            instructions(work, 2);
            if (zoom > 0x00800000) {
                instructions(work, 1);
                tables_.map_scale = zoom;
            }
        }
        panel_offset_ = 0;
        instructions(work, 8 + unsigned((player.system_condition[6] & 128) != 0));
        panel(state_.controls.left, frame, rendered_view, player, scene, assets.terrain, tick, instrument_camera, work, camera_provider);
        panel_offset_ = 26;
        instructions(work, 9 + unsigned((player.system_condition[7] & 128) != 0));
        panel(state_.controls.right, frame, rendered_view, player, scene, assets.terrain, tick, instrument_camera, work, camera_provider);
        panel_offset_ = 0;
        instructions(work, 1);
    }
    instructions(work, 2);
    if (tables_.heading_tape) {
        instructions(work, 4);
        glyph(2, 164, 5, nullptr, work);
        instructions(work, 4);
        heading(player.view.heading, work);
    }
    instructions(work, 2);
    if (!tables_.flight_hud) {
        instructions(work, 2);
        state_.previous_height = player.view.height;
        target_marker(player, work);
        return cues;
    }
    const auto difference = int(player.view.height) - state_.previous_height;
    const auto average = static_cast<std::uint8_t>(static_cast<std::uint8_t>((difference >> 1) * 4) + state_.vertical_rate);
    state_.vertical_rate = static_cast<std::uint8_t>(static_cast<std::int8_t>(average) >> 1);
    const auto climb = static_cast<std::int8_t>(state_.vertical_rate);
    instructions(work, 9 + unsigned(climb != 0));
    if (climb > 0) {
        instructions(work, 8 + unsigned(climb + 1 >= 32));
        bar(296, 87, std::min<int>(climb + 1, 32), assets_.theme[6], work);
    } else if (climb < 0) {
        const auto size = std::min<unsigned>(static_cast<std::uint8_t>(~climb), 32);
        instructions(work, 9 + unsigned(static_cast<std::uint8_t>(~climb) >= 32));
        bar(296, static_cast<std::uint8_t>(88 + size), size + 1, assets_.theme[6], work);
    }
    instructions(work, 4);
    glyph(21, 66, 136, nullptr, work);
    const auto speed = std::max(magnitude(player.forward_speed), magnitude(player.side_speed)) >> 17;
    instructions(work, 11 + unsigned(magnitude(player.side_speed) <= magnitude(player.forward_speed)));
    number(speed, 66, 136, work);
    instructions(work, 2 + 4); // Disabled debug coordinates and altitude glyph arguments.
    glyph(4, 288, 120, nullptr, work);
    const auto altitude = (player.height_gap >> 2) + 1;
    instructions(work, 6);
    bar(288, 120, altitude, assets_.theme[6], work);
    instructions(work, 9 + unsigned(unsigned(player.height_gap) * 2 < 10));
    number(unsigned(player.height_gap) * 2 >= 10 ? unsigned(player.height_gap) * 2 - 10 : 0,
           272, static_cast<std::uint8_t>(123 - altitude), work);
    const auto y = static_cast<std::uint8_t>((-player.forward_speed >> 19) + 90);
    const auto x = (player.side_speed >> 19) + 165;
    instructions(work, 11);
    glyph(33, x, y, nullptr, work);
    instructions(work, 1);
    vector_line(x, y, work);
    instructions(work, 6);
    overlay(player.firing ? 2 : 1, 164, 90, work);
    instructions(work, 2);
    state_.previous_height = player.view.height;
    target_marker(player, work);
    return cues;
}

void CockpitRenderer::target_marker(const FlightState& player, const CpuWorkSink& work) {
    instructions(work, 6);
    if (player.target.slot == 0 || player.target.projection.x == 0) {
        instructions(work, 1);
        return;
    }
    instructions(work, 6 + 21);
    const auto page = state_.back_page / page_displacement;
    const auto item = sprite(assets_.console, static_cast<std::uint8_t>(5 + page));
    auto& marker = markers_[page];
    const auto x = int(player.target.projection.x) * 2;
    const auto y = player.target.projection.y;
    marker.origin = state_.back_page + ((x - item.x_origin) >> 2) + (int(y) + item.y_origin) * line_stride;
    marker.width = item.width >> 2;
    marker.height = item.height;
    marker.pixels.clear();
    for (std::size_t row = 0; row < marker.height; ++row) {
        for (unsigned plane = 0; plane < plane_count; ++plane) {
            instructions(work, plane == plane_count - 1 ? 2 : 1);
            if (work) { work(CpuOperation::port_write_word, 1); }
            instructions(work, plane == plane_count - 1 ? 1 : 2);
            if (work) { work(CpuOperation::repeat_copy_bytes, static_cast<unsigned>(marker.width)); }
            instructions(work, 1);
            for (std::size_t column = 0; column < marker.width; ++column) {
                marker.pixels.push_back(surface_.read(plane, marker.origin - row * line_stride + column));
            }
        }
        instructions(work, 3);
    }
    instructions(work, 1 + 3);
    overlay(4, x, y, work);
    instructions(work, 1);
}
void CockpitRenderer::apply_settings(const MenuSettings& settings) {
    tables_.center_marker = settings.display(DisplayOption::artificial_horizon);
    tables_.flight_hud = settings.display(DisplayOption::indicators);
    tables_.heading_tape = settings.display(DisplayOption::compass);
    tables_.missile_camera = settings.display(DisplayOption::missile_camera);
}
} // namespace comanchecd
