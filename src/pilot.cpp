#include "comanchecd/pilot.hpp"
#include "pilot_presentation.hpp"
#include "comanchecd/debrief.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace comanchecd {
namespace {
constexpr std::size_t row_bytes = 43;
constexpr std::size_t swapped_row_bytes = 40;
constexpr std::size_t selected_offset = 0x17f;
constexpr std::size_t campaign_offset = 0x180;
constexpr std::size_t committed_mission_offset = 0x1d8;
constexpr std::size_t remembered_missions_offset = 0x188;
constexpr std::size_t score_offset = 0x1da;
constexpr std::size_t counter_offset = 0x1fa;
constexpr std::size_t statistic_offset = 0x21a;
constexpr std::size_t mission_records_offset = 0x23a;
constexpr std::size_t secondary_flags_offset = 0x55a;
constexpr std::size_t mission_count = 10;
constexpr std::size_t mission_record_bytes = 100;
constexpr std::size_t name_offset = 9;
constexpr std::size_t editor_prompt_offset = 21;
constexpr unsigned name_pixel_limit = 108;
constexpr std::uint8_t padding = 0xfc;
constexpr std::uint8_t fixed_space = 0xfd;
constexpr std::uint8_t scan_escape = 0x01;
constexpr std::uint8_t scan_backspace = 0x0e;
constexpr std::uint8_t scan_tab = 0x0f;
constexpr std::uint8_t scan_enter = 0x1c;
constexpr std::uint8_t scan_space = 0x39;
constexpr std::uint8_t scan_up = 0x48;
constexpr std::uint8_t scan_left = 0x4b;
constexpr std::uint8_t scan_right = 0x4d;
constexpr std::uint8_t scan_down = 0x50;
constexpr std::uint8_t scan_delete = 0x53;

void check_slot(std::uint8_t slot) {
    if (slot >= pilot_count) {
        throw std::invalid_argument("Pilot slot out of range");
    }
}

void check_campaign(std::uint8_t campaign) {
    if (campaign >= mission_count) {
        throw std::invalid_argument("Campaign index out of range");
    }
}
}

PilotStorage::PilotStorage(std::vector<std::uint8_t> record) : record_(std::move(record)) {
    if (record_.size() != pilot_record_bytes) {
        throw std::invalid_argument("Expected the complete pilot record");
    }
}

const std::vector<std::uint8_t>& PilotStorage::record() const { return record_; }

std::vector<std::uint8_t> PilotStorage::statistics(const std::vector<std::uint8_t>& text) const {
    constexpr std::size_t statistics_bytes = 130, statistics_name = 8, statistics_name_bytes = 33;
    constexpr std::array<std::size_t, 3> positions{65, 77, 103}, widths{8, 3, 4};
    constexpr std::array<std::size_t, 3> fields{counter_offset, score_offset, statistic_offset};
    if (text.size() != statistics_bytes) {
        throw std::invalid_argument("Invalid original pilot statistics text");
    }
    const auto slot = selected();
    check_slot(slot);
    auto result = text;
    std::copy_n(record_.begin() + slot * row_bytes + name_offset, statistics_name_bytes,
                result.begin() + statistics_name);
    for (std::size_t field = 0; field < fields.size(); ++field) {
        std::fill_n(result.begin() + positions[field], field == 1 ? 4 : widths[field], padding);
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
            value |= std::uint32_t(record_[fields[field] + slot * 4 + byte]) << (byte * 8);
        }
        for (std::size_t digit = 0; digit < widths[field]; ++digit) {
            result[positions[field] + widths[field] - 1 - digit] = static_cast<std::uint8_t>('0' + value % 10);
            value /= 10;
            if (value == 0) {
                break;
            }
        }
    }
    return result;
}

std::uint8_t PilotStorage::selected() const { return record_[selected_offset]; }

std::uint8_t PilotStorage::campaign(std::uint8_t slot) const {
    check_slot(slot);
    return record_[campaign_offset + slot];
}

std::uint8_t PilotStorage::remembered_mission(std::uint8_t slot, std::uint8_t campaign) const {
    check_slot(slot);
    check_campaign(campaign);
    return record_[remembered_missions_offset + slot * mission_count + campaign];
}

std::uint8_t PilotStorage::committed_mission() const { return record_[committed_mission_offset]; }

bool PilotStorage::completed(std::uint8_t slot, std::uint8_t campaign, std::uint8_t mission) const {
    check_slot(slot);
    check_campaign(campaign);
    if (mission >= mission_count) {
        throw std::invalid_argument("Mission index out of range");
    }
    return record_[mission_records_offset + slot * mission_record_bytes + campaign * mission_count + mission] != 0;
}

void PilotStorage::choose_campaign(std::uint8_t slot, std::uint8_t campaign) {
    check_slot(slot);
    check_campaign(campaign);
    record_[campaign_offset + slot] = campaign;
}

void PilotStorage::choose_mission(std::uint8_t slot, std::uint8_t campaign, std::uint8_t mission) {
    check_slot(slot);
    check_campaign(campaign);
    if (mission >= mission_count) {
        throw std::invalid_argument("Mission index out of range");
    }
    record_[remembered_missions_offset + slot * mission_count + campaign] = mission;
    record_[committed_mission_offset] = mission;
}

std::array<std::uint8_t, pilot_name_bytes> PilotStorage::name(std::uint8_t slot) const {
    check_slot(slot);
    std::array<std::uint8_t, pilot_name_bytes> result{};
    std::copy_n(record_.begin() + slot * row_bytes + name_offset, result.size(), result.begin());
    return result;
}

bool PilotStorage::blank(std::uint8_t slot) const {
    check_slot(slot);
    return record_[slot * row_bytes + name_offset] == ' ';
}

void PilotStorage::select(std::uint8_t slot) {
    check_slot(slot);
    record_[selected_offset] = slot;
}

std::uint32_t PilotStorage::score(std::size_t slot) const {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        value |= std::uint32_t(record_[score_offset + slot * 4 + byte]) << (byte * 8);
    }
    return value;
}

void PilotStorage::swap_records(std::size_t first, std::size_t second) {
    const auto swap_span = [&](std::size_t offset, std::size_t stride, std::size_t count) {
        for (std::size_t byte = 0; byte < count; ++byte) {
            std::swap(record_[offset + first * stride + byte], record_[offset + second * stride + byte]);
        }
    };
    swap_span(campaign_offset, 1, 1);
    swap_span(remembered_missions_offset, mission_count, mission_count);
    swap_span(secondary_flags_offset, mission_count, mission_count);
    for (const auto offset : {score_offset, counter_offset, statistic_offset}) {
        swap_span(offset, 4, 4);
    }
    // The original shifts 43 >> 2 and swaps ten dwords, leaving three tail bytes.
    swap_span(0, row_bytes, swapped_row_bytes);
    swap_span(mission_records_offset, mission_record_bytes, mission_record_bytes);
}

std::uint8_t PilotStorage::normalize(std::uint8_t selection, const CpuWorkSink& work) {
    check_slot(selection);
    const auto instructions = [&](unsigned count) { if (work) { work(CpuOperation::instructions, count); } };
    instructions(2);
    for (std::size_t last = pilot_count - 1; last > 0; --last) {
        instructions(5);
        auto minimum = last;
        auto value = score(last);
        for (auto candidate = last; candidate-- > 0;) {
            instructions(4);
            if (score(candidate) < value) {
                instructions(2);
                value = score(candidate);
                minimum = candidate;
            }
        }
        instructions(45 + 2);
        if (selection == minimum) {
            instructions(2);
            selection = static_cast<std::uint8_t>(last);
        } else {
            instructions(2);
            if (selection == last) {
                instructions(1);
                selection = static_cast<std::uint8_t>(minimum);
            }
        }
        if (work) {
            // 2d842 swaps ten row dwords, then 25 mission-progress dwords.
            for (const auto bytes : {swapped_row_bytes, mission_record_bytes}) {
                instructions(10);
                for (std::size_t offset = 0; offset < bytes; offset += 4) {
                    instructions(2);
                    work(CpuOperation::string_bytes, 1);
                    instructions(2);
                }
            }
        }
        swap_records(minimum, last);
        instructions(1);
    }
    instructions(4 + 3);
    for (std::size_t slot = pilot_count; slot-- > 0;) {
        auto value = score(slot);
        instructions(19 + (value / 10 % 10 == 0 ? 2 : 0) + (value / 100 % 10 == 0 ? 1 : 0) +
            (value / 10 == 0 ? 1 : 0));
        record_[slot * row_bytes + 5] = static_cast<std::uint8_t>('0' + value % 10);
        value /= 10;
        const auto tens = value % 10;
        value /= 10;
        record_[slot * row_bytes + 4] = tens == 0 && value == 0 ? fixed_space : static_cast<std::uint8_t>('0' + tens);
        const auto hundreds = value % 10;
        record_[slot * row_bytes + 3] = hundreds == 0 ? fixed_space : static_cast<std::uint8_t>('0' + hundreds);
    }
    record_[selected_offset] = selection;
    instructions(3);
    return selection;
}

std::uint8_t PilotStorage::commit(std::uint8_t slot, const EditorBuffer& name, const PilotAssets& assets) {
    check_slot(slot);
    for (const auto offset : {remembered_missions_offset, secondary_flags_offset}) {
        std::fill_n(record_.begin() + offset + slot * mission_count, mission_count, 0);
    }
    for (const auto offset : {score_offset, counter_offset, statistic_offset}) {
        std::fill_n(record_.begin() + offset + slot * 4, 4, 0);
    }
    record_[campaign_offset + slot] = 0;
    auto row = record_.begin() + slot * row_bytes;
    row = std::copy(assets.row_prefix.begin(), assets.row_prefix.end(), row);
    row = std::copy_n(name.begin(), pilot_name_bytes, row);
    std::copy(assets.row_tail.begin(), assets.row_tail.end(), row);
    std::fill_n(record_.begin() + mission_records_offset + slot * mission_record_bytes, mission_record_bytes, 0);
    return normalize(slot);
}

void PilotStorage::record_result(MissionResult& result, const CampaignMissions& missions, const CpuWorkSink& work) {
    const auto slot = selected();
    check_slot(slot);
    const auto campaign_id = campaign(slot);
    check_campaign(campaign_id);
    const auto mission = committed_mission();
    const auto& definition = missions.missions.at(mission);
    const auto instructions = [&](unsigned count) { if (work) { work(CpuOperation::instructions, count); } };
    const auto progress = mission_records_offset + slot * mission_record_bytes + campaign_id * mission_count;
    const auto add_counter = [&](std::size_t offset, std::uint32_t amount) {
        offset += slot * 4;
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
            value |= std::uint32_t(record_[offset + byte]) << (byte * 8);
        }
        value += amount;
        for (unsigned byte = 0; byte < 4; ++byte) {
            record_[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        }
    };
    instructions(2 + (result.survived ? 2 : 0));
    if (result.survived && result.completed) {
        instructions(21 + 2); // 2bd78 through the alternate-campaign test.
        record_[progress + remembered_mission(slot, campaign_id)] = 1;
        add_counter(score_offset, 1);
        add_counter(counter_offset, result.score);
        if (definition.alternate_campaign != 0) {
            instructions(14);
            record_[secondary_flags_offset + slot * mission_count + campaign_id] = 1;
            for (std::size_t position = 32; position < 42; ++position) {
                instructions(3);
                auto& value = record_[slot * row_bytes + position];
                if (value != padding) { instructions(2); }
                if (value == padding || value == definition.alternate_campaign || position == 41) {
                    if (value != padding && value != definition.alternate_campaign) { instructions(1); }
                    instructions(1);
                    value = definition.alternate_campaign;
                    break;
                }
                instructions(1);
            }
        }
        instructions(8);
    } else {
        instructions(2);
        if (!result.survived) {
            instructions(9);
            if (record_[secondary_flags_offset + slot * mission_count + campaign_id] != 1) {
                instructions(12);
                for (int row = mission; row >= 0 && row > int(mission) - 2; --row) {
                    instructions(2);
                    if (missions.missions.at(row).restricted) {
                        instructions(2);
                        if (record_[progress + row] != 0) {
                            instructions(5);
                            if (work) { work(CpuOperation::repeat_copy_bytes, 47); }
                            instructions(1);
                            record_[progress + row] = 0;
                            result.message = DebriefMessage::setback;
                        }
                    }
                    instructions(3 + unsigned(row != 0));
                }
            }
        }
        instructions(6);
    }
    add_counter(statistic_offset, result.enemies_destroyed);
    normalize(slot, work);
}

void PilotController::record_result(MissionResult& result, const CampaignMissions& campaign, const CpuWorkSink& work) {
    storage_.record_result(result, campaign, work);
    selection_ = storage_.selected();
}

std::vector<std::uint8_t> PilotStorage::menu(const std::vector<std::uint8_t>& header) const {
    auto stream = header;
    stream.insert(stream.end(), record_.begin(), record_.begin() + selected_offset);
    return stream;
}

PilotController::PilotController(PilotStorage storage, PilotAssets assets, std::uint8_t selection)
    : storage_(std::move(storage)), assets_(std::move(assets)), selection_(selection) {
    check_slot(selection);
    if (assets_.editor_prompt.size() < editor_prompt_offset + editor_buffer_bytes + 1) {
        throw std::invalid_argument("Truncated name-entry prompt");
    }
    std::copy_n(assets_.editor_prompt.begin() + editor_prompt_offset, editor_.size(), editor_.begin());
}

PilotMode PilotController::mode() const { return mode_; }
std::uint8_t PilotController::selection() const { return selection_; }
std::uint8_t PilotController::editor_length() const { return length_; }
const EditorBuffer& PilotController::editor_buffer() const { return editor_; }
const PilotStorage& PilotController::storage() const { return storage_; }
std::vector<std::uint8_t> PilotController::menu_stream() const { return storage_.menu(assets_.menu_header); }

std::vector<std::uint8_t> PilotController::editor_stream() const {
    auto stream = assets_.editor_prompt;
    std::copy(editor_.begin(), editor_.end(), stream.begin() + editor_prompt_offset);
    return stream;
}

void PilotController::cursor() {
    editor_[length_] = '_';
    editor_[length_ + 1] = padding;
}

PilotUpdate PilotController::open_editor() {
    mode_ = PilotMode::editor;
    length_ = 0;
    editor_.fill(padding);
    cursor();
    return PilotUpdate::editor_open;
}

PilotUpdate PilotController::editor_key(PilotKey key) {
    if (key.scan == scan_escape || (key.scan == scan_enter && length_ == 0)) {
        mode_ = PilotMode::list;
        return PilotUpdate::editor_close;
    }
    if (key.scan == scan_enter) {
        editor_[length_] = padding;
        selection_ = storage_.commit(selection_, editor_, assets_);
        mode_ = PilotMode::list;
        return PilotUpdate::editor_commit;
    }
    if (key.scan == scan_backspace) {
        if (length_ == 0) {
            return PilotUpdate::ignored;
        }
        --length_;
        cursor();
        return PilotUpdate::editor;
    }
    if (key.scan >= assets_.normal_keys.size() || assets_.normal_keys[key.scan] == 0) {
        return PilotUpdate::ignored;
    }
    const auto character = key.shift != PilotShift::none ? assets_.shifted_keys[key.scan] : assets_.normal_keys[key.scan];
    editor_[length_] = character;
    if (length_ < pilot_name_bytes) {
        unsigned width = 0;
        for (std::size_t index = 0; index <= length_; ++index) {
            width += assets_.glyph_widths[editor_[index]];
        }
        if (width < name_pixel_limit) {
            ++length_;
        }
    }
    cursor();
    return PilotUpdate::editor;
}

PilotUpdate PilotController::consume(PilotKey key) {
    if (mode_ == PilotMode::editor) {
        return editor_key(key);
    }
    if (mode_ != PilotMode::list) {
        throw std::logic_error("Pilot selection has already returned to its caller");
    }
    const bool previous = key.scan == scan_up || key.scan == scan_left ||
                          (key.scan == scan_tab && key.shift != PilotShift::none);
    if (previous) {
        selection_ = selection_ == 0 ? pilot_count - 1 : selection_ - 1;
        return PilotUpdate::list;
    }
    if (key.scan == scan_down || key.scan == scan_right || key.scan == scan_space || key.scan == scan_tab) {
        selection_ = static_cast<std::uint8_t>((selection_ + 1) % pilot_count);
        return PilotUpdate::list;
    }
    if (key.scan == scan_delete || (key.scan == scan_enter && storage_.blank(selection_))) {
        return open_editor();
    }
    if (key.scan == scan_enter) {
        storage_.select(selection_);
        mode_ = PilotMode::accepted;
        return PilotUpdate::accepted;
    }
    if (key.scan == scan_escape) {
        mode_ = PilotMode::exit_prompt;
        return PilotUpdate::exit_prompt;
    }
    return PilotUpdate::ignored;
}

void PilotController::resume() {
    if (mode_ == PilotMode::exit_prompt) {
        mode_ = PilotMode::list;
        return;
    }
    if (mode_ != PilotMode::accepted) {
        throw std::logic_error("Only an accepted pilot selection can reopen");
    }
    selection_ = storage_.normalize(storage_.selected());
    mode_ = PilotMode::list;
}

void PilotController::choose_campaign(std::uint8_t campaign) { storage_.choose_campaign(selection_, campaign); }

void PilotController::choose_mission(std::uint8_t mission) {
    storage_.choose_mission(selection_, storage_.campaign(selection_), mission);
}

PilotFrontend::PilotFrontend(PilotController controller, MenuRenderer renderer, PilotWriter writer, PilotObserver observer)
    : controller_(std::move(controller)), renderer_(std::move(renderer)), writer_(std::move(writer)),
      observer_(std::move(observer)) {
    if (!writer_) {
        throw std::invalid_argument("Pilot persistence writer is required");
    }
}

void detail::present_pilot(PilotUpdate update, const PilotController& controller, MenuRenderer& renderer,
                           const PilotWriter& writer, const PilotObserver& observer) {
    const auto observe = [&] {
        if (observer) {
            observer(controller, renderer);
        }
    };
    const auto draw_list = [&] {
        renderer.draw_list(controller.menu_stream(), controller.selection(), ListLayout::standard);
        observe();
        renderer.present();
    };
    const auto draw_editor = [&] {
        renderer.draw_name_entry(controller.editor_stream());
        observe();
        renderer.present();
    };
    if (update == PilotUpdate::editor_commit) {
        writer(controller.storage().record());
    }
    switch (update) {
    case PilotUpdate::editor_open:
        renderer.restore_background();
        renderer.present();
        draw_editor();
        break;
    case PilotUpdate::editor:
        draw_editor();
        break;
    case PilotUpdate::editor_close:
    case PilotUpdate::editor_commit:
        renderer.restore_background();
        renderer.present();
        draw_list();
        break;
    case PilotUpdate::list:
        draw_list();
        break;
    case PilotUpdate::accepted:
    case PilotUpdate::exit_prompt:
        renderer.restore_background();
        renderer.present();
        observe();
        break;
    case PilotUpdate::ignored:
        observe();
        break;
    }
}

void PilotFrontend::start() { detail::present_pilot(PilotUpdate::list, controller_, renderer_, writer_, observer_); }

PilotUpdate PilotFrontend::consume(PilotKey key) {
    const auto update = controller_.consume(key);
    detail::present_pilot(update, controller_, renderer_, writer_, observer_);
    return update;
}

} // namespace comanchecd
