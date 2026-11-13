#include "loading_work.hpp"
#include "color_metric.hpp"
#include <algorithm>

namespace comanchecd::loading_work {
namespace {
constexpr unsigned palette_colors = 256;
constexpr unsigned reserved_water_color = 251;
constexpr unsigned blend_source_work = 20;
constexpr unsigned search_return_work = 7;

void blend(const Palette& palette, const Transfer& transfer, const std::array<std::uint8_t, 3>& tint,
           unsigned last, const CpuWorkSink& work) {
    // 2e4ed initializes the identity table, then processes source indices 0..last-1.
    work(CpuOperation::instructions, 1 + 3 * palette_colors + 5);
    for (unsigned index = 0; index < last; ++index) {
        auto color = palette[index];
        for (unsigned channel = 0; channel < color.size(); ++channel) {
            color[channel] = static_cast<std::uint8_t>((color[channel] + tint[channel] + 1) >> 1);
        }
        work(CpuOperation::instructions, blend_source_work);
        color_metric::nearest(palette, transfer, color, 1, last, work);
        work(CpuOperation::instructions, search_return_work);
    }
    work(CpuOperation::instructions, 1);
}
}

namespace {
GameplayPayload rle(const std::vector<std::uint8_t>& encoded, PayloadKind kind, ExecutionClock& clock, InputBuffer& buffer) {
    constexpr unsigned palette_bytes = 768;
    const unsigned header_bytes = kind == PayloadKind::picture ? 128 : 8;
    // Validate before advancing the retained clock. The boundary is 16f9d..17021.
    const auto decoded = decode_payload(encoded, kind);
    std::size_t cursor = header_bytes, loaded = header_bytes, window = 0, pixels = 0;
    const auto instructions = [&](unsigned count) { clock.execute(CpuOperation::instructions, count); };
    const auto refill = [&] {
        const auto bytes = static_cast<unsigned>(std::min<std::size_t>(input_buffer_bytes, encoded.size() - loaded));
        instructions(10);
        clock.read_file(bytes);
        instructions(9);
        std::copy_n(encoded.begin() + loaded, bytes, buffer.begin());
        loaded += bytes;
        window = 0;
    };
    const auto check = [&](unsigned branch_work) {
        instructions(2);
        if (window >= input_buffer_bytes) {
            instructions(branch_work);
            refill();
        }
    };
    const auto byte = [&] {
        if (cursor >= encoded.size() && kind == PayloadKind::picture) {
            throw std::invalid_argument("Missing timed picture trailer");
        }
        clock.execute(CpuOperation::string_bytes, 1);
        ++cursor;
        return buffer.at(window++);
    };
    if (kind == PayloadKind::resource) { instructions(1); } // 16f97 MOV EBP.
    instructions(1); // Initial refill CALL; later refill branches own their entry work.
    refill();
    instructions(4);
    while (pixels < decoded.bytes.size()) {
        check(2);
        const auto value = byte();
        instructions(2);
        if (value < 192) {
            clock.execute(CpuOperation::string_bytes, 1);
            ++pixels;
            instructions(2);
            if (pixels == decoded.bytes.size()) { instructions(1); }
            continue;
        }
        instructions(2);
        check(2);
        byte();
        const unsigned count = value & 63;
        clock.execute(CpuOperation::repeat_store_bytes, count);
        pixels += count;
        instructions(2);
    }
    check(1);
    const auto marker = byte();
    instructions(2);
    if (marker != 12) { return decoded; }
    instructions(2);
    for (unsigned channel = 0; channel < palette_bytes; ++channel) {
        check(2);
        byte();
        instructions(1);
        clock.execute(CpuOperation::string_bytes, 1);
        instructions(1);
    }
    return decoded;
}

}

GameplayPayload picture(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock) {
    InputBuffer buffer{};
    return picture(encoded, clock, buffer);
}

GameplayPayload picture(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock, InputBuffer& buffer) {
    return rle(encoded, PayloadKind::picture, clock, buffer);
}

GameplayPayload compressed_resource(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock) {
    constexpr unsigned minimum_buffered_resource_bytes = 8 + 8192;
    if (encoded.size() < minimum_buffered_resource_bytes || encoded[3] != '1') {
        throw std::invalid_argument("Compressed startup timing requires a complete initial input buffer");
    }
    // KRL1 uses the picture decoder, including its read beyond the decoded
    // allocation. A short refill leaves the preceding buffer tail in place.
    InputBuffer buffer{};
    return compressed_resource(encoded, clock, buffer);
}

GameplayPayload compressed_resource(const std::vector<std::uint8_t>& encoded, ExecutionClock& clock, InputBuffer& buffer) {
    return rle(encoded, PayloadKind::resource, clock, buffer);
}

void cockpit_remap(const GameplayPayload& picture, const Palette& destination, const Transfer& transfer,
                   const CpuWorkSink& work) {
    if (!work) { return; }
    if (picture.palette.size() != palette_colors * 3 || picture.bytes.size() < 2) {
        throw std::invalid_argument("Cockpit remap work requires a decoded picture and palette");
    }
    // 2f36e, including its RET; the caller owns the CALL and palette preparation.
    work(CpuOperation::instructions, 8);
    for (unsigned index = 1; index < palette_colors - 1; ++index) {
        const std::array<std::uint8_t, 3> color{picture.palette[index * 3], picture.palette[index * 3 + 1],
                                               picture.palette[index * 3 + 2]};
        work(CpuOperation::instructions, 10);
        color_metric::nearest(destination, transfer, color, 1, palette_colors - 2, work);
        work(CpuOperation::instructions, search_return_work);
    }
    work(CpuOperation::instructions, 4);
    for (std::size_t pair = 0; pair < picture.bytes.size() / 2; ++pair) {
        work(CpuOperation::instructions, 4);
        work(CpuOperation::string_bytes, 1);
        work(CpuOperation::instructions, 1);
    }
    work(CpuOperation::instructions, 1);
    if ((picture.bytes.size() & 1) != 0) {
        work(CpuOperation::instructions, 2);
        work(CpuOperation::string_bytes, 1);
    }
    work(CpuOperation::instructions, 1);
}

void cockpit_spans(const GameplayPayload& picture, const CpuWorkSink& work) {
    if (!work) { return; }
    constexpr unsigned mask_rows = 182, maximum_spans = 2048;
    constexpr std::uint8_t transparent = 255;
    if (picture.width != line_stride * plane_count || picture.height < mask_rows) {
        throw std::invalid_argument("Cockpit spans require the original backing width and visible rows");
    }
    const auto instructions = [&](unsigned count) { work(CpuOperation::instructions, count); };
    unsigned spans = 0;
    for (unsigned pair = 0; pair < 2; ++pair) {
        instructions(pair == 0 ? 12 : 11);
        bool full = false;
        for (unsigned row = mask_rows; row-- > 0;) {
            instructions(2);
            unsigned left = line_stride, column = 0;
            const auto word = [&](unsigned at) {
                const auto offset = row * picture.width + at * plane_count + pair * 2;
                return picture.bytes.at(offset) == transparent && picture.bytes.at(offset + 1) == transparent;
            };
            for (;;) {
                instructions(3);
                if (left == 0) { break; }
                bool match = false;
                do {
                    match = word(column++);
                    --left;
                    instructions(5);
                } while (left && !match);
                instructions(1);
                if (!match) { break; }
                const auto start = column - 1;
                const auto previous = std::int64_t(row * picture.width + column * plane_count + pair * 2) - 7;
                instructions(2);
                if (previous >= 0 && picture.bytes.at(previous) == transparent) { instructions(1); }
                instructions(6);
                --column;
                ++left;
                do {
                    match = word(column++);
                    --left;
                    instructions(5);
                } while (left && match);
                instructions(1);
                if (!match) {
                    const auto offset = row * picture.width + (column - 1) * plane_count + pair * 2;
                    instructions(2);
                    if (picture.bytes.at(offset) == transparent) { instructions(1); }
                    instructions(4);
                    if (column - start <= 1) { break; }
                } else {
                    instructions(2);
                }
                instructions(12);
                if (++spans >= maximum_spans) {
                    full = true;
                    break;
                }
                if (match) { break; }
                instructions(1);
            }
            if (full) { break; }
            instructions(7);
        }
        instructions(2); // Row counter cleanup or the normal JMP, then saved span count.
    }
    instructions(2 + 5 * spans + 1);
}

void scenery_palette(const GameplaySetup& setup, const Palette& palette, const Transfer& transfer,
                     const CpuWorkSink& work) {
    if (!work) { return; }
    // 27558..2776e: effects, fog and all four water tables are unconditional.
    const unsigned last = setup.terrain_variant == 0 ? palette_colors - 1 : reserved_water_color - 1;
    work(CpuOperation::instructions, 5);
    work(CpuOperation::repeat_store_dwords, 48);
    work(CpuOperation::instructions, 2);
    work(CpuOperation::repeat_store_dwords, 32);
    work(CpuOperation::instructions, 2);
    work(CpuOperation::repeat_store_dwords, 64);
    work(CpuOperation::instructions, 4 + (setup.terrain_variant != 0 ? 1 : 0) + 10);
    const std::array<std::uint8_t, 3> solid{setup.light_color[0], setup.light_color[1], setup.light_color[2]};
    color_metric::nearest(palette, transfer, solid, 1, last, work);
    work(CpuOperation::instructions, 3);
    blend(palette, transfer, solid, last, work);
    work(CpuOperation::instructions, 8);
    blend(palette, transfer, {setup.secondary_color[0], setup.secondary_color[1], setup.secondary_color[2]}, last, work);
    work(CpuOperation::instructions, 6 + 3 + 3 * palette_colors + 5);
    for (unsigned index = 0; index < last; ++index) {
        auto color = palette[index];
        for (unsigned channel = 0; channel < color.size(); ++channel) {
            color[channel] = static_cast<std::uint8_t>((color[channel] * 3 + palette.back()[channel] + 3) >> 2);
        }
        work(CpuOperation::instructions, 23);
        color_metric::nearest(palette, transfer, color, 1, last, work);
        work(CpuOperation::instructions, search_return_work);
    }
    work(CpuOperation::instructions, 8);
    blend(palette, transfer, palette.back(), last, work);
    work(CpuOperation::instructions, 3);
    for (unsigned water = reserved_water_color; water < palette_colors - 1; ++water) {
        work(CpuOperation::instructions, 12);
        for (const auto& sample : palette) {
            auto color = sample;
            for (unsigned channel = 0; channel < color.size(); ++channel) {
                color[channel] = static_cast<std::uint8_t>((color[channel] + palette[water][channel] + 1) >> 1);
            }
            work(CpuOperation::instructions, blend_source_work);
            color_metric::nearest(palette, transfer, color, 1, palette_colors - 1, work);
            work(CpuOperation::instructions, search_return_work);
        }
        work(CpuOperation::instructions, 5);
    }
    work(CpuOperation::instructions, 1);
}
}
