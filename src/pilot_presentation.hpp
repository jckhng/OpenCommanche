#pragma once

#include "comanchecd/pilot.hpp"

namespace comanchecd::detail {

void present_pilot(PilotUpdate update, const PilotController& controller, MenuRenderer& renderer,
                   const PilotWriter& writer, const PilotObserver& observer);

}
