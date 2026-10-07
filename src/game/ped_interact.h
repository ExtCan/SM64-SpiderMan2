// Pedestrians asking Spider-Man for a picture (or a high five).
//
// When a pedestrian walks up to the hero to interact, the game starts the
// BehaviorPedestrianInteract AI behaviour on them (states "Wait For
// Interact", "Face Target", "Interact", ...; it puts the PROMPT_INTERACT
// prompt on screen). Its activation (a virtual function found by run-time
// type name, bindings.ini [PedestrianInteract]) is hooked to note which
// pedestrian it is; the mod then has Mario turn to them and pose.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <vector>

#include "../common/ini.h"

namespace sm2m {

class Sm2Game;

namespace ped_interact {

// Once. False (and logged) if the binding doesn't fit this game version.
bool Install(Sm2Game& game, const Ini& bindings);
bool Installed();
// Actor handles of pedestrians that started an interaction since the last call.
std::vector<uint32_t> Take();

} // namespace ped_interact
} // namespace sm2m

#endif
