#include "comanchecd/menu_renderer.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        comanchecd::Palette palette{};
        for (auto& color : palette) {
            color = {63, 63, 63};
        }
        palette[0] = {0, 0, 0};
        palette[1] = {2, 2, 2};
        comanchecd::Transfer transfer{};
        for (std::size_t index = 0; index < transfer.size(); ++index) {
            transfer[index] = static_cast<std::uint8_t>(index);
        }
        const auto remap = comanchecd::build_box_remap(palette, transfer);
        require(remap[1] == 0, "Nearest-color tie must retain first index");
        palette[1] = {63, 63, 63};
        require(comanchecd::build_box_remap(palette, transfer)[1] == 1,
                "Odd-channel darkening must round upward");

        comanchecd::PlanarSurface surface(std::vector<std::uint8_t>(comanchecd::plane_bytes * 4, 99));
        surface.write(100, 5, 7);
        require(surface.read(0, 100) == 7 && surface.read(2, 100) == 7 &&
                surface.read(1, 100) == 99 && surface.read(3, 100) == 99,
                "Plane masks must preserve unselected bytes");
        std::vector<std::uint8_t> font(780);
        constexpr std::size_t widths = 8, offsets = 264, glyph = 776;
        font[widths + 'A'] = 1;
        font[offsets + 'A' * 2] = static_cast<std::uint8_t>(glyph);
        font[offsets + 'A' * 2 + 1] = static_cast<std::uint8_t>(glyph >> 8);
        font[glyph] = 3; // Foreground then black at the same pixel.
        comanchecd::Theme theme{};
        theme[0] = 7;
        comanchecd::Background background;
        for (auto& plane : background) {
            plane.resize(comanchecd::background_bytes);
        }
        comanchecd::MenuRenderer renderer(std::move(surface), comanchecd::Font(font), theme, remap,
                                          background, 0, 0);
        const auto cursor = renderer.text({'A', 0xfc, 0xfd, 255}, {100, 8, 7});
        require(renderer.surface().read(3, 100) == 0, "Glyph black write must win over foreground");
        require(renderer.surface().read(3, 100 + comanchecd::line_stride) == 99,
                "Transparent glyph pairs must preserve destination");
        require(cursor.offset == 102 && cursor.plane == 4 && cursor.consumed == 4 && cursor.marker == 255,
                "Glyph, padding, six-pixel space and plane wrap cursor");
        for (const auto page : {std::size_t(0), comanchecd::page_displacement}) {
            const auto other = page ^ comanchecd::page_displacement;
            auto pages = std::vector<std::uint8_t>(comanchecd::plane_count * comanchecd::plane_bytes, 99);
            for (std::size_t plane = 0; plane < comanchecd::plane_count; ++plane) {
                for (std::size_t offset = 0; offset < comanchecd::background_bytes; ++offset) {
                    pages[plane * comanchecd::plane_bytes + page + offset] = static_cast<std::uint8_t>(plane + offset);
                }
            }
            comanchecd::MenuRenderer briefing(comanchecd::PlanarSurface(pages), comanchecd::Font(font),
                theme, remap, background, page, 0);
            briefing.prepare_briefing();
            require(briefing.back_page() == other, "Briefing preparation must flip once");
            for (std::size_t plane = 0; plane < comanchecd::plane_count; ++plane) {
                for (std::size_t offset = 0; offset < comanchecd::background_bytes; ++offset) {
                    const auto expected = offset < comanchecd::line_stride * comanchecd::briefing_page_height ?
                        static_cast<std::uint8_t>(plane + offset) : std::uint8_t(99);
                    require(briefing.surface().read(plane, other + offset) == expected,
                        "Briefing latch copy must cover 240 rows on all planes and preserve the remaining rows");
                }
            }
            briefing.restore_background();
            for (std::size_t plane = 0; plane < comanchecd::plane_count; ++plane) {
                for (std::size_t offset = 0; offset < comanchecd::background_bytes; ++offset) {
                    require(briefing.surface().read(plane, other + offset) == static_cast<std::uint8_t>(plane + offset),
                        "Briefing background restore must use the newly saved 245-row page");
                }
            }
            comanchecd::MenuRenderer debrief(comanchecd::PlanarSurface(pages), comanchecd::Font(font),
                theme, remap, background, page, 0);
            debrief.draw_debrief({255}, comanchecd::DebriefBackground::capture);
            debrief.restore_background();
            for (std::size_t plane = 0; plane < comanchecd::plane_count; ++plane) {
                for (std::size_t offset = 0; offset < comanchecd::background_bytes; ++offset) {
                    require(debrief.surface().read(plane, page + offset) == static_cast<std::uint8_t>(plane + offset),
                        "Initial debrief must save the loaded page before drawing its result box");
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
