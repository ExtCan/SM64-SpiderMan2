// The game's own physics, asked on the game's thread.
//
// Spider-Man 2 answers ray casts against its Havok world through an
// Insomniac wrapper (Physics' immediate ray cast): a CollRequest says what the
// ray collides with (a preset per purpose - the hero's movement, water, ...)
// and how many hits to collect, and the result lists every hit with its
// position, normal, physics material and the actor it belongs to. The game
// casts these from its update thread, outside the physics step, and frees the
// results at the end of each physics frame.
//
// So the mod hooks that end of frame (bindings.ini [PhysicsFrame]: the step
// has finished, the frame's query results were just freed) and casts the rays
// it queued right after it - a few dozen per frame, leaving most of the
// game's per-frame query pool to the game. The same hook tells whether the
// world is running at all: the game skips its physics frame while it is
// paused (pause menu, photo mode, loading), and Mario pauses with it.
//
// PhysicsWorld (src/world/physics_world.h) decides which rays to cast and
// turns the answers into Mario's collision.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <vector>

#include "../common/ini.h"
#include "../world/physics_world.h"

namespace sm2m {

class Sm2Game;

namespace game_physics {

// An answer as the game gave it, plus the actor each hit belongs to (0: the
// static world). The mod thread sorts the actors into GameHit::kind.
struct RawResult {
    GameRayResult r;
    uintptr_t actors[kMaxGameHits] = {};
};

struct Options {
    int raysPerFrame = 24; // at most this many rays per game frame
    int poolHeadroom = 96; // ... and only while the game has used fewer than (pool size - this) query results
};

// Resolves bindings.ini [PhysicsRaycast], [PhysicsFrame], [CollRequest*],
// [QueryResultActor], [PhysicsQueryPool], [DamageSphere] and hooks the
// physics frame (once). Returns Available().
bool Install(Sm2Game& game, const Ini& bindings, const Options& opt);
// Rays can be cast (the ray cast, its request and the frame hook resolved).
bool Available();
// The frame hook can tell when the game's world is paused.
bool PauseDetection();
// One line for the log: what resolved.
std::string Describe();

// ---- mod thread
// Rays are cast only while active. `hero` (an Actor*, may be 0) is left out
// of every ray.
void SetActive(bool on, uintptr_t hero);
// The actor left out of the rays from now on (0: none - Spider-Man is gone
// for a moment, or a different hero actor took over).
void SetHero(uintptr_t hero);
int Queued();
void Submit(const std::vector<GameRay>& rays);
// Rays that go before everything queued (the camera's). Queued rays whose
// tag has all of `replace`'s bits set are dropped first (only the newest of
// those matter).
void SubmitFirst(const std::vector<GameRay>& rays, uint32_t replace);
// Answers since the last call (appended).
void Take(std::vector<RawResult>& out);
// Drops queued rays and unread answers.
void Clear();

// The world's heartbeat, counted in the frame hook.
struct Heartbeat {
    uint64_t calls = 0;   // physics frames
    uint64_t steps = 0;   // ... in which the world ran
};
Heartbeat GetHeartbeat();

// ---- the game's damage system
// A hit, the game's way: the DamageSystem damages one actor
// (DamageSystem::DamageActor, by its handle) or what a sphere touches
// (DamageSphere) with the game's own rules - health, hit reactions and
// knockback animations, friendly fire, deaths. Sent on the game's thread like
// the rays.
struct DamageOrder {
    uint32_t target = 0;     // actor handle: that actor (DamageActor) - else the sphere
    DVec3 center;            // game space (the sphere)
    float radius = 0.5f;     // m
    float amount = 0;        // health points
    int type = 1;            // the game's DamageType (1 = kMelee)
    int knockback = 2;       // the game's Knockback (Reaction in combat.h)
    float knockbackAmount = 100.0f; // the game fills stagger meters with it: its own hits use 10, 100, 1000
    uint32_t flags = 0;      // the game's RequestFlags
    uint32_t damager = 0;    // actor handle of the attacker (Spider-Man)
};
// The game's own hits: knockbackAmount 100 for a knockdown.
constexpr float kGameKnockbackAmount = 100.0f;
// RequestFlags bits (the game's enum)
constexpr uint32_t kDamageAllowFriendly = 1u << 3;
constexpr uint32_t kDamageFromLocalPlayer = 1u << 13;
constexpr uint32_t kDamageNoSpawnRewards = 1u << 16;
constexpr uint32_t kDamageForceReact = 1u << 17;
constexpr uint32_t kDamagePreventKill = 1u << 30;

bool DamageAvailable();
// DamageActor resolved too (damage by actor handle).
bool DamageActorAvailable();
void SubmitDamage(const DamageOrder& d);

struct Stats {
    uint64_t damage = 0;    // damage requests sent
    uint64_t damageActor = 0; // ... of them by actor handle
    uint64_t damageRefused = 0; // the game had no room for them (its request pool was full)
    uint64_t cast = 0;      // rays cast
    uint64_t hits = 0;      // hits returned
    uint64_t empty = 0;     // rays that hit nothing
    uint64_t deferred = 0;  // rays left for a later frame (the game was using its query pool)
    uint64_t frames = 0;    // frames rays were cast in
    double maxFrameMs = 0;  // longest time spent casting in one frame
    double totalMs = 0;
    int poolUsedMax = -1;   // most query results the game had used when the mod cast (-1: unknown)
    uint32_t thread = 0;    // the game thread the rays run on
    bool broken = false;    // a call faulted: casting stopped for the session
};
// Counters since the last call (they restart; `thread` and `broken` stay).
Stats TakeStats();

} // namespace game_physics
} // namespace sm2m

#endif
