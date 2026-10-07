// Places the game's camera around Mario.
//
// Spider-Man 2's follow camera chases its target (Spider-Man, pinned to
// Mario) through springs tuned for swinging: it trails Mario's jumps and
// runs, and 0.4's lead on its target only made up part of that. So the mod
// now moves the camera itself: the game still aims it (the player's stick and
// mouse, its own pitch and turns) and still decides how far back it sits
// (it pulls in when a wall is in the way), but where it is comes from Mario,
// every frame, with no lag.
//
// How: the camera is an actor; the game writes its transform each frame
// through the engine's Transform setters, which the mod already hooks (for
// the hero pin). The transform that is written each frame where the game
// renders its view from - same place, same aim - is the camera's. From then
// on its writes keep the game's rotation and get a new position:
//
//     camera = pivot + o.x * row0 + o.y * row1 + o.z * row2
//
// where the rows are the rotation the game wrote and `o` is where the game's
// own camera sits relative to Mario at rest (learnt while he stands still:
// how far back, how high, how far to the side - its framing; until then the
// usual one, 4.7 m back and 1.35 m up). The framing is kept when the camera
// is lost and found again (0.5 learnt it afresh each time - minutes of the
// game's lagging camera in combat, which switches cameras often), and only
// learnt from a plausible rest (0.5 learnt 1.6 m once, from a camera pulled in
// by a wall). The pivot is Mario, his height eased a very little.
//
// Walls: the mod asks the game's physics the camera's own question (its
// kCamera query) along three rays from Mario's chest to where the camera
// goes - the middle one and one either side - and pulls the camera in front
// of what two of them hit (one alone is a post or a sign the camera can see
// past), easing back out when the way is clear. Without the game's physics
// (Mario colliding with the rendered world) the game's own camera tells
// instead: the distance along the view only shrinks when the game's camera
// comes closer than learnt (a wall behind it - or just its lag, as Mario
// walks towards it), never grows (the camera falling behind a running Mario).
//
// Pure arithmetic (no game access), unit tested. The hooks (game/hero_pin)
// apply what Plan() publishes.
#pragma once

#include <cstdint>

#include "../common/vec.h"

namespace sm2m {

class CameraOverride {
public:
    struct Params {
        float verticalTauGround = 0.06f; // s: pivot height smoothing on the ground
        float verticalTauAir = 0.05f;    // s: ... in the air (0.5: 0.15 - it lagged behind his jumps)
        float blendTime = 0.35f;         // s: easing in and out of the override
        float searchRadius = 1.5f;       // m: a transform write this close to the rendered camera ...
        float searchAim = 0.9f;          // ... with a rotation row this close to its aim is a candidate
        int searchFrames = 24;           // frames of evidence before a candidate is chosen
        float matchTolerance = 0.6f;     // m: rendered camera vs the chosen transform (frames apart)
        float maxGameDistance = 25.0f;   // m: the game's camera further than this from Mario isn't following him ...
        float minGameDistance = 0.3f;    // m: ... nor this close (a first-person or cinematic view)
        // ... unless it is chasing him: it was following, and moves on from
        // its last write by less than this per write (a cut to a cinematic
        // shot jumps), at most this far behind. (0.6 let go of the camera at
        // 25 m - when Mario flew or fell faster than the game's camera
        // spring, and its own camera trailed further behind him than that.)
        float maxChaseStep = 8.0f;       // m per write
        float maxChaseDistance = 150.0f; // m
        float restSpeed = 0.5f;          // m/s: Mario slower than this counts as standing
        float restTime = 0.4f;           // s: standing this long before the framing is learnt
        float learnRate = 0.15f;         // per frame
        float maxLearnStep = 0.08f;      // m per frame (the game's own moves at rest are slow)
        // The framing used until one is learnt (m: back along the view, up,
        // to the side) and what a learnt one may be.
        float defaultBack = 4.7f, defaultUp = 1.35f;
        float minBack = 2.2f, maxBack = 8.0f, minUp = -0.5f, maxUp = 3.5f, maxSide = 2.0f;
        // [Camera] Distance: the camera sits this many times as far from
        // Mario as the framing (the same direction, so he stays where he is
        // on screen, only smaller or bigger).
        float distance = 1.0f;
        // Walls (the game's physics)
        float lookHeight = 1.0f;         // m above Mario's feet: where the collision rays start (his chest)
        float collisionMargin = 0.35f;   // m kept between the camera and what a ray hit
        float collisionSide = 0.45f;     // m: the side rays end this far either side of the camera
        float collisionRelease = 0.35f;  // s: easing back out once the way is clear
        float collisionMinDistance = 0.3f; // m: never closer to his chest than this
        float collisionFresh = 0.5f;     // s: answers older than this don't count
    };

    static constexpr int kCollisionRays = 3;

    // What the game's camera transform got this frame, before the override
    // (from the hooks): its position and rotation rows.
    struct GameSample {
        bool valid = false;
        DVec3 pos;
        Vec3 rows[3];
        uint32_t seq = 0; // changes with each write
        // The view last rendered (set by the mod): the framing is only learnt
        // from writes aimed like it (not from something else's transform).
        bool haveView = false;
        Vec3 viewForward; // unit
        // Its framing may be learnt: the camera the game has rendered from
        // for a while (not one of its others, in a moment of switching).
        bool learn = true;
    };

    // What the hooks apply this frame.
    struct Plan {
        bool active = false; // override the camera transform's writes
        DVec3 pivot;         // game space
        Vec3 offset;         // in the transform's rows (see above)
        int forwardRow = 2;  // the row along the view ...
        float forwardSign = 1.0f; // ... and its direction (row * sign = into the screen)
        float blend = 0;     // 0 = the game's position, 1 = the mod's
        // Walls: with `collide`, the camera goes `reach` (0..1] of the way
        // from Mario's chest (`lookHeight` up axis `up` from the pivot) to its
        // spot; without, the game's own camera distance limits it (see above).
        bool collide = false;
        float reach = 1.0f;
        float lookHeight = 1.0f;
        int up = 1;
        // The view last rendered (set by the mod, not by Update): a write is
        // only placed if it is aimed like it and near it - so a transform the
        // game freed and reused for something else is never moved.
        bool checkView = false;
        Vec3 viewForward;
        DVec3 viewPos;
        // The mod's frame it was made in (with Mario's frame of the same
        // number): the log matches where the view was rendered from against
        // the Mario frame drawn in it.
        uint32_t tag = 0;
        // When (NowSeconds) the pivot was where it says, and where it was at
        // the mod's frame before, `frameTime` earlier: for a write on another
        // thread (PivotAt). (0.6 took the pivot as it was: the game writes its
        // camera on its own thread, sometimes just before the mod's frame and
        // sometimes just after - a frame's worth of Mario's motion either
        // way, every frame, when he flew.)
        double time = 0;
        DVec3 prevPivot;
        float frameTime = 0; // s between the mod's last two frames (0: no frame before - as it is)
    };
    // The pivot for a camera write at time `t` on another thread than the
    // mod's frames: where it was one frame before `t`, between the last two
    // frames' pivots. So it no longer matters whether the write came just
    // before the mod's frame or just after: one frame before either is the
    // same moment, give or take a hair - not a frame apart. (The hooks note
    // the pivot they used; the injector draws Mario where that was.)
    static DVec3 PivotAt(const Plan& plan, double t);

    void Reset();

    // The camera's transform is known (the search finished, or it was told).
    void SetCamera(int forwardRow, float forwardSign);
    // Not any more: nothing is placed until one is found again (the framing
    // is kept for it).
    void LoseCamera();
    bool HasCamera() const { return haveCamera_; }
    // A framing was learnt from the game's own camera at rest (otherwise the
    // default one is used).
    bool Learnt() const { return learnt_; }
    // The framing: m to the side, up and back (along the camera's rows).
    Vec3 Framing() const { return framing_; }
    // ... as the offset along the rows of the camera last seen (see above).
    Vec3 Offset() const { return offset_; }
    // The game's camera was following Mario at the last Update (0.3-25 m away,
    // or further behind while chasing him - see Params).
    bool Following() const { return following_; }
    // Back from a moment without the hero: ease in again rather than snap.
    void ResetBlend() { blend_ = 0; }
    // The game's samples come from another of its cameras from now on (the
    // one it renders from): its position is no jump of the camera's.
    void SwitchGameSource() {
        haveLastGame_ = false;
        sourceSwitched_ = true;
        lastSeq_ = 0;
    }

    // One frame (Present). `mario` = Mario's feet (game space) as rendered;
    // `airborne`, `groundSpeed` (m/s) his state; `up` the up axis index;
    // `allowed` false while the game is paused, in photo mode, or CAMERA
    // FOLLOWS JUMPS is off (eases out; nothing is learnt then). Cinematics
    // are only told apart by distance: a game camera more than
    // maxGameDistance from Mario isn't following him. Returns what the hooks
    // apply.
    Plan Update(double t, const DVec3& mario, bool airborne, float groundSpeed, int up, const GameSample& game,
                bool allowed, const Params& p);

    // The camera position the hooks compute from a plan and the rotation the
    // game wrote, given where the game put it (`gamePos`). Shared with the
    // hooks so both use one formula.
    static DVec3 Place(const Plan& plan, const Vec3 rows[3], const DVec3& gamePos);

    // Is a write to the camera's transform (position `pos`, rotation `rows`)
    // the camera's, to be placed - not something else's (its memory reused by
    // another object, which must never be moved)? With Plan::checkView: a
    // rotation, aimed like the view last rendered or near it, and not far
    // from it - or, when the game's own camera trails far behind a flying or
    // falling Mario, moving on from its previous write (`prevGame`, if
    // `havePrev`) and looking at him. (0.6 only compared it with the view
    // rendered - which the mod had placed: with the game's camera more than
    // 40 m behind, every other write was left to the game, and the camera
    // snapped between the two.)
    static bool AcceptWrite(const Plan& plan, const DVec3& pos, const Vec3 rows[3], bool havePrev,
                            const DVec3& prevGame);

    // Walls. The rays to cast for a plan and the rotation the game wrote
    // last: from Mario's chest to where the camera goes (and a margin past
    // it), and either side of that. False: nothing to cast (no plan, or the
    // camera's spot is at his chest).
    static bool CollisionRays(const Plan& plan, const Vec3 rows[3], const Params& p, DVec3 from[kCollisionRays],
                              DVec3 to[kCollisionRays]);
    // An answer to ray `index` of a CollisionRays() call: the first thing it
    // hit was `hit` metres along it (< 0: nothing), of its `length`.
    void AcceptCollision(double t, int index, float hit, float length, const Params& p);
    // How far the camera may go now (0..1 of the way), from the answers.
    float Reach() const { return reach_; }

private:
    // The row offset for the framing in the current camera's rows (false: its
    // rows haven't been seen yet).
    bool OffsetFor(Vec3& out) const;
    bool haveCamera_ = false;
    int forwardRow_ = 2;
    float forwardSign_ = 1.0f;
    int upRow_ = -1;           // the row most along the world's up (-1: not seen yet)
    float upSign_ = 1.0f;
    float sideSign_ = 1.0f;    // the across row's direction against up x forward (the same side for any layout)
    bool learnt_ = false;
    Vec3 framing_;             // side, up, back (m)
    bool framingSet_ = false;
    Vec3 offset_;
    float distance_ = 1.0f;    // Params::distance, as of the last Update
    double pivotUp_ = 0;
    bool havePivot_ = false;
    DVec3 lastPivot_;          // the last Update's pivot (the plan's prevPivot)
    double lastPivotT_ = -1;
    double lastT_ = 0;
    bool started_ = false;
    double restSince_ = -1;
    float blend_ = 0;
    uint32_t lastSeq_ = 0;
    DVec3 lastGamePos_;
    bool haveLastGame_ = false;
    bool sourceSwitched_ = false;
    bool following_ = false;
    // Walls: each ray's latest answer (as a fraction of the way), and when.
    float rayReach_[kCollisionRays] = {1.0f, 1.0f, 1.0f};
    double rayTime_[kCollisionRays] = {-1e9, -1e9, -1e9};
    float reach_ = 1.0f;
};

// Is a transform write a candidate for the camera's? `pos` and `rows` what
// was written; `camPos` / `camForward` the rendered view. Returns the row
// index along the view (sign in `sign`), or -1.
int CameraCandidateRow(const DVec3& pos, const Vec3 rows[3], const DVec3& camPos, const Vec3& camForward, float radius,
                       float aim, float& sign);

} // namespace sm2m
