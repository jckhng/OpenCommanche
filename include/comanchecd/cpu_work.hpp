#pragma once

#include <cstdint>
#include <functional>

namespace comanchecd {
enum class CpuOperation {
    instructions, string_bytes, repeat_store_bytes, repeat_store_dwords, repeat_copy_bytes, repeat_copy_dwords, vga_store_words,
    port_read_byte, port_write_byte, port_write_word,
    disable_interrupts, enable_interrupts, stack_segment, callback_return, fm_status_read, repeat_scan_bytes, repeat_copy_words,
    interrupt_return
};
using CpuWorkSink = std::function<void(CpuOperation, unsigned)>;

struct CpuAccounting {
    std::uint64_t dispatches = 0;
    std::uint64_t string_iterations = 0;
    std::uint64_t repeat_restarts = 0;
    std::uint64_t special_stosb_iterations = 0;
    std::uint64_t terminal_charges = 0;
    std::uint64_t io_cycles = 0;
    std::uint64_t idle_calls = 0, idle_cycles = 0, stack_segment_credits = 0;
};
}
