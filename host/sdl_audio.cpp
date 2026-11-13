#include "sdl_audio.hpp"
#include "sdl_common.hpp"

#include <algorithm>
#include <limits>

namespace comanchecd::host {
namespace {
constexpr auto preroll_bytes = MissionAudio::sample_rate / 10 * sizeof(std::int16_t);
}

class SdlAudioOutput::Impl {
public:
    enum class State { paused, priming, running };
    Impl() {
        SDL_AudioSpec wanted{};
        wanted.freq = MissionAudio::sample_rate;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 1;
        wanted.samples = 512;
        device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        if (device == 0) {
            throw sdl_error("SDL2 audio device failed");
        }
    }
    ~Impl() { SDL_CloseAudioDevice(device); }

    void start_if_ready() {
        if (state == State::priming && SDL_GetQueuedAudioSize(device) >= preroll_bytes) {
            SDL_PauseAudioDevice(device, 0);
            state = State::running;
        }
    }

    SdlSubsystem subsystem{SDL_INIT_AUDIO};
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec obtained{};
    State state = State::paused;
};

SdlAudioOutput::SdlAudioOutput() : impl_(std::make_unique<Impl>()) {}
SdlAudioOutput::~SdlAudioOutput() = default;

void SdlAudioOutput::queue(const std::vector<std::int16_t>& samples) {
    if (samples.size() > std::numeric_limits<Uint32>::max() / sizeof(std::int16_t)) {
        throw std::invalid_argument("SDL2 audio block exceeds the queue size limit");
    }
    if (SDL_QueueAudio(impl_->device, samples.data(), static_cast<Uint32>(samples.size() * sizeof(std::int16_t))) != 0) {
        throw sdl_error("SDL2 audio queue failed");
    }
    impl_->start_if_ready();
}
std::size_t SdlAudioOutput::queued_samples() const { return SDL_GetQueuedAudioSize(impl_->device) / sizeof(std::int16_t); }
void SdlAudioOutput::clear() {
    SDL_PauseAudioDevice(impl_->device, 1);
    SDL_ClearQueuedAudio(impl_->device);
    if (impl_->state == Impl::State::running) { impl_->state = Impl::State::priming; }
}
void SdlAudioOutput::set_playback(Playback playback) {
    if (playback == Playback::paused) {
        SDL_PauseAudioDevice(impl_->device, 1);
        impl_->state = Impl::State::paused;
        return;
    }
    if (impl_->state == Impl::State::paused) { impl_->state = Impl::State::priming; }
    impl_->start_if_ready();
}

void SdlAudioOutput::play_blocking(const std::vector<std::int16_t>& samples) {
    constexpr std::size_t block_samples = MissionAudio::sample_rate / 20;
    clear();
    set_playback(Playback::running);
    for (std::size_t offset = 0; offset < samples.size(); offset += block_samples) {
        while (queued_samples() >= block_samples * 2) {
            SDL_Delay(2);
        }
        const auto end = std::min(offset + block_samples, samples.size());
        queue({samples.begin() + offset, samples.begin() + end});
    }
    if (impl_->state == Impl::State::priming) {
        SDL_PauseAudioDevice(impl_->device, 0);
        impl_->state = Impl::State::running;
    }
    while (queued_samples() != 0) {
        SDL_Delay(2);
    }
    // The final queued block may still be in the device buffer.
    SDL_Delay((impl_->obtained.samples * 1000u + impl_->obtained.freq - 1) / impl_->obtained.freq);
    set_playback(Playback::paused);
}
} // namespace comanchecd::host
