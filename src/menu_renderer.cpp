#include "comanchecd/menu_renderer.hpp"
#include "color_metric.hpp"
#include "menu_work.hpp"
#include "flight_work.hpp"
#include "comanchecd/execution_clock.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace comanchecd {
namespace {
constexpr std::size_t font_widths = 8;
constexpr std::size_t font_offsets = font_widths + 256;
constexpr std::size_t glyph_height = 10;
constexpr std::uint8_t all_planes = 15;
using color_metric::weights;
using color_metric::convert;
constexpr std::array<std::uint8_t, 10> control_colors{5, 7, 0, 4, 8, 5, 7, 4, 8, 5};

std::size_t color_slot(std::uint8_t code) {
    switch (code) {
    case 'r': return 5;
    case 'g': return 6;
    case 'b': return 4;
    case 'p': return 7;
    case 'y': return 9;
    case 'o': return 8;
    case 'd': return 2;
    case 'l': return 3;
    default: return 0;
    }
}

void advance(TextCursor& cursor) {
    cursor.plane = static_cast<std::uint8_t>(cursor.plane << 1);
    if (cursor.plane == 16) {
        cursor.plane = 1;
        ++cursor.offset;
    }
}


}

Theme build_theme(const Palette& palette, const Transfer& transfer) {
    constexpr std::array<std::array<std::uint8_t, 3>, 10> targets{{
        {63, 63, 63}, {16, 16, 16}, {28, 28, 28}, {40, 40, 40}, {16, 48, 63},
        {63, 16, 16}, {16, 63, 16}, {63, 16, 63}, {63, 32, 16}, {63, 56, 16}
    }};
    Theme theme{};
    for (std::size_t slot = 0; slot < theme.size(); ++slot) {
        theme[slot] = color_metric::nearest(palette, transfer, targets[slot], 0, 255);
    }
    return theme;
}

ColorMap build_box_remap(const Palette& palette, const Transfer& transfer) {
    std::array<std::array<int, 3>, 256> converted{};
    for (std::size_t index = 0; index < palette.size(); ++index) {
        converted[index] = convert(palette[index], transfer);
    }
    ColorMap output{};
    for (std::size_t index = 0; index < palette.size(); ++index) {
        auto darkened = palette[index];
        for (auto& channel : darkened) {
            channel = static_cast<std::uint8_t>((channel + 1) >> 1);
        }
        const auto target = convert(darkened, transfer);
        auto best = std::numeric_limits<unsigned>::max();
        for (std::size_t candidate = 0; candidate < palette.size(); ++candidate) {
            unsigned distance = 0;
            for (std::size_t channel = 0; channel < target.size(); ++channel) {
                const int difference = converted[candidate][channel] - target[channel];
                distance += weights[channel] * static_cast<unsigned>(difference * difference);
            }
            if (distance < best) {
                best = distance;
                output[index] = static_cast<std::uint8_t>(candidate);
            }
            if (distance == 0) {
                break;
            }
        }
    }
    return output;
}

PlanarSurface::PlanarSurface(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {
    if (bytes_.size() != plane_count * plane_bytes) {
        throw std::invalid_argument("Expected four 64 KiB planes");
    }
}

const std::vector<std::uint8_t>& PlanarSurface::bytes() const { return bytes_; }

std::uint8_t PlanarSurface::read(std::size_t plane, std::size_t offset) const {
    if (plane >= plane_count || offset >= plane_bytes) {
        throw std::out_of_range("Planar read");
    }
    return bytes_[plane * plane_bytes + offset];
}

void PlanarSurface::write(std::size_t offset, std::uint8_t mask, std::uint8_t color) {
    if (offset >= plane_bytes) {
        throw std::out_of_range("Planar write");
    }
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        if ((mask & (1u << plane)) != 0) {
            bytes_[plane * plane_bytes + offset] = color;
        }
    }
}

void PlanarSurface::restore(std::size_t page, const Background& background) {
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        if (background[plane].size() != background_bytes || page + background_bytes > plane_bytes) {
            throw std::invalid_argument("Unsupported background extent");
        }
        std::copy(background[plane].begin(), background[plane].end(), bytes_.begin() + plane * plane_bytes + page);
    }
}

Font::Font(std::vector<std::uint8_t> embedded) : embedded_(std::move(embedded)) {
    if (embedded_.size() < font_offsets + 512) {
        throw std::invalid_argument("Truncated font tables");
    }
    for (unsigned code = 0; code < 256; ++code) {
        if (glyph_offset(static_cast<std::uint8_t>(code)) + width(static_cast<std::uint8_t>(code)) * 4 > embedded_.size()) {
            throw std::invalid_argument("Truncated glyph columns");
        }
    }
}

std::size_t Font::width(std::uint8_t code) const { return embedded_[font_widths + code]; }

std::size_t Font::glyph_offset(std::uint8_t code) const {
    const auto offset = font_offsets + code * 2;
    return embedded_[offset] | (static_cast<std::size_t>(embedded_[offset + 1]) << 8);
}

std::uint32_t Font::column(std::uint8_t code, std::size_t index) const {
    if (index >= width(code)) {
        throw std::out_of_range("Glyph column");
    }
    const auto offset = glyph_offset(code) + index * 4;
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(embedded_[offset + byte]) << (byte * 8);
    }
    return value;
}

MenuRenderer::MenuRenderer(PlanarSurface surface, Font font, Theme theme, ColorMap remap, Background background,
                           std::size_t back_page, std::size_t pan, ExecutionClock* clock, MenuInputReset reset_input)
    : surface_(std::move(surface)), font_(std::move(font)), theme_(theme), remap_(remap),
      background_(std::move(background)), back_page_(back_page), pan_(pan), clock_(clock),
      work_(clock ? CpuWorkSink([clock](CpuOperation op, unsigned count) { clock->execute(op, count); }) : CpuWorkSink{}),
      reset_input_(std::move(reset_input)) {}

const PlanarSurface& MenuRenderer::surface() const { return surface_; }
std::size_t MenuRenderer::back_page() const { return back_page_; }
void MenuRenderer::present() {
    if (clock_) {
        clock_->execute(CpuOperation::instructions, 1);
        flight_work::present(*clock_);
    }
    back_page_ ^= page_displacement;
}

void MenuRenderer::restore_background() {
    if (work_) { work_(CpuOperation::instructions, 1); }
    menu_work::background(work_);
    surface_.restore(back_page_, background_);
}

void MenuRenderer::save_background() {
    if (work_) { work_(CpuOperation::instructions, 1); }
    menu_work::save_background(work_);
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        background_[plane].resize(background_bytes);
        for (std::size_t offset = 0; offset < background_bytes; ++offset) {
            background_[plane][offset] = surface_.read(plane, back_page_ + offset);
        }
    }
}

void MenuRenderer::copy_front_page() {
    if (clock_) {
        clock_->execute(CpuOperation::instructions, 1);
    }
    menu_work::copy_page(briefing_page_height, work_);
    const auto front_page = back_page_ ^ page_displacement;
    for (std::size_t offset = 0; offset < line_stride * briefing_page_height; ++offset) {
        for (std::size_t plane = 0; plane < plane_count; ++plane) {
            surface_.write(back_page_ + offset, static_cast<std::uint8_t>(1u << plane),
                surface_.read(plane, front_page + offset));
        }
    }
}

void MenuRenderer::begin_list() {
    if (work_) { work_(CpuOperation::instructions, 3); }
    copy_front_page();
    save_background();
}

void MenuRenderer::prepare_briefing() {
    save_background();
    present();
    copy_front_page();
}

void MenuRenderer::draw_name_entry(const std::vector<std::uint8_t>& stream) {
    restore_background();
    const auto origin = back_page_ + 1 + pan_;
    box(origin + 10 + 206 * line_stride, 60, 15);
    const auto result = text(stream, {origin + 0x434f, 1, theme_[6]});
    if (result.marker != 255) {
        throw std::invalid_argument("Name-entry prompt must end with the final row marker");
    }
}

void MenuRenderer::box(std::size_t origin, std::uint8_t width, std::uint8_t height) {
    if (width < 2 || height < 3) {
        throw std::invalid_argument("Unsupported box dimensions");
    }
    if (work_) { work_(CpuOperation::instructions, 1); }
    menu_work::box(width, height, work_);
    for (std::size_t x = 0; x + 1 < width; ++x) {
        surface_.write(origin + x, all_planes, theme_[2]);
    }
    surface_.write(origin + width - 1, 7, theme_[2]);
    for (std::size_t y = 1; y + 1 < height; ++y) {
        for (std::size_t plane = 0; plane < plane_count; ++plane) {
            for (std::size_t x = 0; x < width; ++x) {
                const auto address = origin + y * line_stride + x;
                surface_.write(address, static_cast<std::uint8_t>(1u << plane), remap_[surface_.read(plane, address)]);
            }
        }
    }
    for (std::size_t x = 0; x < width; ++x) {
        surface_.write(origin + (height - 1) * line_stride + x, all_planes, theme_[1]);
    }
    surface_.write(origin + (height + 1) * line_stride, 14, 0);
    surface_.write(origin + height * line_stride, 14, 0);
    for (std::size_t y = height; y <= static_cast<std::size_t>(height) + 1; ++y) {
        for (std::size_t x = 1; x < width; ++x) {
            surface_.write(origin + y * line_stride + x, all_planes, 0);
        }
    }
    for (std::size_t y = 0; y + 1 < height; ++y) {
        surface_.write(origin + y * line_stride, 1, theme_[2]);
    }
    for (std::size_t y = 1; y < height; ++y) {
        const auto address = origin + y * line_stride + width - 1;
        surface_.write(address, 4, theme_[1]);
    }
    for (std::size_t y = 1; y < height; ++y) {
        const auto address = origin + y * line_stride + width - 1;
        surface_.write(address, 8, 0);
    }
}

void MenuRenderer::highlight(std::size_t origin, std::size_t interior_bytes, std::size_t height) {
    if (height == 0) {
        throw std::invalid_argument("Highlight height must be positive");
    }
    if (work_) { work_(CpuOperation::instructions, 1); }
    menu_work::highlight(static_cast<unsigned>(interior_bytes), static_cast<unsigned>(height), work_);
    for (std::size_t y = 0; y < height; ++y) {
        const auto start = origin + y * line_stride;
        surface_.write(start, 14, theme_[3]);
        for (std::size_t x = 1; x <= interior_bytes; ++x) {
            surface_.write(start + x, all_planes, theme_[3]);
        }
        surface_.write(start + interior_bytes + 1, 3, theme_[3]);
    }
}

TextCursor MenuRenderer::text(const std::vector<std::uint8_t>& stream, TextCursor cursor) {
    if (cursor.plane != 1 && cursor.plane != 2 && cursor.plane != 4 && cursor.plane != 8) {
        throw std::invalid_argument("Invalid text plane");
    }
    const auto instructions = [&](unsigned n) { if (work_) { work_(CpuOperation::instructions, n); } };
    instructions(1); // Caller CALL; internal character branches return to 2a21e.
    while (cursor.consumed < stream.size()) {
        const auto code = stream[cursor.consumed++];
        instructions(4);
        if (code == 0xfc) { continue; }
        instructions(3);
        if (code == 0xfe) {
            throw std::invalid_argument("Editor field registration requires editor state");
        }
        instructions(2);
        if (code == 0) {
            instructions(2);
            cursor.marker = code;
            return cursor;
        }
        instructions(2);
        if (code == 255) {
            instructions(2);
            cursor.marker = code;
            return cursor;
        }
        instructions(2);
        if (code == '|') {
            if (cursor.consumed == stream.size()) {
                throw std::invalid_argument("Truncated text markup");
            }
            const auto command = stream[cursor.consumed++];
            if (command == 0xf9) {
                throw std::invalid_argument("Auxiliary field span requires editor state");
            }
            instructions(23 + unsigned(color_slot(command) != 0));
            cursor.foreground = theme_[color_slot(command)];
            continue;
        }
        instructions(4);
        if (code >= 1 && code <= control_colors.size()) {
            instructions(3 * code);
            cursor.foreground = theme_[control_colors[code - 1]];
        }
        instructions(2);
        if (code == 0xfd) {
            auto plane = cursor.plane;
            instructions(1);
            for (unsigned pixel = 0; pixel < 2; ++pixel) {
                instructions(3);
                plane <<= 1;
                if (plane == 16) { plane = 1; instructions(2); }
            }
            instructions(2);
            for (unsigned pixel = 0; pixel < 6; ++pixel) {
                advance(cursor);
            }
            continue;
        }
        const auto width = font_.width(code);
        instructions(4);
        if (width != 0) { instructions(3); }
        for (std::size_t column = 0; column < width; ++column) {
            instructions(2);
            if (work_) { work_(CpuOperation::port_write_word, 1); }
            instructions(2);
            auto bits = font_.column(code, column);
            for (std::size_t row = 0; row < glyph_height; ++row) {
                const auto address = cursor.offset + row * line_stride;
                instructions(2);
                if ((bits & 1) != 0) {
                    surface_.write(address, cursor.plane, cursor.foreground);
                    instructions(1);
                }
                instructions(2);
                if ((bits & 2) != 0) {
                    surface_.write(address, cursor.plane, 0);
                    instructions(1);
                }
                bits >>= 2;
                instructions(1);
            }
            instructions(4);
            if (cursor.plane == 8) { instructions(2); }
            instructions(2);
            advance(cursor);
        }
        instructions(2);
    }
    throw std::invalid_argument("Unterminated text row");
}

void MenuRenderer::text_rows(const std::vector<std::uint8_t>& stream, std::size_t origin, std::uint8_t foreground) {
    std::size_t consumed = 0;
    while (consumed < stream.size()) {
        const std::vector<std::uint8_t> remaining(stream.begin() + consumed, stream.end());
        const auto cursor = text(remaining, {origin, 2, foreground});
        if (cursor.marker == 255) {
            return;
        }
        consumed += cursor.consumed;
        foreground = cursor.foreground;
        origin += text_line_stride;
    }
    throw std::invalid_argument("Missing final text row marker");
}

void MenuRenderer::draw_briefing(const std::vector<std::uint8_t>& description,
                                 const std::vector<std::uint8_t>& choices, std::uint8_t selection) {
    if (selection >= 2) {
        throw std::invalid_argument("Briefing choice out of range");
    }
    restore_background();
    const auto origin = back_page_ + 1 + pan_;
    box(origin + 4 + 30 * line_stride, 72, 160);
    text_rows(description, origin + 0xa45, theme_[6]);
    const auto clear = [&] { if (reset_input_) { reset_input_(); } };
    if (clock_) { clock_->instruction(clear); }
    else { clear(); }
    box(origin + 24 + 180 * line_stride, 32, 27);
    highlight(origin + 24 + 0x3a4c + selection * text_line_stride, 30, 12);
    text_rows(choices, origin + 0x3b0a, theme_[6]);
}

void MenuRenderer::draw_loading(const std::vector<std::uint8_t>& prompt) {
    const auto origin = back_page_ + 1 + pan_;
    box(origin + 25 + 206 * line_stride, 30, 15);
    text_rows(prompt, origin + 0x4360, theme_[0]);
}

void MenuRenderer::draw_debrief(const std::vector<std::uint8_t>& stream, DebriefBackground background) {
    if (background == DebriefBackground::capture) { save_background(); }
    else { restore_background(); }
    const auto origin = back_page_ + 1 + pan_;
    if (work_) { work_(CpuOperation::instructions, 4); }
    box(origin + 4 + 30 * line_stride, 72, 160);
    if (work_) { work_(CpuOperation::instructions, 7); }
    std::size_t consumed = 0, row_origin = origin + 0xc34;
    auto foreground = theme_[6];
    while (consumed < stream.size()) {
        if (work_) { work_(CpuOperation::instructions, 2); }
        const std::vector<std::uint8_t> remaining(stream.begin() + consumed, stream.end());
        const auto cursor = text(remaining, {row_origin, 2, foreground});
        if (work_) { work_(CpuOperation::instructions, 5); }
        if (cursor.marker == 255) { return; }
        consumed += cursor.consumed;
        foreground = cursor.foreground;
        row_origin += text_line_stride;
    }
    throw std::invalid_argument("Missing final debrief row marker");
}

void MenuRenderer::draw_statistics(const std::vector<std::uint8_t>& stream) {
    restore_background();
    const auto origin = back_page_ + 1 + pan_;
    box(origin + 4 + 65 * line_stride, 72, 90);
    text_rows(stream, origin + 0x1860, theme_[6]);
}

void MenuRenderer::draw_locked_mission(const std::vector<std::uint8_t>& stream) {
    restore_background();
    const auto origin = back_page_ + 1 + pan_;
    box(origin + 15 + 66 * line_stride, 50, 15);
    text(stream, {origin + 0x167b, 2, theme_[5]});
}

void MenuRenderer::draw_settings(const SettingsAssets& assets, const MenuSettings& settings, const SettingsMenu& menu,
                                 SettingsNotice notice) {
    constexpr std::size_t header_origin = 0xf7, row_stride = line_stride * 11;
    const auto origin = back_page_ + 1 + pan_;
    std::vector<TextCursor> tabs;
    TextCursor cursor{origin + header_origin, 1, theme_[4]};
    for (std::size_t index = 0; index < assets.header.size(); ++index) {
        const auto code = assets.header[index];
        if (code == 255) {
            break;
        }
        if (code == 0xfe) {
            tabs.push_back(cursor);
            continue;
        }
        if (code == '|') {
            ++index;
            continue;
        }
        const auto width = code == 0xfc ? 0 : code == 0xfd ? 6 : font_.width(code);
        for (std::size_t pixel = 0; pixel < width; ++pixel) {
            advance(cursor);
        }
    }
    if (tabs.size() != assets.menus.size() + 1) {
        throw std::invalid_argument("Invalid original settings header");
    }
    restore_background();
    box(origin, 80, 14);
    const auto category = menu.category();
    const auto tab = tabs.at(category);
    const auto tab_width = tabs.at(category + 1).offset - tab.offset - 2;
    for (std::size_t row = 0; row < 12; ++row) {
        const auto start = tab.offset - 0xa5 + row * line_stride;
        surface_.write(start, 14, theme_[3]);
        for (std::size_t x = 1; x <= tab_width; ++x) {
            surface_.write(start + x, all_planes, theme_[3]);
        }
    }
    auto header = assets.header;
    header.erase(std::remove(header.begin(), header.end(), 0xfe), header.end());
    text(header, {origin + header_origin, 1, theme_[4]});
    if (notice != SettingsNotice::none) {
        if (notice == SettingsNotice::about) {
            box(origin + 15 + 30 * line_stride, 50, 90);
            text_rows(assets.about, origin + 0x812, theme_[5]);
        } else {
            box(origin + 22 + 66 * line_stride, 36, 15);
            text_rows(assets.saved, origin + 0x1682, theme_[5]);
        }
        return;
    }
    const auto width = assets.widths.at(category);
    box(origin + tab.offset - origin - header_origin + 15 * line_stride,
        width, static_cast<std::uint8_t>(assets.rows.at(category) * 11 + 3));
    highlight(tab.offset + 0x429 + menu.row() * row_stride, width - 2, 12);
    auto stream = settings.menu(assets, category);
    cursor = {tab.offset + 0x148 + row_stride, tab.plane, theme_[6]};
    std::vector<std::uint8_t> segment;
    for (std::size_t index = 0; index < stream.size(); ++index) {
        const auto code = stream[index];
        if (code == '|' && index + 1 < stream.size() && stream[index + 1] == 0xf9) {
            segment.push_back(0);
            cursor = text(segment, cursor);
            cursor.consumed = 0;
            segment.clear();
            surface_.write(cursor.offset + 0x147, 8, cursor.foreground);
            for (std::size_t x = 1; x <= std::size_t(width - 2); ++x) {
                surface_.write(cursor.offset + 0x147 + x, all_planes, cursor.foreground);
            }
            ++index;
            continue;
        }
        segment.push_back(code);
        if (code == 0 || code == 255) {
            const auto end = text(segment, cursor);
            cursor = {tab.offset + 0x148 + row_stride +
                      ((cursor.offset - tab.offset - 0x148) / row_stride) * row_stride,
                      tab.plane, end.foreground};
            segment.clear();
        }
    }
}

void MenuRenderer::draw_list(const std::vector<std::uint8_t>& stream, std::uint8_t selection,
                             ListLayout layout, const DrawObserver& observer) {
    const auto instructions = [&](unsigned n) { if (work_) { work_(CpuOperation::instructions, n); } };
    const auto notify = [&](DrawStage stage, std::size_t row) {
        if (observer) {
            observer(stage, row, surface_);
        }
    };
    restore_background();
    notify(DrawStage::background, 0);
    const auto clear = [&] { if (reset_input_) { reset_input_(); } };
    if (clock_) { clock_->instruction(clear); }
    else { clear(); }
    instructions(4);
    const auto origin = back_page_ + 1 + pan_;
    box(origin + 14 + 30 * line_stride, 52, 160);
    notify(DrawStage::box, 0);
    instructions(16 + (layout == ListLayout::main ? 2 : 0));
    highlight(origin + 14 + 66 * line_stride + selection * text_line_stride +
              (layout == ListLayout::main ? 24 * line_stride : 0), 50, 12);
    notify(DrawStage::highlight, 0);
    instructions(7);
    auto foreground = theme_[0];
    std::size_t consumed = 0;
    for (std::size_t row = 0; consumed < stream.size(); ++row) {
        const std::vector<std::uint8_t> remaining(stream.begin() + consumed, stream.end());
        instructions(2);
        const auto result = text(remaining, {origin + 0xa4f + row * text_line_stride, 2, foreground});
        instructions(6);
        consumed += result.consumed;
        foreground = result.foreground;
        notify(DrawStage::text_row, row);
        if (result.marker == 255) {
            instructions(3);
            const auto option_count = static_cast<std::uint8_t>(row + 1 - (layout == ListLayout::main ? 8 : 6));
            if (selection >= option_count) {
                throw std::invalid_argument("Selection requires an additional original redraw");
            }
            notify(DrawStage::complete, row);
            return;
        }
    }
    throw std::invalid_argument("Menu table has no final row marker");
}

} // namespace comanchecd
