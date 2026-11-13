#include "sdl_host.hpp"
#include "sdl_common.hpp"

#include <algorithm>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

void keyboard(Uint32 type, SDL_Scancode key) {
    SDL_Event event{};
    event.type = type;
    event.key.keysym.scancode = key;
    require(SDL_PushEvent(&event) == 1, "Keyboard fixture must enter the SDL2 queue");
}
}

int main() {
#if !SDL_VERSION_ATLEAST(2, 0, 14)
    std::cout << "Virtual controller qualification requires SDL2 2.0.14 or later\n";
    return 77;
#else
    try {
        using namespace comanchecd;
        using namespace comanchecd::host;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SdlSubsystem subsystem(SDL_INIT_GAMECONTROLLER);
        for (int index = 0; index < SDL_NumJoysticks(); ++index) {
            if (SDL_IsGameController(index)) {
                std::cout << "Virtual controller isolation requires a process without another active game controller\n";
                return 77;
            }
        }
        const auto device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        require(device >= 0, "Virtual SDL2 game controller must attach");
        std::unique_ptr<SDL_Joystick, decltype(&SDL_JoystickClose)> joystick(SDL_JoystickOpen(device), SDL_JoystickClose);
        require(bool(joystick), "Virtual joystick must open");
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick.get()), guid, sizeof(guid));
        const auto mapping = std::string(guid) + ",Comanche test controller,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6," +
            "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,righttrigger:a5,";
        require(SDL_GameControllerAddMapping(mapping.c_str()) >= 0, "Virtual controller mapping must install");
        require(SDL_JoystickSetVirtualAxis(joystick.get(), SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768) == 0,
                "Trigger must begin released");
        auto host = std::make_unique<SdlFlightHost>(SdlHostOptions{InputProfile::enhanced, {}});
        host->poll();
        const auto button = [&](SDL_GameControllerButton key, bool down) {
            require(SDL_JoystickSetVirtualButton(joystick.get(), key, down ? 1 : 0) == 0, "Virtual button must update");
            return host->poll();
        };
        host->set_input_context(InputContext::menu);
        host->poll();
        auto input = button(SDL_CONTROLLER_BUTTON_A, true);
        require(input.scan_bytes == std::vector<std::uint8_t>{0x1c} && !input.controls.fire,
                "Controller A must send an IRQ-bound Enter in menus");
        keyboard(SDL_KEYDOWN, SDL_SCANCODE_RETURN);
        host->poll();
        input = button(SDL_CONTROLLER_BUTTON_A, false);
        require(std::find(input.scan_bytes.begin(), input.scan_bytes.end(), 0x9c) == input.scan_bytes.end(),
                "Controller release must preserve a physically held keyboard Enter");
        keyboard(SDL_KEYUP, SDL_SCANCODE_RETURN);
        require(host->poll().scan_bytes == std::vector<std::uint8_t>{0x9c}, "The final Enter owner must send its release");
        button(SDL_CONTROLLER_BUTTON_A, true);
        host->set_input_context(InputContext::flight);
        require(!host->poll().controls.fire, "Accept held across mission entry must remain suppressed until release");
        button(SDL_CONTROLLER_BUTTON_A, false);
        input = button(SDL_CONTROLLER_BUTTON_A, true);
        require(input.controls.fire && input.pressed.fire, "Fresh flight fire must provide held and buffered input");
        button(SDL_CONTROLLER_BUTTON_A, false);
        require(button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true).controls.weapon == WeaponSelection::next,
                "Shoulder press must select the next weapon");
        require(host->poll().controls.weapon == WeaponSelection::unchanged, "Weapon selection must remain transient");
        button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
        require(button(SDL_CONTROLLER_BUTTON_Y, true).pressed.target, "Y must buffer target acquisition");
        button(SDL_CONTROLLER_BUTTON_Y, false);
        require(button(SDL_CONTROLLER_BUTTON_DPAD_UP, true).controls.throttle == 1, "D-pad Up must increase retained throttle");
        button(SDL_CONTROLLER_BUTTON_DPAD_UP, false);
        require(SDL_JoystickSetVirtualAxis(joystick.get(), SDL_CONTROLLER_AXIS_LEFTX, 32767) == 0, "Steering axis must update");
        require(SDL_JoystickSetVirtualAxis(joystick.get(), SDL_CONTROLLER_AXIS_LEFTY, -32768) == 0, "Pitch axis must update");
        input = host->poll();
        require(input.controls.horizontal == 350 && input.controls.vertical == -350,
                "Analog extrema must stay within the recovered steering range");
        SDL_Event focus{};
        focus.type = SDL_WINDOWEVENT;
        focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        SDL_PushEvent(&focus);
        input = host->poll();
        require(input.pause == PauseAction::pause && input.controls.horizontal == 0 && !input.controls.fire,
                "Focus loss must pause and neutralize controller input");
        focus.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
        SDL_PushEvent(&focus);
        require(host->poll().controls.horizontal == 0, "Returning focus must require neutral axes before steering resumes");
        SDL_JoystickSetVirtualAxis(joystick.get(), SDL_CONTROLLER_AXIS_LEFTX, 0);
        SDL_JoystickSetVirtualAxis(joystick.get(), SDL_CONTROLLER_AXIS_LEFTY, 0);
        host->poll();
        SDL_JoystickSetVirtualAxis(joystick.get(), SDL_CONTROLLER_AXIS_LEFTX, 7999);
        require(host->poll().controls.horizontal == 0, "Stick deadzone must suppress drift");
        SDL_JoystickSetVirtualAxis(joystick.get(), SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
        input = host->poll();
        require(input.controls.fire && input.pressed.fire, "Trigger crossing must provide held and buffered fire");
        require(SDL_JoystickDetachVirtual(device) == 0, "Virtual controller must detach");
        input = host->poll();
        require(input.pause == PauseAction::pause && !input.controls.fire && input.controls.horizontal == 0,
                "Disconnect must release actions and pause the active flight");
        const auto replacement = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        require(replacement >= 0, "Replacement controller must hot-plug");
        std::unique_ptr<SDL_Joystick, decltype(&SDL_JoystickClose)> replacement_joystick(
            SDL_JoystickOpen(replacement), SDL_JoystickClose);
        require(bool(replacement_joystick), "Replacement controller must open");
        SDL_JoystickSetVirtualAxis(replacement_joystick.get(), SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        host->poll();
        SDL_JoystickSetVirtualButton(replacement_joystick.get(), SDL_CONTROLLER_BUTTON_A, 1);
        require(host->poll().controls.fire, "Hot-plugged controller must accept a fresh flight press");
        host.reset();
        {
            SdlFlightHost faithful;
            faithful.poll();
            SDL_JoystickSetVirtualButton(replacement_joystick.get(), SDL_CONTROLLER_BUTTON_A, 0);
            faithful.poll();
            SDL_JoystickSetVirtualButton(replacement_joystick.get(), SDL_CONTROLLER_BUTTON_A, 1);
            input = faithful.poll();
            require(!input.controls.fire && !input.pressed.fire && input.scan_bytes.empty(),
                    "Faithful input must leave enhanced controller routing disabled");
        }
        require(SDL_JoystickDetachVirtual(replacement) == 0, "Replacement virtual controller must detach");
        std::cout << "Virtual SDL2 controller routing and lifecycle passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#endif
}
