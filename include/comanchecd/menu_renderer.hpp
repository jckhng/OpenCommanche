#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include "comanchecd/options.hpp"
#include "comanchecd/cpu_work.hpp"

namespace comanchecd {
class ExecutionClock;

constexpr std::size_t plane_bytes = 65536;
constexpr std::size_t plane_count = 4;
constexpr std::size_t line_stride = 82;
constexpr std::size_t page_displacement = 32000;
constexpr std::size_t background_bytes = 20088;
constexpr unsigned briefing_page_height = 240;
constexpr std::size_t text_line_stride = line_stride * 12;
using Palette = std::array<std::array<std::uint8_t, 3>, 256>;
using Transfer = std::array<std::uint8_t, 64>;
using ColorMap = std::array<std::uint8_t, 256>;
using Theme = std::array<std::uint8_t, 10>;
using Background = std::array<std::vector<std::uint8_t>, plane_count>;

ColorMap build_box_remap(const Palette& palette, const Transfer& transfer);
Theme build_theme(const Palette& palette, const Transfer& transfer);

class PlanarSurface {
public:
    explicit PlanarSurface(std::vector<std::uint8_t> bytes);
    const std::vector<std::uint8_t>& bytes() const;
    std::uint8_t read(std::size_t plane, std::size_t offset) const;
    void write(std::size_t offset, std::uint8_t mask, std::uint8_t color);
    void restore(std::size_t page, const Background& background);

private:
    std::vector<std::uint8_t> bytes_;
};

class Font {
public:
    explicit Font(std::vector<std::uint8_t> embedded);
    std::size_t width(std::uint8_t code) const;
    std::uint32_t column(std::uint8_t code, std::size_t index) const;

private:
    std::vector<std::uint8_t> embedded_;
    std::size_t glyph_offset(std::uint8_t code) const;
};

struct TextCursor {
    std::size_t offset;
    std::uint8_t plane;
    std::uint8_t foreground;
    std::size_t consumed = 0;
    std::uint8_t marker = 0;
};

enum class DrawStage { background, box, highlight, text_row, complete };
enum class ListLayout { standard, main };
enum class DebriefBackground { restore, capture };
using DrawObserver = std::function<void(DrawStage, std::size_t, const PlanarSurface&)>;
using MenuInputReset = std::function<void()>;

class MenuRenderer {
public:
    MenuRenderer(PlanarSurface surface, Font font, Theme theme, ColorMap remap, Background background,
                 std::size_t back_page, std::size_t pan, ExecutionClock* clock = nullptr,
                 MenuInputReset reset_input = {});
    const PlanarSurface& surface() const;
    std::size_t back_page() const;
    void draw_list(const std::vector<std::uint8_t>& stream, std::uint8_t selection,
                   ListLayout layout, const DrawObserver& observer = {});
    void present();
    void restore_background();
    void begin_list();
    void prepare_briefing();
    void draw_name_entry(const std::vector<std::uint8_t>& stream);
    void draw_briefing(const std::vector<std::uint8_t>& description, const std::vector<std::uint8_t>& choices,
                       std::uint8_t selection);
    void draw_loading(const std::vector<std::uint8_t>& prompt);
    void draw_debrief(const std::vector<std::uint8_t>& stream, DebriefBackground background = DebriefBackground::restore);
    void draw_statistics(const std::vector<std::uint8_t>& stream);
    void draw_locked_mission(const std::vector<std::uint8_t>& stream);
    void draw_settings(const SettingsAssets& assets, const MenuSettings& settings, const SettingsMenu& menu,
                       SettingsNotice notice = SettingsNotice::none);
    TextCursor text(const std::vector<std::uint8_t>& stream, TextCursor cursor);

private:
    void save_background();
    void copy_front_page();
    void box(std::size_t origin, std::uint8_t width, std::uint8_t height);
    void highlight(std::size_t origin, std::size_t interior_bytes, std::size_t height);
    void text_rows(const std::vector<std::uint8_t>& stream, std::size_t origin, std::uint8_t foreground);
    PlanarSurface surface_;
    Font font_;
    Theme theme_;
    ColorMap remap_;
    Background background_;
    std::size_t back_page_;
    std::size_t pan_;
    ExecutionClock* clock_;
    CpuWorkSink work_;
    MenuInputReset reset_input_;
};

} // namespace comanchecd
