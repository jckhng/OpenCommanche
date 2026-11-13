#include "comanchecd/audio_scheduler.hpp"
#include "comanchecd/audio.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
}

int main() {
    try {
        using namespace comanchecd;
        constexpr unsigned rate = 48000, budget = 20000;
        constexpr unsigned irq_delay_ns = 838;
        const auto pit_numerator = std::uint64_t(budget) * 1000 * mission_pit_divisor;
        const auto first_pit_cycle = (pit_numerator + pit_frequency - 1) / pit_frequency;
        constexpr std::uint64_t nanoseconds_per_ms = 1000000;
        const auto ready_denominator = std::uint64_t(pit_frequency) * nanoseconds_per_ms;
        const auto ready_numerator = pit_numerator * nanoseconds_per_ms +
            std::uint64_t(irq_delay_ns) * budget * pit_frequency;
        const auto first_ready_cycle = (ready_numerator + ready_denominator - 1) / ready_denominator;
        VideoTiming sparse_video;
        sparse_video.line_ms = 1;
        sparse_video.frame_ms = 525;
        sparse_video.display_ms = 480;
        sparse_video.retrace_start_ms = 490;
        sparse_video.retrace_end_ms = 492;
        AudioScheduler rtc_boundary(rate, budget);
        rtc_boundary.set_video_timing(sparse_video);
        constexpr auto rtc_second_cycles = std::uint64_t(budget) * 1000;
        rtc_boundary.run_cycles(rtc_second_cycles - 10);
        rtc_boundary.read_retrace();
        rtc_boundary.run_cycles(2 * rtc_second_cycles - 30 - rtc_boundary.cycles());
        const auto rtc_charges = rtc_boundary.accounting().terminal_charges;
        rtc_boundary.read_retrace();
        rtc_boundary.execute(CpuOperation::instructions, 2);
        rtc_boundary.read_retrace();
        require(rtc_boundary.cycles() == 2 * rtc_second_cycles + 16 &&
                rtc_boundary.accounting().terminal_charges - rtc_charges == 2,
                "A CMOS update must end its own CPU slice before the following millisecond boundary");
        AudioScheduler masked_keyboard(rate);
        masked_keyboard.set_video_timing(sparse_video);
        masked_keyboard.set_interrupt_delay(irq_delay_ns);
        masked_keyboard.enable_interrupts(false);
        masked_keyboard.queue_key_byte(0x0f);
        masked_keyboard.execute(CpuOperation::instructions, 6200);
        require(masked_keyboard.accounting().terminal_charges == 3 && masked_keyboard.cycles() == 6203,
                "Masked keyboard transfer must retain the delayed PIC arbitration slice before IRQ delivery");
        AudioScheduler metadata_return(rate);
        metadata_return.set_video_timing(sparse_video);
        metadata_return.set_interrupt_delay(irq_delay_ns);
        std::uint64_t last_dispatch = 0, return_irq_cycle = 0, return_dispatch_cycle = 0, return_credits = 0;
        metadata_return.set_cpu_observer([&](CpuOperation, std::uint64_t cycle) { last_dispatch = cycle; });
        metadata_return.set_pit(16, [&] {
            return_irq_cycle = metadata_return.cycles();
            return_dispatch_cycle = last_dispatch;
            return_credits = metadata_return.accounting().stack_segment_credits;
        });
        metadata_return.run_cycles(240);
        metadata_return.file_metadata(DosFileCall::find);
        require(return_irq_cycle != 0 && return_credits == 3 && return_irq_cycle == return_dispatch_cycle,
                "A DOS return restoring IF with PIC arbitration pending must yield before a terminal charge");
        AudioScheduler halted_dma(rate);
        halted_dma.configure(AudioChannel::digital, {true, 11111, {audio_gain_one, audio_gain_one}, 0});
        halted_dma.run_cycles(budget / 2);
        halted_dma.flush();
        halted_dma.arm_dma(std::vector<std::uint8_t>(2048, 128));
        halted_dma.run_cycles(budget / 2);
        halted_dma.halt_dma();
        const auto consumed = halted_dma.dma_consumed();
        const auto remaining = halted_dma.dma_remaining();
        halted_dma.run_cycles(2 * budget);
        require(consumed > 0 && remaining > 0 && halted_dma.dma_consumed() == consumed &&
                halted_dma.dma_remaining() == remaining,
                "DMA halt must preserve its cursor while the SB16 mixer remains enabled");
        halted_dma.arm_dma(std::vector<std::uint8_t>(2048, 128));
        halted_dma.run_cycles(budget);
        require(halted_dma.dma_consumed() > consumed,
                "A newly armed transfer must resume DMA without resetting the mixer channel");
        AudioScheduler keyboard_bridge(rate);
        unsigned bridge_entries = 0;
        bool handler_returned = false, bridge_return_seen = false;
        CpuOperation bridge_return_operation = CpuOperation::instructions;
        keyboard_bridge.set_cpu_observer([&](CpuOperation operation, std::uint64_t) {
            if (handler_returned && !bridge_return_seen) {
                bridge_return_operation = operation;
                bridge_return_seen = true;
            }
        });
        std::uint64_t bridge_entry_credits = 0;
        keyboard_bridge.set_keyboard_handler([&] {
            ++bridge_entries;
            bridge_entry_credits = keyboard_bridge.accounting().stack_segment_credits;
            keyboard_bridge.read_keyboard();
            keyboard_bridge.acknowledge_keyboard();
            handler_returned = true;
        });
        keyboard_bridge.queue_key_byte(1);
        keyboard_bridge.read_file(50000);
        require(bridge_entries == 1 && bridge_entry_credits == 3 &&
                keyboard_bridge.accounting().stack_segment_credits == 6,
                "A keyboard IRQ during a DOS read must retain its extender bridge and three SS credits");
        require(bridge_return_seen && bridge_return_operation == CpuOperation::disable_interrupts,
                "The real-mode keyboard return bridge must execute CLI before switching modes");
        AudioScheduler dsp_event(rate);
        dsp_event.set_video_timing(sparse_video);
        dsp_event.write_dsp_timed(0x14);
        require(dsp_event.cycles() == 17 && dsp_event.accounting().terminal_charges == 1,
                "A newly queued DSP completion before the current CPU slice must return its unused budget immediately");
        AudioScheduler dsp_reset_event(rate);
        dsp_reset_event.set_video_timing(sparse_video);
        dsp_reset_event.write_dsp_reset(1);
        dsp_reset_event.write_dsp_reset(0);
        require(dsp_reset_event.cycles() == 33 && dsp_reset_event.accounting().terminal_charges == 1,
                "DSP reset completion must return the current CPU budget when it becomes the first queued event");
        AudioScheduler video_mode(rate);
        video_mode.set_video_timing(sparse_video);
        video_mode.initialize_video();
        require(video_mode.cycles() == 38 && video_mode.accounting().dispatches == 41 &&
                video_mode.accounting().stack_segment_credits == 3,
                "The BIOS video bridge must preserve its option-ROM jump, callback and stack-segment credits");
        constexpr unsigned retrace_probe_cycle = 16000;
        video_mode.run_cycles(retrace_probe_cycle - video_mode.cycles());
        require(!video_mode.read_retrace(), "BIOS mode setup must not start graphics retrace immediately");
        video_mode.run_cycles(320000 - video_mode.cycles());
        require(!video_mode.read_retrace(), "Graphics retrace must retain the deferred mode event origin");
        video_mode.run_cycles(332000 - video_mode.cycles());
        require(video_mode.read_retrace(), "The deferred BIOS mode event must replace inherited VGA deadlines");
        AudioScheduler masked_pit(rate);
        masked_pit.set_video_timing(sparse_video);
        masked_pit.set_interrupt_delay(irq_delay_ns);
        masked_pit.enable_interrupts(false);
        unsigned masked_entries = 0;
        masked_pit.set_pit(mission_pit_divisor, [&] { ++masked_entries; });
        masked_pit.run_cycles(first_pit_cycle);
        masked_pit.execute(CpuOperation::instructions, 20);
        require(masked_pit.cycles() == first_pit_cycle + 21 &&
                masked_pit.accounting().terminal_charges == 1 && masked_entries == 0,
                "PIT arbitration must end its CPU slice while IF is clear without delivering IRQ0");
        masked_pit.execute(CpuOperation::enable_interrupts, 1);
        require(masked_entries == 0, "Masked PIT arbitration must retain STI's following-instruction shadow");
        masked_pit.execute(CpuOperation::instructions, 1);
        require(masked_entries == 1, "The arbitrated PIT must deliver after STI's following instruction");
        AudioScheduler direct_pit(rate);
        direct_pit.set_video_timing(sparse_video);
        direct_pit.set_interrupt_delay(irq_delay_ns);
        std::uint64_t direct_entry = 0;
        direct_pit.set_pit(mission_pit_divisor, [&] { direct_entry = direct_pit.cycles(); });
        direct_pit.run_cycles(first_pit_cycle);
        direct_pit.execute(CpuOperation::instructions, 20);
        require(direct_entry == first_ready_cycle,
                "Direct IRQ0 delivery must use the fractional PIC event boundary before its first instruction");
        AudioScheduler overshot_pit(rate);
        overshot_pit.set_video_timing(sparse_video);
        overshot_pit.set_interrupt_delay(irq_delay_ns);
        bool overshot_checked = false;
        overshot_pit.set_pit(mission_pit_divisor, [&] {
            const auto entry = overshot_pit.cycles();
            overshot_pit.execute(CpuOperation::instructions, 1);
            overshot_pit.execute(CpuOperation::port_read_byte, 1);
            require(overshot_pit.cycles() == entry + 23,
                    "An I/O overshoot must service due arbitration before scheduling the accepted IRQ's follow-up check");
            overshot_checked = true;
        });
        overshot_pit.run_cycles(first_pit_cycle - 1);
        overshot_pit.execute(CpuOperation::port_read_byte, 1);
        require(overshot_checked, "The I/O overshoot regression must deliver the pending PIT");
        AudioScheduler metadata(rate);
        metadata.file_metadata(DosFileCall::find);
        require(metadata.accounting().dispatches == 49 && metadata.accounting().stack_segment_credits == 3 &&
                metadata.accounting().idle_calls == 0 && metadata.cycles() == 46,
                "Find-first must cross the DOS bridge without introducing a disk wait");
        metadata.file_metadata(DosFileCall::open_read);
        const auto open_idle_calls = metadata.accounting().idle_calls;
        metadata.file_metadata(DosFileCall::close);
        require(open_idle_calls > 0 && metadata.accounting().idle_calls > open_idle_calls,
                "Open and close must preserve their separate disk waits");
        AudioScheduler reset(rate);
        reset.enable_interrupts(false);
        reset.write_dsp_reset(1);
        reset.write_dsp_reset(0);
        require(!reset.read_dsp_ready(), "DSP reset release must wait before publishing its acknowledgement");
        reset.run_cycles(budget / 50);
        require(reset.read_dsp_ready() && reset.read_dsp_data() == 0xaa && !reset.read_dsp_ready(),
                "DSP reset must publish one acknowledgement independently of IF");
        AudioScheduler opl(rate);
        opl.write_fm(2, 0xff);
        opl.write_fm(4, 1);
        require(opl.read_fm_status() == 0 && opl.cycles() == 30 && opl.accounting().io_cycles == 29,
                "OPL status must include its device delay before the first timer overflow");
        opl.execute(CpuOperation::instructions, 1600);
        require(opl.read_fm_status() == 0xc0, "OPL timer 1 must latch overflow and the summary flag");
        opl.write_fm(4, 0x80);
        require(opl.read_fm_status() == 0, "OPL status reset must clear overflow without restarting its period");
        opl.write_fm(4, 0x41);
        opl.execute(CpuOperation::instructions, 1600);
        require(opl.read_fm_status() == 0, "A masked OPL timer must not latch an overflow flag");
        AudioScheduler opl_second(rate);
        opl_second.write_fm(3, 0xff);
        opl_second.write_fm(4, 2);
        opl_second.execute(CpuOperation::instructions, 6400);
        require(opl_second.read_fm_status() == 0xa0, "OPL timer 2 must use its independent 320-us clock");
        AudioScheduler opl_boundary(rate);
        opl_boundary.run_cycles(budget - 10);
        require(opl_boundary.read_fm_status() == 0 && opl_boundary.cycles() == budget + 1 &&
                opl_boundary.accounting().io_cycles == 9 && opl_boundary.accounting().terminal_charges == 1,
                "The OPL handler must clamp its delay at the remaining normal-core budget");
        AudioScheduler continuous(rate), fragmented(rate);
        AudioChannelConfig digital{true, Sb16Playback::source_rate, {audio_gain_one, audio_gain_one}, 27600};
        for (auto* scheduler : {&continuous, &fragmented}) {
            scheduler->configure(AudioChannel::digital, digital);
            scheduler->configure(AudioChannel::fm, {true, rate, {12288, 12288}, 0});
            scheduler->arm_dma(std::vector<std::uint8_t>(2048, 160));
        }
        unsigned completions = 0;
        continuous.set_dma_handler([&] {
            ++completions;
            continuous.arm_dma(std::vector<std::uint8_t>(2048, 192));
        });
        fragmented.set_dma_handler([&] { fragmented.arm_dma(std::vector<std::uint8_t>(2048, 192)); });
        continuous.run_cycles(budget * 500);
        for (unsigned i = 0; i < 10000; ++i) { fragmented.run_cycles(1000); }
        require(continuous.take_output() == fragmented.take_output(), "Host work partition must preserve scheduled output");
        require(completions == 2 && continuous.dma_consumed() == fragmented.dma_consumed(), "DMA must complete and rearm autonomously");

        AudioScheduler masked_dma(rate);
        masked_dma.configure(AudioChannel::digital, digital);
        unsigned dma_interrupts = 0;
        masked_dma.set_dma_handler([&] { ++dma_interrupts; });
        masked_dma.enable_interrupts(false);
        masked_dma.arm_dma({128, 129, 130});
        masked_dma.run_cycles(budget);
        require(masked_dma.dma_remaining() == 0 && dma_interrupts == 0, "DMA consumption must proceed while CPU interrupts are masked");
        masked_dma.enable_interrupts(true);
        require(dma_interrupts == 1, "DMA completion must remain pending until CPU interrupt delivery");

        AudioScheduler clock(rate);
        enum class ReloadEdge { none, rising };
        const auto inherited_reload = [&](std::uint64_t at, ReloadEdge edge) {
            AudioScheduler inherited(rate);
            inherited.set_interrupt_delay(838);
            inherited.run_cycles(at);
            std::vector<std::uint64_t> entries;
            unsigned writes = 0;
            std::uint64_t high_byte_dispatch = 0;
            inherited.set_cpu_observer([&](CpuOperation operation, std::uint64_t cycle) {
                if (operation == CpuOperation::port_write_byte && ++writes == 3) { high_byte_dispatch = cycle; }
            });
            inherited.program_pit(mission_pit_divisor, [&] { entries.push_back(inherited.cycles()); },
                                  PitStart::inherited_square_wave);
            inherited.execute(CpuOperation::instructions, 1000);
            require(entries.size() == (edge == ReloadEdge::rising ? 1u : 0u),
                    "PIT0 reload must retain the inherited square-wave output level");
            if (edge == ReloadEdge::rising) {
                constexpr unsigned byte_write_delay_ns = 1020;
                constexpr auto write_delay = std::uint64_t(budget) * byte_write_delay_ns * 3 / (4 * nanoseconds_per_ms);
                constexpr auto irq_delay = (std::uint64_t(irq_delay_ns) * budget + nanoseconds_per_ms - 1) / nanoseconds_per_ms;
                require(entries.front() == high_byte_dispatch + write_delay + irq_delay,
                        "The PIT0 edge must follow the high-byte handler and exact PIC delivery delay");
            }
        };
        const auto inherited_period = static_cast<long double>(bios_pit_divisor) * budget * 1000 / pit_frequency;
        inherited_reload(0, ReloadEdge::none);
        inherited_reload(static_cast<std::uint64_t>(inherited_period / 2) - 17, ReloadEdge::none);
        inherited_reload(static_cast<std::uint64_t>(inherited_period / 2) - 1, ReloadEdge::rising);
        inherited_reload(static_cast<std::uint64_t>(inherited_period) - 17, ReloadEdge::rising);
        inherited_reload(static_cast<std::uint64_t>(inherited_period) - 1, ReloadEdge::none);
        inherited_reload(static_cast<std::uint64_t>(inherited_period) + 1, ReloadEdge::none);
        AudioScheduler reload_edge(rate);
        std::vector<std::uint64_t> reload_irqs;
        reload_edge.set_interrupt_delay(838);
        reload_edge.set_pit(mission_pit_divisor, [&] { reload_irqs.push_back(reload_edge.cycles()); }, PitStart::rising_edge);
        reload_edge.execute(CpuOperation::instructions, 1000);
        require(reload_irqs == std::vector<std::uint64_t>{17},
                "A reload edge must dispatch before the first IRQ instruction debits cycle 18");
        std::vector<std::uint64_t> irq_times;
        clock.set_pit(mission_pit_divisor, [&] { irq_times.push_back(clock.cycles()); });
        clock.run_cycles(budget * 1000);
        require(irq_times.size() == pit_frequency / mission_pit_divisor, "PIT cadence must derive from its divisor");
        for (std::size_t i = 0; i < irq_times.size(); ++i) {
            const auto numerator = std::uint64_t(i + 1) * budget * 1000 * mission_pit_divisor;
            require(irq_times[i] == (numerator + pit_frequency - 1) / pit_frequency, "Fractional PIT deadlines must retain carry");
        }
        clock.enable_interrupts(false);
        const auto delivered = irq_times.size();
        clock.run_cycles(budget * 100);
        require(irq_times.size() == delivered, "Masked timer requests must wait");
        clock.enable_interrupts(true);
        require(irq_times.size() == delivered + 1, "Pending timer requests must coalesce until enabled");

        AudioScheduler interrupted(rate);
        unsigned calls = 0;
        interrupted.set_pit(mission_pit_divisor, [&] { ++calls; interrupted.run_cycles(100); });
        interrupted.run_cycles(1000000);
        require(interrupted.cycles() == 1000000 + calls * 100, "IRQ execution must consume time in addition to foreground work");

        AudioScheduler delayed(rate);
        std::vector<std::uint64_t> delivered_at;
        delayed.set_pit(mission_pit_divisor, [&] {
            delivered_at.push_back(delayed.cycles());
            if (delivered_at.size() == 1) { delayed.run_cycles(400000); }
        });
        delayed.run_cycles(307684);
        require(delivered_at.size() == 2 && delivered_at[1] == delivered_at[0] + 400000,
                "A timer request arriving inside an IRQ must be delivered at interrupt return");

        AudioScheduler arbitration(rate);
        arbitration.set_interrupt_delay(838);
        std::vector<std::uint64_t> arbitration_edges;
        arbitration.set_pit(mission_pit_divisor, [&] { arbitration_edges.push_back(arbitration.cycles()); });
        arbitration.run_cycles(budget * 1000);
        require(arbitration_edges.size() == pit_frequency / mission_pit_divisor,
                "PIC arbitration must preserve the delivered timer count");
        for (std::size_t i = 0; i < arbitration_edges.size(); ++i) {
            const auto period_numerator = std::uint64_t(i + 1) * budget * 1000 * mission_pit_divisor;
            const auto numerator = period_numerator % pit_frequency * 1000000 +
                std::uint64_t(838) * budget * pit_frequency;
            const auto denominator = std::uint64_t(pit_frequency) * 1000000;
            require(arbitration_edges[i] == period_numerator / pit_frequency + (numerator + denominator - 1) / denominator,
                    "PIC delay must combine with the fractional PIT edge before cycle rounding");
        }
        AudioScheduler fractional(44100);
        fractional.run_cycles(budget * 1000);
        require(fractional.take_output().size() == 44100, "Millisecond sample counts must retain fractional rate carry");
        AudioScheduler instructions(rate);
        instructions.run_cycles(budget - 1);
        instructions.execute(CpuOperation::vga_store_words, 1);
        require(instructions.cycles() == budget + 2 && instructions.accounting().terminal_charges == 1,
                "A scalar string store must finish its instruction and charge the failed core condition before servicing the timer");
        require(instructions.take_output().size() == 48,
                "The overshot millisecond must still emit exactly its scheduled samples");
        AudioScheduler repeat(rate);
        repeat.execute(CpuOperation::repeat_store_bytes, 9);
        const auto rep = repeat.accounting();
        require(rep.dispatches == 2 && rep.repeat_restarts == 1 && rep.string_iterations == 8 &&
                rep.special_stosb_iterations == 1 && repeat.cycles() == 11,
                "REP STOSB must execute its capped final five-byte packet and preserve the observer counter distinction");
        AudioScheduler copy(rate);
        copy.execute(CpuOperation::repeat_copy_bytes, 9);
        require(copy.accounting().dispatches == 3 && copy.accounting().string_iterations == 9 &&
                copy.accounting().special_stosb_iterations == 0 && copy.cycles() == 12,
                "REP MOVSB must retain ordinary packet limits when five bytes remain");
        AudioScheduler scan(rate);
        AudioScheduler words(rate);
        words.execute(CpuOperation::repeat_copy_words, 9);
        require(words.accounting().dispatches == 3 && words.accounting().string_iterations == 9 &&
                words.accounting().repeat_restarts == 2 && words.cycles() == 12,
                "REP MOVSW must retain word iterations and normal-core restart packets");
        words.execute(CpuOperation::repeat_copy_words, 0);
        require(words.cycles() == 13 && words.accounting().string_iterations == 9,
                "An empty word copy must still dispatch its instruction");
        scan.execute(CpuOperation::repeat_scan_bytes, 9);
        require(scan.accounting().dispatches == 3 && scan.accounting().string_iterations == 9 &&
                scan.accounting().repeat_restarts == 2 && scan.cycles() == 12,
                "A bounded REPNE SCASB scan must retain its comparison count and capped restart packets");
        AudioScheduler opl_writer(rate);
        opl_writer.write_fm_timed(1, 0x20, 239);
        const auto opl_work = opl_writer.accounting();
        require(opl_work.dispatches == 12 * 239 + 27 && opl_work.io_cycles == 30,
                "The OPL writer must account for its CALL, calibrated instruction loops, two port writes and RET");
        AudioScheduler dsp(rate);
        unsigned accepted_bytes = 0;
        dsp.write_dsp_timed(0x14, [&] { ++accepted_bytes; });
        dsp.write_dsp_timed(0, [&] { ++accepted_bytes; });
        dsp.execute(CpuOperation::instructions, 4);
        dsp.write_dsp_timed(0, [&] { ++accepted_bytes; });
        require(accepted_bytes == 2 && dsp.accounting().io_cycles == 45,
                "DSP writes must retain command busy state through its scheduled completion boundary");
        AudioScheduler status(rate);
        unsigned polls = 0;
        while (status.read_dsp_busy()) {
            require(++polls < 16, "SB16 phase polling must eventually leave the busy half-cycle");
        }
        require(status.accounting().io_cycles == 20 * (polls + 1),
                "DSP status reads must use the full byte-read I/O delay");
        AudioScheduler shadow(rate);
        unsigned shadow_irqs = 0;
        shadow.enable_interrupts(false);
        shadow.set_pit(mission_pit_divisor, [&] { ++shadow_irqs; });
        shadow.run_cycles(budget * 100);
        shadow.execute(CpuOperation::enable_interrupts, 1);
        require(shadow_irqs == 0, "STI must defer a pending IRQ through the following instruction");
        shadow.execute(CpuOperation::disable_interrupts, 1);
        require(shadow_irqs == 0, "An adjacent CLI must suppress the pending IRQ after STI");
        shadow.execute(CpuOperation::enable_interrupts, 1);
        shadow.execute(CpuOperation::instructions, 1);
        require(shadow_irqs == 1, "A pending IRQ must run after STI's following ordinary instruction");
        AudioScheduler shadow_edge(rate);
        std::uint64_t shadow_edge_irq = 0;
        shadow_edge.enable_interrupts(false);
        shadow_edge.set_pit(mission_pit_divisor, [&] { shadow_edge_irq = shadow_edge.cycles(); });
        shadow_edge.run_cycles(budget * 100 - 1);
        shadow_edge.take_output();
        shadow_edge.execute(CpuOperation::enable_interrupts, 1);
        require(shadow_edge.take_output().empty() && shadow_edge_irq == 0,
                "A pending STI at the slice boundary must force its following instruction before servicing queued events");
        shadow_edge.execute(CpuOperation::instructions, 1);
        require(shadow_edge_irq == budget * 100 + 2 && shadow_edge.take_output().size() == 48,
                "STI's forced instruction must incur one terminal charge before pending event and IRQ servicing");
        AudioScheduler sampled(rate);
        unsigned sampled_irqs = 0, counter_value = 1;
        sampled.set_pit(mission_pit_divisor, [&] { ++sampled_irqs; });
        const auto first_pit = (std::uint64_t(budget) * 1000 * mission_pit_divisor + pit_frequency - 1) / pit_frequency;
        sampled.run_cycles(first_pit - 1);
        sampled.instruction([&] { counter_value = sampled_irqs; });
        require(counter_value == 0 && sampled_irqs == 1,
                "A counter access must sample inside its instruction before the newly due IRQ can modify it");
        AudioScheduler retrace(rate);
        retrace.run_cycles(311398);
        require(!retrace.read_retrace(), "VGA status must remain inactive before the retained retrace edge");
        retrace.execute(CpuOperation::instructions, 1);
        require(retrace.read_retrace(), "VGA status reads must sample retrace after their I/O delay");
        retrace.run_cycles(1230);
        require(!retrace.read_retrace(), "VGA status must leave retrace at the retained end line");
        AudioScheduler stack_credit(rate);
        stack_credit.execute(CpuOperation::stack_segment, 3);
        require(stack_credit.cycles() == 0 && stack_credit.accounting().dispatches == 3 &&
                stack_credit.accounting().stack_segment_credits == 3,
                "MOV SS must refund its dispatch cycle without losing the instruction count");
        AudioScheduler callback(rate);
        callback.run_cycles(budget - 1);
        callback.execute(CpuOperation::callback_return, 1);
        require(callback.cycles() == budget && callback.accounting().terminal_charges == 0,
                "A returning callback must leave the core before its failed-loop terminal charge");
        AudioScheduler ordinary_iret(rate);
        ordinary_iret.run_cycles(budget - 1);
        ordinary_iret.execute(CpuOperation::interrupt_return, 1);
        require(ordinary_iret.cycles() == budget + 1 && ordinary_iret.accounting().terminal_charges == 1,
                "IRET without pending PIC arbitration must retain normal slice exhaustion");
        AudioScheduler callback_shadow(rate);
        unsigned callback_irqs = 0, callback_sample = 0;
        callback_shadow.enable_interrupts(false);
        callback_shadow.set_pit(mission_pit_divisor, [&] { ++callback_irqs; });
        callback_shadow.run_cycles(budget * 100);
        callback_shadow.execute(CpuOperation::enable_interrupts, 1);
        callback_shadow.execute(CpuOperation::callback_return, 1);
        require(callback_irqs == 0, "A callback after STI must return before pending IRQ delivery");
        callback_shadow.instruction([&] { callback_sample = callback_irqs; });
        require(callback_sample == 1, "The returned callback must release STI's interrupt shadow before the next instruction");
        AudioScheduler disk(rate);
        unsigned disk_irqs = 0;
        disk.set_pit(256, [&] { ++disk_irqs; disk.execute(CpuOperation::instructions, 5); });
        disk.read_file(8192);
        disk.flush();
        require(disk_irqs > 0 && disk.accounting().idle_calls > 2 &&
                disk.accounting().stack_segment_credits == 3 + 3 * disk_irqs && !disk.take_output().empty(),
                "DOS idle waits must advance the shared VGA, PIT and mixer clocks");
        AudioScheduler keyboard(rate);
        std::vector<std::uint64_t> key_times;
        std::vector<std::uint8_t> key_bytes;
        keyboard.set_interrupt_delay(838);
        keyboard.set_keyboard_handler([&] {
            key_times.push_back(keyboard.cycles());
            key_bytes.push_back(keyboard.read_keyboard());
            keyboard.acknowledge_keyboard();
            keyboard.execute(CpuOperation::instructions, 3);
        });
        keyboard.queue_key_byte(0xe0);
        keyboard.queue_key_byte(0x48);
        keyboard.run_cycles(6016);
        require(key_times.empty(), "Keyboard transfer and PIC arbitration must delay IRQ1");
        keyboard.run_cycles(1);
        require(key_times == std::vector<std::uint64_t>{6017} && key_bytes == std::vector<std::uint8_t>{0xe0},
                "The float 0.300ms transfer and 838ns PIC delay must precede delivery");
        keyboard.run_cycles(6100);
        require(key_bytes == std::vector<std::uint8_t>{0xe0, 0x48} && key_times[1] > key_times[0] + 6000,
                "Reading port 60h must schedule the next byte without dropping E0 prefixes");
        AudioScheduler key_arbitration(rate);
        bool arbitration_checked = false;
        key_arbitration.set_interrupt_delay(838);
        key_arbitration.set_keyboard_handler([&] {
            const auto entry = key_arbitration.cycles();
            key_arbitration.execute(CpuOperation::instructions, 1);
            key_arbitration.read_keyboard();
            require(key_arbitration.cycles() == entry + 23,
                    "The follow-up PIC event must terminate the handler's first I/O slice");
            const auto before_sti = key_arbitration.cycles();
            key_arbitration.execute(CpuOperation::enable_interrupts, 1);
            key_arbitration.execute(CpuOperation::instructions, 1);
            require(key_arbitration.cycles() == before_sti + 3,
                    "STI must release the PIC check after its following instruction even with IRQ1 in service");
            key_arbitration.acknowledge_keyboard();
            arbitration_checked = true;
        });
        key_arbitration.queue_key_byte(0x48);
        key_arbitration.execute(CpuOperation::instructions, 7000);
        require(arbitration_checked, "The keyboard arbitration regression must enter IRQ1");
        AudioScheduler key_priority(rate);
        std::vector<unsigned> priorities;
        key_priority.enable_interrupts(false);
        key_priority.set_pit(256, [&] { priorities.push_back(0); });
        key_priority.set_keyboard_handler([&] {
            priorities.push_back(1);
            key_priority.read_keyboard();
            key_priority.acknowledge_keyboard();
        });
        key_priority.queue_key_byte(0x1c);
        key_priority.run_cycles(7000);
        key_priority.enable_interrupts(true);
        require(priorities == std::vector<unsigned>{0, 1}, "Pending PIT IRQ0 must precede keyboard IRQ1");
        AudioScheduler key_eoi(rate);
        unsigned key_entries = 0;
        std::vector<unsigned> key_order;
        key_eoi.set_keyboard_handler([&] {
            const auto entry = ++key_entries;
            key_order.push_back(entry);
            key_eoi.read_keyboard();
            key_eoi.execute(CpuOperation::enable_interrupts, 1);
            if (entry == 1) { key_eoi.execute(CpuOperation::instructions, 13000); }
            key_eoi.acknowledge_keyboard();
            key_order.push_back(entry + 2);
            key_eoi.execute(CpuOperation::instructions, 3);
        });
        key_eoi.queue_key_byte(0xe0);
        key_eoi.queue_key_byte(0x48);
        key_eoi.execute(CpuOperation::instructions, 30000);
        require(key_order == std::vector<unsigned>{1, 2, 4, 3},
                "EOI must admit a pending keyboard IRQ before the previous handler's IRET");
        AudioScheduler batched(rate), scalar(rate);
        AudioScheduler calendar(rate), partitioned_calendar(rate);
        std::vector<std::uint8_t> calendar_bytes, partitioned_bytes;
        std::vector<std::uint64_t> calendar_times, partitioned_times;
        for (auto* scheduler : {&calendar, &partitioned_calendar}) {
            auto& bytes = scheduler == &calendar ? calendar_bytes : partitioned_bytes;
            auto& times = scheduler == &calendar ? calendar_times : partitioned_times;
            scheduler->set_keyboard_handler([scheduler, &bytes, &times] {
                times.push_back(scheduler->cycles());
                bytes.push_back(scheduler->read_keyboard());
                scheduler->acknowledge_keyboard();
            });
            scheduler->schedule_key_byte(50000, 0xe0);
            scheduler->schedule_key_byte(50000, 0x48);
            scheduler->schedule_key_byte(80000, 0xc8);
            scheduler->run_cycles(49999);
            require(bytes.empty(), "A future keyboard arrival must not enter the controller early");
        }
        calendar.run_cycles(50001);
        for (unsigned i = 0; i < 50001; ++i) { partitioned_calendar.run_cycles(1); }
        require(calendar_bytes == std::vector<std::uint8_t>{0xe0, 0x48, 0xc8} && calendar_bytes == partitioned_bytes &&
                calendar_times == partitioned_times && calendar.take_output() == partitioned_calendar.take_output(),
                "CPU-cycle input arrivals must preserve FIFO order, IRQ timestamps and PCM across host partitions");
        bool rejected = false;
        try { calendar.schedule_key_byte(0, 0x1c); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "A past input arrival must reject instead of fitting the session clock");
        rejected = false;
        try {
            calendar.schedule_key_byte(200000, 0x1c);
            calendar.schedule_key_byte(150000, 0x9c);
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "An unordered input movie must reject instead of silently reordering events");
        scalar.set_cpu_observer([](CpuOperation, std::uint64_t) {});
        std::vector<std::uint64_t> batched_irqs, scalar_irqs;
        for (auto* scheduler : {&batched, &scalar}) {
            auto& times = scheduler == &batched ? batched_irqs : scalar_irqs;
            scheduler->set_pit(mission_pit_divisor, [scheduler, &times] {
                times.push_back(scheduler->cycles());
                scheduler->execute(CpuOperation::instructions, 7);
            });
            scheduler->execute(CpuOperation::instructions, 1000000);
        }
        require(batched.cycles() == scalar.cycles() && batched_irqs == scalar_irqs &&
                batched.accounting().dispatches == scalar.accounting().dispatches &&
                batched.accounting().terminal_charges == scalar.accounting().terminal_charges &&
                batched.take_output() == scalar.take_output(),
                "Batching empty instructions must preserve VGA/core boundaries, IRQ work and mixer output");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
