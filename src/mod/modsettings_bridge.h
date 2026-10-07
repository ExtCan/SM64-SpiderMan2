// Mario Mode's page in the ModSettings mod's pause menu
// (nexusmods.com/marvelsspiderman2/mods/579), when that mod is installed.
//
// ModSettings exports a C++ API compiled with MSVC: menus are objects of a
// class with virtual functions, callbacks are std::function. This mod is
// built with MinGW, so the bridge lays those objects out by hand the way MSVC
// does (vtable order, 64-byte std::function with its implementation pointer
// last, callee-destroyed by-value arguments) and calls the exports by their
// decorated names. Without ModSettings nothing happens (the F8 menu remains).
#pragma once

#ifdef _WIN32

#include <string>

#include "settings.h"

namespace sm2m {
namespace modsettings {

// From script_enable (the game's main thread, all scripts loaded): finds the
// ModSettings DLL and registers the menu. Safe to call more than once.
// `logResult`: log the outcome (the log is open already).
void Register(bool logResult = false);
// What Register() found ("" until it ran), for the log.
std::string Status();
bool Registered();

// The settings the menu shows when it opens (call after every change).
void Publish(const LiveSettings& s);
// Changes made in the menu since the last call (false if none).
bool TakeChanges(LiveSettings& s);
// True once after ModSettings asked its mods to save.
bool TakeSaveRequest();

} // namespace modsettings
} // namespace sm2m

#endif
