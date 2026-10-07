// Real Super Mario 64 physics (libsm64 + the user's ROM) on collision built
// from ray casts, the way the mod builds it from Spider-Man 2's physics:
//
//   synthetic world answering ray casts like the game (tests/physics_scene.h)
//   -> PhysicsWorld (a few dozen rays per frame) -> libsm64 surfaces -> Mario
//
// 0.3.0 had Mario run through buildings and fall through roofs. Here he runs
// into a building, along it, jumps onto a low wall and a car roof, walks
// through a person, crosses grass (and SM64 plays grass footsteps there), and
// lands on a roof he jumps up to from a ledge. Both kinds of ray answers are
// played: faces hit only from the front, and from both sides.
//
//   SM2MARIO_TEST_ROM=/path/to/sm64.us.z64 tests/run_physics_test.sh <libsm64 build dir>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "../src/sm64/sm64_defs.h"
#include "../src/world/coords.h"
#include "../src/world/physics_world.h"
#include "../src/world/surface_types.h"
#include "libsm64.h"
#include "physics_scene.h"

using namespace sm2m;

static int g_fail = 0, g_checks = 0;
constexpr uint32_t kActCrouchSlide = 0x04808459; // SM64's ACT_CROUCH_SLIDE
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
        }                                                                         \
    } while (0)

// Footsteps SM64 asked for (SOUND_ACTION_TERRAIN_STEP + terrain sound).
static std::map<int, int> g_steps;
static void OnSound(uint32_t bits, float*) {
    const uint32_t bank = bits >> 28, id = (bits >> 16) & 0xFF;
    if (bank == 0 && id >= 0x10 && id <= 0x17) g_steps[int(id - 0x10)]++;
}

struct PSim {
    phys_test::PhysScene scene;
    PhysicsWorld world;
    WorldMapping map;
    CollisionParams params;
    std::vector<SM64Surface> surfaces;
    CollisionStats cstats;
    int32_t id = -1;
    SM64MarioState state{};
    std::vector<float> pos, nrm, col, uv;
    SM64MarioGeometryBuffers geo{};
    double now = 0;
    double lastBuild = -1e9;
    Vec3 lastBuildAt;
    int tick = 0;
    int lavaBoosts = 0; // ticks Mario spent in a lava boost
    DVec3 camDir{1, 0, 0};
    int budget = 24;
    int answerDelay = 1; // game frames until the game has cast a ray (its next physics frame)
    std::vector<std::pair<int, GameRay>> queued;
    int frame = 0;

    PSim() {
        map.Configure('Y', true, 100.0);
        map.SetOrigin(DVec3(0, 0, 0));
        world.Configure(PhysicsWorldParams(), 1);
        pos.resize(9 * SM64_GEO_MAX_TRIANGLES);
        nrm.resize(9 * SM64_GEO_MAX_TRIANGLES);
        col.resize(9 * SM64_GEO_MAX_TRIANGLES);
        uv.resize(6 * SM64_GEO_MAX_TRIANGLES);
        geo.position = pos.data();
        geo.normal = nrm.data();
        geo.color = col.data();
        geo.uv = uv.data();
    }
    DVec3 Mario() const { return map.ToGame(Vec3(state.position[0], state.position[1], state.position[2])); }

    // One game frame (60 Hz): the mod schedules rays, the game answers them
    // (here at once; in game on its next physics frame).
    void Frame(const DVec3& feet) {
        // The answers to what was asked `answerDelay` frames ago...
        size_t done = 0;
        for (; done < queued.size() && done < size_t(budget) && queued[done].first + answerDelay <= frame; ++done) {
            GameRayResult res;
            scene.Cast(queued[done].second, res);
            world.Accept(res, now);
        }
        queued.erase(queued.begin(), queued.begin() + long(done));
        // ... and new questions (as MarioMod::PumpPhysics: room for two frames' worth).
        std::vector<GameRay> rays;
        const Vec3 vg = map.DirToGame(Vec3(state.velocity[0], state.velocity[1], state.velocity[2])) * 0.3f;
        const int room = budget * 2 - int(queued.size());
        if (room > 0) world.Schedule(feet, vg, now, room, rays);
        for (const GameRay& r : rays) queued.push_back({frame, r});
        if (answerDelay == 0) { // (answered at once)
            for (const auto& q : queued) {
                GameRayResult res;
                scene.Cast(q.second, res);
                world.Accept(res, now);
            }
            queued.clear();
        }
        ++frame;
        now += 1.0 / 60.0;
    }

    void Rebuild() {
        const Vec3 c(state.position[0], state.position[1], state.position[2]);
        world.Build(map, c, params, surfaces, cstats);
        sm64_static_surfaces_load(surfaces.data(), uint32_t(surfaces.size()));
        lastBuild = now;
        lastBuildAt = c;
    }

    // `settle`: frames the game gets to answer about the area first; 0 = as
    // the mod, the moment it has answered for the ground right under him.
    bool Spawn(const DVec3& feet, int settle = 40) {
        const Vec3 l = map.ToLocal(feet);
        state.position[0] = l.x;
        state.position[1] = l.y;
        state.position[2] = l.z;
        for (int i = 0; i < 40 && (i < settle || !world.Ready(feet)); ++i) Frame(feet);
        if (!world.Ready(feet)) return false;
        Rebuild();
        id = sm64_mario_create(l.x, l.y + 1.0f, l.z);
        return id >= 0;
    }

    // One 30 Hz tick (two game frames). `dir`: horizontal game-space direction.
    void Tick(const DVec3& dir, bool a = false, bool b = false, bool z = false) {
        SM64MarioInputs in{};
        const double len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
        if (len > 1e-6) {
            camDir = DVec3(dir.x / len, 0, dir.z / len);
            in.stickY = -1.0f; // away from the camera
        }
        const Vec3 look = map.DirToLocal(camDir.ToFloat());
        in.camLookX = look.x;
        in.camLookZ = look.z;
        in.buttonA = a;
        in.buttonB = b;
        in.buttonZ = z;
        Frame(Mario());
        Frame(Mario());
        // As MarioMod::SimTick: rebuild on a change near Mario (at most every
        // 0.1 s), on moving 1.5 m (3 m vertically), and every 0.25 s.
        const Vec3 p(state.position[0], state.position[1], state.position[2]);
        const Vec3 moved = p - lastBuildAt;
        if ((world.TakeChanged() && now - lastBuild > 0.1) || std::sqrt(moved.x * moved.x + moved.z * moved.z) > 150.0f ||
            std::fabs(moved.y) > 300.0f || now - lastBuild > 0.25)
            Rebuild();
        if (sm64::IsAirborne(state.action) && state.velocity[1] < -51.0f) {
            const float fy = sm64_surface_find_floor_height(p.x, p.y, p.z);
            if (p.y + state.velocity[1] * 1.5f <= fy + 10.0f)
                sm64_set_mario_velocity(id, state.velocity[0], -51.0f, state.velocity[2]);
        }
        sm64_mario_tick(id, &in, &state, &geo);
        if (state.action == sm64::ACT_LAVA_BOOST) ++lavaBoosts;
        ++tick;
    }
    void Idle(int n) {
        for (int i = 0; i < n; ++i) Tick(DVec3(0, 0, 0));
    }
    // Walks to `target` (horizontal) until within `tol`.
    bool GoTo(const DVec3& target, double tol, int maxTicks) {
        for (int i = 0; i < maxTicks; ++i) {
            DVec3 d = target - Mario();
            d.y = 0;
            if (std::sqrt(d.x * d.x + d.z * d.z) < tol) return true;
            Tick(d);
        }
        return false;
    }
    bool Grounded() const { return !sm64::IsAirborne(state.action); }
    int FloorTerrain() const {
        SM64SurfaceCollisionData* floor = nullptr;
        sm64_surface_find_floor(state.position[0], state.position[1] + 50.0f, state.position[2], &floor);
        return floor ? int(floor->terrain) : -1;
    }
};

// Somewhere to stand: a box of game x/z, above `minY`.
struct Area {
    double minX, maxX, minZ, maxZ, minY;
};

// Standing (or walking) there - not in the air, not hanging from the edge.
static bool StandsIn(const PSim& sim, const Area& a) {
    const uint32_t g = sm64::ActionGroup(sim.state.action);
    const DVec3 m = sim.Mario();
    return (g == sm64::ACT_GROUP_STATIONARY || g == sm64::ACT_GROUP_MOVING) && m.y > a.minY && m.x > a.minX &&
           m.x < a.maxX && m.z > a.minZ && m.z < a.maxZ;
}

// Runs along `dir` and jumps (holding A for `hold` ticks: 10 is a full jump)
// once `jumpHere`; again if he falls back short. A grabbed ledge is climbed
// (the stick stays forward). Returns the height Mario stands at inside `a`,
// or -1.
template <class F>
static double JumpOnto(PSim& sim, const DVec3& dir, F jumpHere, const Area& a, const char* name, int hold = 10) {
    const bool trace = std::getenv("SM2MARIO_TRACE") != nullptr;
    int held = 0;
    bool prevA = false;
    for (int i = 0; i < 240; ++i) {
        if (held == 0 && !prevA && sim.Grounded() && sim.Mario().y < a.minY && jumpHere(sim)) held = hold;
        const bool press = held > 0;
        if (held > 0) --held;
        sim.Tick(dir, press);
        prevA = press;
        if (trace)
            std::printf("    %s %3d: (%.2f %.2f %.2f) act %08x%s\n", name, i, sim.Mario().x, sim.Mario().y,
                        sim.Mario().z, sim.state.action, press ? " A" : "");
        if (StandsIn(sim, a)) return sim.Mario().y;
    }
    return -1;
}

static void Street(phys_test::PhysScene& sc) {
    sc.groundY = 0;
    sc.groundMaterial = 2;                                                          // asphalt
    sc.boxes.push_back({DVec3(4, 0, -8), DVec3(14, 12, 8), 4});                    // building: facade at x = 4
    sc.boxes.push_back({DVec3(-4.2, 0, -3), DVec3(-4.0, 1.0, 3), 9});              // 1 m wall at x = -4
    // A car (a vehicle actor). Its body says kAcid (1), as the game's cars
    // answer: 0.4 made that lava, and Mario bounced off every car roof.
    phys_test::PBox car{DVec3(-1, 0, 4), DVec3(1.8, 1.45, 6), 1};
    car.kind = HitKind::Movable;
    car.actor = true;
    sc.boxes.push_back(car);
    phys_test::PBox person{DVec3(0.8, 0, -4.3), DVec3(1.2, 1.8, -3.9), 15};        // someone in the street
    person.kind = HitKind::Character;
    sc.boxes.push_back(person);
    sc.boxes.push_back({DVec3(-3, 0, -9), DVec3(3, 0.02, -6), 28});                // grass
    sc.boxes.push_back({DVec3(-12, 0, -2), DVec3(-9, 2.5, 2), 9});                 // a ledge 2.5 m up
    sc.boxes.push_back({DVec3(-15, 0, -3), DVec3(-12, 4.2, 3), 54});               // a roof above it (4.2 m)
}

static void Run(bool twoSided) {
    std::printf("[%s]\n", twoSided ? "faces answered from both sides" : "faces answered from the front");
    PSim sim;
    Street(sim.scene);
    sim.scene.twoSided = twoSided;
    CHECK(sim.Spawn(DVec3(0, 0, 0)));
    sim.Idle(15);

    // 1) Sprint into the building: never inside (Mario's wall radius is 0.5 m).
    double maxX = -1e9;
    for (int i = 0; i < 120; ++i) {
        sim.Tick(DVec3(1, 0, 0));
        maxX = std::max(maxX, sim.Mario().x);
    }
    std::printf("  into the facade (x = 4): max x %.2f, now %.2f\n", maxX, sim.Mario().x);
    CHECK(maxX < 3.7);
    CHECK(sim.Mario().x > 3.2);
    CHECK(sim.Grounded() && std::fabs(sim.Mario().y) < 0.05);

    // 2) Along it (diagonally into the wall): slides, stays outside.
    double maxX2 = -1e9;
    const double z0 = sim.Mario().z;
    for (int i = 0; i < 60; ++i) {
        sim.Tick(DVec3(1, 0, 1));
        maxX2 = std::max(maxX2, sim.Mario().x);
    }
    std::printf("  along the facade: max x %.2f, moved %.2f m along it\n", maxX2, sim.Mario().z - z0);
    CHECK(maxX2 < 3.7);
    CHECK(sim.Mario().z - z0 > 2.0);

    // 2b) A long jump into it (48 units a frame): bonks off, stays outside.
    CHECK(sim.GoTo(DVec3(-2.0, 0, 1.0), 0.3, 200));
    sim.Idle(15);
    double maxX3 = -1e9;
    bool longJumped = false;
    for (int i = 0; i < 70; ++i) {
        const bool crouch = sim.Mario().x > 0.6 && !longJumped && sim.Grounded();
        const bool jump = crouch && sim.state.action == kActCrouchSlide;
        if (jump) longJumped = true;
        sim.Tick(DVec3(1, 0, 0), jump || (longJumped && i % 40 < 30), false, crouch);
        maxX3 = std::max(maxX3, sim.Mario().x);
    }
    std::printf("  long jump into the facade: %s, max x %.2f, now %.2f\n", longJumped ? "jumped" : "no long jump",
                maxX3, sim.Mario().x);
    CHECK(longJumped);
    CHECK(maxX3 < 3.7);
    CHECK(sim.Mario().x < 3.7 && std::fabs(sim.Mario().y) < 0.05);

    // 3) Through the person: people aren't solid.
    CHECK(sim.GoTo(DVec3(1.0, 0, -2.0), 0.3, 200));
    double minZ = 1e9;
    for (int i = 0; i < 30; ++i) {
        sim.Tick(DVec3(1.0 - sim.Mario().x, 0, -1));
        minZ = std::min(minZ, sim.Mario().z);
    }
    std::printf("  through the person (z -4.3..-3.9): reached z %.2f\n", minZ);
    CHECK(minZ < -5.0);

    // 4) Grass: grass floors, grass footsteps.
    CHECK(sim.GoTo(DVec3(-2.0, 0, -7.5), 0.3, 200));
    g_steps.clear();
    int grassTicks = 0;
    for (int i = 0; i < 90; ++i) {
        sim.Tick(DVec3((i / 30) % 2 ? -1 : 1, 0, (-7.5 - sim.Mario().z) * 0.5));
        if (sim.Grounded() && sim.FloorTerrain() == sm64s::TERRAIN_GRASS) ++grassTicks;
    }
    std::printf("  on the grass: %d ticks on grass floors; footsteps:", grassTicks);
    for (const auto& kv : g_steps) std::printf(" %s x%d", FootstepSoundName(kv.first), kv.second);
    std::printf("\n");
    CHECK(grassTicks > 30);
    CHECK(g_steps[sm64s::SOUND_TERRAIN_GRASS] > 0);
    g_steps.clear();
    CHECK(sim.GoTo(DVec3(-2.0, 0, -1.0), 0.3, 200)); // back on the asphalt
    for (int i = 0; i < 40; ++i) sim.Tick(DVec3(0, 0, (i / 20) % 2 ? 1 : -1));
    std::printf("  back on the street: footsteps:");
    for (const auto& kv : g_steps) std::printf(" %s x%d", FootstepSoundName(kv.first), kv.second);
    std::printf("\n");
    CHECK(g_steps[sm64s::SOUND_TERRAIN_STONE] > 0 && g_steps[sm64s::SOUND_TERRAIN_GRASS] == 0);

    // 5) Onto the car roof (1.45 m) with a running jump (a ledge grab and a
    //    climb also count: the roof is there to stand on either way).
    CHECK(sim.GoTo(DVec3(0.4, 0, 1.0), 0.25, 200));
    sim.Idle(10);
    // (A short hop: a full jump sails right over a 2 m long car.)
    const double roofY = JumpOnto(sim, DVec3(0, 0, 1), [](const PSim& s) { return s.Mario().z > 2.3 && s.Mario().z < 3.5; },
                                  Area{-1.0, 1.8, 3.9, 6.1, 1.0}, "car", 5);
    std::printf("  onto the car: stands at y %.2f, now (%.2f %.2f %.2f)\n", roofY, sim.Mario().x, sim.Mario().y,
                sim.Mario().z);
    CHECK(std::fabs(roofY - 1.45) < 0.06);
    CHECK(sim.lavaBoosts == 0); // the car's "acid" isn't lava
    // He stays up there (or runs off its far end with the speed he landed
    // with) - never sinks into it.
    bool inCar = false;
    for (int i = 0; i < 20; ++i) {
        sim.Idle(1);
        const DVec3 m = sim.Mario();
        if (m.x > -0.8 && m.x < 1.6 && m.z > 4.2 && m.z < 5.8 && m.y < 1.4) inCar = true;
    }
    std::printf("  then: (%.2f %.2f %.2f), lava boosts %d\n", sim.Mario().x, sim.Mario().y, sim.Mario().z,
                sim.lavaBoosts);
    CHECK(!inCar);
    CHECK(sim.lavaBoosts == 0);
    CHECK(std::fabs(sim.Mario().y - 1.45) < 0.06 || (sim.Mario().z > 6.2 && std::fabs(sim.Mario().y) < 0.05));

    // 6) A 1 m wall: blocked walking, over it with a jump.
    CHECK(sim.GoTo(DVec3(-2.0, 0, 0.0), 0.3, 300));
    for (int i = 0; i < 60; ++i) sim.Tick(DVec3(-1, 0, -sim.Mario().z * 0.5));
    const double blockedX = sim.Mario().x;
    std::printf("  walked into the 1 m wall (x = -4): x %.2f\n", blockedX);
    CHECK(blockedX > -4.0 && blockedX < -3.2);
    for (int i = 0; i < 40; ++i) sim.Tick(DVec3(-1, 0, 0), i < 8);
    std::printf("  jumped over it: now (%.2f %.2f)\n", sim.Mario().x, sim.Mario().y);
    CHECK(sim.Mario().x < -4.2);

    // 7) A ledge 2.5 m up (a running jump), then the roof 4.2 m up from it.
    CHECK(sim.GoTo(DVec3(-5.0, 0, 0), 0.3, 300));
    const double ledgeY = JumpOnto(sim, DVec3(-1, 0, 0), [](const PSim& s) { return s.Mario().x < -7.0; },
                                   Area{-12.0, -9.0, -2.0, 2.0, 2.0}, "ledge");
    std::printf("  onto the ledge: stands at y %.2f, now (%.2f %.2f %.2f)\n", ledgeY, sim.Mario().x, sim.Mario().y,
                sim.Mario().z);
    CHECK(std::fabs(ledgeY - 2.5) < 0.06);
    sim.Idle(10);
    const double roof2Y = JumpOnto(sim, DVec3(-1, 0, 0), [](const PSim& s) { return s.Mario().x < -10.4; },
                                   Area{-15.0, -12.0, -3.0, 3.0, 3.5}, "roof");
    std::printf("  onto the roof: stands at y %.2f, now (%.2f %.2f %.2f)\n", roof2Y, sim.Mario().x, sim.Mario().y,
                sim.Mario().z);
    CHECK(std::fabs(roof2Y - 4.2) < 0.06);
    // Walks off the far edge of the roof (x = -15) and lands in the street.
    for (int i = 0; i < 90 && !(sim.Grounded() && sim.Mario().x < -15.3); ++i) sim.Tick(DVec3(-1, 0, 0));
    sim.Idle(30);
    std::printf("  off the roof: now (%.2f %.2f %.2f)\n", sim.Mario().x, sim.Mario().y, sim.Mario().z);
    CHECK(sim.Mario().x < -15.3 && std::fabs(sim.Mario().y) < 0.05);
    sm64_mario_delete(sim.id);
}

// As the mod does it: Mario on the game's physics the moment it has answered
// for the ground under him, running off at once - into what nothing has been
// asked about yet. He must never drop into the street (or through the
// building's walls); SM64 holds him at the edge of what's known instead.
static void RunAtOnce(bool twoSided, int delay, int budget) {
    std::printf("[straight off, %d rays a frame answered %d frame(s) late%s]\n", budget, delay,
                twoSided ? ", both sides" : "");
    PSim sim;
    Street(sim.scene);
    sim.scene.twoSided = twoSided;
    sim.answerDelay = delay;
    sim.budget = budget;
    CHECK(sim.Spawn(DVec3(0, 0, 0), 0));
    double minY = 1e9, maxX = -1e9;
    for (int i = 0; i < 90; ++i) {
        sim.Tick(DVec3(1, 0, 0));
        minY = std::min(minY, sim.Mario().y);
        maxX = std::max(maxX, sim.Mario().x);
    }
    std::printf("  ran at the facade: lowest y %.2f, max x %.2f, now (%.2f %.2f %.2f)\n", minY, maxX, sim.Mario().x,
                sim.Mario().y, sim.Mario().z);
    CHECK(minY > -0.05);
    CHECK(maxX < 3.7 && sim.Mario().x > 3.0);
    minY = 1e9;
    double minX = 1e9;
    for (int i = 0; i < 120; ++i) {
        sim.Tick(DVec3(-1, 0, -sim.Mario().z * 0.5));
        minY = std::min(minY, sim.Mario().y);
        minX = std::min(minX, sim.Mario().x);
    }
    std::printf("  ran back at the 1 m wall: lowest y %.2f, min x %.2f, now (%.2f %.2f %.2f)\n", minY, minX,
                sim.Mario().x, sim.Mario().y, sim.Mario().z);
    CHECK(minY > -0.05);
    CHECK(minX > -4.0);
    sm64_mario_delete(sim.id);
}

// Faster than the game answers about the ground ahead (here: two rays a
// frame, at full speed): Mario must stop at the edge of what's known for a
// moment, not fall into it.
static void Outrun() {
    std::printf("[faster than the rays]\n");
    PSim sim;
    Street(sim.scene);
    sim.budget = 2;
    CHECK(sim.Spawn(DVec3(-1.5, 0, 0)));
    double minY = 1e9, minZ = 1e9;
    for (int i = 0; i < 150; ++i) {
        sim.Tick(DVec3(0, 0, -1));
        minY = std::min(minY, sim.Mario().y);
        minZ = std::min(minZ, sim.Mario().z);
    }
    sim.Idle(30);
    std::printf("  lowest y %.2f, got to z %.2f, now (%.2f %.2f %.2f)\n", minY, minZ, sim.Mario().x, sim.Mario().y,
                sim.Mario().z);
    CHECK(minY > -0.05 && std::fabs(sim.Mario().y) < 0.05);
    CHECK(minZ < -10.0); // (and he does get somewhere)
    sm64_mario_delete(sim.id);
}

// Under a fire escape: six metal decks stacked 3 m apart over the pavement,
// answered from both sides (every column ray runs out of hits on them long
// before the street). Mario walks under them and back out, on the street.
static void UnderDecks() {
    std::printf("[under a stack of decks]\n");
    PSim sim;
    sim.scene.groundY = 0;
    sim.scene.twoSided = true;
    // (four decks 1.9 m apart from 2 m up: within the 8 m the floor rays start
    // above him, so they run out of hits before the street)
    for (int k = 0; k < 4; ++k) sim.scene.boxes.push_back({DVec3(2, 2.0 + 1.9 * k, -4), DVec3(8, 2.1 + 1.9 * k, 4), 51});
    CHECK(sim.Spawn(DVec3(0, 0, 0), 0));
    double minY = 1e9, maxX = -1e9;
    for (int i = 0; i < 120; ++i) {
        sim.Tick(DVec3(1, 0, -sim.Mario().z * 0.5));
        minY = std::min(minY, sim.Mario().y);
        maxX = std::max(maxX, sim.Mario().x);
    }
    for (int i = 0; i < 60; ++i) {
        sim.Tick(DVec3(-1, 0, 0));
        minY = std::min(minY, sim.Mario().y);
    }
    std::printf("  walked to x %.2f under them and back: lowest y %.2f, now (%.2f %.2f %.2f)\n", maxX, minY,
                sim.Mario().x, sim.Mario().y, sim.Mario().z);
    CHECK(maxX > 6.0 && minY > -0.05 && std::fabs(sim.Mario().y) < 0.05);
    sm64_mario_delete(sim.id);
}

// Dropped 18 m above a building (a far respawn: nothing known there yet):
// lands on its roof, then walks off its edge and lands in the street.
static void DropOntoRoof(bool twoSided) {
    std::printf("[dropped onto a roof%s]\n", twoSided ? ", both sides" : "");
    PSim sim;
    Street(sim.scene);
    sim.scene.twoSided = twoSided;
    CHECK(sim.Spawn(DVec3(9, 30, 0), 0));
    double minY = 1e9;
    for (int i = 0; i < 150 && !(sim.Grounded() && sim.Mario().y < 13.0); ++i) {
        sim.Tick(DVec3(0, 0, 0));
        minY = std::min(minY, sim.Mario().y);
    }
    sim.Idle(60);
    std::printf("  landed at (%.2f %.2f %.2f), lowest y %.2f\n", sim.Mario().x, sim.Mario().y, sim.Mario().z, minY);
    CHECK(std::fabs(sim.Mario().y - 12.0) < 0.06 && minY > 11.9);
    // Off the roof's edge (z = 8): 12 m down to the street.
    for (int i = 0; i < 150 && !(sim.Grounded() && sim.Mario().z > 8.3); ++i) sim.Tick(DVec3(0, 0, 1));
    sim.Idle(60);
    std::printf("  walked off the edge: now (%.2f %.2f %.2f)\n", sim.Mario().x, sim.Mario().y, sim.Mario().z);
    CHECK(sim.Mario().z > 8.3 && std::fabs(sim.Mario().y) < 0.05);
    sm64_mario_delete(sim.id);
}

int main() {
    const char* romPath = std::getenv("SM2MARIO_TEST_ROM");
    if (!romPath) {
        std::printf("set SM2MARIO_TEST_ROM to your SM64 ROM\n");
        return 2;
    }
    std::ifstream f(romPath, std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (rom.size() < 8 * 1024 * 1024) {
        std::printf("couldn't read the ROM\n");
        return 2;
    }
    std::vector<uint8_t> texture(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
    sm64_global_init(rom.data(), texture.data());
    sm64_register_play_sound_function(OnSound);
    Run(false);
    Run(true);
    RunAtOnce(false, 1, 24);
    RunAtOnce(true, 3, 24);
    RunAtOnce(false, 3, 6); // a struggling game: a quarter of the rays, late
    Outrun();
    UnderDecks();
    DropOntoRoof(false);
    DropOntoRoof(true);
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
