#include "comanchecd/callback_audio.hpp"
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
        constexpr unsigned rate = 48000;
        constexpr std::array<std::int32_t, 2> unity{8192, 8192};
        AudioChannelConfig config{true, rate, unity, 0};
        CallbackAudio device(rate);
        device.configure(AudioChannel::digital, config);
        device.queue_digital({128, 129, 130});
        const auto first = device.mix(AudioChannel::digital, 1);
        const auto second = device.mix(AudioChannel::digital, 2);
        require(first.size() == 1 && CallbackAudio::output(first[0], unity)[0] == 0, "Initial interpolation must retain lookahead");
        require(second.size() == 1 && CallbackAudio::output(second[0], unity)[0] == 256, "Partial mixer calls must advance once");
        require(device.mix(AudioChannel::digital, 2).empty(), "Repeated interval must not consume more samples");
        device.end_frame(AudioChannel::digital, 2);
        device.queue_digital({131});
        require(CallbackAudio::output(device.mix(AudioChannel::digital, 1)[0], unity)[0] == 512,
                "Interpolation must carry across mixer intervals");

        CallbackAudio gap(rate);
        config.gain = {16384, 16384};
        gap.configure(AudioChannel::digital, config);
        gap.queue_digital({129, 131});
        const auto padded = gap.mix(AudioChannel::digital, 3);
        require(CallbackAudio::output(padded[0], unity)[0] == 512 && padded[1][0] == 768 && padded[2][0] == 768,
                "Short callbacks must preserve the reference unscaled padding behavior");

        CallbackAudio filtered(rate);
        config.gain = unity;
        config.lowpass_alpha = 32768;
        filtered.configure(AudioChannel::digital, config);
        filtered.queue_digital({129, 130, 131});
        require(CallbackAudio::output(filtered.mix(AudioChannel::digital, 1)[0], unity)[0] == 128,
                "Low-pass must operate on the fractional accumulator");
        filtered.end_frame(AudioChannel::digital, 1);
        require(CallbackAudio::output(filtered.mix(AudioChannel::digital, 1)[0], unity)[0] == 320,
                "Filter and buffered samples must survive frame boundaries");
        require(CallbackAudio::output({1000000000, -1000000000}, unity) == std::array<std::int16_t, 2>{32767, -32768},
                "The final mix must clip both signs");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
