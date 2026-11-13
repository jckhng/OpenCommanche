#include "fanfare_timing.hpp"
#include "flight_work.hpp"
#include "loading_work.hpp"
#include "startup_resource.hpp"
#include "menu_work.hpp"
#include <algorithm>
#include <stdexcept>

namespace comanchecd {
namespace {
constexpr unsigned picture_header_bytes = 128;
constexpr unsigned filename_byte_work = 12, palette_bytes = 768;

template<class Clock>
void instructions(Clock& clock, unsigned count) { clock.execute(CpuOperation::instructions, count); }

void open_file(AudioScheduler& clock, StartupLocation location) {
    clock.file_metadata(DosFileCall::find);
    instructions(clock, location == StartupLocation::local ? 3 : 5);
    clock.file_metadata(DosFileCall::open_read);
    instructions(clock, 6);
}

void picture_header(AudioScheduler& clock, StartupLocation location, unsigned filename_bytes = 9) {
    // 16ea5 through the initial refill CALL, excluding that CALL.
    instructions(clock, 5 + filename_bytes * filename_byte_work + 7 + 3 + 2 + 2 + 3);
    open_file(clock, location);
    clock.read_file(picture_header_bytes);
    instructions(clock, 3);
    clock.execute(CpuOperation::string_bytes, 1);
    instructions(clock, 2);
    clock.execute(CpuOperation::string_bytes, 1);
    instructions(clock, 6);
}

template<class Clock>
void draw_picture(Clock& clock, unsigned height) {
    constexpr unsigned words_per_row = line_stride / 2;
    instructions(clock, 1);
    for (unsigned plane = 0; plane < plane_count; ++plane) {
        instructions(clock, 2);
        clock.execute(CpuOperation::port_write_word, 1);
        instructions(clock, 2 + 4);
        for (unsigned row = 0; row < height; ++row) {
            for (unsigned word = 0; word < words_per_row; ++word) {
                instructions(clock, 4);
                clock.execute(CpuOperation::vga_store_words, 1);
            }
            instructions(clock, 2);
        }
        instructions(clock, 1);
    }
    instructions(clock, 1);
}

template<class Clock>
void palette(Clock& clock) {
    instructions(clock, 3);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(clock, 4);
    for (unsigned channel = 0; channel < palette_bytes; ++channel) {
        clock.execute(CpuOperation::string_bytes, 1);
        clock.execute(CpuOperation::port_write_byte, 1);
        instructions(clock, 2);
    }
    instructions(clock, 1);
}

template<class Clock>
void copy_page(Clock& clock, unsigned height) {
    menu_work::copy_page(height, [&](CpuOperation operation, unsigned count) { clock.execute(operation, count); });
}
}

std::vector<std::uint8_t> prepare_fanfare(AudioScheduler& scheduler, ExecutionClock& clock,
    const FanfareAssets& assets, const std::function<void(FanfareBoundary)>& observer) {
    const auto score = decode_payload(assets.score, PayloadKind::resource);
    const auto picture = decode_payload(assets.picture, PayloadKind::picture);
    if (assets.score.at(3) != '0' || picture.width != line_stride * plane_count ||
            picture.height == 0 || picture.height > 240 || picture.palette.size() != palette_bytes) {
        throw std::invalid_argument("Unsupported cold fanfare resource or backdrop");
    }
    const auto mark = [&](FanfareBoundary boundary) { if (observer) { observer(boundary); } };
    // 58d4: reset allocation, restore music setting, advance demo index, branch.
    instructions(scheduler, 1 + 1 + 9 + 1 + 1 + 22 + 6 + 1);
    mark(FanfareBoundary::resource);
    read_startup_resource(scheduler, clock, {assets.score, assets.score_location}, 12);
    instructions(scheduler, 5); // Carry test, saved score pointer, image destination and CALL.
    mark(FanfareBoundary::picture);
    picture_header(scheduler, assets.picture_location);
    mark(FanfareBoundary::decoder);
    loading_work::picture(assets.picture, clock);
    mark(FanfareBoundary::picture_ready);
    instructions(scheduler, 2);
    scheduler.file_metadata(DosFileCall::close);
    instructions(scheduler, 1 + 1);
    mark(FanfareBoundary::first_page);
    draw_picture(scheduler, picture.height);
    instructions(scheduler, 1);
    flight_work::present(clock);
    mark(FanfareBoundary::presented);
    instructions(scheduler, 1);
    mark(FanfareBoundary::palette);
    palette(scheduler);
    instructions(scheduler, 1);
    mark(FanfareBoundary::second_page);
    draw_picture(scheduler, picture.height);
    // The shared color builder owns its calling CALL.
    mark(FanfareBoundary::colors);
    Palette colors;
    for (unsigned index = 0; index < colors.size(); ++index) {
        std::copy_n(picture.palette.begin() + index * 3, 3, colors[index].begin());
    }
    menu_work::colors(colors, assets.transfer, [&](CpuOperation operation, unsigned count) { scheduler.execute(operation, count); });
    instructions(scheduler, 1);
    mark(FanfareBoundary::latch_copy);
    copy_page(scheduler, picture.height);
    mark(FanfareBoundary::ready);
    instructions(scheduler, 4); // Pending scan reset, AL/EBX arguments and music CALL.
    return score.bytes;
}

void prepare_menu_picture(AudioScheduler& scheduler, ExecutionClock& clock,
                          const StartupResource& picture, unsigned filename_bytes) {
    const auto decoded = decode_payload(picture.encoded, PayloadKind::picture);
    if (decoded.width != line_stride * plane_count || !decoded.height || decoded.height > 240 ||
            decoded.palette.size() != palette_bytes) {
        throw std::invalid_argument("Unsupported menu picture geometry");
    }
    read_loading_picture(scheduler, clock, picture, filename_bytes);
    instructions(scheduler, 1);
    draw_picture(scheduler, decoded.height);
    instructions(scheduler, 1);
    flight_work::present(clock);
    instructions(scheduler, 1);
    palette(scheduler);
}

GameplayPayload read_loading_picture(AudioScheduler& scheduler, ExecutionClock& clock,
    const StartupResource& picture, unsigned filename_bytes, loading_work::InputBuffer* buffer) {
    const auto decoded = decode_payload(picture.encoded, PayloadKind::picture);
    picture_header(scheduler, picture.location, filename_bytes);
    if (buffer) {
        std::copy_n(picture.encoded.begin(), picture_header_bytes, buffer->begin());
        loading_work::picture(picture.encoded, clock, *buffer);
    } else {
        loading_work::picture(picture.encoded, clock);
    }
    instructions(scheduler, 2);
    scheduler.file_metadata(DosFileCall::close);
    instructions(scheduler, 1);
    return decoded;
}

void draw_loading_picture(ExecutionClock& clock, unsigned height) { draw_picture(clock, height); }
void apply_loading_palette(ExecutionClock& clock) { palette(clock); }
void copy_loading_page(ExecutionClock& clock, unsigned height) { copy_page(clock, height); }
}
