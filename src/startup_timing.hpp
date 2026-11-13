#pragma once

#include "comanchecd/audio_scheduler.hpp"

namespace comanchecd {
struct StartupFiles;
struct StartupCampaign;
enum class MusicLevel;
using CalibrationObserver = std::function<void(unsigned, unsigned)>;
unsigned calibrate_opl_delay(AudioScheduler& clock, const CalibrationObserver& observer = {});
enum class StartupBoundary { find_cdrom, read_cdrom, read_configuration, detect_digital, clear_digital, detect_fm };
void pre_fm_startup(AudioScheduler& clock, const StartupFiles& files,
                    const std::function<void(StartupBoundary)>& observer = {});
void prepare_startup_video(AudioScheduler& clock);
enum class DigitalCommand { start, stop };
using StartupDigital = std::function<void(DigitalCommand)>;
void disable_menu_samples(AudioScheduler& clock, bool active, const StartupDigital& digital,
                          const std::function<unsigned()>& reset_voices);
void clear_sample_voices(AudioScheduler& clock, const std::function<unsigned()>& reset_voices);
enum class SettingsPass { cold, post_start };

enum class LoadingBoundary { find_pilots, read_pilots, find_settings, read_settings, settings, metrics, detail, reset, complete };
void prepare_startup_loading(AudioScheduler& clock, const StartupFiles& files,
    IrqState* state, const std::function<void(MusicLevel)>& music,
    const std::function<void(LoadingBoundary)>& observer = {}, const StartupDigital& digital = {});
void apply_startup_settings(AudioScheduler& clock, const StartupFiles& files, SettingsPass pass,
    const std::function<void(MusicLevel)>& music, const StartupDigital& digital,
    const std::function<void(LoadingBoundary)>& observer = {});

enum class CampaignBoundary { begin, find, read, decode, scanned, complete };
void read_campaign(AudioScheduler& clock, const StartupCampaign& campaign,
                   const std::function<void()>& before_decode = {});
void prepare_startup_campaigns(AudioScheduler& clock, const StartupFiles& files,
                              const std::function<void(CampaignBoundary, unsigned)>& observer = {});
}
