#ifdef _WIN32

#include "sm2_hero.h"

#include <cmath>
#include <cstring>

#include "../common/log.h"
#include "../common/platform.h"
#include "hero_pin.h"

namespace sm2m {

void HeroController::Configure(const std::string& mode, const Ini& b, Sm2Game* game) {
    game_ = game;
    mode_ = Ini::Lower(mode);
    // v0.1's "scale" mode never worked (the renderer ignores transform writes
    // that aren't flagged dirty); old settings files still say it.
    if (mode_ == "scale" || mode_.empty()) mode_ = "engine";
    writeBase_ = Ini::Lower(b.GetString("HideHero", "base", "transform"));
    writeOffset_ = b.GetInt("HideHero", "offset", -1);
    writeType_ = Ini::Lower(b.GetString("HideHero", "type", "u8"));
    hiddenValue_ = b.GetFloat("HideHero", "hidden_value", 1);
    shownValue_ = b.GetFloat("HideHero", "shown_value", 0);
    if (mode_ == "write" && writeOffset_ < 0) {
        LOGW("hero: HideHero mode 'write' needs [HideHero] offset in bindings.ini; using 'engine'");
        mode_ = "engine";
    }
    if (mode_ != "none" && mode_ != "engine" && mode_ != "write") mode_ = "engine";
    if (mode_ == "engine" && game_ && !game_->HideAvailable())
        LOGW("hero: the game's Transform::Hide wasn't found - Spider-Man will stay visible");
}

bool HeroController::SameActor(uintptr_t actor) const {
    return actor && actor == actor_ && game_->ActorSerial(actor) == serial_;
}

bool HeroController::HiddenBitSet(uintptr_t actor, bool& set) const {
    uint32_t flags = 0;
    if (!game_->TransformFlags(actor, flags)) return false;
    set = (flags & game_->Layout().transformHiddenBit) != 0;
    return true;
}

bool HeroController::WriteValue(uintptr_t actor, bool hidden) {
    uintptr_t base = actor;
    if (writeBase_ == "transform" && !game_->GetTransform(actor, base)) return false;
    const double v = hidden ? hiddenValue_ : shownValue_;
    const uintptr_t addr = base + uintptr_t(writeOffset_);
    if (writeType_ == "f32") {
        float f = float(v);
        return SafeWrite(addr, &f, 4);
    }
    if (writeType_ == "u32") {
        uint32_t u = uint32_t(int64_t(v));
        return SafeWrite(addr, &u, 4);
    }
    if (writeType_ == "u16") {
        uint16_t u = uint16_t(int64_t(v));
        return SafeWrite(addr, &u, 2);
    }
    uint8_t u = uint8_t(int64_t(v));
    return SafeWrite(addr, &u, 1);
}

bool HeroController::Hide(uintptr_t hero) {
    bool already = false;
    if (mode_ == "write") {
        if (!WriteValue(hero, true)) return false;
        ownsHide_ = true;
    } else {
        if (!game_->HideAvailable() || !HiddenBitSet(hero, already)) return false;
        // If a script already hid him (cutscene, gadget), don't unhide him later.
        if (!already && !game_->SetHidden(hero, true)) return false;
        ownsHide_ = !already;
    }
    actor_ = hero;
    serial_ = game_->ActorSerial(hero);
    hidden_ = true;
    pendingRestore_ = false;
    reshows_ = 0;
    KeepHidden(true);
    status_ = mode_ == "write" ? "hidden (write)" : (already ? "hidden (by the game)" : "hidden (engine)");
    return true;
}

// Transform::Unhide refuses to show the hidden hero (hero_pin) while he is
// meant to stay hidden; Mario Mode's own unhide must get through.
void HeroController::KeepHidden(bool on) {
    uintptr_t t = 0;
    if (on && mode_ == "engine" && actor_ && game_->GetTransform(actor_, t)) hero_pin::KeepHidden(t);
    else hero_pin::KeepHidden(0);
}

bool HeroController::Restore() {
    KeepHidden(false);
    // The game tried to show him while Mario stood in: it wants him visible
    // now, even if it had hidden him itself when Mario Mode started.
    if (hero_pin::TakeUnhideRefused()) ownsHide_ = true;
    if (!actor_) return true;
    if (game_->ActorSerial(actor_) != serial_) {
        LOGI("hero: the hidden actor no longer exists; nothing to restore");
        actor_ = 0;
        return true;
    }
    bool ok = true;
    if (ownsHide_) {
        if (mode_ == "write") {
            ok = WriteValue(actor_, false);
        } else {
            bool set = false;
            ok = HiddenBitSet(actor_, set);
            if (ok && set) ok = game_->SetHidden(actor_, false);
        }
    }
    if (ok) actor_ = 0;
    return ok;
}

void HeroController::Update(uintptr_t hero, bool wantHidden) {
    if (mode_ == "none") {
        status_ = "visible (HideHero = none)";
        return;
    }
    if (pendingRestore_ && Restore()) pendingRestore_ = false;
    if (hidden_ && !SameActor(hero)) {
        // Character swap or reload: show the old actor again if it still exists.
        hidden_ = false;
        if (!Restore()) {
            pendingRestore_ = true;
            pendingSince_ = NowSeconds();
        }
    }
    if (!hero) return;
    if (wantHidden && !hidden_ && !pendingRestore_) {
        if (!Hide(hero)) status_ = game_->HideAvailable() ? "could not hide the hero" : "no Transform::Hide in this game version";
        return;
    }
    if (!wantHidden && hidden_) {
        hidden_ = false;
        if (!Restore()) {
            pendingRestore_ = true;
            pendingSince_ = NowSeconds();
        }
        status_ = "visible";
        return;
    }
    if (!hidden_) return;
    if (mode_ == "write") {
        WriteValue(actor_, true);
        return;
    }
    if (hero_pin::TakeUnhideRefused()) ownsHide_ = true;
    KeepHidden(true); // (again: it was let go while the hero was missing)
    // A script may show him again (end of a gadget or cinematic): hide again.
    bool set = true;
    if (HiddenBitSet(actor_, set) && !set) {
        if (game_->SetHidden(actor_, true)) {
            ownsHide_ = true;
            ++reshows_;
            const double now = NowSeconds();
            if (now - lastReshowLog_ > 10.0) {
                lastReshowLog_ = now;
                LOGI("hero: the game showed Spider-Man again (%d time(s)); hid him again", reshows_);
            }
        }
    }
}

// The hero is missing for a moment (a load, a cutscene): his transform may
// be freed and reused - stop refusing Unhide on it until he's back.
void HeroController::Suspend() { hero_pin::KeepHidden(0); }

void HeroController::Release() {
    if (hidden_) {
        hidden_ = false;
        if (!Restore()) {
            pendingRestore_ = true;
            pendingSince_ = NowSeconds();
            LOGW("hero: couldn't show Spider-Man again right now; will retry");
        }
    }
    status_ = "off";
}

void HeroController::Maintain() {
    if (!pendingRestore_) return;
    if (Restore()) {
        pendingRestore_ = false;
        LOGI("hero: shown again after retry");
    } else if (NowSeconds() - pendingSince_ > 30.0) {
        pendingRestore_ = false;
        actor_ = 0;
        LOGW("hero: gave up showing the hero again (actor unreadable for 30 s)");
    }
}

} // namespace sm2m

#endif
