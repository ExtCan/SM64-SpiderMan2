// Mario's collision from the game's own physics.
//
// Spider-Man 2 moves Spider-Man through a Havok world; the mod asks it the
// same questions with ray casts (src/game/sm2_physics.cpp runs them on the
// game's thread) and turns the answers into libsm64 surfaces:
//
//   columns  multi-hit rays straight down on a world-aligned grid (0.5 m):
//            every floor, roof, awning and street at that spot, with the
//            physics material of each (concrete, grass, wood, metal ...)
//   ring     horizontal rays out from Mario at knee, hip and head height in
//            32 directions: the walls, posts and railings around him, exactly
//            where the game has them (whatever the camera sees)
//   water    a water-only ray under Mario: the river and the sea
//
// The game answers a limited number of rays a frame, so they are scheduled:
// the ground under Mario first, then the ring, then the rest of the area,
// refreshing old answers (often near Mario, fast for things that move).
// Everything is kept in world space, so it stays valid as Mario moves.
//
// Platform independent (unit tested with a synthetic world).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/vec.h"
#include "collision.h"
#include "coords.h"
#include "surface_types.h"

namespace sm2m {

enum class HitKind : uint8_t {
    World = 0,     // static world geometry
    Movable = 1,   // belongs to an actor: vehicles, props (refreshed often)
    Character = 2, // a person: not solid for Mario
};

struct GameHit {
    DVec3 pos;               // game space
    Vec3 normal;             // game space, unit length
    int16_t material = -1;   // physics material (surface_types.h)
    HitKind kind = HitKind::World;
    bool actor = false;      // it belongs to an actor (a car, a prop, a person), not the static world
    uintptr_t actorId = 0;   // which one (opaque to the world model: for the log)
};

constexpr int kMaxGameHits = 8;
constexpr int kMaxColumnLayers = 16; // a column's layers, over its follow-up rays

struct GameRay {
    DVec3 from, to;          // game space
    uint32_t tag = 0;
    uint8_t type = 3;        // the game's collision query type (3 = kHeroMove)
    uint8_t maxHits = 1;
};

struct GameRayResult {
    uint32_t tag = 0;
    int count = 0;           // hits, nearest first
    GameHit hits[kMaxGameHits];
};

struct PhysicsWorldParams {
    float cell = 0.5f;            // column spacing (m)
    int radius = 10;              // columns kept around Mario for his floor grid (cells)
    // Column rays start this far above Mario's feet (m): above anything he
    // can jump onto (floors count up to ProbeUp above him), not so far that
    // awnings, fire escapes and trees overhead use up a ray's hits before it
    // reaches his floor (0.5: 20 m - a refreshed answer that stopped above
    // his floor left a hole under him until its follow-up came in).
    float castAbove = 8.0f;
    float castBelow = 60.0f;      // ... and end this far below them
    int columnHits = 8;
    int ringDirs = 32;
    int ringHits = 6;             // hits per ring ray (people in the way don't hide the wall behind them)
    float ringRadius = 5.0f;      // m
    float ringHeights[3] = {0.3f, 0.9f, 1.55f};
    double nearRefresh = 0.4;     // s: columns within nearRadius
    double farRefresh = 3.0;      // s: the rest
    double movableRefresh = 0.2;  // s: columns with a vehicle or prop in them
    double ringRefresh = 0.12;    // s: each ring ray
    double wallLife = 4.0;        // s: a wall nothing has confirmed for this long is dropped
    float unknownTopAbove = 8.0f; // m: a wall whose top no ray found reaches this far above where it was hit
                                  // (more than Mario jumps; the ring finds it again higher up as he rises)
    float airApron = 40.0f;       // m: in the air, the safety floor reaches this far past the grid
    float nearRadius = 2.5f;      // m
    uint8_t floorType = 3;        // query types (the game's CollRequest types; 3 = kHeroMove)
    uint8_t wallType = 3;
    uint8_t waterType = 17;       // kWater
    double waterRefresh = 0.3;    // s
};

// Where a surface of a Build() came from (one per surface, for the log:
// what Mario bumped into, what he stood on).
struct SurfaceOrigin {
    enum Kind : uint8_t { Floor = 0, StepWall = 1, RingWall = 2, SafetyFloor = 3 };
    Kind kind = Floor;
    uint32_t wall = 0; // RingWall: the wall's id (PhysicsWorld::DescribeWall)
};

struct PhysicsWorldStats {
    uint64_t raysSent = 0;
    uint64_t results = 0;
    uint64_t hits = 0;
    uint64_t lost = 0;            // rays that never got an answer
    uint64_t actorBurning = 0;    // hits on actors whose material would burn Mario (cars say kAcid): plain ground
    uint64_t partials = 0;        // answers that stopped at their hit limit (continued by a follow-up ray)
    uint64_t keptLayers = 0;      // floors kept over an answer that didn't have them (see Layer::kept)
    size_t columns = 0;
    size_t walls = 0;
    size_t pending = 0;
};

class PhysicsWorld {
public:
    // upAxis: 1 = Y up (Spider-Man 2), 2 = Z up.
    void Configure(const PhysicsWorldParams& p, int upAxis);
    const PhysicsWorldParams& Params() const { return p_; }
    void Reset();

    // Appends up to `budget` rays to cast next. `feet` = Mario's feet, game space.
    void Schedule(const DVec3& feet, const Vec3& velocity, double now, int budget, std::vector<GameRay>& out);
    // An answer from the game. Unknown tags are ignored.
    void Accept(const GameRayResult& r, double now);

    // libsm64 surfaces around Mario (local units, `marioLocal`) from what is
    // known: floors (with their materials) from the columns, walls from the
    // ring, step walls where the floor jumps, a safety floor under what the
    // game has answered for (nothing where it hasn't yet: SM64 treats a spot
    // without a floor like a wall, so Mario waits at its edge).
    // `origins`, if given, gets one entry per surface in `out`.
    void Build(const WorldMapping& map, const Vec3& marioLocal, const CollisionParams& cp,
               std::vector<SM64Surface>& out, CollisionStats& stats,
               std::vector<SurfaceOrigin>* origins = nullptr) const;

    // For the log: a wall the ring found (its id from a SurfaceOrigin), and
    // what a column says (the one nearest a game-space point). `actor` gets
    // the wall's actor (0: the static world).
    std::string DescribeWall(uint32_t id, double now, uintptr_t* actor = nullptr) const;
    std::string DescribeColumn(const DVec3& at, double now) const;

    // Mario is in the air: the next Build() puts the safety floor under all
    // of the grid and well past it. (On the ground it is only under what the
    // game has answered for, so he waits at the edge of what is known - in the
    // air that edge would be an invisible wall: SM64 bonks him off a spot
    // without a floor.)
    void SetAirborne(bool air) { airborne_ = air; }
    bool Airborne() const { return airborne_; }

    // The ground under Mario has been asked about (enough to stand on).
    bool Ready(const DVec3& feet) const;
    // Height (game up coordinate) of the water surface under Mario, if a
    // water ray found one recently.
    bool Water(double& level) const;
    // Something near Mario changed since the last call (new floor, wall...).
    bool TakeChanged();
    // The physics material of the floor right under `feet` (-2: unknown).
    int FloorMaterialAt(const DVec3& feet) const;

    const PhysicsWorldStats& Stats() const;
    std::string Debug(const DVec3& feet) const;

    // World column index of a game-space point (exposed for tests).
    int64_t ColumnIndex(double v) const;

private:
    struct Layer {
        float y = 0;             // game up coordinate of the hit
        float up = 0;            // normal's up component
        int16_t material = -1;
        HitKind kind = HitKind::World;
        // Kept from an older answer: the newest one didn't confirm it (it
        // stopped at its hit limit above it, or missed it once - a ray can
        // slip through a seam). Dropped if the next answer doesn't have it
        // either; never a reason to stop calling the column known.
        bool kept = false;
    };
    struct Column {
        Layer layers[kMaxColumnLayers];
        int count = 0;
        double lo = 0, hi = 0;   // vertical range the last ray covered
        double time = -1;        // last answer
        double asked = -1;       // ray in flight since
        bool movable = false;
        // The ray stopped at its hit limit before reaching `lo` (a stack of
        // decks, a fire escape: every slab answers twice): what lies below
        // `below` is asked by a follow-up ray. Until then the column isn't
        // known all the way down.
        bool partial = false;
        double below = 0;
        int followUps = 0;
        // Partial, but what the answer before said about below `below` is
        // kept meanwhile (that one went all the way down): still known.
        bool filled = false;
        // Known all the way down (or kept from an answer that was).
        bool Known() const { return time >= 0 && (!partial || filled); }
    };
    struct Wall {
        uint32_t id = 0;
        DVec3 pos;               // hit point
        Vec3 n;                  // horizontal unit normal (game space)
        float half = 0.3f;       // half width (m)
        float gap = 0.6f;        // m along its face to where the next ring ray would meet it
        double base = 0;         // game up coordinate of Mario's feet when found
        double top = 0;          // its top (game up), when a top ray found one
        bool topKnown = false;
        double topAsked = -1;    // a top ray was sent (or -1)
        int16_t material = -1;
        HitKind kind = HitKind::World;
        uintptr_t actor = 0;
        double time = 0;
        double found = 0;        // first seen
    };
    enum class Purpose : uint8_t { Column, Ring, Water, WallTop };
    struct Pending {
        Purpose purpose = Purpose::Column;
        uint64_t column = 0;
        bool followUp = false;   // a column's follow-up ray (continues below its last hit)
        double columnTime = -1;  // ... of the answer it continues
        int ringSlot = 0;
        uint32_t wall = 0;
        DVec3 from, to;
        double base = 0;
        double time = 0;
    };

    static uint64_t Key(int64_t ix, int64_t iz);
    double Up(const DVec3& v) const { return v[up_]; }
    void Horizontal(const DVec3& v, double& a, double& b) const;
    DVec3 Point(double a, double b, double up) const;
    const Column* FindColumn(int64_t ix, int64_t iz) const;
    void Carve(const DVec3& from, const DVec3& to, double now);
    // A ring ray at height `up` went over a wall's footprint (nothing there
    // up to `clear`): the wall can't reach that high.
    void CapWalls(const DVec3& from, const DVec3& dir, double clear, double up);
    bool WallAlone(const Wall& w) const;
    void AddWall(const GameHit& h, const DVec3& dir, double dist, double base, double now);
    void Expire(const DVec3& feet, double now);

    PhysicsWorldParams p_;
    int up_ = 1, ha_ = 0, hb_ = 2;
    std::unordered_map<uint64_t, Column> columns_;
    std::vector<Wall> walls_;
    std::vector<double> ringTime_;      // per ring slot: last time asked
    std::unordered_map<uint32_t, Pending> pending_;
    uint32_t nextTag_ = 1;
    uint32_t nextWall_ = 1;
    double waterAsked_ = -1e9, waterTime_ = -1e9;
    bool waterKnown_ = false;
    double waterLevel_ = 0;
    DVec3 lastFeet_;
    bool changed_ = false;
    bool airborne_ = false;
    mutable PhysicsWorldStats stats_;
};

} // namespace sm2m
