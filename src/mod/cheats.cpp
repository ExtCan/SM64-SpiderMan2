#include "cheats.h"

#include <cmath>

#include "../sm64/sm64_defs.h"

namespace sm2m {

namespace {
uint32_t WantedCaps(const LiveSettings& set) {
    return (set.wingCap ? sm64::MARIO_WING_CAP : 0u) | (set.metalCap ? sm64::MARIO_METAL_CAP : 0u) |
           (set.vanishCap ? sm64::MARIO_VANISH_CAP : 0u);
}
} // namespace

void Cheats::Reset() {
    appliedCaps_ = -1;
    capTicks_ = 0;
    prevA_ = false;
}

void Cheats::BeforeTick(const Sm64Api& api, int32_t id, const SM64MarioState& s, const SM64MarioInputs& in,
                        const LiveSettings& set) {
    const bool aPressed = in.buttonA && !prevA_;
    prevA_ = in.buttonA != 0;
    // Moon jump: while jump is held in the air, keep rising (not while
    // ground-pounding, diving or flying, which have their own vertical rules).
    if (set.moonJump && in.buttonA && sm64::IsAirborne(s.action) && s.action != sm64::ACT_GROUND_POUND &&
        s.action != sm64::ACT_DIVE && s.action != sm64::ACT_FLYING && s.velocity[1] < kMoonJumpSpeed &&
        api.set_mario_velocity)
        api.set_mario_velocity(id, s.velocity[0], kMoonJumpSpeed, s.velocity[2]);
    // BLJ anywhere: during (or landing from) a backwards long jump, crouch +
    // jump starts another one - SM64 multiplies the speed by 1.5 each time
    // and never caps it going backwards.
    if (set.bljAnywhere && aPressed && in.buttonZ && s.forwardVelocity < -15.0f &&
        (s.action == sm64::ACT_LONG_JUMP || s.action == sm64::ACT_LONG_JUMP_LAND) && api.set_mario_action) {
        api.set_mario_action(id, sm64::ACT_LONG_JUMP);
        if (s.forwardVelocity * 1.5f < -kMaxBljSpeed && api.set_mario_forward_velocity)
            api.set_mario_forward_velocity(id, -kMaxBljSpeed);
    }
}

void Cheats::AfterTick(const Sm64Api& api, int32_t id, const SM64MarioState& s, const LiveSettings& set) {
    if (set.infiniteHealth && s.health > 0xFF && s.health < sm64::HEALTH_FULL && !sm64::IsDying(s.action) &&
        api.set_mario_health)
        api.set_mario_health(id, uint16_t(sm64::HEALTH_FULL));

    if (!api.mario_interact_cap || !api.set_mario_state) return;
    const uint32_t want = WantedCaps(set);
    const uint32_t have = s.flags & sm64::MARIO_SPECIAL_CAPS;
    if (want == 0) {
        // Take off caps this cheat put on (SM64's own timer would too, later).
        if (appliedCaps_ > 0 && have) api.set_mario_state(id, s.flags & ~sm64::MARIO_SPECIAL_CAPS);
        appliedCaps_ = 0;
        return;
    }
    if (sm64::IsDying(s.action)) return;
    if (appliedCaps_ != int64_t(want) || have != want) {
        // Off with the caps no longer wanted; on with the missing ones, all
        // at once (SM64 ORs a cap into Mario's flags: they combine).
        if (have & ~want) api.set_mario_state(id, s.flags & ~(have & ~want));
        // Long timer, no cap music (Mario puts it on if he's standing still).
        if (want & ~have) api.mario_interact_cap(id, want & ~have, 60000, 0);
        appliedCaps_ = int64_t(want);
        capTicks_ = 0;
        return;
    }
    // SM64 takes the cap off when its timer runs out: keep topping it up.
    if (++capTicks_ >= 30000 && api.mario_extend_cap) {
        api.mario_extend_cap(id, 30000);
        capTicks_ = 0;
    }
}

const char* PoseName(Pose p) {
    switch (p) {
    case Pose::Wave: return "wave";
    case Pose::PeaceSign: return "peace sign";
    case Pose::StarDance: return "star dance";
    default: return "none";
    }
}

bool PoseController::CanPose(uint32_t action) {
    const uint32_t g = sm64::ActionGroup(action);
    if (g != sm64::ACT_GROUP_STATIONARY && g != sm64::ACT_GROUP_MOVING) return false;
    if (action & (sm64::ACT_FLAG_INTANGIBLE | sm64::ACT_FLAG_INVULNERABLE)) return false;
    return !sm64::IsDying(action);
}

int PoseController::DurationTicks(Pose p) {
    switch (p) {
    case Pose::Wave: return 75;
    case Pose::PeaceSign: return 60;
    case Pose::StarDance: return 90;
    default: return 0;
    }
}

int PoseController::HoldTicks(Pose p) {
    // (Picked from renders of Mario's mesh at each tick with the real libsm64.)
    switch (p) {
    case Pose::Wave: return 16;      // the hand up and out
    case Pose::PeaceSign: return 48; // the sign at his face, still facing you
    case Pose::StarDance: return 46; // the fist pumped
    default: return 0;
    }
}

bool PoseController::Start(const Sm64Api& api, int32_t id, const SM64MarioState& s, Pose p, float faceAngle) {
    if (p == Pose::None || !api.set_mario_action || !api.set_mario_animation || !api.set_mario_faceangle) return false;
    if (!CanPose(s.action) && !(Active() && s.action == sm64::ACT_END_WAVING_CUTSCENE)) return false;
    // A cutscene action libsm64 leaves empty: Mario stands still and plays
    // whatever animation is set.
    api.set_mario_action(id, sm64::ACT_END_WAVING_CUTSCENE);
    api.set_mario_velocity(id, 0, 0, 0);
    if (api.set_mario_forward_velocity) api.set_mario_forward_velocity(id, 0);
    face_ = std::remainder(faceAngle, 6.2831853f);
    api.set_mario_faceangle(id, face_);
    const int32_t anim = p == Pose::Wave        ? sm64::MARIO_ANIM_CREDITS_WAVING
                         : p == Pose::PeaceSign ? sm64::MARIO_ANIM_CREDITS_PEACE_SIGN
                                                : sm64::MARIO_ANIM_STAR_DANCE;
    api.set_mario_animation(id, anim);
    if (api.set_mario_anim_frame) api.set_mario_anim_frame(id, 0);
    pose_ = p;
    ticks_ = 0;
    return true;
}

void PoseController::Tick(const Sm64Api& api, int32_t id, const SM64MarioState& s, const SM64MarioInputs& in) {
    if (pose_ == Pose::None) return;
    ++ticks_;
    // Something else took over (damage, a fall, a respawn). (`s` is from the
    // tick before: on the first one it predates the pose.)
    if (ticks_ > 1 && s.action != sm64::ACT_END_WAVING_CUTSCENE) {
        pose_ = Pose::None;
        return;
    }
    const bool input = std::fabs(in.stickX) > 0.35f || std::fabs(in.stickY) > 0.35f || in.buttonA || in.buttonB ||
                       in.buttonZ;
    if (pose_ == Pose::StarDance && ticks_ == 38 && api.play_sound_global) api.play_sound_global(sm64::SOUND_MARIO_HERE_WE_GO);
    if ((input && ticks_ > 6) || ticks_ >= DurationTicks(pose_)) {
        api.set_mario_action(id, sm64::ACT_IDLE);
        pose_ = Pose::None;
    }
}

} // namespace sm2m
