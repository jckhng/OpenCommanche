#include "../host/presentation_mailbox.hpp"
#include "../host/pending_input.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

int main() {
    try {
        using namespace comanchecd;
        host::PendingFlightInput pending;
        host::HostInput tap;
        tap.pressed = {true, true, true, true};
        tap.controls.weapon = WeaponSelection::next;
        pending.queue(tap);
        pending.queue({});
        const auto buffered = pending.apply({});
        if (!buffered.fire || !buffered.cycle_target || !buffered.chaff || !buffered.flare || buffered.weapon != WeaponSelection::next) {
            throw std::runtime_error("Short enhanced presses must survive polls until one simulation tick consumes them");
        }
        const auto consumed = pending.apply({});
        if (consumed.fire || consumed.cycle_target || consumed.chaff || consumed.flare || consumed.weapon != WeaponSelection::unchanged) {
            throw std::runtime_error("Buffered enhanced presses must be consumed once");
        }
        pending.queue(tap);
        pending.clear_presses();
        if (pending.apply({}).fire) { throw std::runtime_error("Pause must discard buffered enhanced presses"); }
        pending.queue(tap);
        pending.clear();
        if (pending.apply({}).weapon != WeaponSelection::unchanged) { throw std::runtime_error("A mode transition must clear pending actions"); }
        host::PresentationMailbox mailbox;
        if (mailbox.read()) { throw std::runtime_error("An empty presentation mailbox must have no frame"); }
        auto initial = std::make_shared<SessionPresentation>();
        initial->cpu_cycles = 1;
        initial->image.pixels.fill(1);
        mailbox.publish(initial);
        initial.reset();
        const auto held = mailbox.read();
        static_assert(std::is_const_v<std::remove_reference_t<decltype(*held)>>);
        std::thread producer([&] {
            for (unsigned index = 2; index < 100; ++index) {
                auto frame = std::make_shared<SessionPresentation>();
                frame->cpu_cycles = index;
                frame->image.pixels.fill(static_cast<std::uint8_t>(index));
                mailbox.publish(std::move(frame));
            }
        });
        for (unsigned read = 0; read < 100; ++read) {
            const auto latest = mailbox.read();
            if (latest->image.pixels.front() != latest->cpu_cycles || latest->image.pixels.back() != latest->cpu_cycles) {
                producer.join();
                throw std::runtime_error("A published frame must retain coherent owned image/state");
            }
        }
        producer.join();
        if (held->cpu_cycles != 1 || held->image.pixels.back() != 1 || mailbox.read()->cpu_cycles != 99) {
            throw std::runtime_error("Both eyes must retain the held frame while newer publications replace the mailbox");
        }
        mailbox.publish({});
        if (mailbox.read() || held->image.pixels.front() != 1) {
            throw std::runtime_error("Clearing the mailbox must preserve an outstanding consumer frame");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
