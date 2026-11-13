#pragma once

#include "comanchecd/menu_renderer.hpp"
#include "comanchecd/mission.hpp"

namespace comanchecd {
struct MissionResult;

constexpr std::size_t pilot_count = 8;
constexpr std::size_t pilot_record_bytes = 1450;
constexpr std::size_t pilot_name_bytes = 22;
constexpr std::size_t editor_buffer_bytes = 32;
using KeyTable = std::array<std::uint8_t, 128>;
using EditorBuffer = std::array<std::uint8_t, editor_buffer_bytes>;

struct PilotAssets {
    std::vector<std::uint8_t> menu_header;
    std::vector<std::uint8_t> editor_prompt;
    std::array<std::uint8_t, 9> row_prefix;
    std::array<std::uint8_t, 12> row_tail;
    std::array<std::uint8_t, 256> glyph_widths;
    KeyTable normal_keys;
    KeyTable shifted_keys;
};

class PilotStorage {
public:
    explicit PilotStorage(std::vector<std::uint8_t> record);
    const std::vector<std::uint8_t>& record() const;
    std::uint8_t selected() const;
    std::uint8_t campaign(std::uint8_t slot) const;
    std::uint8_t remembered_mission(std::uint8_t slot, std::uint8_t campaign) const;
    std::uint8_t committed_mission() const;
    bool completed(std::uint8_t slot, std::uint8_t campaign, std::uint8_t mission) const;
    void choose_campaign(std::uint8_t slot, std::uint8_t campaign);
    void choose_mission(std::uint8_t slot, std::uint8_t campaign, std::uint8_t mission);
    std::array<std::uint8_t, pilot_name_bytes> name(std::uint8_t slot) const;
    bool blank(std::uint8_t slot) const;
    void select(std::uint8_t slot);
    std::uint8_t normalize(std::uint8_t selection, const CpuWorkSink& work = {});
    std::uint8_t commit(std::uint8_t slot, const EditorBuffer& name, const PilotAssets& assets);
    void record_result(MissionResult& result, const CampaignMissions& campaign, const CpuWorkSink& work = {});
    std::vector<std::uint8_t> menu(const std::vector<std::uint8_t>& header) const;
    std::vector<std::uint8_t> statistics(const std::vector<std::uint8_t>& text) const;

private:
    std::vector<std::uint8_t> record_;
    std::uint32_t score(std::size_t slot) const;
    void swap_records(std::size_t first, std::size_t second);
};

enum class PilotMode { list, editor, accepted, exit_prompt };
enum class PilotUpdate { ignored, list, editor_open, editor, editor_close, editor_commit, accepted, exit_prompt };
enum class PilotShift { none, left, right, both };

struct PilotKey {
    std::uint8_t scan;
    PilotShift shift = PilotShift::none;
};

class PilotController {
public:
    PilotController(PilotStorage storage, PilotAssets assets, std::uint8_t selection);
    PilotUpdate consume(PilotKey key);
    void resume();
    void choose_campaign(std::uint8_t campaign);
    void choose_mission(std::uint8_t mission);
    void record_result(MissionResult& result, const CampaignMissions& campaign, const CpuWorkSink& work = {});
    PilotMode mode() const;
    std::uint8_t selection() const;
    std::uint8_t editor_length() const;
    const EditorBuffer& editor_buffer() const;
    const PilotStorage& storage() const;
    std::vector<std::uint8_t> menu_stream() const;
    std::vector<std::uint8_t> editor_stream() const;

private:
    PilotStorage storage_;
    PilotAssets assets_;
    std::uint8_t selection_;
    PilotMode mode_ = PilotMode::list;
    EditorBuffer editor_{};
    std::uint8_t length_ = 0;
    PilotUpdate open_editor();
    PilotUpdate editor_key(PilotKey key);
    void cursor();
};

using PilotWriter = std::function<void(const std::vector<std::uint8_t>&)>;
using PilotObserver = std::function<void(const PilotController&, const MenuRenderer&)>;

class PilotFrontend {
public:
    PilotFrontend(PilotController controller, MenuRenderer renderer, PilotWriter writer, PilotObserver observer = {});
    void start();
    PilotUpdate consume(PilotKey key);

private:
    PilotController controller_;
    MenuRenderer renderer_;
    PilotWriter writer_;
    PilotObserver observer_;
};

} // namespace comanchecd
