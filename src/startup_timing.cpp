#include "startup_timing.hpp"
#include "comanchecd/audio.hpp"
#include <stdexcept>
#include <algorithm>

namespace comanchecd {
namespace {
constexpr std::uint8_t pit2_mode = 0xb6, pit2_latch = 0x86;
constexpr unsigned calibration_limit = 500, minimum_count = 0x398, maximum_count = 0x3fc;

void instructions(AudioScheduler& clock, unsigned count) { clock.execute(CpuOperation::instructions, count); }

void reset_dsp(AudioScheduler& clock) {
    clock.execute(CpuOperation::disable_interrupts, 1);
    instructions(clock, 3);
    clock.write_dsp_reset(1);
    instructions(clock, 2);
    constexpr unsigned reset_hold_reads = 10, reset_poll_limit = 2000;
    for (unsigned i = 0; i < reset_hold_reads; ++i) {
        clock.execute(CpuOperation::port_read_byte, 1);
        instructions(clock, 2);
    }
    instructions(clock, 1);
    clock.write_dsp_reset(0);
    instructions(clock, 4);
    bool ready = false;
    for (unsigned i = 0; i < reset_poll_limit; ++i) {
        ready = clock.read_dsp_ready();
        instructions(clock, 3);
        if (ready) { break; }
    }
    instructions(clock, 2);
    const auto response = clock.read_dsp_data();
    instructions(clock, 1);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 4);
    if (!ready || response != 0xaa) { throw std::runtime_error("Cold DSP reset did not acknowledge"); }
}

void dsp_byte(AudioScheduler& clock, std::uint8_t value, const std::function<void()>& accepted = {}) {
    bool busy;
    do {
        busy = clock.read_dsp_busy();
        instructions(clock, 3);
    } while (busy);
    instructions(clock, 1);
    clock.write_dsp_timed(value, accepted);
}

void stop_digital(AudioScheduler& clock, const StartupDigital& digital) {
    clock.execute(CpuOperation::disable_interrupts, 1);
    instructions(clock, 2);
    dsp_byte(clock, 0xd0, [&] { if (digital) { digital(DigitalCommand::stop); } });
    instructions(clock, 3);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 1);
}

void start_digital(AudioScheduler& clock, const StartupDigital& digital) {
    instructions(clock, 1); // 2075 CALL reset.
    reset_dsp(clock);
    instructions(clock, 1);
    clock.execute(CpuOperation::disable_interrupts, 1);
    instructions(clock, 9 + 1 + 1 + 12 + 6); // Primary IRQ, IDT hook and DMA mask setup.
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(clock, 2);
    dsp_byte(clock, 0xd1);
    dsp_byte(clock, 0x40);
    dsp_byte(clock, 0xa6);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 3);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 5);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 5);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 4);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 4);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 4);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    dsp_byte(clock, 0x14);
    dsp_byte(clock, 0xff);
    dsp_byte(clock, 0x07, [&] { if (digital) { digital(DigitalCommand::start); } });
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 1);
}

void digital_selector(AudioScheduler& clock, unsigned selector, bool& active, const StartupDigital& digital,
                      const std::function<unsigned()>& reset_voices = {}) {
    instructions(clock, 7);
    if (active) {
        instructions(clock, 1);
        stop_digital(clock, digital);
    }
    instructions(clock, 18);
    clock.execute(CpuOperation::repeat_store_dwords, 1024);
    instructions(clock, 4 + 7); // Valid selector and shared clear CALL.
    clear_sample_voices(clock, reset_voices);
    instructions(clock, 1); // Driver CALL.
    if (selector) { start_digital(clock, digital); }
    else { instructions(clock, 1); }
    active = selector != 0;
    instructions(clock, 2);
}

unsigned coarse_delay(AudioScheduler& clock) {
    clock.execute(CpuOperation::disable_interrupts, 1);
    instructions(clock, 1);
    clock.pit2_control(pit2_mode);
    instructions(clock, 2);
    clock.pit2_write(0);
    instructions(clock, 2);
    clock.pit2_write(2);
    const auto speaker = clock.read_speaker();
    instructions(clock, 2);
    clock.write_speaker((speaker & 0xfc) | 1);
    instructions(clock, 1);
    unsigned rounds = 0;
    for (;;) {
        instructions(clock, 1);
        clock.pit2_control(pit2_latch);
        instructions(clock, 1);
        clock.pit2_read();
        instructions(clock, 1);
        const auto high = clock.pit2_read();
        instructions(clock, 4);
        ++rounds;
        if (high == 0) { break; }
    }
    const auto stopped = clock.read_speaker();
    instructions(clock, 1);
    clock.write_speaker(stopped & 0xfc);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 4);
    return rounds + 1;
}

unsigned measure_delay(AudioScheduler& clock, unsigned iterations) {
    clock.execute(CpuOperation::disable_interrupts, 1);
    instructions(clock, 1);
    clock.pit2_control(pit2_mode);
    instructions(clock, 1);
    clock.pit2_write(0);
    instructions(clock, 1);
    clock.pit2_write(0);
    const auto speaker = clock.read_speaker();
    instructions(clock, 2);
    clock.write_speaker((speaker & 0xfc) | 1);
    instructions(clock, 2);
    constexpr unsigned measurement_rounds = 16;
    for (unsigned round = 0; round < measurement_rounds; ++round) { instructions(clock, 2 + 2 * iterations); }
    instructions(clock, 1);
    clock.pit2_control(pit2_latch);
    const auto low = clock.pit2_read();
    instructions(clock, 1);
    const auto high = clock.pit2_read();
    instructions(clock, 3);
    const auto stopped = clock.read_speaker();
    instructions(clock, 1);
    clock.write_speaker(stopped & 0xfc);
    instructions(clock, 1);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 1);
    return static_cast<std::uint16_t>(-static_cast<std::uint16_t>(low | unsigned(high) << 8));
}
}

void clear_sample_voices(AudioScheduler& clock, const std::function<unsigned()>& reset_voices) {
    instructions(clock, 2);
    clock.execute(CpuOperation::disable_interrupts, 1);
    instructions(clock, 12 + 3 * 3);
    if (reset_voices) { instructions(clock, 6 * reset_voices()); }
    instructions(clock, 2);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 1);
}

void disable_menu_samples(AudioScheduler& clock, bool active, const StartupDigital& digital,
                          const std::function<unsigned()>& reset_voices) {
    digital_selector(clock, 0, active, digital, reset_voices);
}

unsigned calibrate_opl_delay(AudioScheduler& clock, const CalibrationObserver& observer) {
    instructions(clock, 1);
    auto iterations = coarse_delay(clock);
    instructions(clock, 1);
    const auto measure = [&] {
        const auto count = measure_delay(clock, iterations);
        if (observer) { observer(iterations, count); }
        return count;
    };
    measure();
    instructions(clock, 1);
    unsigned remaining = calibration_limit;
    while (remaining != 0) {
        instructions(clock, 1);
        const auto count = measure();
        instructions(clock, 2);
        if (count <= minimum_count) {
            instructions(clock, 3);
            ++iterations;
            --remaining;
            if (remaining == 0) { instructions(clock, 1); }
            continue;
        }
        instructions(clock, 2);
        if (count < maximum_count) { break; }
        instructions(clock, 3);
        --iterations;
        --remaining;
    }
    instructions(clock, 2);
    return iterations;
}

void pre_fm_startup(AudioScheduler& clock, const StartupFiles& files,
                    const std::function<void(StartupBoundary)>& observer) {
    constexpr unsigned configuration_fields = 6, file_block_bytes = 0xfa00;
    constexpr unsigned digital_selector = 1, music_selector = 2, port_selector = 3;
    constexpr unsigned fm_driver = 3, sb_port = 2, maximum_digital_driver = 4;
    constexpr unsigned inline_name_bytes = 13, name_byte_work = 12;
    constexpr unsigned file_prefix_work = 12 + inline_name_bytes * name_byte_work;
    if (files.configuration.size() < configuration_fields || files.configuration.size() >= file_block_bytes ||
            (files.cdrom && files.cdrom->size() >= file_block_bytes)) {
        throw std::invalid_argument("Unsupported cold startup file length");
    }
    const unsigned digital = files.configuration[digital_selector] & 15;
    if ((files.configuration[music_selector] & 15) != fm_driver || digital > maximum_digital_driver ||
            (digital && (files.configuration[port_selector] & 15) != sb_port)) {
        throw std::invalid_argument("Unsupported cold startup sound profile");
    }
    const auto mark = [&](StartupBoundary boundary) { if (observer) { observer(boundary); } };
    const auto read = [&](const std::vector<std::uint8_t>& bytes) {
        // 16880: copy the inline filename, then find/open/read/close the file.
        instructions(clock, file_prefix_work);
        clock.file_metadata(DosFileCall::find);
        instructions(clock, 3);
        clock.file_metadata(DosFileCall::open_read);
        instructions(clock, 7);
        clock.read_file(static_cast<unsigned>(bytes.size()));
        instructions(clock, 12);
        clock.file_metadata(DosFileCall::close);
        instructions(clock, 2);
    };
    instructions(clock, 11); // 5713 CALL, pointer-copy helper and find CALL.
    mark(StartupBoundary::find_cdrom);
    instructions(clock, file_prefix_work);
    clock.file_metadata(DosFileCall::find);
    instructions(clock, 4);
    if (files.cdrom) {
        instructions(clock, 3);
        mark(StartupBoundary::read_cdrom);
        read(*files.cdrom);
        instructions(clock, 4);
    } else {
        instructions(clock, 3);
    }
    mark(StartupBoundary::read_configuration);
    read(files.configuration);
    instructions(clock, 15);
    mark(StartupBoundary::detect_digital);
    instructions(clock, 6); // 51c1 dispatcher through its tail jump.
    if (digital) {
        reset_dsp(clock);
        instructions(clock, 3);
        mark(StartupBoundary::clear_digital);
        // 51e8 receives detector result -1 as its selector. It clears the
        // buffer, rejects selector 255 and returns without starting DMA.
        instructions(clock, 25);
        clock.execute(CpuOperation::repeat_store_dwords, 1024);
        instructions(clock, 7);
        instructions(clock, 34);
    } else {
        instructions(clock, 1); // Disabled detector is RET.
        instructions(clock, 3);
        instructions(clock, 33);
    }
    mark(StartupBoundary::detect_fm);
}

void read_campaign(AudioScheduler& clock, const StartupCampaign& campaign,
                   const std::function<void()>& before_decode) {
    constexpr unsigned filename_bytes = 6, filename_byte_work = 12, decode_packet_bytes = 8;
    if (campaign.encoded.empty() || campaign.encoded.size() >= 0xfa00) {
        throw std::invalid_argument("Unsupported campaign file size");
    }
    instructions(clock, 12 + filename_bytes * filename_byte_work);
    clock.file_metadata(DosFileCall::find);
    instructions(clock, campaign.read_location == StartupLocation::local ? 3 : 5);
    clock.file_metadata(DosFileCall::open_read);
    instructions(clock, 7);
    clock.read_file(static_cast<unsigned>(campaign.encoded.size()));
    instructions(clock, 12);
    clock.file_metadata(DosFileCall::close);
    instructions(clock, 2);
    instructions(clock, 1); // Decoder CALL.
    if (before_decode) { before_decode(); }
    if (campaign.encoded.front() == '{') {
        instructions(clock, 11);
        return;
    }
    instructions(clock, 12 + (campaign.encoded.front() == 'x' ? 0 : 2));
    for (std::size_t packet = 0; packet < (campaign.encoded.size() + decode_packet_bytes - 1) / decode_packet_bytes; ++packet) {
        for (unsigned word = 0; word < 2; ++word) {
            clock.execute(CpuOperation::string_bytes, 1);
            instructions(clock, 1);
            clock.execute(CpuOperation::string_bytes, 1);
        }
        instructions(clock, 1);
    }
    instructions(clock, 1);
}

void prepare_startup_campaigns(AudioScheduler& clock, const StartupFiles& files,
                              const std::function<void(CampaignBoundary, unsigned)>& observer) {
    constexpr unsigned campaign_limit = 10, file_block_bytes = 0xfa00;
    constexpr unsigned filename_bytes = 6, filename_byte_work = 12, decode_packet_bytes = 8;
    constexpr unsigned campaign_title_limit = 29;
    if (!files.campaigns || files.campaigns->size() > campaign_limit) {
        throw std::invalid_argument("Missing startup campaign scan inputs");
    }
    const bool cd_prefix = files.cdrom && !files.cdrom->empty() && files.cdrom->front() != '@';
    for (const auto& campaign : *files.campaigns) {
        if (campaign.encoded.empty() && campaign.decoded.empty()) { continue; }
        if (campaign.encoded.empty() || campaign.encoded.size() >= file_block_bytes ||
                campaign.encoded.size() % decode_packet_bytes || campaign.encoded.front() == '{') {
            throw std::invalid_argument("Unsupported startup campaign encoding");
        }
        if (!cd_prefix && campaign.read_location == StartupLocation::cdrom) {
            throw std::invalid_argument("Startup campaign requires a configured CD-ROM path");
        }
        const auto open = std::find(campaign.decoded.begin(), campaign.decoded.end(), '{');
        const auto close = std::find(open, campaign.decoded.end(), '}');
        if (open == campaign.decoded.end() || close == campaign.decoded.end() || close - open - 1 > campaign_title_limit) {
            throw std::invalid_argument("Invalid startup campaign title");
        }
    }
    const auto mark = [&](CampaignBoundary boundary, unsigned index) {
        if (observer) { observer(boundary, index); }
    };
    const auto find = [&](StartupLocation location) {
        instructions(clock, 9 + filename_bytes * filename_byte_work);
        if (cd_prefix) {
            instructions(clock, 2);
            clock.file_metadata(DosFileCall::find);
            instructions(clock, 1);
            if (location == StartupLocation::cdrom) {
                instructions(clock, 3);
                return;
            }
        }
        instructions(clock, 3);
        clock.file_metadata(DosFileCall::find);
        instructions(clock, 4);
    };
    // Dispatcher return, STI, the 256-pointer fixup and the campaign CALL.
    instructions(clock, 7);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 6 + 3 * 256 + 4);
    mark(CampaignBoundary::begin, 0);
    instructions(clock, 4);
    for (unsigned index = 0; index < campaign_limit; ++index) {
        instructions(clock, 3);
        mark(CampaignBoundary::find, index);
        const bool present = index < files.campaigns->size() && !files.campaigns->at(index).encoded.empty();
        find(present ? files.campaigns->at(index).find_location : StartupLocation::local);
        instructions(clock, 2);
        if (!present) {
            instructions(clock, 4); // Advance the filename digit and continue the ten-slot search.
            continue;
        }
        const auto& campaign = files.campaigns->at(index);
        instructions(clock, 5);
        mark(CampaignBoundary::read, index);
        read_campaign(clock, campaign, [&] { mark(CampaignBoundary::decode, index); });
        instructions(clock, 4);
        const auto open = std::find(campaign.decoded.begin(), campaign.decoded.end(), '{');
        const auto close = std::find(open, campaign.decoded.end(), '}');
        clock.execute(CpuOperation::repeat_scan_bytes, static_cast<unsigned>(open - campaign.decoded.begin() + 1));
        instructions(clock, 3);
        clock.execute(CpuOperation::repeat_scan_bytes, static_cast<unsigned>(close - open));
        instructions(clock, 8);
        clock.execute(CpuOperation::repeat_copy_bytes, static_cast<unsigned>(close - open - 1));
        instructions(clock, 11);
        mark(CampaignBoundary::scanned, index);
    }
    instructions(clock, 1);
    mark(CampaignBoundary::complete, campaign_limit);
}

void prepare_startup_video(AudioScheduler& clock) {
    instructions(clock, 1); // 582f CALL.
    clock.initialize_video();
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_word, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::disable_interrupts, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::instructions, 1);
    clock.execute(CpuOperation::port_read_byte, 1);
    clock.execute(CpuOperation::instructions, 3);

    constexpr unsigned attribute_entries = 16;
    for (unsigned index = 0; index < attribute_entries; ++index) {
        clock.execute(CpuOperation::port_write_byte, 1);
        instructions(clock, 1);
        clock.execute(CpuOperation::port_write_byte, 1);
        instructions(clock, 2);
    }
    instructions(clock, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(clock, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    instructions(clock, 2);
    clock.execute(CpuOperation::port_write_byte, 1);
    clock.execute(CpuOperation::enable_interrupts, 1);
    instructions(clock, 1); // 1678f RET.
}

void apply_startup_settings(AudioScheduler& clock, const StartupFiles& files, SettingsPass pass,
    const std::function<void(MusicLevel)>& music, const StartupDigital& digital,
    const std::function<void(LoadingBoundary)>& observer) {
    if (!files.loading || files.loading->active_settings.size() != MenuSettings::record_bytes) {
        throw std::invalid_argument("Missing startup settings inputs");
    }
    const auto& loading = *files.loading;
    const auto& settings = loading.active_settings;
    constexpr unsigned sound_field = 25, music_field = 26, control_field = 27;
    constexpr unsigned scenery_field = 28, pixels_field = 29, overlay_field = 30, reflections_field = 31;
    constexpr unsigned peripheral_field = 41, joystick_field = 42, enabled = 125;
    const auto mark = [&](LoadingBoundary boundary) { if (observer) { observer(boundary); } };
    const auto read = [&](const std::vector<std::uint8_t>& bytes, unsigned filename_bytes, StartupLocation location) {
        instructions(clock, 12 + filename_bytes * 12);
        clock.file_metadata(DosFileCall::find);
        instructions(clock, location == StartupLocation::local ? 3 : 5);
        clock.file_metadata(DosFileCall::open_read);
        instructions(clock, 7);
        clock.read_file(static_cast<unsigned>(bytes.size()));
        instructions(clock, 12);
        clock.file_metadata(DosFileCall::close);
        instructions(clock, 2);
    };
    mark(LoadingBoundary::settings);
    instructions(clock, 3);
    if (settings[music_field] == '0' || settings[music_field] == '1') {
        if (settings[music_field] == '1') { instructions(clock, 2); }
        instructions(clock, 4 + 22 + 1);
        music(settings[music_field] == '0' ? MusicLevel::loud : MusicLevel::soft);
    } else {
        instructions(clock, 4);
        if (settings[music_field] == '2') {
            instructions(clock, 3 + 12 + 1);
            music(MusicLevel::off);
            instructions(clock, 3 + 7);
        }
    }
    instructions(clock, 3);
    bool digital_active = false;
    if (settings[sound_field] == '0' || settings[sound_field] == '1') {
        if (settings[sound_field] == '1') { instructions(clock, 2); }
        instructions(clock, 2);
        digital_selector(clock, files.configuration.at(1) & 15, digital_active, digital);
        instructions(clock, settings[sound_field] == '1' ? 3 : 1);
    } else {
        instructions(clock, 4);
        if (settings[sound_field] == '2') {
            instructions(clock, 2);
            digital_selector(clock, 0, digital_active, digital);
        }
    }
    instructions(clock, 30); // Flag copies.
    mark(LoadingBoundary::metrics);
    instructions(clock, 1); // Metric-table index.
    for (const auto& menu : loading.menus) {
        if (menu.empty() || menu.back() != 255) { throw std::invalid_argument("Invalid startup metric table"); }
        instructions(clock, 4 + 5);
        unsigned width = 0, maximum = 0;
        for (std::size_t index = 1; index < menu.size(); ++index) {
            const auto code = menu[index];
            clock.execute(CpuOperation::string_bytes, 1);
            instructions(clock, code == 0 ? 2 : 4);
            if (code != 0 && code != 255) {
                width += loading.glyph_widths[code];
                instructions(clock, 4);
                continue;
            }
            instructions(clock, 2);
            if (width > maximum) { maximum = width; instructions(clock, 1); }
            width = 0;
            instructions(clock, 4);
        }
        instructions(clock, 7);
    }
    instructions(clock, 4 + 1); // Sentinel-table test and tail jump to 290fe.
    instructions(clock, 5);
    if (settings[control_field] <= '2') {
        instructions(clock, 2);
        if (settings[control_field] == '0') { instructions(clock, 1); }
        else {
            instructions(clock, 2);
            if (settings[peripheral_field] != enabled) { instructions(clock, 1); }
        }
    }
    instructions(clock, 2);
    if (settings[joystick_field] == enabled) { instructions(clock, 1); }
    instructions(clock, 3 + 2);
    if (settings[scenery_field] == enabled) { instructions(clock, 3); }
    instructions(clock, 1 + 2);
    if (settings[pixels_field] != enabled) { instructions(clock, 1); }
    instructions(clock, 1 + 2);
    if (settings[overlay_field] == enabled) { instructions(clock, 1); }
    instructions(clock, 1 + 2);
    if (settings[reflections_field] == enabled) { instructions(clock, 1); }
    instructions(clock, 3);
    if (pass == SettingsPass::cold) {
        instructions(clock, 2);
        const unsigned detail = MenuSettings(settings).detail();
        if (detail != 0) { instructions(clock, 2); }
        instructions(clock, 2);
        mark(LoadingBoundary::detail);
        constexpr std::array<unsigned, 3> detail_filename_bytes{9, 11, 8};
        read(loading.detail, detail_filename_bytes[detail], loading.detail_location);
        instructions(clock, 1 + 8); // 165a9 CALL and cold null-table return.
        if (detail != 2) { instructions(clock, 1); }
    }
    instructions(clock, 2); // Remember the selected detail in both paths.
    instructions(clock, 2 + 22 + 1);
    clock.execute(CpuOperation::instructions, 4);
    clock.execute(CpuOperation::repeat_store_dwords, 32);
    instructions(clock, 1 + 1); // Keyboard clear and settings RETs.
}

void prepare_startup_loading(AudioScheduler& clock, const StartupFiles& files,
    IrqState* state, const std::function<void(MusicLevel)>& music, const std::function<void(LoadingBoundary)>& observer, const StartupDigital& digital) {
    if (!files.loading || files.loading->active_settings.size() != MenuSettings::record_bytes) {
        throw std::invalid_argument("Missing startup loading inputs");
    }
    const auto& loading = *files.loading;
    const auto& settings = loading.active_settings;
    constexpr unsigned sound_field = 25;
    const auto mark = [&](LoadingBoundary boundary) { if (observer) { observer(boundary); } };
    const auto find = [&] {
        instructions(clock, 9 + 13 * 12);
        if (files.cdrom && !files.cdrom->empty() && files.cdrom->front() != '@') {
            instructions(clock, 2);
            clock.file_metadata(DosFileCall::find);
            instructions(clock, 1);
        }
        instructions(clock, 3);
        clock.file_metadata(DosFileCall::find);
        instructions(clock, 4);
    };
    const auto read = [&](const std::vector<std::uint8_t>& bytes, unsigned filename_bytes, StartupLocation location) {
        instructions(clock, 12 + filename_bytes * 12);
        clock.file_metadata(DosFileCall::find);
        instructions(clock, location == StartupLocation::local ? 3 : 5);
        clock.file_metadata(DosFileCall::open_read);
        instructions(clock, 7);
        clock.read_file(static_cast<unsigned>(bytes.size()));
        instructions(clock, 12);
        clock.file_metadata(DosFileCall::close);
        instructions(clock, 2);
    };
    instructions(clock, 1);
    mark(LoadingBoundary::find_pilots);
    find();
    instructions(clock, 1);
    if (loading.pilots) {
        instructions(clock, 2);
        mark(LoadingBoundary::read_pilots);
        read(*loading.pilots, 13, StartupLocation::local);
    }
    instructions(clock, 2);
    mark(LoadingBoundary::find_settings);
    find();
    instructions(clock, 1);
    if (loading.settings) {
        instructions(clock, 2);
        mark(LoadingBoundary::read_settings);
        read(*loading.settings, 13, StartupLocation::local);
    }
    instructions(clock, 1);
    apply_startup_settings(clock, files, SettingsPass::cold, music, digital, observer);
    bool digital_active = settings[sound_field] != '2' && (files.configuration.at(1) & 15) != 0;
    mark(LoadingBoundary::reset);
    instructions(clock, 2);
    digital_selector(clock, 0, digital_active, digital);
    instructions(clock, 1); // MOV ESP.
    clock.instruction([&] { if (state) { state->shake_offset = 0; } });
    clock.instruction([&] { if (state) { state->shake_ticks = 1; } });
    instructions(clock, 2); // Pan reset and statistics CALL.
    // Cold statistics: all 60 actor/flag slots are initially clear. Ratios use
    // the executable's fixed initial counters; no mission state is imported.
    instructions(clock, 11 + 60 * 5 + 7);
    instructions(clock, 4 + 23 + 4 + 26 + 12 + 23 + 30 + 18 + 4 + 18 + 14);
    clock.execute(CpuOperation::repeat_copy_bytes, 47);
    instructions(clock, 9 + 34 + 1);
    instructions(clock, 1 + 3); // Active-array clear CALL and prefix.
    clock.execute(CpuOperation::repeat_store_bytes, 140);
    instructions(clock, 3);
    mark(LoadingBoundary::complete);
}

}
