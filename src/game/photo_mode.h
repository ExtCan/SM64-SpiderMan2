// Spider-Man 2's photo mode, as Mario Mode sees it.
//
// The game's PhotomodeSystem is one global object. Its OnActivate sets a byte
// in it (bindings.ini [PhotoMode] active_offset) and its OnDeactivate clears
// it: that is how the mod knows photo mode is open. In selfie mode the game
// swaps the hero for a "doppelganger" - a copy of Spider-Man it spawns at his
// spot and poses, with its own head actor and the phone it holds - whose actor
// handles the object keeps too. With Mario out, the mod hides those, so Mario
// is the one in the selfie.
//
// Found where the game's camera-mode switch passes the object
// (lea rcx, [rip+X] ; call ...). Only memory reads: no hooks, no game calls.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>

#include "../common/ini.h"

namespace sm2m {

class Sm2Game;

namespace photo_mode {

// Once. False (and logged) if the binding doesn't fit this game version.
bool Install(Sm2Game& game, const Ini& bindings);
bool Available();
// Photo mode is open (false if unavailable or unreadable).
bool Open();
// The stand-ins' actor handles (0: none): the selfie's Spider-Man, his head
// and the phone.
struct StandIns {
    uint32_t doppelganger = 0, head = 0, phone = 0;
};
StandIns ReadStandIns();
std::string Describe();

} // namespace photo_mode
} // namespace sm2m

#endif
