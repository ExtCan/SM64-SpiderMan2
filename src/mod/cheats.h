// Cheats and poses, applied around each libsm64 tick. Platform independent
// (tested against the real libsm64 with the user's ROM).
#pragma once

#include <cstdint>

#include "../sm64/sm64_api.h"
#include "settings.h"

namespace sm2m {

class Cheats {
public:
    // Before sm64_mario_tick: may adjust Mario so the tick sees it (moon
    // jump, BLJ). `s` is Mario's state after the previous tick.
    void BeforeTick(const Sm64Api& api, int32_t id, const SM64MarioState& s, const SM64MarioInputs& in,
                    const LiveSettings& set);
    // After the tick: health and caps.
    void AfterTick(const Sm64Api& api, int32_t id, const SM64MarioState& s, const LiveSettings& set);
    // A new Mario (spawn / respawn): caps must be put on again.
    void Reset();

    // Fastest a BLJ gets (SM64 units per frame, backwards).
    static constexpr float kMaxBljSpeed = 300.0f;
    static constexpr float kMoonJumpSpeed = 28.0f;

private:
    bool prevA_ = false;
    int64_t appliedCaps_ = -1; // the caps (MARIO_*_CAP flags) the cheat last put on (-1: none yet)
    uint32_t capTicks_ = 0;    // ticks since the cap timer was last topped up
};

enum class Pose { None, Wave, PeaceSign, StarDance };
const char* PoseName(Pose p);

// Mario strikes a pose: he stops, turns to face a direction and plays one of
// SM64's celebration animations, then goes back to idle. Any stick or button
// input ends it early.
class PoseController {
public:
    // False if Mario can't pose right now (in the air, swimming, hurt...).
    bool Start(const Sm64Api& api, int32_t id, const SM64MarioState& s, Pose p, float faceAngle);
    // Every tick before sm64_mario_tick.
    void Tick(const Sm64Api& api, int32_t id, const SM64MarioState& s, const SM64MarioInputs& in);
    bool Active() const { return pose_ != Pose::None; }
    Pose Current() const { return pose_; }
    int Ticks() const { return ticks_; }
    void Cancel() { pose_ = Pose::None; }
    static bool CanPose(uint32_t action);
    static int DurationTicks(Pose p);
    // Ticks into the pose where it shows best - where photo mode holds it
    // (the hand up, the peace sign out, the fist in the air).
    static int HoldTicks(Pose p);

private:
    Pose pose_ = Pose::None;
    int ticks_ = 0;
    float face_ = 0;
};

} // namespace sm2m
