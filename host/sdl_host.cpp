#include "sdl_host.hpp"
#include "sdl_audio.hpp"
#include "sdl_common.hpp"
#include "sdl_controller.hpp"

#include <algorithm>
#include <sstream>

namespace comanchecd::host {
namespace {
constexpr std::int32_t keyboard_control = 350;
constexpr std::uint8_t maximum_throttle = 15;

std::uint8_t menu_scan(SDL_Scancode key) {
    constexpr std::array<std::uint8_t, 26> letters{
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
    if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z) {
        return letters[key - SDL_SCANCODE_A];
    }
    if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_0) {
        return static_cast<std::uint8_t>(2 + key - SDL_SCANCODE_1);
    }
    switch (key) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_BACKSPACE: return 0x0e;
    case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_RETURN: return 0x1c;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_UP: return 0x48;
    case SDL_SCANCODE_LEFT: return 0x4b;
    case SDL_SCANCODE_RIGHT: return 0x4d;
    case SDL_SCANCODE_DOWN: return 0x50;
    case SDL_SCANCODE_DELETE: return 0x53;
    case SDL_SCANCODE_MINUS: return 0x0c;
    case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    default: return 0;
    }
}

void key_bytes(std::vector<std::uint8_t>& bytes, SDL_Scancode key, bool down) {
    auto scan = menu_scan(key);
    if (scan == 0) {
        switch (key) {
        case SDL_SCANCODE_F1: case SDL_SCANCODE_F2: case SDL_SCANCODE_F3: case SDL_SCANCODE_F4:
        case SDL_SCANCODE_F5: case SDL_SCANCODE_F6: case SDL_SCANCODE_F7: case SDL_SCANCODE_F8:
        case SDL_SCANCODE_F9: case SDL_SCANCODE_F10: scan = 0x3b + key - SDL_SCANCODE_F1; break;
        case SDL_SCANCODE_F11: scan = 0x57; break;
        case SDL_SCANCODE_F12: scan = 0x58; break;
        case SDL_SCANCODE_LSHIFT: scan = 0x2a; break;
        case SDL_SCANCODE_RSHIFT: scan = 0x36; break;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: scan = 0x1d; break;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: scan = 0x38; break;
        default: break;
        }
    }
    if (scan == 0) { return; }
    if (key == SDL_SCANCODE_UP || key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT ||
            key == SDL_SCANCODE_DOWN || key == SDL_SCANCODE_DELETE || key == SDL_SCANCODE_RCTRL ||
            key == SDL_SCANCODE_RALT) {
        bytes.push_back(0xe0);
    }
    bytes.push_back(static_cast<std::uint8_t>(scan | (down ? 0 : 0x80)));
}

template<class T, void (*Destroy)(T*)>
using SdlPointer = std::unique_ptr<T, decltype(Destroy)>;
}

class SdlFlightHost::Impl {
public:
    explicit Impl(SdlHostOptions options) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        window.reset(SDL_CreateWindow("Comanche CD training preview", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      960, 720, SDL_WINDOW_RESIZABLE));
        if (!window) {
            throw sdl_error("SDL2 window creation failed");
        }
        renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
        if (!renderer) {
            renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
        }
        if (!renderer || SDL_RenderSetLogicalSize(renderer.get(), cockpit_width, cockpit_height) != 0) {
            throw sdl_error("SDL2 renderer creation failed");
        }
        texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                       cockpit_width, cockpit_height));
        if (!texture) {
            throw sdl_error("SDL2 texture creation failed");
        }
        if (options.input == InputProfile::enhanced) {
            controller = std::make_unique<SdlController>(options.controller_mappings);
        }
    }

    void flush_controller_keys(HostInput& input) {
        if (!controller) { return; }
        for (const auto& change : controller->take_keys()) {
            controller_held[change.key] = change.down;
            if (change.down || !held[change.key]) { key_bytes(input.scan_bytes, change.key, change.down); }
            if (change.down) {
                input.keys.push_back({menu_scan(change.key)});
                input.escape = input.escape || change.key == SDL_SCANCODE_ESCAPE;
            }
        }
    }

    HostInput poll() {
        HostInput input;
        flush_controller_keys(input);
        const bool flight_input = context == InputContext::flight || context == InputContext::training;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (controller) {
                controller->event(event, input, throttle);
                flush_controller_keys(input);
            }
            if (event.type == SDL_QUIT) {
                input.quit = true;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    input.quit = true;
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST || event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    for (std::size_t key = 0; key < held.size(); ++key) {
                        if (held[key] && !controller_held[key]) { key_bytes(input.scan_bytes, static_cast<SDL_Scancode>(key), false); }
                    }
                    held.fill(false);
                    if (controller) { controller->set_focus(false); flush_controller_keys(input); }
                    input.pause = PauseAction::pause;
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    if (controller) { controller->set_focus(true); }
                }
            } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const auto key = event.key.keysym.scancode;
                if (key < SDL_SCANCODE_UNKNOWN || key >= SDL_NUM_SCANCODES) {
                    continue;
                }
                held[key] = event.type == SDL_KEYDOWN;
                if (event.type == SDL_KEYDOWN || !controller_held[key]) {
                    key_bytes(input.scan_bytes, key, event.type == SDL_KEYDOWN);
                }
                if (event.type == SDL_KEYUP) {
                    continue;
                }
                input.key_pressed = true;
                const auto scan = menu_scan(key);
                const bool modified = (event.key.keysym.mod & (KMOD_ALT | KMOD_CTRL)) != 0;
                if (flight_input && key == SDL_SCANCODE_A && (event.key.keysym.mod & KMOD_ALT) != 0 && event.key.repeat == 0) {
                    input.abort_mission = true;
                }
                if (scan != 0) {
                    input.keys.push_back({scan, (event.key.keysym.mod & KMOD_SHIFT) != 0 ? PilotShift::both : PilotShift::none});
                }
                if (key == SDL_SCANCODE_ESCAPE) {
                    input.escape = true;
                }
                if (key == SDL_SCANCODE_P && !modified && context != InputContext::name_entry &&
                        event.key.repeat == 0 && input.pause != PauseAction::pause) {
                    input.pause = PauseAction::toggle;
                }
                if (key == SDL_SCANCODE_R && context == InputContext::training && !modified && event.key.repeat == 0) {
                    input.reset = true;
                }
                if (!flight_input || modified) {
                    continue;
                }
                if (key == SDL_SCANCODE_E) {
                    throttle = std::min<unsigned>(throttle + 1, maximum_throttle);
                } else if (key == SDL_SCANCODE_D && throttle > 0) {
                    --throttle;
                } else if (key == SDL_SCANCODE_LEFTBRACKET && context == InputContext::training) {
                    input.controls.weapon = WeaponSelection::previous;
                } else if (key == SDL_SCANCODE_RIGHTBRACKET && context == InputContext::training) {
                    input.controls.weapon = WeaponSelection::next;
                } else if (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F6) {
                    displays.left = static_cast<InstrumentDisplay>(key - SDL_SCANCODE_F1);
                } else if (key >= SDL_SCANCODE_F7 && key <= SDL_SCANCODE_F12) {
                    displays.right = static_cast<InstrumentDisplay>(key - SDL_SCANCODE_F7);
                }
            }
        }
        if (controller && input.pause != PauseAction::none) {
            controller->reset();
            flush_controller_keys(input);
            input.pressed = {};
        }
        if (flight_input) {
            input.controls.horizontal = keyboard_control * (int(held[SDL_SCANCODE_RIGHT]) - int(held[SDL_SCANCODE_LEFT]));
            input.controls.vertical = keyboard_control * (int(held[SDL_SCANCODE_DOWN]) - int(held[SDL_SCANCODE_UP]));
            input.controls.throttle = throttle;
            input.controls.fire = held[SDL_SCANCODE_SPACE];
            input.controls.cycle_target = held[SDL_SCANCODE_TAB] || held[SDL_SCANCODE_RETURN];
            input.controls.chaff = context == InputContext::training && held[SDL_SCANCODE_SEMICOLON];
            input.controls.flare = context == InputContext::training && held[SDL_SCANCODE_APOSTROPHE];
            input.cockpit = displays;
        }
        if (controller) { controller->sample(input); }
        return input;
    }

    SdlSubsystem subsystem{SDL_INIT_VIDEO | SDL_INIT_EVENTS};
    SdlPointer<SDL_Window, SDL_DestroyWindow> window{nullptr, SDL_DestroyWindow};
    SdlPointer<SDL_Renderer, SDL_DestroyRenderer> renderer{nullptr, SDL_DestroyRenderer};
    SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr, SDL_DestroyTexture};
    SdlAudioOutput audio;
    std::unique_ptr<SdlController> controller;
    std::array<bool, SDL_NUM_SCANCODES> held{};
    std::array<bool, SDL_NUM_SCANCODES> controller_held{};
    InputContext context = InputContext::flight;
    std::uint8_t throttle = 0;
    CockpitControls displays;
    std::array<std::uint32_t, cockpit_width * cockpit_height> pixels{};
};

SdlFlightHost::SdlFlightHost(SdlHostOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
SdlFlightHost::~SdlFlightHost() = default;
HostInput SdlFlightHost::poll() { return impl_->poll(); }
void SdlFlightHost::set_input_context(InputContext context) {
    impl_->context = context;
    if (impl_->controller) { impl_->controller->set_context(context); }
}

void SdlFlightHost::present(const CockpitImage& image, const FlightState& state, const std::string& status) {
    std::array<std::uint32_t, 256> palette{};
    for (std::size_t index = 0; index < palette.size(); ++index) {
        const auto& color = image.palette[index];
        palette[index] = 0xff000000u | ((color[0] * 255u / 63u) << 16) |
                         ((color[1] * 255u / 63u) << 8) | (color[2] * 255u / 63u);
    }
    std::transform(image.pixels.begin(), image.pixels.end(), impl_->pixels.begin(), [&](auto index) { return palette[index]; });
    if (SDL_UpdateTexture(impl_->texture.get(), nullptr, impl_->pixels.data(), cockpit_width * sizeof(std::uint32_t)) != 0 ||
            SDL_RenderClear(impl_->renderer.get()) != 0 ||
            SDL_RenderCopy(impl_->renderer.get(), impl_->texture.get(), nullptr, nullptr) != 0) {
        throw sdl_error("SDL2 scene presentation failed");
    }
    SDL_RenderPresent(impl_->renderer.get());
    std::ostringstream title;
    title << "Comanche CD | " << status << " | throttle " << unsigned(impl_->throttle)
          << "/15 | height " << unsigned(state.view.height) << " | armor " << state.armor
          << " | weapon " << unsigned(state.weapon + 1) << " | ammo " << state.ammunition[state.weapon];
    const auto text = title.str();
    if (text != SDL_GetWindowTitle(impl_->window.get())) {
        SDL_SetWindowTitle(impl_->window.get(), text.c_str());
    }
}
void SdlFlightHost::queue_audio(const std::vector<std::int16_t>& samples) { impl_->audio.queue(samples); }
std::size_t SdlFlightHost::queued_audio_samples() const { return impl_->audio.queued_samples(); }
void SdlFlightHost::clear_audio() { impl_->audio.clear(); }
void SdlFlightHost::set_playback(Playback playback) { impl_->audio.set_playback(playback); }
void SdlFlightHost::idle() { SDL_Delay(1); }
void SdlFlightHost::reset_controls() {
    if (impl_->controller) { impl_->controller->reset(); }
    impl_->held.fill(false);
    impl_->throttle = 0;
    impl_->displays = {};
}
} // namespace comanchecd::host
