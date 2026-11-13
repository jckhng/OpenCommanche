#include "movie_timing.hpp"
#include "startup_resource.hpp"
#include <stdexcept>

namespace comanchecd {
void prepare_startup_movie(AudioScheduler& scheduler, ExecutionClock& clock,
    const StartupFiles& files, const StartupMovieAssets& assets,
    const std::function<void(MusicLevel)>& music, const StartupDigital& digital,
    const std::function<void()>& reset_counter,
    const std::function<void(MovieLoadingBoundary)>& observer) {
    constexpr unsigned movie_filename_bytes = 10, cue_filename_bytes = 10;
    constexpr unsigned score_filename_bytes = 13, sound_filename_bytes = 9;
    constexpr unsigned score_copy_dwords = 0xdac, maximum_read_bytes = 0xfa00;
    const auto score = decode_payload(assets.score.encoded, PayloadKind::resource);
    decode_payload(assets.sound.encoded, PayloadKind::resource);
    decode_payload(assets.movie.encoded, PayloadKind::resource);
    if (score.bytes.size() < 50 || assets.cues.encoded.size() >= maximum_read_bytes) {
        throw std::invalid_argument("Unsupported startup movie resource lengths");
    }
    const auto instructions = [&](unsigned count) { scheduler.execute(CpuOperation::instructions, count); };
    const auto mark = [&](MovieLoadingBoundary boundary) { if (observer) { observer(boundary); } };
    mark(MovieLoadingBoundary::settings);
    instructions(1); // 5b31 CALL.
    apply_startup_settings(scheduler, files, SettingsPass::post_start, music, digital);
    mark(MovieLoadingBoundary::find);
    instructions(1 + 9 + movie_filename_bytes * 12);
    if (files.cdrom && !files.cdrom->empty() && files.cdrom->front() != '@') {
        instructions(2);
        scheduler.file_metadata(DosFileCall::find);
        instructions(1);
        if (assets.movie.location == StartupLocation::local) {
            instructions(3);
            scheduler.file_metadata(DosFileCall::find);
            instructions(1);
        }
    } else {
        instructions(3);
        scheduler.file_metadata(DosFileCall::find);
        instructions(1);
    }
    instructions(3 + 1); // Found result, RET and carry test.
    mark(MovieLoadingBoundary::counter);
    scheduler.instruction(reset_counter); // 5b4b timer reset precedes all movie reads.
    instructions(2 + 12 + cue_filename_bytes * 12);
    scheduler.file_metadata(DosFileCall::find);
    instructions(assets.cues.location == StartupLocation::local ? 3 : 6);
    scheduler.file_metadata(DosFileCall::open_read);
    instructions(7);
    scheduler.read_file(static_cast<unsigned>(assets.cues.encoded.size()));
    instructions(12);
    scheduler.file_metadata(DosFileCall::close);
    instructions(2);
    mark(MovieLoadingBoundary::cues);
    instructions(1 + 7 + 1); // Allocation checkpoint and resource CALL.
    read_startup_resource(scheduler, clock, assets.score, score_filename_bytes);
    mark(MovieLoadingBoundary::score);
    instructions(4);
    scheduler.execute(CpuOperation::repeat_copy_dwords, score_copy_dwords);
    instructions(1);
    read_startup_resource(scheduler, clock, assets.sound, sound_filename_bytes);
    instructions(1); // Carry test.
    mark(MovieLoadingBoundary::sound);
    instructions(1 + 1 + 7 + 1); // Save bank, allocator checkpoint, movie CALL.
    read_startup_resource(scheduler, clock, assets.movie, movie_filename_bytes);
    instructions(1); // Carry test.
    mark(MovieLoadingBoundary::movie);
    instructions(1); // Save movie; caller continues at the timer wait.
}
}
