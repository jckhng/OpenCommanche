#include "sdl_controller.hpp"

#include <algorithm>
#include <cstdlib>

namespace comanchecd::host {
namespace {
constexpr int stick_deadzone = 8000;
constexpr int trigger_deadzone = 3276;
constexpr int steering_limit = 350;
constexpr std::uint8_t maximum_throttle = 15;

int steering(Sint16 value) {
    const int magnitude = std::abs(int(value));
    if (magnitude <= stick_deadzone) { return 0; }
    const auto extent = value < 0 ? 32768 : 32767;
    const int scaled = (magnitude - stick_deadzone) * steering_limit / (extent - stick_deadzone);
    return value < 0 ? -scaled : scaled;
}

SDL_Scancode menu_key(Uint8 button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return SDL_SCANCODE_RETURN;
    case SDL_CONTROLLER_BUTTON_B: return SDL_SCANCODE_ESCAPE;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return SDL_SCANCODE_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return SDL_SCANCODE_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return SDL_SCANCODE_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return SDL_SCANCODE_RIGHT;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}
}

SdlController::SdlController(const std::string& mappings) {
    if (!mappings.empty() && SDL_GameControllerAddMappingsFromFile(mappings.c_str()) < 0) {
        throw sdl_error("SDL2 controller mappings failed");
    }
    open_available();
}

SdlController::~SdlController() {
    if (controller_) { SDL_GameControllerClose(controller_); }
}

void SdlController::open_available() {
    if (controller_) { return; }
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (!SDL_IsGameController(index)) { continue; }
        controller_ = SDL_GameControllerOpen(index);
        if (!controller_) { continue; }
        instance_ = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller_));
        for (unsigned button = 0; button < held_.size(); ++button) {
            held_[button] = SDL_GameControllerGetButton(controller_, static_cast<SDL_GameControllerButton>(button)) != 0;
        }
        reset();
        return;
    }
}

bool SdlController::flight_input() const { return context_ == InputContext::flight || context_ == InputContext::training; }

void SdlController::release_keys() {
    for (auto& key : menu_keys_) {
        if (key != SDL_SCANCODE_UNKNOWN) { keys_.push_back({key, false}); key = SDL_SCANCODE_UNKNOWN; }
    }
}

void SdlController::reset() {
    release_keys();
    suppressed_ = held_;
    axes_armed_ = false;
    trigger_down_ = false;
}

void SdlController::set_context(InputContext context) {
    if (context == context_) { return; }
    reset();
    context_ = context;
}

void SdlController::set_focus(bool focused) { focused_ = focused; reset(); }

std::vector<ControllerKey> SdlController::take_keys() {
    auto keys = std::move(keys_);
    keys_.clear();
    return keys;
}

void SdlController::event(const SDL_Event& event, HostInput& input, std::uint8_t& throttle) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) { open_available(); return; }
    if (controller_ && event.type == SDL_CONTROLLERDEVICEREMOVED && event.cdevice.which == instance_) {
        reset();
        SDL_GameControllerClose(controller_);
        controller_ = nullptr;
        instance_ = -1;
        held_.fill(false);
        suppressed_.fill(false);
        if (flight_input()) { input.pause = PauseAction::pause; }
        open_available();
        return;
    }
    if (!controller_ || (event.type != SDL_CONTROLLERBUTTONDOWN && event.type != SDL_CONTROLLERBUTTONUP)) { return; }
    if (event.cbutton.which != instance_) { return; }
    const auto button = event.cbutton.button;
    if (button >= held_.size()) { return; }
    const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
    const bool previous = held_[button];
    held_[button] = down;
    if (!down) {
        suppressed_[button] = false;
        if (menu_keys_[button] != SDL_SCANCODE_UNKNOWN) {
            keys_.push_back({menu_keys_[button], false});
            menu_keys_[button] = SDL_SCANCODE_UNKNOWN;
        }
        return;
    }
    if (previous || suppressed_[button] || !focused_) { return; }
    input.key_pressed = true;
    if (button == SDL_CONTROLLER_BUTTON_START && context_ != InputContext::name_entry) {
        if (input.pause != PauseAction::pause) { input.pause = PauseAction::toggle; }
        return;
    }
    const auto key = flight_input() ?
        (button == SDL_CONTROLLER_BUTTON_BACK ? SDL_SCANCODE_ESCAPE : SDL_SCANCODE_UNKNOWN) : menu_key(button);
    if (key != SDL_SCANCODE_UNKNOWN) {
        menu_keys_[button] = key;
        keys_.push_back({key, true});
        return;
    }
    if (!flight_input()) { return; }
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: input.pressed.fire = true; break;
    case SDL_CONTROLLER_BUTTON_Y: input.pressed.target = true; break;
    case SDL_CONTROLLER_BUTTON_X: input.pressed.chaff = true; break;
    case SDL_CONTROLLER_BUTTON_B: input.pressed.flare = true; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: input.controls.weapon = WeaponSelection::previous; break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: input.controls.weapon = WeaponSelection::next; break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: throttle = std::min<unsigned>(throttle + 1, maximum_throttle); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (throttle) { --throttle; } break;
    default: break;
    }
}

void SdlController::sample(HostInput& input) {
    if (!controller_ || !focused_ || !flight_input()) { return; }
    const auto x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
    const auto y = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);
    const auto trigger = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (!axes_armed_) {
        axes_armed_ = std::abs(int(x)) <= stick_deadzone && std::abs(int(y)) <= stick_deadzone && trigger <= trigger_deadzone;
    }
    if (axes_armed_) {
        if (input.controls.horizontal == 0) { input.controls.horizontal = steering(x); }
        if (input.controls.vertical == 0) { input.controls.vertical = steering(y); }
        const bool trigger_down = trigger > trigger_deadzone;
        input.pressed.fire = input.pressed.fire || (trigger_down && !trigger_down_);
        input.controls.fire = input.controls.fire || trigger_down;
        trigger_down_ = trigger_down;
    }
    input.controls.fire = input.controls.fire || (held_[SDL_CONTROLLER_BUTTON_A] && !suppressed_[SDL_CONTROLLER_BUTTON_A]);
}
}
