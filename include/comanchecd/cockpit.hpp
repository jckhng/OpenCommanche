#pragma once

#include "comanchecd/flight.hpp"
#include <functional>
#include <memory>

namespace comanchecd {
constexpr std::size_t cockpit_width = 320;
constexpr std::size_t cockpit_height = 240;
constexpr std::size_t cockpit_backing_width = line_stride * plane_count;

struct CockpitImage {
    std::array<std::uint8_t, cockpit_width * cockpit_height> pixels{};
    Palette palette{};
};

enum class InstrumentDisplay : std::uint8_t { map, radar, camera, objectives, systems, disabled };

struct CockpitControls {
    InstrumentDisplay left = InstrumentDisplay::map;
    InstrumentDisplay right = InstrumentDisplay::camera;
};

struct InstrumentCamera {
    TerrainFrame frame;
    SceneView view;
    std::unique_ptr<InstrumentCamera> second;
};

enum class InstrumentPanel { left, right };
enum class MapZoom { in, hold, out };
// Providers own monitor rendering, model and foreground callback work.
using InstrumentCameraProvider = std::function<const InstrumentCamera*(InstrumentPanel)>;

struct CockpitTables {
    std::array<std::uint8_t, 6> panel_y{}, left_panel_x{}, right_panel_x{};
    std::array<std::uint8_t, 6> weapon_flash{0, 0, 0, 0, 1, 1};
    std::array<std::uint32_t, 4> shake_displacement{};
    std::uint32_t map_scale = 0;
    std::uint8_t chaff = 0, flares = 0, category_count = 0;
    bool center_marker = true, heading_tape = true, flight_hud = true, missile_camera = true;
};

struct CockpitAssets {
    Palette palette{};
    Theme theme{};
    std::vector<std::uint8_t> normal_picture, crash_picture, console, hud;
};

CockpitAssets materialize_cockpit(const GameplayPayload& normal, const GameplayPayload& crash,
                                const SceneAssets& scene, const Transfer& transfer);

struct CockpitEntry {
    std::vector<std::uint8_t> planes;
    std::uint32_t back_page = 0;
    std::uint8_t previous_height = 0, vertical_rate = 0, flash_frame = 0, nearby = 0;
    std::uint8_t mode = 1;
    std::int32_t zoom_step = 0;
    CockpitControls controls;
    CockpitControls previous_controls{InstrumentDisplay::map, InstrumentDisplay::map};
};

class CockpitRenderer {
public:
    CockpitRenderer(CockpitAssets assets, CockpitTables tables, const FlightState& player,
                    CockpitEntry entry = {});
    const PlanarSurface& surface() const;
    const CockpitEntry& state() const;
    void prepare_frame(const CpuWorkSink& work = {});
    void begin_frame(const TerrainFrame& frame, std::uint8_t mode, const CockpitControls& controls,
                     const CpuWorkSink& work = {});
    std::vector<SoundCue> draw_instruments(const TerrainFrame& frame, const SceneView& rendered_view,
        const FlightState& player, const MissionScene& scene, const SceneAssets& assets, std::uint32_t tick,
        const CockpitControls& controls, const InstrumentCamera* camera = nullptr, const CpuWorkSink& work = {},
        const InstrumentCameraProvider& camera_provider = {}, const SoundCueHandler& sound_handler = {});
    CockpitImage present(const FlightState& player);
    void copy_front_page();
    void apply_settings(const MenuSettings& settings);
    void refresh_stores(const FlightState& player, const CpuWorkSink& work = {});
    void refresh_countermeasures(const FlightState& player, const CpuWorkSink& work = {});
    void update_pending(const FlightState& player, const CpuWorkSink& work = {});
    void select_display(InstrumentPanel panel, InstrumentDisplay display, const CpuWorkSink& work = {});
    void set_zoom(MapZoom zoom);
    void restore_console(const FlightState& player);
    void initialize_displays(const FlightState& player, const CpuWorkSink& work);

private:
    void restore_marker(const CpuWorkSink& work);
    void load_picture(std::uint8_t mode);
    void console(std::uint8_t number, std::int32_t x, std::uint8_t y, const CpuWorkSink& work = {});
    void overlay(std::uint8_t number, std::int32_t x, std::uint8_t y, const CpuWorkSink& work = {});
    void glyph(std::uint8_t number, std::int32_t x, std::uint8_t y, const TerrainFrame* warp = nullptr,
               const CpuWorkSink& work = {});
    void bar(std::int32_t x, std::uint8_t y, unsigned height, std::uint8_t color, const CpuWorkSink& work = {});
    void digits(std::uint32_t value, std::int32_t x, std::uint8_t y, unsigned count, std::uint8_t first,
                const CpuWorkSink& work = {});
    void number(std::uint32_t value, std::int32_t x, std::uint8_t y, const CpuWorkSink& work = {});
    void lamps(const CpuWorkSink& work = {});
    void stores(const FlightState& player);
    void ammunition(const FlightState& player, const CpuWorkSink& work = {});
    void countermeasures(const FlightState& player, const CpuWorkSink& work = {});
    void panel(InstrumentDisplay display, const TerrainFrame& frame, const SceneView& rendered_view,
        const FlightState& player, const MissionScene& scene, const Terrain& terrain, std::uint32_t tick,
        const InstrumentCamera*& camera, const CpuWorkSink& work, const InstrumentCameraProvider& camera_provider);
    void map(const TerrainFrame& frame, const FlightState& player, const Terrain& terrain, const CpuWorkSink& work = {});
    void camera(const TerrainFrame& frame, const CpuWorkSink& work = {});
    void heading(std::uint32_t angle, const CpuWorkSink& work = {});
    void vector_line(std::int32_t x, std::uint8_t y, const CpuWorkSink& work = {});
    void pixel(std::int32_t x, std::int32_t y, std::uint8_t color);
    void target_marker(const FlightState& player, const CpuWorkSink& work = {});

    struct MarkerBackground {
        std::size_t origin = 0, width = 0, height = 0;
        std::vector<std::uint8_t> pixels;
    };

    CockpitAssets assets_;
    CockpitTables tables_;
    CockpitEntry state_;
    PlanarSurface surface_;
    std::size_t panel_offset_ = 0;
    std::array<MarkerBackground, 2> markers_;
    bool frame_prepared_ = false;
    bool stores_pending_ = false, countermeasures_pending_ = false;
};
} // namespace comanchecd
