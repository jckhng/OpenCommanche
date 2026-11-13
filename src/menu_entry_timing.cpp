#include "menu_entry_timing.hpp"
#include "fanfare_timing.hpp"
#include <stdexcept>

namespace comanchecd {
void prepare_pilot_menu(AudioScheduler& scheduler, ExecutionClock& clock, const PilotMenuAssets& assets,
    bool digital_active, const StartupDigital& digital, const std::function<unsigned()>& reset_voices,
    const std::function<void()>& music, const std::function<void(MenuEntryBoundary)>& observer) {
    constexpr unsigned score_filename_bytes = 13, picture_filename_bytes = 10;
    constexpr unsigned maximum_read_bytes = 0xfa00, entity_slots = 140;
    for (const auto& score : assets.scores) {
        if (score.encoded.size() < 50 || score.encoded.size() >= maximum_read_bytes ||
                score.encoded[0] != 'M' || score.encoded[1] != 'S') {
            throw std::invalid_argument("Invalid pilot-menu score");
        }
    }
    decode_payload(assets.picture.encoded, PayloadKind::picture);
    const auto instructions = [&](unsigned count) { scheduler.execute(CpuOperation::instructions, count); };
    const auto mark = [&](MenuEntryBoundary boundary) { if (observer) { observer(boundary); } };
    mark(MenuEntryBoundary::exit);
    instructions(24); // 5f86 key reset through the digital-selector arguments.
    mark(MenuEntryBoundary::digital);
    instructions(2);
    disable_menu_samples(scheduler, digital_active, digital, reset_voices);
    instructions(1);
    mark(MenuEntryBoundary::reset);
    instructions(4 + 6);
    scheduler.execute(CpuOperation::repeat_store_bytes, entity_slots);
    instructions(1 + 3);
    scheduler.execute(CpuOperation::disable_interrupts, 1);
    instructions(12 + 3 * 3);
    if (reset_voices) { instructions(6 * reset_voices()); }
    instructions(2);
    scheduler.execute(CpuOperation::enable_interrupts, 1);
    instructions(1 + 10 + 1); // Clear RET, allocator reset and menu CALL.
    mark(MenuEntryBoundary::entry);
    instructions(2);
    const auto read = [&](const StartupResource& resource) {
        instructions(12 + score_filename_bytes * 12);
        scheduler.file_metadata(DosFileCall::find);
        instructions(resource.location == StartupLocation::local ? 3 : 5);
        scheduler.file_metadata(DosFileCall::open_read);
        instructions(7);
        scheduler.read_file(static_cast<unsigned>(resource.encoded.size()));
        instructions(12);
        scheduler.file_metadata(DosFileCall::close);
        instructions(2);
    };
    instructions(1);
    mark(MenuEntryBoundary::first_score);
    read(assets.scores[0]);
    instructions(1);
    mark(MenuEntryBoundary::second_score);
    read(assets.scores[1]);
    instructions(3);
    music();
    instructions(3 + 9); // AL=3 queues the second score without another key-off.
    instructions(2 + 1);
    mark(MenuEntryBoundary::picture);
    prepare_menu_picture(scheduler, clock, assets.picture, picture_filename_bytes);
    mark(MenuEntryBoundary::colors); // Frontend construction owns the shared color builder.
}
}
