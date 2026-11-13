#include "../src/menu_work.hpp"
#include "comanchecd/menu_renderer.hpp"
#include "comanchecd/assets.hpp"
#include "loading_clock.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
}
int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        if (argc == 5) {
            std::ifstream input(argv[1], std::ios::binary);
            input.exceptions(std::ios::failbit | std::ios::badbit);
            std::ofstream output(argv[2]);
            const auto integer = [&] {
                std::array<unsigned char, 4> b{};
                input.read(reinterpret_cast<char*>(b.data()), b.size());
                return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
            };
            const auto font = Installation(argv[3], argv[4]).load_frontend().font;
            const auto count = integer();
            require(count <= 10000, "Too many menu work cases");
            for (unsigned index = 0; index < count; ++index) {
                const auto kind = integer(), width = integer(), height = integer();
                LoadingClock clock;
                const CpuWorkSink work = [&](CpuOperation op, unsigned n) { clock.execute(op, n); };
                if (kind == 0) {
                    menu_work::background(work);
                } else if (kind == 1) {
                    require(width >= 2 && height >= 3, "Invalid box work dimensions");
                    menu_work::box(width, height, work);
                } else if (kind == 2) {
                    menu_work::highlight(width, height, work);
                } else if (kind == 4) {
                    Palette palette{};
                    Transfer transfer{};
                    input.read(reinterpret_cast<char*>(palette.data()), palette.size() * 3);
                    input.read(reinterpret_cast<char*>(transfer.data()), transfer.size());
                    const auto colors = menu_work::colors(palette, transfer, work);
                    require(colors.theme == build_theme(palette, transfer) && colors.remap == build_box_remap(palette, transfer),
                            "Clocked palette construction must retain every theme and remap entry");
                } else if (kind == 3) {
                    require(width <= 512, "Text fixture exceeds bounded row");
                    std::vector<std::uint8_t> stream(width);
                    input.read(reinterpret_cast<char*>(stream.data()), stream.size());
                    MenuRenderer renderer(PlanarSurface(std::vector<std::uint8_t>(plane_count * plane_bytes)),
                                          Font(font), {}, {}, {}, 0, 0, &clock);
                    renderer.text(stream, {0, static_cast<std::uint8_t>(height), 1});
                } else {
                    throw std::invalid_argument("Unknown menu work kind");
                }
                const auto cost = clock.accounting();
                output << cost.dispatches - cost.repeat_restarts << ' '
                       << cost.string_iterations + cost.special_stosb_iterations << ' ' << cost.io_cycles << '\n';
            }
            require(bool(output), "Cannot write menu work output");
            return 0;
        }
        require(argc == 1, "Usage: menu_work_tests [INPUT OUTPUT INSTALLATION PAYLOADS]");
        LoadingClock background;
        menu_work::background([&](CpuOperation op, unsigned n) { background.execute(op, n); });
        auto cost = background.accounting();
        require(cost.dispatches - cost.repeat_restarts == 31 && cost.string_iterations == background_bytes && cost.io_cycles == 36,
                "Original background restore must retain all four VGA masks and REP copies");
        LoadingClock saved_background;
        menu_work::save_background([&](CpuOperation op, unsigned n) { saved_background.execute(op, n); });
        cost = saved_background.accounting();
        require(cost.dispatches - cost.repeat_restarts == 34 && cost.string_iterations == background_bytes && cost.io_cycles == 36,
                "Original background save must retain four read-plane selections and REP copies");
        LoadingClock highlight;
        menu_work::highlight(3, 2, [&](CpuOperation op, unsigned n) { highlight.execute(op, n); });
        cost = highlight.accounting();
        require(cost.dispatches - cost.repeat_restarts == 40 && cost.string_iterations == 10 && cost.io_cycles == 54,
                "Highlight work must retain its three masks and edge stores per row");
        std::vector<std::uint8_t> font(800);
        font[8 + 'A'] = 1;
        constexpr unsigned glyph = 780, offsets = 8 + 256;
        font[offsets + 'A' * 2] = glyph & 255;
        font[offsets + 'A' * 2 + 1] = glyph >> 8;
        font[glyph] = 3;
        unsigned briefing_resets = 0;
        Background briefing_background;
        for (auto& plane : briefing_background) { plane.resize(background_bytes); }
        LoadingClock briefing_clock;
        MenuRenderer briefing(PlanarSurface(std::vector<std::uint8_t>(plane_count * plane_bytes)),
            Font(font), {}, {}, briefing_background, 0, 0, &briefing_clock, [&] { ++briefing_resets; });
        briefing.draw_briefing({'A', 255}, {'A', 255}, 0);
        require(briefing_resets == 1,
                "Briefing must clear its old pending input at 2cb83 before drawing choices");
        LoadingClock text;
        MenuRenderer renderer(PlanarSurface(std::vector<std::uint8_t>(plane_count * plane_bytes)),
                              Font(font), {}, {}, {}, 0, 0, &text);
        renderer.text({'A', 0}, {0, 1, 1});
        cost = text.accounting();
        require(cost.dispatches == 103 && cost.io_cycles == 9,
                "Text must charge both writes of a set two-bit pixel, glyph mask and terminating marker");
        LoadingClock list;
        Background saved;
        for (auto& plane : saved) { plane.resize(background_bytes); }
        MenuRenderer menu(PlanarSurface(std::vector<std::uint8_t>(plane_count * plane_bytes)),
                          Font(font), {}, {}, saved, 0, 0, &list);
        menu.draw_list({0, 0, 0, 0, 0, 0, 255}, 0, ListLayout::standard);
        cost = list.accounting();
        require(cost.dispatches - cost.repeat_restarts == 109735 &&
                cost.string_iterations + cost.special_stosb_iterations == 37351 && cost.io_cycles == 13221,
                "Whole list redraw must include background, clear, caller and row-loop work");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
