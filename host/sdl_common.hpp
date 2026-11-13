#pragma once

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdexcept>
#include <string>

namespace comanchecd::host {
inline std::runtime_error sdl_error(const char* operation) {
    return std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

class SdlSubsystem {
public:
    explicit SdlSubsystem(Uint32 flags) : flags_(flags) {
        SDL_SetMainReady();
        if (SDL_InitSubSystem(flags_) != 0) {
            throw sdl_error("SDL2 initialization failed");
        }
    }
    ~SdlSubsystem() { SDL_QuitSubSystem(flags_); }
    SdlSubsystem(const SdlSubsystem&) = delete;
    SdlSubsystem& operator=(const SdlSubsystem&) = delete;

private:
    Uint32 flags_;
};
} // namespace comanchecd::host
