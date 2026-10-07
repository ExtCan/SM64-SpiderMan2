// Keeps the game's idea of where Spider-Man is on Mario during the game's own
// frame, not just at Present.
//
// 0.2 wrote Spider-Man's position once per frame, from the Present hook. The
// game's character controller runs after that (it puts him back on the ground
// under Mario) and the follow camera reads him after the controller, so the
// camera followed Mario's X/Z but stayed at ground height when he jumped.
//
// Two layers, each optional:
//  * transform writes - the engine's Transform setters (SetPosition,
//    SetMatrix and two variants, and MarkDirty, which follows inline writes)
//    get Mario's position substituted when they move the hero's transform
//    away from him by less than `radius` horizontally. Larger moves are the
//    game's own teleports (cutscenes, fast travel) and go through.
//  * camera target - Camera2::CameraTarget's GetPosition / GetMatrix /
//    GetTrackPosition (vtable slots from bindings.ini [CameraTarget]) report
//    Mario's position for the hero's target, whatever moved the transform.
//
// The hooks only compare a pointer for every other transform. Counters and the
// call sites that fought the pin are kept for the log.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <vector>

#include "../common/ini.h"
#include "../common/vec.h"
#include "../mod/camera_override.h"

namespace sm2m {

class Sm2Game;

namespace hero_pin {

struct Options {
    bool transformWrites = true; // sm2mario.ini [Hero] HoldDuringFrame
    bool cameraTarget = true;    // [Camera] FollowMarioHeight
    bool keepHidden = true;      // [Hero] KeepHidden: refuse Transform::Unhide on the hidden hero
};

// Resolves the bindings and hooks what `opt` asks for (once; later calls do nothing).
void Install(Sm2Game& game, const Ini& bindings, const Options& opt);
bool TransformHooks();
bool CameraHooks();

// Every frame while Mario Mode is on, before the hero is moved there: his
// transform and where it has to stay (game coordinates). `up` is the up axis
// index (1 = Y); `radius` the horizontal distance (metres) a write may move
// him and still be undone.
// `hold` off: the setters let the game move him (an interaction with a
// pedestrian walks and animates him) - the camera target still reports the pin.
void SetPin(uintptr_t transform, const float pos[3], int up, float radius, bool hold = true);
void ClearPin();
// The transform Transform::Unhide must leave hidden (Spider-Man's, while
// Mario Mode hides him; 0: none). The hook refuses to show it and counts who
// tried.
void KeepHidden(uintptr_t transform);
// The hook refused to show that transform since the last call: the game
// wants him visible (so when Mario Mode ends, he must be shown).
bool TakeUnhideRefused();
// ... and these (up to 4; photo mode's stand-ins for Spider-Man). n = 0: none.
void KeepHiddenToo(const uintptr_t* transforms, int n);
bool UnhideHooked();

// ---- the camera override (mod/camera_override.h)
// While searching, transform writes within `radius` of the rendered camera
// whose rotation has a row within `aim` (cosine) of its view are noted; one
// call per frame. `stamp` numbers the frame (0: the next number) - the
// candidates' writes carry it (MatchViewToCandidates).
void SetCameraSearch(bool on, const DVec3& camPos, const Vec3& camForward, float radius, float aim,
                     uint32_t stamp = 0);
// ... around two places at once: the rendered camera, and where another of
// the game's cameras rendered from lately (carried along with Mario) - while
// the mod places the camera it keeps watching for the game's others (0.6.1).
struct SearchArea {
    DVec3 pos;
    Vec3 forward;      // the aim of the view rendered from there
    float radius = 0;  // m (0: none)
};
void SetCameraSearch(bool on, const SearchArea& a, const SearchArea& b, float aim, uint32_t stamp);
// Forgets the candidates not written since frame `stamp` (the table has room
// for 16).
void PruneCameraCandidates(uint32_t stamp);
struct CameraCandidate {
    uintptr_t transform = 0;
    uint32_t hits = 0; // frames it was written in like the camera
    int row = -1;      // its row along the view ...
    float sign = 1.0f; // ... and direction
    float meanGap = 0; // m: how far from the rendered view it was written, on average
                       // (MatchViewToCandidates: how far from the view it was written then)
};
std::vector<CameraCandidate> TakeCameraCandidates();
// The candidates (none of the camera slots) the game wrote within `tolerance`
// of `view` in a frame stamped stampLo..stampHi: the camera that view was
// rendered from, if the game rendered it from one the mod doesn't place.
// Returns how many (at most `max`) went into `out`.
int MatchViewToCandidates(uint32_t stampLo, uint32_t stampHi, const DVec3& view, float tolerance,
                          CameraCandidate* out, int max);
// Where the camera was placed in its last writes, with the tag of the plan
// each came from (CameraOverride::Plan::tag), the pivot it was placed around
// (the plan's, or between its last two: CameraOverride::PivotAt) and the
// blend - where that camera had Mario - and when (NowSeconds), in the order
// written (`order` increases).
struct CameraPlacement {
    uint32_t tag = 0;
    DVec3 pos;
    DVec3 pivot;
    float blend = 0;
    double time = 0;
    uint32_t order = 0;
    int slot = 0; // which of the cameras (SetCameraTransform / AddCameraTransform)
};
std::vector<CameraPlacement> RecentCameraPlacements();
// The same into `out` (at most `max`, no allocation: for the render hooks); returns how many.
int RecentCameraPlacements(CameraPlacement* out, int max);
// Forgets them (a new camera, or none).
void ClearCameraPlacements();
// When the game wrote its camera against the mod's frames, since the last
// call: placed writes, how long after the plan's frame (microseconds, summed
// over `aheadCount` writes, and the most), and the thread it writes on.
struct CameraTiming {
    uint32_t writes = 0;
    uint64_t aheadUs = 0, aheadCount = 0;
    uint32_t aheadMaxUs = 0;
    uint32_t thread = 0;
    uint64_t carried = 0; // writes on another thread than the mod's frames: the pivot as it was a frame before them
};
CameraTiming TakeCameraTiming();
// The thread the mod's frames (Present) run on: camera writes on another one
// get the plan's pivot as it was a frame before they happen.
void SetPresentThread(uint32_t id);
void ClearCameraCandidates();
// The cameras' transforms: their writes get the plan's position. The game
// has several cameras and renders from one or another; slot 0 is the one
// found first (the search), the others are added when the game is seen
// rendering from them.
constexpr int kCameraSlots = 4;
// Slot 0 (0: none) - and no others.
void SetCameraTransform(uintptr_t transform);
// Another one, in a free slot: returns the slot (or the one it already has), -1 if none is free.
int AddCameraTransform(uintptr_t transform);
void RemoveCameraTransform(int slot); // (not slot 0)
void SwapCameraSlots(int a, int b);
uintptr_t CameraTransform();          // slot 0's
uintptr_t CameraTransformAt(int slot);
struct CameraSlotStats {
    uintptr_t transform = 0;
    uint64_t writes = 0, refused = 0; // since the last call
    double lastWrite = -1;            // NowSeconds() of its last write (-1: none yet)
};
CameraSlotStats TakeCameraSlotStats(int slot);
void SetCameraPlan(const CameraOverride::Plan& plan);
// What the game last wrote into a camera's transform (before the override).
bool GameCameraSample(CameraOverride::GameSample& out, int slot = 0);
// Camera writes overridden since the last call (all of them).
uint64_t TakeCameraWrites();
// Camera writes left alone since the last call: not aimed like the view last
// rendered, or far from it and not chasing Mario (Plan::checkView).
uint64_t TakeCameraRefused();

// Metres added to the camera target's height (along `up`) on top of the pin:
// keeps the game camera with Mario's jumps (mod/camera_lead.h).
void SetCameraLead(float metres);

enum Kind { kSetPosition, kSetMatrix, kSetMatrixEx, kSetMatrixEx2, kMarkDirty, kKinds };
const char* KindName(int kind);

struct Writer {
    uintptr_t caller = 0; // return address in the game
    uint32_t count = 0;
    float maxOffset = 0;  // metres
    int kind = 0;
};

struct Stats {
    uint64_t undone[kKinds] = {}; // writes moved back onto Mario, per setter
    uint64_t passed = 0;          // writes far from Mario, let through (the game's teleports)
    uint64_t targetReads = 0;     // camera-target reads answered with Mario's position
    std::vector<Writer> writers;  // who fought the pin
    uint64_t unhideBlocked = 0;   // Transform::Unhide calls on the hidden hero, refused
    std::vector<Writer> unhiders; // ... and where they came from
};
// Counters since the last call (they restart from zero).
Stats TakeStats();

} // namespace hero_pin
} // namespace sm2m

#endif
