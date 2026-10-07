// Mario's picture of the city, built from what the game renders.
//
// The game's G-buffer holds the linear depth of every visible surface. The
// mod reads it back (with the exact view that rendered it), turns pixels into
// world points with normals, and keeps them in a sparse 2.5D grid around
// Mario:
//   - floor layers per cell (street, kerb, car roof, awning: up to 4 heights)
//   - wall columns per cell (facades, car sides, poles) with their normal
// Cells are remembered after they leave the screen, so the ground behind a
// car or under Mario's feet is still there, and they are carved away when a
// later frame sees through them (a car that drove off).
//
// The model answers ray casts, so the existing CollisionBuilder turns it into
// libsm64 surfaces exactly as it would a physics raycast.
// Platform independent (unit tested against libsm64 with a real ROM).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/vec.h"
#include "../render/view_constants.h"
#include "collision.h"

namespace sm2m {

struct DepthFrame {
    int width = 0, height = 0;
    std::vector<float> depth; // linear view depth in metres, row 0 at the top; <= 0 = nothing
    // Optional: the G-buffer's motion vectors at the same pixels (2 floats
    // each; the game's encoding: (uv now - uv last frame) * view row 33).
    // Pixels that moved differently from what the camera alone explains belong
    // to moving things (cars, people): they never become collision.
    std::vector<float> motion;
    ViewConstants view;       // the view that rendered it
    double time = 0;
};

struct Cylinder {
    DVec3 base;               // feet
    float radius = 0.6f;
    float below = 0.15f;      // ignore points this far below the feet ...
    float height = 2.0f;      // ... up to this far above them
};

// Axis-aligned box in game space (e.g. the bounds of Mario's mesh as drawn in
// the frame the depth comes from).
struct ExcludeBox {
    DVec3 lo, hi;
};

struct WorldModelParams {
    float cell = 0.25f;          // metres
    float range = 30.0f;         // only points within this horizontal distance of the focus are kept
    float maxViewDepth = 90.0f;  // ignore pixels further than this from the camera
    int stride = 2;              // pixel step when integrating
    float floorMinUp = 0.70f;    // normal.up >= this: floor
    float wallMaxUp = 0.40f;     // |normal.up| <= this: wall
    float layerMerge = 0.22f;    // floor samples within this height share a layer
    float edgeJump = 0.05f;      // neighbour depth jump (fraction of depth) = silhouette edge
    float carveRadius = 12.0f;   // cells within this of the focus are re-checked every frame
    float carveTolerance = 0.30f;// metres, plus 3% of the distance to the camera
    int minFloorHits = 2;        // observations before a floor layer is trusted
    int minWallHits = 2;
    int forgetMisses = 3;        // see-through observations that delete a layer / wall
    float holeFill = 0.6f;       // floor queries borrow heights from cells this close
    float wallTopMargin = 0.05f; // walls extend this far above the highest point seen
    float wallBottomMargin = 0.4f;
    float forgetDistance = 60.0f;// cells further than this from the focus are dropped
    // Moving pixels: off from the camera's own motion by more than this many
    // render pixels, plus this fraction of the camera's motion there.
    float movingPixels = 3.0f;
    float movingRelative = 0.12f;
    // A frame where more than this fraction "moved" doesn't match the camera
    // (a cut, or vectors encoded differently): its motion is ignored.
    float maxMovingFraction = 0.4f;
    float changeRadius = 6.0f;   // removals this close to the focus are reported (collision rebuild)
};

struct WorldModelStats {
    int frames = 0;
    int cells = 0;
    int floorLayers = 0;
    int wallCells = 0;
    int samples = 0;        // points integrated by the last frame
    int floorSamples = 0, wallSamples = 0, excluded = 0, edges = 0;
    int carved = 0;         // layers / walls removed by the last frame
    int moving = 0;         // samples of moving things (not integrated) in the last frame
    int unsettled = 0;      // layers / walls the last frame saw moving (no longer solid)
    int nearChanges = 0;    // carved or unsettled within changeRadius of the focus
    bool motionUsed = false;     // the last frame's motion vectors were used
    int motionFrames = 0, motionRejected = 0; // frames with motion vectors / ignored as inconsistent
};

class WorldModel {
public:
    explicit WorldModel(int upIndex = 1) { SetUpAxis(upIndex); }
    void SetUpAxis(int upIndex);
    void Configure(const WorldModelParams& p) { p_ = p; }
    const WorldModelParams& Params() const { return p_; }
    void Clear();

    // A floor known without seeing it (Spider-Man's feet at activation).
    void Seed(const DVec3& feet, float radius = 0.75f);

    // Adds one captured depth frame. `focus` is Mario; `exclude` / `boxes` are
    // bodies in the frame that must not become collision (Mario himself where
    // he was drawn, the hero if he is visible).
    void Integrate(const DepthFrame& f, const DVec3& focus, const std::vector<Cylinder>& exclude,
                   const std::vector<ExcludeBox>& boxes = {});

    // Ground used where nothing has been seen yet, within `radius` of `centre`
    // (so Mario never falls through unobserved ground next to him).
    void SetFallbackGround(const DVec3& centre, double height, double radius) {
        fbCentre_ = centre;
        fbHeight_ = height;
        fbRadius_ = radius;
    }

    // Nearest hit on the segment (game space), IRaycaster semantics.
    bool Cast(const DVec3& from, const DVec3& to, RayHit& hit) const;
    // Highest trusted floor at or below `below` near (x, z); false if none.
    bool FloorBelow(const DVec3& p, double& height) const;
    // Highest trusted floor layer of the cell containing p within [bottom, top]
    // (that cell only: no hole filling, no fallback ground).
    bool CellFloorAt(const DVec3& p, double top, double bottom, double& height) const;

    const WorldModelStats& Stats() const { return stats_; }
    // The cells within `r` cells of `p`, one per line (tests / debugging).
    std::string DebugString(const DVec3& p, int r) const;
    void DebugDump(const DVec3& p, int r) const;
    size_t CellCount() const { return cells_.size(); }

private:
    // hits = confidence (capped, so a surface seen for minutes still goes
    // away a second after it is seen to be gone); misses = times a later
    // frame looked through it.
    static constexpr uint16_t kMaxConfidence = 24;
    struct Layer {
        float h = 0;
        uint16_t hits = 0;
        uint16_t misses = 0;
        float sx = 0, sz = 0, sw = 0; // where in the cell it was seen (sums, relative to the cell origin)
    };
    struct Cell {
        Layer floor[4];
        uint8_t nFloor = 0;
        uint16_t wallHits = 0, wallMisses = 0;
        float wallLo = 1e30f, wallHi = -1e30f;
        float wallSamples = 0;   // weight of the sums below
        float nx = 0, nz = 0;    // summed horizontal normal (h0, h1 axes)
        float cx = 0, cz = 0;    // summed horizontal position, relative to the cell origin
    };
    using Key = uint64_t;
    Key KeyOf(int64_t i, int64_t j) const { return (uint64_t(uint32_t(int32_t(i))) << 32) | uint32_t(int32_t(j)); }
    void CellIndex(const DVec3& p, int64_t& i, int64_t& j) const;
    DVec3 CellCentre(int64_t i, int64_t j, double up) const;
    void AddFloor(Cell& c, float h, double ox, double oz);
    void AddWall(Cell& c, float h, const Vec3& n, double ox, double oz);
    void Carve(const DepthFrame& f, const DVec3& focus, const std::vector<Cylinder>& exclude,
               const std::vector<ExcludeBox>& boxes);
    // Marks pixels of moving things (motion vectors that the camera's own
    // motion doesn't explain). False if the frame's vectors can't be used.
    bool MovingMask(const DepthFrame& f, std::vector<uint8_t>& mask);
    // A moving thing is at P now: whatever the cell remembers there is it (or
    // was it): no longer solid until seen standing still again.
    void Unsettle(Cell& c, const DVec3& P, float nu, bool near);
    bool Near(const DVec3& p, const DVec3& focus) const;
    bool CellFloor(const Cell& c, double top, double bottom, double& h) const;
    bool WallSolid(const Cell& c) const { return c.wallHits >= p_.minWallHits; }

    int up_ = 1, h0_ = 0, h1_ = 2;
    WorldModelParams p_;
    std::unordered_map<Key, Cell> cells_;
    WorldModelStats stats_;
    std::vector<uint8_t> movingMask_;
    DVec3 fbCentre_;
    double fbHeight_ = 0, fbRadius_ = 0;
};

// IRaycaster adapter for CollisionBuilder.
class WorldModelRaycaster final : public IRaycaster {
public:
    explicit WorldModelRaycaster(const WorldModel* m) : m_(m) {}
    bool Cast(const DVec3& from, const DVec3& to, RayHit& hit) override { return m_->Cast(from, to, hit); }
    const char* Name() const override { return "rendered world"; }

private:
    const WorldModel* m_;
};

} // namespace sm2m
