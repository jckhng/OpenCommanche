#include "startup_resource.hpp"
#include "loading_work.hpp"
#include <algorithm>
#include <stdexcept>

namespace comanchecd {
GameplayPayload read_startup_resource(AudioScheduler& scheduler, ExecutionClock& clock,
    const StartupResource& resource, unsigned filename_bytes, loading_work::InputBuffer* buffer) {
    constexpr unsigned file_buffer_bytes = 8192, header_bytes = 8, filename_byte_work = 12;
    const auto decoded = decode_payload(resource.encoded, PayloadKind::resource);
    if (!buffer && resource.encoded[3] == '1' && resource.encoded.size() < header_bytes + file_buffer_bytes) {
        throw std::invalid_argument("Compressed startup timing requires a complete initial input buffer");
    }
    const auto instructions = [&](unsigned count) { scheduler.execute(CpuOperation::instructions, count); };
    instructions(3 + filename_bytes * filename_byte_work + 7);
    scheduler.file_metadata(DosFileCall::find);
    instructions(resource.location == StartupLocation::local ? 3 : 6);
    scheduler.file_metadata(DosFileCall::open_read);
    instructions(6);
    scheduler.read_file(header_bytes);
    if (buffer) { std::copy_n(resource.encoded.begin(), header_bytes, buffer->begin()); }
    instructions(2);
    scheduler.execute(CpuOperation::string_bytes, 1);
    instructions(2);
    if (resource.encoded[3] == '1') {
        instructions(5 + 19 + 6); // Compressed entry and aligned high-memory allocator.
        if (buffer) { loading_work::compressed_resource(resource.encoded, clock, *buffer); }
        else { loading_work::compressed_resource(resource.encoded, clock); }
        instructions(2);
        scheduler.file_metadata(DosFileCall::close);
        instructions(1 + 3); // Decoder RET, then saved address / CLC / RET.
        return decoded;
    }
    instructions(7 + 19 + 7); // Raw entry and the same allocator.
    unsigned remaining = static_cast<unsigned>(decoded.bytes.size());
    std::size_t cursor = header_bytes;
    for (;;) {
        instructions(10);
        const auto bytes = std::min(remaining, file_buffer_bytes);
        scheduler.read_file(bytes);
        if (buffer) { std::copy_n(resource.encoded.begin() + cursor, bytes, buffer->begin()); }
        cursor += bytes;
        instructions(9 + 2);
        if (remaining < file_buffer_bytes) {
            instructions(2);
            scheduler.execute(CpuOperation::repeat_copy_bytes, remaining);
            break;
        }
        instructions(1);
        scheduler.execute(CpuOperation::repeat_copy_dwords, file_buffer_bytes / 4);
        instructions(2); // JMP and the next refill CALL, including EOF.
        remaining -= file_buffer_bytes;
    }
    instructions(2);
    scheduler.file_metadata(DosFileCall::close);
    instructions(4);
    return decoded;
}
}
