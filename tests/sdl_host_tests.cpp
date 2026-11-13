#include "sdl_host.hpp"
#include "sdl_audio.hpp"
#include "sdl_common.hpp"

#include <algorithm>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void key(Uint32 type, SDL_Scancode code, Uint8 repeat = 0, Uint16 modifiers = 0) {
    SDL_Event event{};
    event.type = type;
    event.key.keysym.scancode = code;
    event.key.repeat = repeat;
    event.key.keysym.mod = modifiers;
    require(SDL_PushEvent(&event) == 1, "Synthetic key must enter the SDL2 event queue");
}
}

int main() {
    try {
        using namespace comanchecd::host;
        {
            SdlFlightHost host;
            host.poll();
            key(SDL_KEYDOWN, SDL_SCANCODE_UP);
            key(SDL_KEYDOWN, SDL_SCANCODE_RIGHT);
            key(SDL_KEYDOWN, SDL_SCANCODE_E);
            key(SDL_KEYDOWN, SDL_SCANCODE_SPACE);
            auto input = host.poll();
            require(input.scan_bytes == std::vector<std::uint8_t>{0xe0, 0x48, 0xe0, 0x4d, 0x12, 0x39},
                    "SDL2 events must preserve E0 prefixes and raw byte order for the shared IRQ clock");
            require(input.controls.horizontal == 350 && input.controls.vertical == -350 && input.controls.throttle == 1 &&
                    input.controls.fire, "SDL2 held keys must map to the recovered flight control range");
            require(host.poll().controls.fire, "Held fire must persist across event polls");
            key(SDL_KEYDOWN, SDL_SCANCODE_LEFT);
            key(SDL_KEYUP, SDL_SCANCODE_SPACE);
            require(host.poll().controls.horizontal == 0 && !host.poll().controls.fire, "Opposite steering must cancel and key-up must release fire");
            for (unsigned count = 0; count < 20; ++count) {
                key(SDL_KEYDOWN, SDL_SCANCODE_E, 1);
            }
            require(host.poll().controls.throttle == 15, "Throttle must saturate at the original maximum");
            key(SDL_KEYDOWN, SDL_SCANCODE_P, 1);
            require(host.poll().pause == PauseAction::none, "Pause must ignore key repeats");
            key(SDL_KEYDOWN, SDL_SCANCODE_P);
            require(host.poll().pause == PauseAction::toggle, "Pause key must toggle once");
            SDL_Event focus{};
            focus.type = SDL_WINDOWEVENT;
            focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            SDL_PushEvent(&focus);
            input = host.poll();
            require(input.pause == PauseAction::pause && input.controls.horizontal == 0 && input.controls.vertical == 0 &&
                    !input.controls.fire, "Focus loss must pause and release held controls");
            key(SDL_KEYDOWN, SDL_SCANCODE_R);
            input = host.poll();
            require(!input.reset && input.controls.throttle == 15,
                    "R in the integrated flight context must preserve throttle and controls");
            host.reset_controls();
            key(SDL_KEYDOWN, SDL_SCANCODE_F4);
            key(SDL_KEYDOWN, SDL_SCANCODE_F11);
            input = host.poll();
            require(input.cockpit.left == comanchecd::InstrumentDisplay::objectives &&
                    input.cockpit.right == comanchecd::InstrumentDisplay::systems &&
                    input.scan_bytes == std::vector<std::uint8_t>{0x3e, 0x57},
                    "Function keys must select the two cockpit display programs independently");
            require(input.key_pressed && !host.poll().key_pressed,
                    "Unmapped menu keys must expose a single event for cancelling demo playback");
            require(host.poll().cockpit.right == comanchecd::InstrumentDisplay::systems,
                    "Instrument selection must persist between input polls");
            key(SDL_KEYDOWN, SDL_SCANCODE_R);
            input = host.poll();
            require(!input.reset && input.cockpit.left == comanchecd::InstrumentDisplay::objectives &&
                    input.cockpit.right == comanchecd::InstrumentDisplay::systems,
                    "Integrated R must preserve both instrument programs");
            host.set_input_context(InputContext::training);
            key(SDL_KEYDOWN, SDL_SCANCODE_R, 1);
            require(!host.poll().reset, "Training reset must ignore key repeats");
            key(SDL_KEYDOWN, SDL_SCANCODE_R);
            require(host.poll().reset, "Training R must request a reset from its controller");
            host.reset_controls();
            input = host.poll();
            require(input.cockpit.left == comanchecd::InstrumentDisplay::map &&
                    input.cockpit.right == comanchecd::InstrumentDisplay::camera,
                    "Reset must restore the original instrument selections");
            host.set_input_context(InputContext::name_entry);
            key(SDL_KEYDOWN, SDL_SCANCODE_LSHIFT, 0, KMOD_SHIFT);
            for (const auto code : {SDL_SCANCODE_R, SDL_SCANCODE_P, SDL_SCANCODE_E, SDL_SCANCODE_D, SDL_SCANCODE_F4}) {
                key(SDL_KEYDOWN, code, 0, KMOD_SHIFT);
            }
            input = host.poll();
            require(!input.reset && input.pause == PauseAction::none && input.controls.throttle == 0 &&
                    input.cockpit.left == comanchecd::InstrumentDisplay::map,
                    "Name entry must preserve raw letters without flight shortcuts");
            key(SDL_KEYDOWN, SDL_SCANCODE_A, 0, KMOD_ALT);
            require(!host.poll().abort_mission, "Name entry must not activate mission abort");
            SDL_PushEvent(&focus);
            input = host.poll();
            require(std::find(input.scan_bytes.begin(), input.scan_bytes.end(), 0xaa) != input.scan_bytes.end(),
                    "R during name entry must retain held Shift for focus-loss release");
            host.set_input_context(InputContext::menu);
            key(SDL_KEYDOWN, SDL_SCANCODE_E);
            key(SDL_KEYDOWN, SDL_SCANCODE_F4);
            require(host.poll().controls.throttle == 0, "Menu letters must not change flight throttle");
            host.set_input_context(InputContext::flight);
            key(SDL_KEYDOWN, SDL_SCANCODE_E, 0, KMOD_ALT);
            input = host.poll();
            require(input.controls.throttle == 0 && input.cockpit.left == comanchecd::InstrumentDisplay::map,
                    "Menu and modified flight keys must not change retained throttle or displays");
            key(SDL_KEYDOWN, SDL_SCANCODE_F4, 0, KMOD_CTRL);
            key(SDL_KEYDOWN, SDL_SCANCODE_P, 0, KMOD_CTRL);
            input = host.poll();
            require(input.cockpit.left == comanchecd::InstrumentDisplay::map && input.pause == PauseAction::none,
                    "Modified display and pause keys must not activate ordinary commands");
            key(SDL_KEYDOWN, SDL_SCANCODE_D);
            require(host.poll().controls.throttle == 0, "Throttle must saturate at zero");
            key(SDL_KEYDOWN, SDL_SCANCODE_TAB);
            input = host.poll();
            require(input.controls.cycle_target && input.keys.back().scan == 0x0f,
                    "Tab must reach both the menu scan queue and flight targeting");
            host.reset_controls();
            require(!host.poll().controls.cycle_target, "A mission transition must release held target input");
            key(SDL_KEYDOWN, SDL_SCANCODE_LEFTBRACKET);
            key(SDL_KEYDOWN, SDL_SCANCODE_SEMICOLON);
            key(SDL_KEYDOWN, SDL_SCANCODE_APOSTROPHE);
            input = host.poll();
            require(input.scan_bytes == std::vector<std::uint8_t>{0x1a, 0x27, 0x28} &&
                    input.controls.weapon == comanchecd::WeaponSelection::unchanged &&
                    !input.controls.chaff && !input.controls.flare,
                    "Integrated keyboard actions must enter once through the pending scan queue");
            host.reset_controls();
            host.set_input_context(InputContext::training);
            key(SDL_KEYDOWN, SDL_SCANCODE_LEFTBRACKET);
            require(host.poll().controls.weapon == comanchecd::WeaponSelection::previous &&
                    host.poll().controls.weapon == comanchecd::WeaponSelection::unchanged,
                    "Previous weapon is a transient command, not a held control");
            key(SDL_KEYDOWN, SDL_SCANCODE_RIGHTBRACKET);
            require(host.poll().controls.weapon == comanchecd::WeaponSelection::next,
                    "Right bracket selects the next weapon");
            host.set_input_context(InputContext::flight);
            key(SDL_KEYDOWN, SDL_SCANCODE_A, 0, KMOD_ALT);
            require(host.poll().abort_mission, "Alt-A must request a mission abort");
            host.set_input_context(InputContext::training);
            key(SDL_KEYDOWN, SDL_SCANCODE_SEMICOLON);
            key(SDL_KEYDOWN, SDL_SCANCODE_APOSTROPHE);
            input = host.poll();
            require(input.controls.chaff && input.controls.flare, "Countermeasure presses must reach live flight controls");
            key(SDL_KEYUP, SDL_SCANCODE_SEMICOLON);
            key(SDL_KEYUP, SDL_SCANCODE_APOSTROPHE);
            input = host.poll();
            require(!input.controls.chaff && !input.controls.flare, "Countermeasure release must clear both held requests");

            comanchecd::CockpitImage image;
            image.palette[1] = {63, 0, 0};
            image.palette[2] = {0, 0, 63};
            for (std::size_t y = 0; y < comanchecd::cockpit_height; ++y) {
                for (std::size_t x = 0; x < comanchecd::cockpit_width; ++x) {
                    image.pixels[y * comanchecd::cockpit_width + x] = x < comanchecd::cockpit_width / 2 ? 1 : 2;
                }
            }
            auto* window = SDL_GetWindowFromID(1);
            require(window != nullptr, "The test owns the first SDL2 window");
            auto* renderer = SDL_GetRenderer(window);
            int logical_width = 0, logical_height = 0;
            SDL_RenderGetLogicalSize(renderer, &logical_width, &logical_height);
            require(logical_width == 320 && logical_height == 240,
                    "SDL2 must present the complete cockpit at its original logical aspect");
            std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> target(
                SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 960, 720), SDL_DestroyTexture);
            require(target && SDL_SetRenderTarget(renderer, target.get()) == 0 &&
                    SDL_RenderSetLogicalSize(renderer, logical_width, logical_height) == 0,
                    "The presentation fixture must retain pixels independently of the window swapchain");
            host.present(image, {}, "Synthetic test");
            const SDL_Rect left{120, 320, 1, 1}, right{840, 320, 1, 1};
            std::uint32_t red = 0, blue = 0;
            require(SDL_RenderReadPixels(renderer, &left, SDL_PIXELFORMAT_ARGB8888, &red, sizeof(red)) == 0 &&
                    SDL_RenderReadPixels(renderer, &right, SDL_PIXELFORMAT_ARGB8888, &blue, sizeof(blue)) == 0 &&
                    red == 0xffff0000u && blue == 0xff0000ffu,
                    "SDL2 presentation must expand the original six-bit palette and preserve orientation");
            require(SDL_SetRenderTarget(renderer, nullptr) == 0 &&
                    SDL_RenderSetLogicalSize(renderer, logical_width, logical_height) == 0,
                    "The fixture must restore the window render target and logical size");
            host.present(image, {}, "Synthetic test");
            const std::vector<std::int16_t> samples(comanchecd::audio_sample_rate / 10, 100);
            host.set_playback(Playback::paused);
            host.queue_audio(samples);
            require(host.queued_audio_samples() == samples.size(), "Paused audio must retain the complete queued block");
            host.set_playback(Playback::running);
            SDL_Delay(150);
            require(host.queued_audio_samples() == 0, "The SDL2 device must consume streamed mono PCM");
            host.set_playback(Playback::paused);
            host.queue_audio(samples);
            host.clear_audio();
            require(host.queued_audio_samples() == 0, "Pause and reset can discard pending audio");
            key(SDL_KEYDOWN, SDL_SCANCODE_ESCAPE);
            input = host.poll();
            require(input.escape && !input.quit && input.keys.back().scan == 1,
                    "Escape must reach the active screen without closing the application window");
            SDL_Event quit{};
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
            require(host.poll().quit, "Window close must quit the viewer");
        }
        SdlAudioOutput audition;
        const std::vector<std::int16_t> short_samples(128);
        audition.set_playback(Playback::running);
        audition.queue(short_samples);
        SDL_Delay(40);
        require(audition.queued_samples() == short_samples.size(),
                "Starting playback must retain a short first block until the audio prebuffer fills");
        audition.clear();
        audition.queue(short_samples);
        SDL_Delay(40);
        require(audition.queued_samples() == short_samples.size(),
                "Clearing active audio must rearm prebuffering before the next block plays");
        audition.play_blocking(short_samples);
        require(audition.queued_samples() == 0, "A short blocking audition must drain below the prebuffer threshold");
        audition.play_blocking(std::vector<std::int16_t>(2205));
        require(audition.queued_samples() == 0, "Blocking audition must drain before returning");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
