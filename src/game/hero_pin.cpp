#ifdef _WIN32

#include "hero_pin.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include <windows.h>

#include "../common/log.h"
#include "../common/platform.h"
#include "../common/seqlock.h"
#include "../mod/camera_override.h"
#include "MinHook.h"
#include "pattern.h"
#include "rtti.h"
#include "sm2.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define SM2M_RETURN_ADDRESS() reinterpret_cast<uintptr_t>(_ReturnAddress())
#else
#define SM2M_RETURN_ADDRESS() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#endif

namespace sm2m {
namespace hero_pin {
namespace {

// A write that moves the hero more than this vertically is not a ground snap
// (those are a few metres): it is the game putting him somewhere (out of the
// water, back from a fall) and goes through. (0.5: 250 m - the game's rescue
// after a fall through the city was undone 230 times in a row, 240 m each.)
constexpr float kMaxVertical = 25.0f;

std::atomic<uintptr_t> g_transform{0};
std::atomic<uint32_t> g_px{0}, g_py{0}, g_pz{0}; // float bits
std::atomic<uint32_t> g_radius2{0};
std::atomic<uint32_t> g_lead{0}; // float bits: camera target lead along the up axis
std::atomic<bool> g_hold{true};  // the setters keep him on the pin (off: an interaction animates him)
std::atomic<int> g_up{1};
uint32_t g_posOffset = 0x30;     // Transform position (float3)
uint32_t g_targetActor = 0x10;   // CameraTarget -> actor (whose first field is the transform)

std::atomic<uint64_t> g_undone[kKinds];
std::atomic<uint64_t> g_passed{0}, g_targetReads{0};

struct Slot {
    std::atomic<uintptr_t> caller{0};
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> maxBits{0};
    std::atomic<int> kind{0};
};
constexpr int kSlots = 32;
Slot g_slots[kSlots];

bool g_installed = false;
bool g_transformHooks = false;
bool g_cameraHooks = false;

// Transform::Unhide on the hero while Mario Mode hides him: refused.
std::atomic<uintptr_t> g_keepHidden{0};
// Others kept hidden the same way (photo mode's selfie stand-ins for Spider-Man).
constexpr int kKeepHiddenExtra = 4;
std::atomic<bool> g_unhideRefused{false};
std::atomic<uintptr_t> g_keepHiddenExtra[kKeepHiddenExtra];
std::atomic<uint64_t> g_unhideBlocked{0};
Slot g_unhideSlots[8];
bool g_unhideHook = false;

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
float Float(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// ---------------------------------------------------------------- the camera
// The values shared with the hooks (common/seqlock.h).
template <size_t N>
using Words = SeqWords<N>;

// The search: transforms written near the rendered camera, aimed like it
// (and, while the mod places the camera, near where another of the game's
// cameras rendered from lately: SearchArea b).
std::atomic<bool> g_camSearch{false};
std::atomic<uint32_t> g_camStamp{0}; // the mod's frame number (stamps the candidates' writes)
Words<16> g_searchView; // area a: position (3), forward (3); area b: the same; radius a, radius b (0: none), aim
// The areas again, for a quick look first (every write of every transform
// comes by while searching): read field by field - torn at worst, which the
// full check (NoteCandidate) catches.
std::atomic<float> g_searchFast[8]; // a: position (3), radius squared; b: the same (radius squared <= 0: none)
struct CamCand {
    std::atomic<uintptr_t> t{0};
    std::atomic<uint32_t> hits{0};
    std::atomic<uint32_t> stamp{0};
    std::atomic<int> row{-1};
    std::atomic<uint32_t> sign{0};   // float bits
    std::atomic<uint32_t> gapMm{0};  // sum of its distances to the view (mm), one per frame counted
    // Its last few writes - the mod's frame number and the position the game
    // wrote - to tell which transform a view the game rendered came from
    // (MatchViewToCandidates). (Two threads writing one candidate at once can
    // tear a record: it then matches nothing, which is harmless.)
    static constexpr int kRecs = 12;
    std::atomic<uint32_t> recStamp[kRecs];
    std::atomic<uint32_t> recPos[kRecs][3];
    std::atomic<uint32_t> recNext{0};
    CamCand() {
        for (int i = 0; i < kRecs; ++i) {
            recStamp[i].store(0, std::memory_order_relaxed);
            for (auto& p : recPos[i]) p.store(0, std::memory_order_relaxed);
        }
    }
};
constexpr int kCamCands = 16;
CamCand g_camCands[kCamCands];

// The camera's transforms - the game has several cameras, and renders from
// one or another (0.6 placed only one: in flight a fifth of the frames came
// from another, unplaced, further back) - the plan the mod published for
// them, and what the game last wrote into each (before the override).
struct CamSlot {
    std::atomic<uintptr_t> t{0};
    Words<12> game;                   // position (3), rows (9)
    std::atomic<uint64_t> writes{0};  // placed writes since TakeCameraSlotStats
    std::atomic<uint64_t> refused{0}; // ... and left alone
    std::atomic<uint64_t> lastWrite{0};   // NowSeconds() of its last write (double bits; 0: none yet)
};
CamSlot g_camSlots[kCameraSlots];
Words<28> g_camPlan;  // active, pivot (3), offset (3), forward row, forward sign, blend, collide, reach,
                      // look height, up, check view, view forward (3), view position (3), tag,
                      // time (2: a double), previous pivot (3), frame time
// Where the cameras were placed lately: the plan's tag, the position written,
// the pivot it was placed around and the blend (the injector draws Mario
// where the camera of the view it renders had him; the log's account of
// Mario's frames against the camera's), when, the order of the writes and
// which camera.
constexpr int kPlacements = 32;
Words<12> g_placements[kPlacements]; // tag, position (3), pivot (3), blend, time (2: a double), order, slot
std::atomic<uint32_t> g_placementNext{0};
std::atomic<uint64_t> g_camWrites{0};
std::atomic<uint64_t> g_camRefused{0}; // writes not aimed like the view (or far from it): left alone

int CameraSlotOf(uintptr_t t) {
    if (!t) return -1;
    for (int i = 0; i < kCameraSlots; ++i)
        if (g_camSlots[i].t.load(std::memory_order_acquire) == t) return i;
    return -1;
}

uint64_t NowBits() {
    const double now = NowSeconds();
    uint64_t b = 0;
    std::memcpy(&b, &now, sizeof(b));
    return b ? b : 1u; // (never 0: "none yet")
}
// For the log: when the game writes its camera against the mod's frames.
std::atomic<uint32_t> g_camFrameWrites{0};   // placed writes since the mod's last frame
std::atomic<uint64_t> g_camAheadUs{0};       // the pivot carried on by, summed (microseconds) ...
std::atomic<uint64_t> g_camAheadCount{0};    // ... over this many writes
std::atomic<uint32_t> g_camAheadMaxUs{0};
std::atomic<uint32_t> g_camThread{0};        // the thread the game writes its camera on
std::atomic<uint32_t> g_presentThread{0};    // the thread the mod's frames run on (Present)
std::atomic<uint64_t> g_camCarried{0};       // writes whose pivot was carried on (another thread's)

// Near either search area (the quick look).
bool NearSearch(const float* p) {
    for (int a = 0; a < 2; ++a) {
        const float r2 = g_searchFast[a * 4 + 3].load(std::memory_order_relaxed);
        if (!(r2 > 0.0f)) continue;
        const float dx = p[0] - g_searchFast[a * 4].load(std::memory_order_relaxed);
        const float dy = p[1] - g_searchFast[a * 4 + 1].load(std::memory_order_relaxed);
        const float dz = p[2] - g_searchFast[a * 4 + 2].load(std::memory_order_relaxed);
        if (dx * dx + dy * dy + dz * dz <= r2) return true;
    }
    return false;
}

void ResetCandidate(CamCand& c) {
    c.hits.store(0, std::memory_order_relaxed);
    c.stamp.store(0, std::memory_order_relaxed);
    c.row.store(-1, std::memory_order_relaxed);
    c.gapMm.store(0, std::memory_order_relaxed);
    for (auto& st : c.recStamp) st.store(0, std::memory_order_relaxed);
}

void NoteCandidate(uintptr_t t, const float pos[3], const float rows[3][3]) {
    uint32_t v[16];
    if (!g_searchView.Read(v)) return;
    Vec3 r[3];
    for (int k = 0; k < 3; ++k) r[k] = Vec3(rows[k][0], rows[k][1], rows[k][2]);
    const DVec3 p(pos[0], pos[1], pos[2]);
    const float aim = Float(v[14]);
    float sign = 1.0f;
    DVec3 cam(Float(v[0]), Float(v[1]), Float(v[2]));
    int row = CameraCandidateRow(p, r, cam, Vec3(Float(v[3]), Float(v[4]), Float(v[5])), Float(v[12]), aim, sign);
    if (row < 0 && Float(v[13]) > 0.0f) {
        cam = DVec3(Float(v[6]), Float(v[7]), Float(v[8]));
        row = CameraCandidateRow(p, r, cam, Vec3(Float(v[9]), Float(v[10]), Float(v[11])), Float(v[13]), aim, sign);
    }
    if (row < 0) return;
    const uint32_t stamp = g_camStamp.load(std::memory_order_relaxed);
    for (CamCand& c : g_camCands) {
        uintptr_t cur = c.t.load(std::memory_order_acquire);
        if (cur == 0) {
            uintptr_t expected = 0;
            if (c.t.compare_exchange_strong(expected, t)) {
                // (A fresh entry: nothing of what had it before - a write
                // still going on to it when it was cleared, say.)
                ResetCandidate(c);
                cur = t;
            } else {
                cur = expected;
            }
        }
        if (cur != t) continue;
        c.row.store(row, std::memory_order_relaxed);
        c.sign.store(Bits(sign), std::memory_order_relaxed);
        if (c.stamp.exchange(stamp, std::memory_order_relaxed) != stamp) {
            c.hits.fetch_add(1, std::memory_order_relaxed);
            const double gap = Length(p - cam);
            c.gapMm.fetch_add(uint32_t(std::min(5000.0, gap * 1000.0)), std::memory_order_relaxed);
        }
        const uint32_t k = c.recNext.fetch_add(1, std::memory_order_relaxed) % CamCand::kRecs;
        c.recStamp[k].store(0, std::memory_order_relaxed); // (invalid while it changes)
        for (int i = 0; i < 3; ++i) c.recPos[k][i].store(Bits(pos[i]), std::memory_order_relaxed);
        c.recStamp[k].store(stamp, std::memory_order_release);
        return;
    }
}

// A write to some transform: `pos` (in/out) and the rotation rows written
// with it. The camera's gets the mod's position; while searching, every
// other transform near the rendered camera is a candidate.
void CameraWrite(uintptr_t t, float pos[3], const float rows[3][3]) {
    const int slot = CameraSlotOf(t);
    if (slot >= 0) {
        CamSlot& cs = g_camSlots[slot];
        // The game's own previous write (before the override), for the check below.
        uint32_t pg[12], pgSeq = 0;
        const bool havePrevGame = cs.game.Read(pg, &pgSeq) && pgSeq != 0;
        const DVec3 prevGame = havePrevGame ? DVec3(Float(pg[0]), Float(pg[1]), Float(pg[2])) : DVec3();
        uint32_t g[12];
        for (int k = 0; k < 3; ++k) g[k] = Bits(pos[k]);
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) g[3 + r * 3 + k] = Bits(rows[r][k]);
        cs.game.Write(g);
        cs.lastWrite.store(NowBits(), std::memory_order_relaxed);
        uint32_t v[28];
        if (!g_camPlan.Read(v) || v[0] == 0) return;
        CameraOverride::Plan plan;
        plan.active = true;
        plan.pivot = DVec3(Float(v[1]), Float(v[2]), Float(v[3]));
        {
            const uint64_t tb = uint64_t(v[22]) | (uint64_t(v[23]) << 32);
            std::memcpy(&plan.time, &tb, sizeof(plan.time));
        }
        plan.prevPivot = DVec3(Float(v[24]), Float(v[25]), Float(v[26]));
        plan.frameTime = Float(v[27]);
        plan.offset = Vec3(Float(v[4]), Float(v[5]), Float(v[6]));
        plan.forwardRow = int(v[7]);
        plan.forwardSign = Float(v[8]);
        plan.blend = Float(v[9]);
        plan.collide = v[10] != 0;
        plan.reach = Float(v[11]);
        plan.lookHeight = Float(v[12]);
        plan.up = int(v[13]);
        Vec3 r[3];
        for (int k = 0; k < 3; ++k) r[k] = Vec3(rows[k][0], rows[k][1], rows[k][2]);
        // The camera's write, not something else's (CameraOverride::AcceptWrite).
        plan.checkView = v[14] != 0;
        plan.viewForward = Vec3(Float(v[15]), Float(v[16]), Float(v[17]));
        plan.viewPos = DVec3(Float(v[18]), Float(v[19]), Float(v[20]));
        if (!CameraOverride::AcceptWrite(plan, DVec3(pos[0], pos[1], pos[2]), r, havePrevGame, prevGame)) {
            g_camRefused.fetch_add(1, std::memory_order_relaxed);
            cs.refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Written on another thread than the mod's frames: when it comes
        // against them varies (just before one, or just after it - a frame's
        // worth of Mario's motion apart with the plan as it was), so it takes
        // the pivot as it was a frame before now (PivotAt). On the same thread
        // the order is fixed (the mod's frame, this write, then the frame
        // drawn with it): as it is.
        const double now = NowSeconds();
        const uint32_t self = uint32_t(GetCurrentThreadId());
        const uint32_t presentThread = g_presentThread.load(std::memory_order_relaxed);
        if (presentThread != 0 && self != presentThread) {
            plan.pivot = CameraOverride::PivotAt(plan, now);
            g_camCarried.fetch_add(1, std::memory_order_relaxed);
        }
        // (The timing for the log: the first camera's writes - the one the
        // game renders from most.)
        if (plan.time > 0.0 && slot == 0) {
            const double aheadS = std::max(0.0, std::min(1.0, now - plan.time));
            const uint32_t us = uint32_t(aheadS * 1e6);
            g_camAheadUs.fetch_add(us, std::memory_order_relaxed);
            g_camAheadCount.fetch_add(1, std::memory_order_relaxed);
            uint32_t cur = g_camAheadMaxUs.load(std::memory_order_relaxed);
            while (us > cur && !g_camAheadMaxUs.compare_exchange_weak(cur, us)) {
            }
        }
        if (slot == 0) g_camFrameWrites.fetch_add(1, std::memory_order_relaxed);
        g_camThread.store(self, std::memory_order_relaxed);
        const DVec3 placed = CameraOverride::Place(plan, r, DVec3(pos[0], pos[1], pos[2]));
        if (!(placed.x == placed.x) || !(placed.y == placed.y) || !(placed.z == placed.z)) return; // NaN
        pos[0] = float(placed.x);
        pos[1] = float(placed.y);
        pos[2] = float(placed.z);
        g_camWrites.fetch_add(1, std::memory_order_relaxed);
        cs.writes.fetch_add(1, std::memory_order_relaxed);
        uint64_t nb = 0;
        std::memcpy(&nb, &now, sizeof(nb));
        const uint32_t order = g_placementNext.fetch_add(1, std::memory_order_relaxed);
        const uint32_t rec[12] = {v[21],
                                  Bits(pos[0]),
                                  Bits(pos[1]),
                                  Bits(pos[2]),
                                  Bits(float(plan.pivot.x)),
                                  Bits(float(plan.pivot.y)),
                                  Bits(float(plan.pivot.z)),
                                  Bits(std::max(0.0f, std::min(1.0f, plan.blend))),
                                  uint32_t(nb),
                                  uint32_t(nb >> 32),
                                  order + 1, // (0: never written)
                                  uint32_t(slot)};
        g_placements[order % kPlacements].Write(rec);
        return;
    }
    if (g_camSearch.load(std::memory_order_relaxed)) NoteCandidate(t, pos, rows);
}

// A write to one of the cameras' transforms, or (searching) one near the
// search areas. `pos`: where it is written (read only while searching).
bool CameraInterest(uintptr_t t, const float* pos) {
    if (!t) return false;
    for (const CamSlot& cs : g_camSlots)
        if (t == cs.t.load(std::memory_order_relaxed)) return true;
    return pos && g_camSearch.load(std::memory_order_relaxed) && NearSearch(pos);
}

void RowsOf(const float* m, float rows[3][3]) {
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) rows[r][k] = m[r * 4 + k];
}

bool LoadPin(uintptr_t t, float pin[3]) {
    if (!t || g_transform.load(std::memory_order_acquire) != t) return false;
    pin[0] = Float(g_px.load(std::memory_order_relaxed));
    pin[1] = Float(g_py.load(std::memory_order_relaxed));
    pin[2] = Float(g_pz.load(std::memory_order_relaxed));
    return true;
}

// The pin, for the transform setters: only while they are to hold him there.
bool LoadHold(uintptr_t t, float pin[3]) { return g_hold.load(std::memory_order_relaxed) && LoadPin(t, pin); }

// 0: already on the pin, 1: near it (undo the move), 2: far (a teleport: let it through)
int Classify(const float* p, const float pin[3], float& offset) {
    const int up = g_up.load(std::memory_order_relaxed);
    float h2 = 0, v = 0;
    for (int k = 0; k < 3; ++k) {
        const float d = p[k] - pin[k];
        if (k == up) v = d;
        else h2 += d * d;
    }
    offset = std::sqrt(h2 + v * v);
    if (!(offset == offset)) return 2; // NaN: not ours to judge
    if (offset < 1e-4f) return 0;
    if (h2 > Float(g_radius2.load(std::memory_order_relaxed)) || std::fabs(v) > kMaxVertical) return 2;
    return 1;
}

void Record(int kind, uintptr_t caller, float offset) {
    g_undone[kind].fetch_add(1, std::memory_order_relaxed);
    for (Slot& s : g_slots) {
        uintptr_t c = s.caller.load(std::memory_order_acquire);
        if (c == 0) {
            uintptr_t expected = 0;
            if (s.caller.compare_exchange_strong(expected, caller)) {
                s.kind.store(kind, std::memory_order_relaxed);
                c = caller;
            } else {
                c = expected;
            }
        }
        if (c != caller) continue;
        s.count.fetch_add(1, std::memory_order_relaxed);
        uint32_t cur = s.maxBits.load(std::memory_order_relaxed);
        while (Float(cur) < offset && !s.maxBits.compare_exchange_weak(cur, Bits(offset))) {
        }
        return;
    }
}

void RecordUnhide(uintptr_t caller) {
    g_unhideBlocked.fetch_add(1, std::memory_order_relaxed);
    for (Slot& s : g_unhideSlots) {
        uintptr_t c = s.caller.load(std::memory_order_acquire);
        if (c == 0) {
            uintptr_t expected = 0;
            c = s.caller.compare_exchange_strong(expected, caller) ? caller : expected;
        }
        if (c != caller) continue;
        s.count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// ---------------------------------------------------------------- transform setters

using SetPositionFn = void (*)(uintptr_t, const float*);
using SetMatrixFn = void (*)(uintptr_t, const float*, float*);
// Transform::SetMatrixEx(t, matrix, rotation, scale) works out the rest from
// the matrix; SetMatrixEx2 is given it: (t, matrix, rotation, scale, flags,
// extra) - six arguments, the sixth a pointer it reads (0.5.0's first build
// passed five, and the game read whatever was on the hook's stack: a crash at
// start-up). Every argument is passed on as the game gave it.
using SetMatrixExFn = void (*)(uintptr_t, const float*, const void*, const void*);
using SetMatrixEx2Fn = void (*)(uintptr_t, const float*, const void*, const void*, uintptr_t, const void*);
using MarkDirtyFn = void (*)(uintptr_t, int);
SetPositionFn o_setPosition = nullptr;
SetMatrixFn o_setMatrix = nullptr;
SetMatrixExFn o_setMatrixEx = nullptr;
SetMatrixEx2Fn o_setMatrixEx2 = nullptr;
MarkDirtyFn o_markDirty = nullptr;

void H_SetPosition(uintptr_t t, const float* p) {
    float pin[3], off;
    if (p && LoadHold(t, pin)) {
        const int c = Classify(p, pin, off);
        if (c == 1) {
            Record(kSetPosition, SM2M_RETURN_ADDRESS(), off);
            o_setPosition(t, pin);
            return;
        }
        if (c == 2) g_passed.fetch_add(1, std::memory_order_relaxed);
    } else if (p && t && CameraInterest(t, p)) {
        // (Its rotation: the transform's, which this write keeps.)
        float q[3] = {p[0], p[1], p[2]}, rows[3][3];
        RowsOf(reinterpret_cast<const float*>(t), rows);
        CameraWrite(t, q, rows);
        o_setPosition(t, q);
        return;
    }
    o_setPosition(t, p);
}

// The matrix's last row is the position.
bool PinnedCopy(uintptr_t t, const float* m, int kind, uintptr_t caller, float* copy) {
    float pin[3], off;
    if (!m || !LoadHold(t, pin)) return false;
    const int c = Classify(m + 12, pin, off);
    if (c == 2) g_passed.fetch_add(1, std::memory_order_relaxed);
    if (c != 1) return false;
    Record(kind, caller, off);
    std::memcpy(copy, m, 64);
    copy[12] = pin[0];
    copy[13] = pin[1];
    copy[14] = pin[2];
    return true;
}

// The camera's (or a candidate's) matrix: a copy with the mod's position.
bool CameraCopy(uintptr_t t, const float* m, float* copy) {
    if (!m || !t || !CameraInterest(t, m + 12)) return false;
    std::memcpy(copy, m, 64);
    float rows[3][3];
    RowsOf(copy, rows);
    CameraWrite(t, copy + 12, rows);
    return true;
}

void H_SetMatrix(uintptr_t t, const float* m, float* scale) {
    alignas(32) float copy[16];
    if (PinnedCopy(t, m, kSetMatrix, SM2M_RETURN_ADDRESS(), copy) || CameraCopy(t, m, copy)) o_setMatrix(t, copy, scale);
    else o_setMatrix(t, m, scale);
}

void H_SetMatrixEx(uintptr_t t, const float* m, const void* a, const void* b) {
    alignas(32) float copy[16];
    if (PinnedCopy(t, m, kSetMatrixEx, SM2M_RETURN_ADDRESS(), copy) || CameraCopy(t, m, copy))
        o_setMatrixEx(t, copy, a, b);
    else o_setMatrixEx(t, m, a, b);
}

void H_SetMatrixEx2(uintptr_t t, const float* m, const void* a, const void* b, uintptr_t flags, const void* extra) {
    alignas(32) float copy[16];
    if (PinnedCopy(t, m, kSetMatrixEx2, SM2M_RETURN_ADDRESS(), copy) || CameraCopy(t, m, copy))
        o_setMatrixEx2(t, copy, a, b, flags, extra);
    else o_setMatrixEx2(t, m, a, b, flags, extra);
}

// Transform::Unhide: the game showing Spider-Man again (the end of a gadget,
// a cinematic or an interaction) while Mario stands in for him.
using UnhideFn = void (*)(uintptr_t);
UnhideFn o_unhide = nullptr;
void H_Unhide(uintptr_t t) {
    if (t && t == g_keepHidden.load(std::memory_order_acquire)) {
        RecordUnhide(SM2M_RETURN_ADDRESS());
        g_unhideRefused.store(true, std::memory_order_release); // the game wants him shown (after Mario)
        return;
    }
    if (t)
        for (const auto& x : g_keepHiddenExtra)
            if (t == x.load(std::memory_order_acquire)) return;
    o_unhide(t);
}

// Called after code that wrote the transform inline.
void H_MarkDirty(uintptr_t t, int frame) {
    float pin[3], off;
    if (LoadHold(t, pin)) {
        float* p = reinterpret_cast<float*>(t + g_posOffset);
        const int c = Classify(p, pin, off);
        if (c == 1) {
            Record(kMarkDirty, SM2M_RETURN_ADDRESS(), off);
            p[0] = pin[0];
            p[1] = pin[1];
            p[2] = pin[2];
        } else if (c == 2) {
            g_passed.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (t && CameraInterest(t, reinterpret_cast<const float*>(t + g_posOffset))) {
        // Written in place before this call: the position is changed in place.
        float* p = reinterpret_cast<float*>(t + g_posOffset);
        float q[3] = {p[0], p[1], p[2]}, rows[3][3];
        RowsOf(reinterpret_cast<const float*>(t), rows);
        CameraWrite(t, q, rows);
        p[0] = q[0];
        p[1] = q[1];
        p[2] = q[2];
    }
    o_markDirty(t, frame);
}

// ---------------------------------------------------------------- camera target

using GetVecFn = float* (*)(uintptr_t, float*);
GetVecFn o_targetPosition = nullptr;
GetVecFn o_targetMatrix = nullptr;
GetVecFn o_targetTrack = nullptr;

// The hero's transform if `self` targets him and he is near the pin.
uintptr_t HeroTarget(uintptr_t self, float pin[3], const float*& cur) {
    if (!self) return 0;
    const uintptr_t actor = *reinterpret_cast<const uintptr_t*>(self + g_targetActor);
    if (!actor) return 0;
    const uintptr_t t = *reinterpret_cast<const uintptr_t*>(actor);
    if (!LoadPin(t, pin)) return 0;
    cur = reinterpret_cast<const float*>(t + g_posOffset);
    // (While an interaction moves him, the camera stays with Mario wherever he is.)
    if (!g_hold.load(std::memory_order_relaxed)) return t;
    float off;
    return Classify(cur, pin, off) == 2 ? 0 : t;
}

// The pin plus the camera lead (along the up axis).
void AddLead(float pin[3]) {
    const int up = g_up.load(std::memory_order_relaxed);
    const float lead = Float(g_lead.load(std::memory_order_relaxed));
    if (up >= 0 && up < 3 && lead == lead) pin[up] += lead;
}

float* H_TargetPosition(uintptr_t self, float* out) {
    float* r = o_targetPosition(self, out);
    float pin[3];
    const float* cur = nullptr;
    if (out && HeroTarget(self, pin, cur)) {
        AddLead(pin);
        out[0] = pin[0];
        out[1] = pin[1];
        out[2] = pin[2];
        g_targetReads.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

float* H_TargetMatrix(uintptr_t self, float* out) {
    float* r = o_targetMatrix(self, out);
    float pin[3];
    const float* cur = nullptr;
    if (out && HeroTarget(self, pin, cur)) {
        AddLead(pin);
        out[12] = pin[0];
        out[13] = pin[1];
        out[14] = pin[2];
        g_targetReads.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// A joint of the hero (or his position): moved by however far he is off Mario.
float* H_TargetTrack(uintptr_t self, float* out) {
    float pin[3];
    const float* cur = nullptr;
    float before[3] = {0, 0, 0};
    const uintptr_t t = HeroTarget(self, pin, cur);
    if (t) std::memcpy(before, cur, sizeof(before));
    float* r = o_targetTrack(self, out);
    if (out && t) {
        AddLead(pin);
        for (int k = 0; k < 3; ++k) out[k] += pin[k] - before[k];
        g_targetReads.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// ---------------------------------------------------------------- install

bool Hook(uintptr_t target, void* detour, void** original, const char* name) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
    if (s != MH_OK) {
        LOGW("hero pin: hooking %s failed (MinHook %d)", name, int(s));
        return false;
    }
    s = MH_EnableHook(reinterpret_cast<void*>(target));
    if (s != MH_OK) {
        LOGW("hero pin: enabling the %s hook failed (MinHook %d)", name, int(s));
        MH_RemoveHook(reinterpret_cast<void*>(target));
        *original = nullptr;
        return false;
    }
    return true;
}

// `check` (bindings.ini, a byte pattern) must occur in the first 48 bytes.
bool LooksRight(uintptr_t fn, const std::string& check) {
    if (check.empty()) return true;
    const Pattern p = Pattern::Parse(check);
    if (!p.valid) return false;
    uint8_t code[48];
    if (!SafeRead(fn, code, sizeof(code))) return false;
    return FindPattern(code, sizeof(code), p) >= 0;
}

void InstallTransformHooks(Sm2Game& game, const Ini& b) {
    std::string d;
    const uintptr_t setPos = game.SetPositionAddress();
    const uintptr_t setMat = game.ResolveFunction(b, "TransformSetMatrix", d);
    const uintptr_t setEx = game.ResolveFunction(b, "TransformSetMatrixEx", d);
    const uintptr_t setEx2 = game.ResolveFunction(b, "TransformSetMatrixEx2", d);
    const uintptr_t dirty = game.ResolveFunction(b, "TransformMarkDirty", d);
    if (!setPos) {
        LOGW("hero pin: no Transform::SetPosition - Spider-Man is only moved once per frame");
        return;
    }
    std::string hooked;
    auto add = [&](uintptr_t a, void* det, void** orig, const char* name) {
        if (Hook(a, det, orig, name)) hooked += hooked.empty() ? name : std::string(", ") + name;
    };
    add(setPos, reinterpret_cast<void*>(&H_SetPosition), reinterpret_cast<void**>(&o_setPosition), "SetPosition");
    add(setMat, reinterpret_cast<void*>(&H_SetMatrix), reinterpret_cast<void**>(&o_setMatrix), "SetMatrix");
    add(setEx, reinterpret_cast<void*>(&H_SetMatrixEx), reinterpret_cast<void**>(&o_setMatrixEx), "SetMatrixEx");
    add(setEx2, reinterpret_cast<void*>(&H_SetMatrixEx2), reinterpret_cast<void**>(&o_setMatrixEx2), "SetMatrixEx2");
    add(dirty, reinterpret_cast<void*>(&H_MarkDirty), reinterpret_cast<void**>(&o_markDirty), "MarkDirty");
    g_transformHooks = o_setPosition != nullptr;
    if (g_transformHooks)
        LOGI("hero pin: Spider-Man is held on Mario through the game's frame (hooked %s)", hooked.c_str());
}

void InstallUnhideHook(Sm2Game& game, const Ini& b) {
    const uintptr_t unhide = game.UnhideAddress();
    if (!unhide) return;
    // The function starts "mov eax, [rcx+5Ch]; test al, 20h" (5 bytes, no
    // branch in them): safe to detour.
    if (!LooksRight(unhide, b.GetString("TransformUnhide", "check", "8B 41 5C A8 20"))) {
        LOGW("hero: Transform::Unhide doesn't start as expected - Spider-Man is only hidden again after the game shows him");
        return;
    }
    g_unhideHook = Hook(unhide, reinterpret_cast<void*>(&H_Unhide), reinterpret_cast<void**>(&o_unhide), "Unhide");
    if (g_unhideHook) LOGI("hero: the game can't show Spider-Man while Mario stands in for him (hooked Transform::Unhide)");
}

void InstallCameraHooks(Sm2Game& game, const Ini& b) {
    const std::string cls = b.GetString("CameraTarget", "class", "");
    if (cls.empty()) {
        LOGW("camera: no [CameraTarget] class in bindings.ini - the camera can't follow Mario's height");
        return;
    }
    const uintptr_t base = game.ModuleBase();
    const uintptr_t vt = rtti::FindVtable(base, rtti::ImageDataSpans(base), cls.c_str());
    if (!vt) {
        LOGW("camera: class %s not found in this game version - the camera can't follow Mario's height", cls.c_str());
        return;
    }
    g_targetActor = uint32_t(b.GetInt("CameraTarget", "actor_offset", 0x10));
    struct Slot {
        const char* key;
        const char* check;
        void* detour;
        void** original;
    } slots[] = {
        {"get_position", "check_get_position", reinterpret_cast<void*>(&H_TargetPosition),
         reinterpret_cast<void**>(&o_targetPosition)},
        {"get_matrix", "check_get_matrix", reinterpret_cast<void*>(&H_TargetMatrix),
         reinterpret_cast<void**>(&o_targetMatrix)},
        {"get_track_position", "check_get_track_position", reinterpret_cast<void*>(&H_TargetTrack),
         reinterpret_cast<void**>(&o_targetTrack)},
    };
    std::string hooked;
    for (const Slot& s : slots) {
        const int64_t index = b.GetInt("CameraTarget", s.key, -1);
        if (index < 0 || index > 200) continue;
        uintptr_t fn = 0;
        if (!SafeReadT(vt + uintptr_t(index) * 8, fn) || fn < base) continue;
        if (!LooksRight(fn, b.GetString("CameraTarget", s.check, ""))) {
            LOGW("camera: %s slot %lld (exe+0x%llx) doesn't look like it did - not hooked", s.key,
                 static_cast<long long>(index), static_cast<unsigned long long>(fn - base));
            continue;
        }
        if (Hook(fn, s.detour, s.original, s.key))
            hooked += (hooked.empty() ? "" : ", ") + std::string(s.key) + "@exe+0x" +
                      [&] {
                          char buf[24];
                          std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(fn - base));
                          return std::string(buf);
                      }();
    }
    g_cameraHooks = o_targetPosition || o_targetMatrix || o_targetTrack;
    if (g_cameraHooks) LOGI("camera: the game's camera target follows Mario (%s: %s)", cls.c_str(), hooked.c_str());
}

} // namespace

const char* KindName(int kind) {
    switch (kind) {
    case kSetPosition: return "SetPosition";
    case kSetMatrix: return "SetMatrix";
    case kSetMatrixEx: return "SetMatrixEx";
    case kSetMatrixEx2: return "SetMatrixEx2";
    case kMarkDirty: return "MarkDirty";
    default: return "?";
    }
}

void Install(Sm2Game& game, const Ini& bindings, const Options& opt) {
    if (g_installed) return;
    g_installed = true;
    g_posOffset = game.Layout().transformPosition;
    if (opt.transformWrites) InstallTransformHooks(game, bindings);
    if (opt.cameraTarget) InstallCameraHooks(game, bindings);
    if (opt.keepHidden) InstallUnhideHook(game, bindings);
}

void KeepHidden(uintptr_t transform) { g_keepHidden.store(transform, std::memory_order_release); }
bool TakeUnhideRefused() { return g_unhideRefused.exchange(false, std::memory_order_acq_rel); }
void KeepHiddenToo(const uintptr_t* transforms, int n) {
    for (int i = 0; i < kKeepHiddenExtra; ++i)
        g_keepHiddenExtra[i].store(i < n && transforms ? transforms[i] : 0, std::memory_order_release);
}
bool UnhideHooked() { return g_unhideHook; }

// ---------------------------------------------------------------- camera override API

void SetCameraSearch(bool on, const DVec3& camPos, const Vec3& camForward, float radius, float aim, uint32_t stamp) {
    SearchArea a;
    a.pos = camPos;
    a.forward = camForward;
    a.radius = radius;
    SetCameraSearch(on, a, SearchArea(), aim, stamp);
}

void SetCameraSearch(bool on, const SearchArea& a, const SearchArea& b, float aim, uint32_t stamp) {
    if (on) {
        const float rb = b.radius > 0.0f ? b.radius : 0.0f;
        const uint32_t v[16] = {Bits(float(a.pos.x)), Bits(float(a.pos.y)), Bits(float(a.pos.z)), Bits(a.forward.x),
                                Bits(a.forward.y),     Bits(a.forward.z),     Bits(float(b.pos.x)), Bits(float(b.pos.y)),
                                Bits(float(b.pos.z)),  Bits(b.forward.x),     Bits(b.forward.y),    Bits(b.forward.z),
                                Bits(a.radius),        Bits(rb),              Bits(aim),            0u};
        g_searchView.Write(v);
        const float fast[8] = {float(a.pos.x), float(a.pos.y), float(a.pos.z), a.radius > 0.0f ? a.radius * a.radius : -1.0f,
                               float(b.pos.x), float(b.pos.y), float(b.pos.z), rb > 0.0f ? rb * rb : -1.0f};
        for (int i = 0; i < 8; ++i) g_searchFast[i].store(fast[i], std::memory_order_relaxed);
        if (stamp) g_camStamp.store(stamp, std::memory_order_relaxed);
        else g_camStamp.fetch_add(1, std::memory_order_relaxed);
    }
    g_camSearch.store(on && g_transformHooks, std::memory_order_release);
}

void PruneCameraCandidates(uint32_t stamp) {
    for (CamCand& c : g_camCands) {
        if (!c.t.load(std::memory_order_acquire) || c.stamp.load(std::memory_order_relaxed) >= stamp) continue;
        ResetCandidate(c);
        c.t.store(0, std::memory_order_release);
    }
}

int MatchViewToCandidates(uint32_t stampLo, uint32_t stampHi, const DVec3& view, float tolerance,
                          CameraCandidate* out, int max) {
    int n = 0;
    const double tol2 = double(tolerance) * double(tolerance);
    for (CamCand& c : g_camCands) {
        if (n >= max) break;
        const uintptr_t t = c.t.load(std::memory_order_acquire);
        if (!t || CameraSlotOf(t) >= 0) continue;
        double best = 1e30;
        for (int k = 0; k < CamCand::kRecs; ++k) {
            const uint32_t st = c.recStamp[k].load(std::memory_order_acquire);
            if (st == 0 || st < stampLo || st > stampHi) continue;
            const DVec3 p(Float(c.recPos[k][0].load(std::memory_order_relaxed)),
                          Float(c.recPos[k][1].load(std::memory_order_relaxed)),
                          Float(c.recPos[k][2].load(std::memory_order_relaxed)));
            if (c.recStamp[k].load(std::memory_order_acquire) != st) continue; // (rewritten meanwhile)
            const DVec3 d = p - view;
            best = std::min(best, Dot(d, d));
        }
        if (!(best <= tol2)) continue;
        CameraCandidate& k = out[n++];
        k.transform = t;
        k.hits = c.hits.load(std::memory_order_relaxed);
        k.row = c.row.load(std::memory_order_relaxed);
        k.sign = Float(c.sign.load(std::memory_order_relaxed));
        k.meanGap = float(std::sqrt(best));
    }
    return n;
}

std::vector<CameraCandidate> TakeCameraCandidates() {
    std::vector<CameraCandidate> out;
    for (CamCand& c : g_camCands) {
        const uintptr_t t = c.t.load(std::memory_order_acquire);
        if (!t) continue;
        CameraCandidate k;
        k.transform = t;
        k.hits = c.hits.load(std::memory_order_relaxed);
        k.row = c.row.load(std::memory_order_relaxed);
        k.sign = Float(c.sign.load(std::memory_order_relaxed));
        k.meanGap = k.hits ? float(c.gapMm.load(std::memory_order_relaxed)) / 1000.0f / float(k.hits) : 0.0f;
        out.push_back(k);
    }
    return out;
}

void ClearCameraCandidates() {
    for (CamCand& c : g_camCands) {
        ResetCandidate(c);
        c.t.store(0, std::memory_order_release);
    }
}

static void ResetSlot(CamSlot& cs, uintptr_t t) {
    const uint32_t zero[12] = {};
    cs.t.store(0, std::memory_order_release); // (no writes placed while it changes)
    cs.game.Write(zero);
    cs.writes.store(0, std::memory_order_relaxed);
    cs.refused.store(0, std::memory_order_relaxed);
    cs.lastWrite.store(0, std::memory_order_relaxed);
    cs.t.store(t, std::memory_order_release);
}

void SetCameraTransform(uintptr_t t) {
    const bool same = g_camSlots[0].t.load(std::memory_order_acquire) == t;
    for (int i = 1; i < kCameraSlots; ++i)
        if (g_camSlots[i].t.load(std::memory_order_acquire)) ResetSlot(g_camSlots[i], 0);
    if (!same) {
        ResetSlot(g_camSlots[0], t);
        ClearCameraPlacements();
    }
}

int AddCameraTransform(uintptr_t t) {
    if (!t) return -1;
    const int have = CameraSlotOf(t);
    if (have >= 0) return have;
    for (int i = 1; i < kCameraSlots; ++i)
        if (!g_camSlots[i].t.load(std::memory_order_acquire)) {
            ResetSlot(g_camSlots[i], t);
            return i;
        }
    return -1;
}

void RemoveCameraTransform(int slot) {
    if (slot <= 0 || slot >= kCameraSlots) return;
    ResetSlot(g_camSlots[slot], 0);
}

void SwapCameraSlots(int a, int b) {
    if (a < 0 || b < 0 || a >= kCameraSlots || b >= kCameraSlots || a == b) return;
    // (Both cleared and set again: a write meanwhile goes unplaced, which a
    // frame of the game's own camera position shows - rare, and only once.)
    const uintptr_t ta = g_camSlots[a].t.load(std::memory_order_acquire);
    const uintptr_t tb = g_camSlots[b].t.load(std::memory_order_acquire);
    uint32_t ga[12] = {}, gb[12] = {};
    g_camSlots[a].game.Read(ga);
    g_camSlots[b].game.Read(gb);
    const uint64_t la = g_camSlots[a].lastWrite.load(std::memory_order_relaxed);
    const uint64_t lb = g_camSlots[b].lastWrite.load(std::memory_order_relaxed);
    g_camSlots[a].t.store(0, std::memory_order_release);
    g_camSlots[b].t.store(0, std::memory_order_release);
    g_camSlots[a].game.Write(gb);
    g_camSlots[b].game.Write(ga);
    g_camSlots[a].lastWrite.store(lb, std::memory_order_relaxed);
    g_camSlots[b].lastWrite.store(la, std::memory_order_relaxed);
    g_camSlots[a].t.store(tb, std::memory_order_release);
    g_camSlots[b].t.store(ta, std::memory_order_release);
}

uintptr_t CameraTransform() { return g_camSlots[0].t.load(std::memory_order_acquire); }

uintptr_t CameraTransformAt(int slot) {
    return slot >= 0 && slot < kCameraSlots ? g_camSlots[slot].t.load(std::memory_order_acquire) : 0;
}

CameraSlotStats TakeCameraSlotStats(int slot) {
    CameraSlotStats st;
    if (slot < 0 || slot >= kCameraSlots) return st;
    CamSlot& cs = g_camSlots[slot];
    st.transform = cs.t.load(std::memory_order_acquire);
    st.writes = cs.writes.exchange(0, std::memory_order_relaxed);
    st.refused = cs.refused.exchange(0, std::memory_order_relaxed);
    const uint64_t lw = cs.lastWrite.load(std::memory_order_relaxed);
    if (lw) std::memcpy(&st.lastWrite, &lw, sizeof(st.lastWrite));
    else st.lastWrite = -1.0;
    return st;
}

void SetCameraPlan(const CameraOverride::Plan& p) {
    uint64_t tb = 0;
    std::memcpy(&tb, &p.time, sizeof(tb));
    const uint32_t v[28] = {p.active ? 1u : 0u,
                            Bits(float(p.pivot.x)),
                            Bits(float(p.pivot.y)),
                            Bits(float(p.pivot.z)),
                            Bits(p.offset.x),
                            Bits(p.offset.y),
                            Bits(p.offset.z),
                            uint32_t(std::max(0, std::min(2, p.forwardRow))),
                            Bits(p.forwardSign),
                            Bits(p.blend),
                            p.collide ? 1u : 0u,
                            Bits(p.reach),
                            Bits(p.lookHeight),
                            uint32_t(std::max(0, std::min(2, p.up))),
                            p.checkView ? 1u : 0u,
                            Bits(p.viewForward.x),
                            Bits(p.viewForward.y),
                            Bits(p.viewForward.z),
                            Bits(float(p.viewPos.x)),
                            Bits(float(p.viewPos.y)),
                            Bits(float(p.viewPos.z)),
                            p.tag,
                            uint32_t(tb),
                            uint32_t(tb >> 32),
                            Bits(float(p.prevPivot.x)),
                            Bits(float(p.prevPivot.y)),
                            Bits(float(p.prevPivot.z)),
                            Bits(p.frameTime)};
    g_camPlan.Write(v);
}

void SetPresentThread(uint32_t id) { g_presentThread.store(id, std::memory_order_relaxed); }

CameraTiming TakeCameraTiming() {
    CameraTiming t;
    t.carried = g_camCarried.exchange(0, std::memory_order_relaxed);
    t.writes = g_camFrameWrites.exchange(0, std::memory_order_relaxed);
    t.aheadUs = g_camAheadUs.exchange(0, std::memory_order_relaxed);
    t.aheadCount = g_camAheadCount.exchange(0, std::memory_order_relaxed);
    t.aheadMaxUs = g_camAheadMaxUs.exchange(0, std::memory_order_relaxed);
    t.thread = g_camThread.load(std::memory_order_relaxed);
    return t;
}

std::vector<CameraPlacement> RecentCameraPlacements() {
    CameraPlacement buf[kPlacements];
    const int n = RecentCameraPlacements(buf, kPlacements);
    return std::vector<CameraPlacement>(buf, buf + n);
}

int RecentCameraPlacements(CameraPlacement* out, int max) {
    int n = 0;
    for (const auto& w : g_placements) {
        if (n >= max) break;
        uint32_t v[12], seq = 0;
        if (!w.Read(v, &seq) || seq == 0 || v[10] == 0) continue;
        CameraPlacement& c = out[n++];
        c.slot = int(v[11]);
        c.tag = v[0];
        c.pos = DVec3(Float(v[1]), Float(v[2]), Float(v[3]));
        c.pivot = DVec3(Float(v[4]), Float(v[5]), Float(v[6]));
        c.blend = Float(v[7]);
        const uint64_t tb = uint64_t(v[8]) | (uint64_t(v[9]) << 32);
        std::memcpy(&c.time, &tb, sizeof(c.time));
        c.order = v[10];
    }
    return n;
}

void ClearCameraPlacements() {
    const uint32_t zero[12] = {};
    for (auto& w : g_placements) w.Write(zero);
}

bool GameCameraSample(CameraOverride::GameSample& out, int slot) {
    uint32_t v[12], seq = 0;
    if (slot < 0 || slot >= kCameraSlots || !g_camSlots[slot].t.load(std::memory_order_acquire) ||
        !g_camSlots[slot].game.Read(v, &seq) || seq == 0 || (v[3] == 0 && v[4] == 0 && v[5] == 0)) {
        out.valid = false;
        return false;
    }
    out.valid = true;
    out.seq = seq;
    out.pos = DVec3(Float(v[0]), Float(v[1]), Float(v[2]));
    for (int r = 0; r < 3; ++r) out.rows[r] = Vec3(Float(v[3 + r * 3]), Float(v[4 + r * 3]), Float(v[5 + r * 3]));
    return true;
}

uint64_t TakeCameraWrites() { return g_camWrites.exchange(0, std::memory_order_relaxed); }
uint64_t TakeCameraRefused() { return g_camRefused.exchange(0, std::memory_order_relaxed); }

bool TransformHooks() { return g_transformHooks; }
bool CameraHooks() { return g_cameraHooks; }

void SetPin(uintptr_t transform, const float pos[3], int up, float radius, bool hold) {
    g_hold.store(hold, std::memory_order_relaxed);
    g_px.store(Bits(pos[0]), std::memory_order_relaxed);
    g_py.store(Bits(pos[1]), std::memory_order_relaxed);
    g_pz.store(Bits(pos[2]), std::memory_order_relaxed);
    g_up.store(up, std::memory_order_relaxed);
    g_radius2.store(Bits(radius * radius), std::memory_order_relaxed);
    g_transform.store(transform, std::memory_order_release);
}

void ClearPin() {
    g_transform.store(0, std::memory_order_release);
    g_lead.store(Bits(0.0f), std::memory_order_relaxed);
}

void SetCameraLead(float metres) {
    if (!(metres == metres)) metres = 0; // NaN
    g_lead.store(Bits(metres), std::memory_order_relaxed);
}

Stats TakeStats() {
    Stats s;
    for (int k = 0; k < kKinds; ++k) s.undone[k] = g_undone[k].exchange(0, std::memory_order_relaxed);
    s.passed = g_passed.exchange(0, std::memory_order_relaxed);
    s.targetReads = g_targetReads.exchange(0, std::memory_order_relaxed);
    s.unhideBlocked = g_unhideBlocked.exchange(0, std::memory_order_relaxed);
    for (Slot& slot : g_unhideSlots) {
        const uintptr_t c = slot.caller.load(std::memory_order_acquire);
        if (!c) continue;
        Writer w;
        w.caller = c;
        w.count = slot.count.exchange(0, std::memory_order_relaxed);
        if (w.count) s.unhiders.push_back(w);
    }
    for (Slot& slot : g_slots) {
        const uintptr_t c = slot.caller.load(std::memory_order_acquire);
        if (!c) continue;
        Writer w;
        w.caller = c;
        w.count = slot.count.exchange(0, std::memory_order_relaxed);
        w.maxOffset = Float(slot.maxBits.exchange(0, std::memory_order_relaxed));
        w.kind = slot.kind.load(std::memory_order_relaxed);
        if (w.count) s.writers.push_back(w);
    }
    return s;
}

} // namespace hero_pin
} // namespace sm2m

#endif
