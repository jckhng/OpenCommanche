#pragma once

#include "flight_host.hpp"
#include "sdl_common.hpp"

namespace comanchecd::host {
struct ControllerKey {
    SDL_Scancode key;
    bool down;
};

class SdlController {
public:
    explicit SdlController(const std::string& mappings);
    ~SdlController();
    void event(const SDL_Event& event, HostInput& input, std::uint8_t& throttle);
    void sample(HostInput& input);
    void set_context(InputContext context);
    void set_focus(bool focused);
    void reset();
    std::vector<ControllerKey> take_keys();
private:
    void open_available();
    bool flight_input() const;
    void release_keys();
    SdlSubsystem subsystem_{SDL_INIT_GAMECONTROLLER};
    SDL_GameController* controller_ = nullptr;
    SDL_JoystickID instance_ = -1;
    InputContext context_ = InputContext::flight;
    bool focused_ = true, axes_armed_ = false, trigger_down_ = false;
    std::array<bool, SDL_CONTROLLER_BUTTON_MAX> held_{}, suppressed_{};
    std::array<SDL_Scancode, SDL_CONTROLLER_BUTTON_MAX> menu_keys_{};
    std::vector<ControllerKey> keys_;
};
}
