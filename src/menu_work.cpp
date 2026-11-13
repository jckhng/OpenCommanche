#include "menu_work.hpp"
#include "comanchecd/menu_renderer.hpp"
#include "color_metric.hpp"

namespace comanchecd::menu_work {
Colors colors(const Palette& palette, const Transfer& transfer, const CpuWorkSink& work) {
    if (!work) { return {build_theme(palette, transfer), build_box_remap(palette, transfer)}; }
    const auto instructions = [&](unsigned n) { work(CpuOperation::instructions, n); };
    Colors result{build_theme(palette, transfer), {}};
    // 2e334: palette copy/preparation, box remap, then theme searches in DOS order.
    instructions(4);
    work(CpuOperation::repeat_copy_dwords, palette.size() * 3 / 4);
    instructions(1 + 4);
    for (unsigned index = 0; index < palette.size(); ++index) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            work(CpuOperation::string_bytes, 1);
            instructions(1);
            work(CpuOperation::string_bytes, 1);
        }
        instructions(1);
    }
    instructions(1 + 8);
    instructions(1 + 3 * palette.size() + 5);
    for (unsigned index = 0; index < palette.size(); ++index) {
        auto target = palette[index];
        for (auto& channel : target) { channel = static_cast<std::uint8_t>((channel + 1) >> 1); }
        instructions(20);
        result.remap[index] = color_metric::nearest(palette, transfer, target, 0, palette.size() - 1, work);
        instructions(7);
    }
    instructions(1);
    constexpr std::array<std::array<std::uint8_t, 3>, 10> targets{{
        {63, 63, 63}, {40, 40, 40}, {28, 28, 28}, {16, 16, 16}, {16, 48, 63},
        {16, 63, 16}, {63, 16, 63}, {63, 56, 16}, {63, 32, 16}, {63, 16, 16}
    }};
    constexpr std::array<unsigned, 10> slots{0, 3, 2, 1, 4, 6, 7, 9, 8, 5};
    for (unsigned index = 0; index < targets.size(); ++index) {
        instructions(4);
        result.theme[slots[index]] = color_metric::nearest(palette, transfer, targets[index], 0, palette.size() - 1, work);
        instructions(1);
    }
    instructions(8);
    color_metric::nearest(palette, transfer, palette.back(), 0, palette.size() - 2, work);
    instructions(2);
    return result;
}

void background(const CpuWorkSink& work) {
    if (!work) { return; }
    work(CpuOperation::instructions, 1);
    for (unsigned plane = 0; plane < plane_count; ++plane) {
        work(CpuOperation::instructions, plane == 0 ? 2 : 1);
        work(CpuOperation::port_write_word, 1);
        work(CpuOperation::instructions, 4);
        work(CpuOperation::repeat_copy_dwords, background_bytes / 4);
    }
    work(CpuOperation::instructions, 1);
}

void save_background(const CpuWorkSink& work) {
    if (!work) { return; }
    // 17907..17990, excluding the caller's CALL.
    work(CpuOperation::instructions, 1);
    for (unsigned plane = 0; plane < plane_count; ++plane) {
        work(CpuOperation::instructions, 2);
        work(CpuOperation::port_write_word, 1);
        work(CpuOperation::instructions, 4);
        work(CpuOperation::repeat_copy_dwords, background_bytes / 4);
    }
    work(CpuOperation::instructions, 1);
}

void copy_page(unsigned height, const CpuWorkSink& work) {
    if (!work) { return; }
    // 1769a..176f4, excluding the caller's CALL.
    work(CpuOperation::instructions, 3);
    work(CpuOperation::port_write_byte, 1);
    work(CpuOperation::instructions, 2);
    work(CpuOperation::port_read_byte, 1);
    work(CpuOperation::instructions, 2);
    work(CpuOperation::port_write_byte, 1);
    work(CpuOperation::instructions, 3);
    work(CpuOperation::port_write_word, 1);
    work(CpuOperation::instructions, 8);
    work(CpuOperation::repeat_copy_bytes, line_stride * height);
    work(CpuOperation::instructions, 2);
    work(CpuOperation::port_write_byte, 1);
    work(CpuOperation::instructions, 2);
    work(CpuOperation::port_read_byte, 1);
    work(CpuOperation::instructions, 2);
    work(CpuOperation::port_write_byte, 1);
    work(CpuOperation::instructions, 2);
}

void highlight(unsigned width, unsigned height, const CpuWorkSink& work) {
    if (!work) { return; }
    const auto instructions = [&](unsigned n) { work(CpuOperation::instructions, n); };
    instructions(1);
    for (unsigned row = 0; row < height; ++row) {
        instructions(3);
        work(CpuOperation::port_write_word, 1);
        instructions(1);
        work(CpuOperation::string_bytes, 1);
        instructions(1);
        work(CpuOperation::port_write_word, 1);
        instructions(1);
        work(CpuOperation::repeat_store_bytes, width);
        instructions(1);
        work(CpuOperation::port_write_word, 1);
        instructions(1);
        work(CpuOperation::string_bytes, 1);
        instructions(5);
    }
    instructions(1);
}

void box(unsigned width, unsigned height, const CpuWorkSink& work) {
    if (!work) { return; }
    const auto instructions = [&](unsigned n) { work(CpuOperation::instructions, n); };
    const auto out = [&] { work(CpuOperation::port_write_word, 1); };
    const auto fill = [&](unsigned n) { work(CpuOperation::repeat_store_bytes, n); };
    instructions(4); out(); instructions(35);
    fill(width - 1);
    instructions(3); out(); instructions(2);
    work(CpuOperation::string_bytes, 1);
    instructions(3);
    for (unsigned row = 0; row < height - 2; ++row) {
        for (unsigned plane = 0; plane < plane_count; ++plane) {
            instructions(3); out(); instructions(3); out();
            instructions(plane == plane_count - 1 ? 3 : 4);
            for (unsigned pair = 0; pair < width / 2; ++pair) {
                instructions(4);
                work(CpuOperation::vga_store_words, 1);
                instructions(1);
            }
            instructions(1);
            if (width & 1) {
                instructions(2);
                work(CpuOperation::string_bytes, 1);
            }
        }
        instructions(2); out(); instructions(4);
    }
    instructions(2); fill(width);
    instructions(4); out(); instructions(2);
    work(CpuOperation::string_bytes, 1);
    instructions(3); out(); instructions(4); fill(width - 1);
    instructions(4); fill(width - 1);
    instructions(5); out(); instructions(5 + 3 * (height - 1));
    instructions(4); out(); instructions(9 + 3 * (height - 1));
    instructions(4); out(); instructions(4 + 3 * (height - 1) + 1);
}
}
