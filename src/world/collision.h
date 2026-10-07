// Builds libsm64 collision around Mario by probing the game world with rays.
//
// Spider-Man 2's collision data can't be handed to libsm64 directly, so the
// mod samples it around Mario every so often:
//   1. a grid of multi-layer downward rays -> floor triangles (roofs, ledges,
//      awnings, the street under them), with height jumps -> step walls
//   2. short horizontal rays along the grid edges near Mario -> contiguous
//      walls, including facades taller than the downward rays' start height
//      (rays that start inside a building pass straight through it)
//   3. a ring of radial rays -> thin things the grid misses (poles, railings)
// Every wall gets its real top from a downward "top-finding" ray, and walls are
// clipped against coplanar walls already emitted: SM64 pushes Mario once per
// overlapping wall ("wall overlaps" quirk), so duplicates would make him bounce.
//
// The ray source is an interface: the game's raycast once that binding is
// known, or a flat ground plane as the no-reverse-engineering fallback.
#pragma once

#include <cstdint>
#include <vector>

#include "../common/vec.h"
#include "coords.h"
#include "libsm64.h"
#include "surface_types.h"

namespace sm2m {

struct RayHit {
    DVec3 point;
    Vec3 normal; // game space, unit length
};

class IRaycaster {
public:
    virtual ~IRaycaster() = default;
    // Nearest hit on the segment from -> to (game space). Rays that start
    // inside a solid may pass through it (one-sided meshes) - the builder
    // copes with either behaviour.
    virtual bool Cast(const DVec3& from, const DVec3& to, RayHit& hit) = 0;
    virtual const char* Name() const = 0;
};

// Infinite horizontal plane at a fixed height. Used when no game raycast is
// available so Mario still has ground to stand on.
class FlatGroundRaycaster final : public IRaycaster {
public:
    FlatGroundRaycaster(char upAxis, double height) : up_(upAxis == 'Z' || upAxis == 'z' ? 2 : 1), h_(height) {}
    bool Cast(const DVec3& from, const DVec3& to, RayHit& hit) override;
    const char* Name() const override { return "flat-ground fallback"; }
    void SetHeight(double h) { h_ = h; }
    double Height() const { return h_; }

private:
    int up_;
    double h_;
};

struct CollisionParams {
    // All distances in game units (metres).
    float cellSize = 0.5f;
    int gridRadiusCells = 10;      // grid is (2R+1)^2 vertices
    float probeUp = 4.5f;          // downward rays start this far above Mario's feet (>= jump height)
    float probeDown = 120.0f;      // and search this far below them
    int maxLayers = 3;             // surfaces stacked at one spot (street under an awning)
    float stepHeight = 0.6f;       // larger jumps between neighbours become walls
    float layerGap = 0.25f;        // re-cast this far below a hit to find the next layer
    float minClearance = 1.8f;     // lower layers with less headroom are dropped (inside solids, under cars)
    int edgeProbeRadiusCells = 6;  // horizontal grid-edge rays within this many cells of Mario
    float edgeProbeHeights[2] = {0.4f, 1.4f};
    float edgeProbeMaxDrop = 3.0f; // only probe from floors within this of Mario's height
    int wallProbeDirs = 16;
    float wallProbeDist = 3.5f;
    float wallProbeHeights[3] = {0.3f, 0.9f, 1.5f};
    float wallSlabWidth = 1.4f;
    float wallSlabBelow = 1.0f;
    float tallWallExtra = 4.0f;    // walls whose top isn't found reach probeUp + this
    float coplanarTolerance = 0.3f;
    int maxSurfaces = 8000;
};

struct CollisionStats {
    int rays = 0;
    int hits = 0;
    int floors = 0;
    int flattened = 0;
    int edgeWalls = 0;
    int stepWalls = 0;
    int probeWalls = 0;
    int clipped = 0; // wall pieces dropped because a coplanar wall already covered them
    int total = 0;
    float safetyFloorY = -1e30f; // local height of the safety floor under everything (Mario on it = out of the world)
};

// Vertical walls emitted so far. New ones are clipped against coplanar walls
// in the wall's plane - along it *and* in height - so SM64 never gets two
// overlapping coplanar walls (it pushes Mario once per wall: the "wall
// overlaps" quirk), while a piece that starts higher up (its floor estimate
// lifted by a stray sample) can't hide the full-height piece next to it.
class WallSet {
public:
    WallSet(std::vector<SM64Surface>& out, int maxSurfaces, float tolerance)
        : out_(out), max_(maxSurfaces), tol_(tolerance) {}
    // Emits the parts of the vertical quad a..b (horizontal extent, local
    // units), lo..hi, facing n, that no coplanar wall covers yet. Returns the
    // triangles emitted; `clipped` counts quads that were (partly) covered.
    int Emit(const Vec3& a0, const Vec3& b0, float lo, float hi, const Vec3& nIn, int& clipped,
             SurfaceKind kind = SurfaceKind{});

private:
    struct Seg {
        Vec3 a, b, n;
        float lo, hi;
    };
    std::vector<SM64Surface>& out_;
    int max_;
    float tol_;
    std::vector<Seg> segs_;
};

// One vertex of the floor grid: the floor heights found there (local units,
// highest first) and what each one is made of.
struct GridVertex {
    float h[4] = {};
    SurfaceKind kind[4] = {};
    int count = 0;
    bool lowKnown = true; // h[count - 1] is the ground there (not just as far down as was found)
};

// The floor grid around Mario: (2R+1)^2 vertices `cell` apart (local units),
// vertex (i, j) at (cx + (i - R) * cell, cz + (j - R) * cell).
struct FloorGrid {
    int R = 0;
    float cx = 0, cz = 0, cell = 1;
    std::vector<GridVertex> v;
    int N() const { return 2 * R + 1; }
    GridVertex& At(int i, int j) { return v[size_t(j) * size_t(N()) + size_t(i)]; }
    const GridVertex& At(int i, int j) const { return v[size_t(j) * size_t(N()) + size_t(i)]; }
    float X(int i) const { return cx + float(i - R) * cell; }
    float Z(int j) const { return cz + float(j - R) * cell; }
};

class CollisionBuilder {
public:
    // Builds surfaces (local SM64 units) around `marioLocal`.
    void Build(IRaycaster& rays, const WorldMapping& map, const Vec3& marioLocal, const CollisionParams& p,
               std::vector<SM64Surface>& out, CollisionStats& stats);

    // Helper: appends one triangle whose normal faces `desired` (local space),
    // skipping degenerate ones. Returns false if skipped.
    static bool AddTri(std::vector<SM64Surface>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                       const Vec3& desired, SurfaceKind kind = SurfaceKind{});

    // The grid stages of Build, for other sources of floor heights:
    // floor triangles between neighbouring vertices whose heights agree
    // within a step (and a floor at the lowest level of a ledge)...
    static void EmitFloors(const FloorGrid& g, const CollisionParams& p, float s, std::vector<SM64Surface>& out,
                           CollisionStats& stats);
    // ... walls where the ground level jumps by more than a step between
    // neighbours (clipped against the walls already in `walls`)...
    static void EmitStepWalls(const FloorGrid& g, const CollisionParams& p, float s, WallSet& walls,
                              CollisionStats& stats);
    // ... and a floor under everything (at local height `y`)...
    // `apron` (local units) reaches past the grid on every side.
    static void EmitSafetyFloor(const FloorGrid& g, float y, std::vector<SM64Surface>& out, CollisionStats& stats,
                                float apron = 0.0f);
    // ... or only under the grid cells whose four corners are known (`known`:
    // a flag per vertex, as FloorGrid::v). Where nothing is known yet there is
    // no floor at all, and SM64 stops Mario at its edge as at a wall (he
    // can't run or fall into a hole the game hasn't been asked about yet).
    static void EmitSafetyFloor(const FloorGrid& g, const std::vector<uint8_t>& known, float y,
                                std::vector<SM64Surface>& out, CollisionStats& stats);
};

// Computes the normal libsm64 will derive for a surface (same formula as
// load_surfaces.c), unnormalised.
Vec3 Sm64SurfaceNormal(const SM64Surface& s);

// Mario fell through ground libsm64 didn't have yet (the camera hadn't seen
// it, or depth frames arrived late at a low frame rate). SM64 only lands him
// on floors up to 78 units above his feet. `path` is where he was on each tick
// of this fall (oldest first, the last entry is now). If he is falling
// (vy < 0) and a floor now exists above him (up to `maxClimb` units) that his
// path went down through - from above it to below it, over it both times -
// returns true with its height in `floorY`. A floor he only moved underneath
// (an awning, a canopy) doesn't count. All in local SM64 units; findFloor is
// sm64_surface_find_floor_height.
bool GroundAboveFallingMario(float (*findFloor)(float, float, float), const Vec3* path, int count, float vy,
                             float maxClimb, float& floorY);

} // namespace sm2m
