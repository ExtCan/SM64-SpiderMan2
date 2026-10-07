// Hides the real hero actor while Mario is active.
//
//   engine - the game's own Transform::Hide / Transform::Unhide (default).
//            They set the transform's "hidden" bit and flag it dirty for the
//            renderer, which then skips Spider-Man in every pass: G-buffer,
//            shadow maps, reflections. Game scripts hide actors the same way.
//   write  - write a value at an offset given in bindings.ini [HideHero]
//   none   - leave Spider-Man visible
//
// The controller remembers which actor it hid (and that actor's handle
// serial), so it never touches a pool slot that has since been reused, it
// re-applies the hide if a game script shows him again, and it leaves him
// hidden on release if the game had hidden him itself (cutscenes).
#pragma once

#ifdef _WIN32

#include <string>

#include "../common/ini.h"
#include "../common/vec.h"
#include "sm2.h"

namespace sm2m {

class HeroController {
public:
    void Configure(const std::string& mode, const Ini& bindings, Sm2Game* game);

    // Every frame while Mario Mode is on. `hero` may change (character swap)
    // or be 0 for a moment (loading).
    void Update(uintptr_t hero, bool wantHidden);
    // Mario Mode is ending: show the hero again (retried by Maintain() if the
    // actor can't be read right now).
    void Release();
    // The hero is missing for a moment: stop keeping his old transform hidden.
    void Suspend();
    // Every frame while Mario Mode is off.
    void Maintain();

    bool Hidden() const { return hidden_; }
    const std::string& Mode() const { return mode_; }
    const std::string& Status() const { return status_; }
    int GameReshows() const { return reshows_; }

private:
    bool Hide(uintptr_t hero);
    bool Restore(); // true when nothing is left to restore
    void KeepHidden(bool on);
    bool WriteValue(uintptr_t actor, bool hidden);
    bool SameActor(uintptr_t actor) const;
    bool HiddenBitSet(uintptr_t actor, bool& set) const;

    Sm2Game* game_ = nullptr;
    std::string mode_ = "engine";
    std::string status_ = "off";

    uintptr_t actor_ = 0; // the actor we hid
    uint32_t serial_ = 0;
    bool hidden_ = false;
    bool ownsHide_ = false; // false: the game had already hidden him
    bool pendingRestore_ = false;
    double pendingSince_ = 0;
    int reshows_ = 0;
    double lastReshowLog_ = 0;

    // write mode
    std::string writeBase_ = "transform";
    int64_t writeOffset_ = -1;
    std::string writeType_ = "u8";
    double hiddenValue_ = 1, shownValue_ = 0;
};

} // namespace sm2m

#endif
