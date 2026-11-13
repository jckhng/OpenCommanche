#include "comanchecd/keyboard.hpp"
#include "loading_clock.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
void receive(comanchecd::KeyboardInput& keyboard, LoadingClock& clock, std::uint8_t scan) {
    class Port final : public comanchecd::KeyboardPort {
    public:
        Port(LoadingClock& clock, std::uint8_t scan) : clock_(clock), scan_(scan) {}
        std::uint8_t read() override { clock_.execute(comanchecd::CpuOperation::port_read_byte, 1); return scan_; }
        void acknowledge() override { clock_.execute(comanchecd::CpuOperation::port_write_byte, 1); }
    private:
        LoadingClock& clock_;
        std::uint8_t scan_;
    } port(clock, scan);
    keyboard.interrupt(clock, port);
}
}
int main(int argc, char** argv) {
    try {
        using namespace comanchecd;
        if (argc == 3) {
            std::ifstream input(argv[1], std::ios::binary);
            input.exceptions(std::ios::failbit | std::ios::badbit);
            std::ofstream output(argv[2]);
            for (;;) {
                const auto scan = input.peek();
                if (scan == std::char_traits<char>::eof()) { break; }
                KeyboardImage image;
                input.get();
                input.read(reinterpret_cast<char*>(image.bytes.data()), image.bytes.size());
                image.throttle = static_cast<std::uint8_t>(input.get());
                KeyboardInput keyboard(image);
                LoadingClock clock;
                receive(keyboard, clock, static_cast<std::uint8_t>(scan));
                const auto cost = clock.accounting();
                output << cost.dispatches - cost.repeat_restarts << ' ' << cost.string_iterations << ' ' << cost.io_cycles << ' ';
                for (const auto value : keyboard.image().bytes) {
                    output << std::hex << std::setw(2) << std::setfill('0') << unsigned(value);
                }
                output << ' ' << unsigned(keyboard.image().throttle) << std::dec << '\n';
            }
            if (!output) { throw std::runtime_error("Cannot write keyboard work results"); }
            return 0;
        }
        require(argc == 1, "Usage: keyboard_tests [INPUT OUTPUT]");
        KeyboardImage dirty;
        dirty.bytes.fill(0x31);
        dirty.throttle = 0x40;
        KeyboardInput initialized(dirty);
        LoadingClock initialization;
        initialized.initialize(initialization);
        const auto installed = initialization.accounting();
        require(installed.dispatches - installed.repeat_restarts == 37 && installed.string_iterations == 65 &&
                installed.io_cycles == 0 && initialized.image().bytes[0] == 0xff &&
                initialized.image().bytes[127] == 0xff && initialized.image().bytes[128] == 0 &&
                initialized.image().bytes[259] == 0 && initialized.image().bytes[260] == 0x31 && initialized.throttle() == 4,
                "Cold installation must retain both REP clears, descriptor work and untouched scan/throttle aliases");
        KeyboardInput keyboard;
        LoadingClock clock;
        receive(keyboard, clock, 0xe0);
        require(clock.accounting().dispatches == 26 && clock.accounting().io_cycles == 85 && keyboard.take_scan() == 0,
                "An extended prefix must take the release path without entering the pending queue");
        const auto before = clock.accounting().dispatches;
        receive(keyboard, clock, 0x48);
        require(clock.accounting().dispatches - before == 43 && keyboard.held(0x48) && keyboard.take_press(0x48) &&
                !keyboard.take_press(0x48) && keyboard.take_scan() == 0x48,
                "A press must latch held, pressed and pending state through the original handler");
        receive(keyboard, clock, 0xc8);
        require(!keyboard.held(0x48) && keyboard.take_scan() == 0, "A key release must clear held without queueing a press");
        for (const auto scan : {0x1e, 0x30, 0x2e, 0x20, 0x12}) {
            receive(keyboard, clock, scan);
        }
        const auto image = keyboard.image();
        LoadingClock pending;
        require(keyboard.take_scan(pending) == 0x12 && pending.accounting().dispatches == 4 &&
                keyboard.take_scan() == 0x20 && keyboard.take_scan() == 0x2e &&
                keyboard.take_scan() == 0x30 && keyboard.take_scan() == 0 &&
                image.bytes[260] == 0x1e && keyboard.image().bytes[260] == 0x1e,
                "The fifth overlapping byte must survive four-byte pending-scan consumption");
        for (unsigned index = 0; index < 20; ++index) { receive(keyboard, clock, 0x12); }
        require(keyboard.throttle() == 15, "Throttle up must saturate at the source E6 comparison");
        for (unsigned index = 0; index < 20; ++index) { receive(keyboard, clock, 0x20); }
        require(keyboard.throttle() == 0, "Throttle down must saturate at zero");
        receive(keyboard, clock, 0xff);
        require(!keyboard.held(0x12) && !keyboard.take_press(0x12) && keyboard.take_scan() == 0,
                "The FF response must clear the source held, pressed and four-byte queue ranges");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
