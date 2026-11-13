#pragma once
#include "comanchecd/keyboard.hpp"

namespace comanchecd {
std::uint8_t poll_list_key(const KeyboardInput& keyboard, ExecutionClock& clock);
}
