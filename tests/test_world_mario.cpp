// Real Super Mario 64 physics (libsm64 + the user's ROM) walking around a
// synthetic street that he only knows through rendered depth frames - the same
// path the mod uses in game:
//
//   scene -> depth image from a third-person camera -> WorldModel ->
//   CollisionBuilder -> libsm64 static surfaces -> Mario
//
// The scene has a building facade, a parked car, stairs up to a plaza and a
// pole; a "body" box always stands where Mario is (the game's depth buffer
// contains Mario himself) and must not become collision.
//
//   SM2MARIO_TEST_ROM=/path/to/sm64.us.z64 tests/run_world_test.sh <libsm64 build dir>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "../src/render/view_constants.h"
#include "../src/sm64/sm64_defs.h"
#include "../src/world/collision.h"
#include "../src/world/coords.h"
#include "../src/world/world_model.h"
#include "libsm64.h"

using namespace sm2m;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
        }                                                                         \
    } while (0)

struct Box {
    DVec3 lo, hi;
    bool enabled = true;
};

struct Scene {
    std::vector<Box> boxes;
    // A body where Mario stands. The mod captures the game's depth before it
    // draws Mario and with Spider-Man hidden, so normally nothing is there;
    // tests enable it to check the exclusion (e.g. if hiding the hero failed).
    DVec3 body;
    bool hasBody = false;

    // Nearest hit along cam + dir * t (t in view-depth units, since dot(dir, forward) == 1).
    double Trace(const DVec3& o, const DVec3& d) const {
        double best = 1e30;
        if (d.y < -1e-9) best = -o.y / d.y; // ground plane y = 0
        auto box = [&](const DVec3& lo, const DVec3& hi) {
            double t0 = 0, t1 = best;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a]) < 1e-12) {
                    if (o[a] < lo[a] || o[a] > hi[a]) return;
                    continue;
                }
                double ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                if (t0 > t1) return;
            }
            if (t0 > 0 && t0 < best) best = t0;
        };
        for (const Box& b : boxes)
            if (b.enabled) box(b.lo, b.hi);
        if (hasBody) box(body + DVec3(-0.3, 0.0, -0.3), body + DVec3(0.3, 1.75, 0.3));
        return best;
    }
};

// Depth image of the scene as the game's G-buffer would hold it.
static void RenderDepth(const Scene& s, const ViewConstants& v, int W, int H, DepthFrame& f) {
    f.width = W;
    f.height = H;
    f.view = v;
    f.depth.assign(size_t(W) * H, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double nx, ny;
            ViewConstants::PixelToNdc(x, y, W, H, nx, ny);
            DVec3 p1;
            v.Unproject(nx, ny, 1.0, p1);
            const double t = s.Trace(v.camPos, p1 - v.camPos);
            f.depth[size_t(y) * W + x] = t < 200.0 ? float(t) : 0.0f;
        }
}

struct Sim {
    Scene scene;
    WorldModel world{1};
    WorldMapping map;
    CollisionBuilder builder;
    CollisionParams params;
    std::vector<SM64Surface> surfaces;
    CollisionStats cstats;
    int32_t id = -1;
    SM64MarioState state{};
    std::vector<float> pos, nrm, col, uv;
    SM64MarioGeometryBuffers geo{};
    DVec3 camDir{1, 0, 0}; // horizontal direction the camera looks along
    double prevVp[4][4] = {};
    DVec3 prevCam;
    bool havePrev = false;
    Vec3 lastBuild;
    int tick = 0;
    // MarioMod::SimTick's safety nets.
    std::vector<Vec3> fallPath;
    int rescues = 0;
    bool outOfWorld = false;
    double integrateMs = 0, buildMs = 0;
    int integrations = 0, builds = 0;
    // In game: Mario is in the captured depth (drawn before the capture), the
    // frame reaches the model `delay` integrations late (GPU read-back), and the
    // mod leaves out a cylinder at where Mario is *now*.
    bool bodyInDepth = false;
    int delay = 0;
    int integrateEvery = 2;
    // 0: a cylinder at Mario's current position (what 0.2.0 betas did),
    // 1: the box Mario was drawn in, in the frame the depth comes from (the mod).
    int exclusion = 1;
    struct Pending {
        DepthFrame f;
        DVec3 body; // where Mario was drawn in it
    };
    std::deque<Pending> pending;
    // Camera fixed behind Mario along +z (like tests/deferred_host.cpp)
    // instead of turning with the direction he moves in.
    bool fixedCam = false;

    Sim() {
        map.Configure('Y', true, 100.0); // left-handed game, like Spider-Man 2
        map.SetOrigin(DVec3(0, 0, 0));
        WorldModelParams wp;
        world.Configure(wp);
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

    ViewConstants Camera() {
        const DVec3 m = Mario();
        const DVec3 eye = fixedCam ? m + DVec3(0, 2.2, -4.0) : m - camDir * 4.5 + DVec3(0, 2.2, 0);
        const DVec3 target = m + DVec3(0, 1.0, 0);
        float rows[kViewCbRows][4];
        BuildViewConstants(eye, (target - eye).ToFloat(), Vec3(0, 1, 0), 60.0f * 3.14159265f / 180.0f, 480, 200,
                           0.1f, havePrev ? prevCam : eye, havePrev ? prevVp : nullptr, rows);
        ViewConstants v;
        std::string why;
        if (!ParseViewConstants(&rows[0][0], v, &why)) std::printf("  view parse failed: %s\n", why.c_str());
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) prevVp[i][j] = v.vp[i][j];
        prevCam = eye;
        havePrev = true;
        return v;
    }

    void Integrate() {
        scene.body = Mario();
        if (bodyInDepth) scene.hasBody = true;
        Pending pf;
        RenderDepth(scene, Camera(), 480, 200, pf.f);
        pf.body = Mario();
        pending.push_back(std::move(pf));
        if (int(pending.size()) <= delay) return;
        Pending g = std::move(pending.front());
        pending.pop_front();
        std::vector<Cylinder> excl;
        std::vector<ExcludeBox> boxes;
        if (!bodyInDepth) {
            excl.resize(1);
            excl[0].base = Mario();
        } else if (exclusion == 0) {
            excl.resize(1);
            excl[0].base = Mario();
            excl[0].radius = 0.85f;
            excl[0].below = 0.2f;
            excl[0].height = 2.2f;
        } else {
            // The body box (see Scene::Trace) plus MarioMod::WorldLoop's margin.
            const double m = 0.12;
            boxes.push_back({g.body + DVec3(-0.3 - m, -m, -0.3 - m), g.body + DVec3(0.3 + m, 1.75 + m, 0.3 + m)});
        }
        auto t0 = std::chrono::steady_clock::now();
        world.Integrate(g.f, Mario(), excl, boxes);
        integrateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++integrations;
    }

    void Rebuild() {
        WorldModelRaycaster rays(&world);
        const Vec3 c(state.position[0], state.position[1], state.position[2]);
        auto t0 = std::chrono::steady_clock::now();
        builder.Build(rays, map, c, params, surfaces, cstats);
        buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++builds;
        sm64_static_surfaces_load(surfaces.data(), uint32_t(surfaces.size()));
        lastBuild = c;
    }

    bool Spawn(const DVec3& feet) {
        world.Seed(feet);
        world.SetFallbackGround(feet, feet.y, 3.0);
        state.position[0] = state.position[1] = state.position[2] = 0;
        const Vec3 l = map.ToLocal(feet);
        state.position[0] = l.x;
        state.position[1] = l.y;
        state.position[2] = l.z;
        Integrate();
        Rebuild();
        id = sm64_mario_create(l.x, l.y + 1.0f, l.z);
        return id >= 0;
    }

    // One 30 Hz tick. `dir` = horizontal game-space direction to move in (or 0).
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
        if (tick % integrateEvery == 0) Integrate();
        const Vec3 p(state.position[0], state.position[1], state.position[2]);
        const Vec3 moved = p - lastBuild;
        if (tick % 10 == 0 || std::sqrt(moved.x * moved.x + moved.z * moved.z) > 50.0f || std::fabs(moved.y) > 150.0f)
            Rebuild();
        // Same fall-speed cap as the mod (see MarioMod::SimTick).
        if (sm64::IsAirborne(state.action) && state.velocity[1] < -51.0f) {
            const float fy = sm64_surface_find_floor_height(p.x, p.y, p.z);
            if (p.y + state.velocity[1] * 1.5f <= fy + 10.0f)
                sm64_set_mario_velocity(id, state.velocity[0], -51.0f, state.velocity[2]);
        }
        sm64_mario_tick(id, &in, &state, &geo);
        ++tick;
        const bool air = sm64::IsAirborne(state.action);
        const Vec3 q(state.position[0], state.position[1], state.position[2]);
        if (!air) fallPath.clear();
        else fallPath.push_back(q);
        float fy = 0;
        if (air && GroundAboveFallingMario(sm64_surface_find_floor_height, fallPath.data(), int(fallPath.size()),
                                           state.velocity[1], (params.probeUp - 0.5f) * 100.0f, fy)) {
            sm64_set_mario_position(id, q.x, fy + 1.0f, q.z);
            sm64_set_mario_velocity(id, state.velocity[0], 0.0f, state.velocity[2]);
            fallPath.clear();
            ++rescues;
        }
        if (!air && sm64_surface_find_floor_height(q.x, q.y + 10.0f, q.z) <= cstats.safetyFloorY + 5.0f) outOfWorld = true;
    }

    void Idle(int n) {
        for (int i = 0; i < n; ++i) Tick(DVec3(0, 0, 0));
    }

    // Walks towards `target` (horizontal) until within `tol` or `maxTicks`.
    bool GoTo(const DVec3& target, double tol, int maxTicks) {
        const bool verbose = std::getenv("SM2MARIO_TEST_VERBOSE") != nullptr;
        for (int i = 0; i < maxTicks; ++i) {
            const DVec3 m = Mario();
            DVec3 d = target - m;
            d.y = 0;
            if (std::sqrt(d.x * d.x + d.z * d.z) < tol) return true;
            Tick(d);
            if (verbose && i % 20 == 0)
                std::printf("    goto t%d (%.2f %.2f %.2f) act %08X fwd %.1f face %.2f\n", i, Mario().x, Mario().y,
                            Mario().z, state.action, state.forwardVelocity, state.faceAngle);
        }
        return false;
    }
};

static bool Grounded(uint32_t action) { return !sm64::IsAirborne(action); }

// tests/deferred_host.cpp's street as the e2e test plays it: run up the stairs,
// over the platform and into the wall, with the depth frames arriving like
// they do in game.
// Pressing right moves Mario to the right on screen. The game camera stands
// behind him looking along game +z; `leftHanded` says how the game's screen
// is oriented (left-handed: screen right = +x, right-handed: -x). The mapping
// must mirror exactly when the game is left-handed (MarioMod::WantMirror).
static void StrafeTest(bool leftHanded) {
    std::printf("[strafe right, %s-handed game]\n", leftHanded ? "left" : "right");
    Sim sim;
    sim.map.Configure('Y', leftHanded, 100.0);
    sim.map.SetOrigin(DVec3(0, 0, 0));
    CHECK(sim.Spawn(DVec3(0, 0, 0)));
    sim.Idle(20);
    for (int i = 0; i < 30; ++i) {
        const DVec3 m = sim.Mario();
        const DVec3 cam = m + DVec3(0, 2.2, -4.0);
        const Vec3 look = sim.map.ToLocal(m) - sim.map.ToLocal(cam); // as MarioMod::SimTick does
        SM64MarioInputs in{};
        in.stickX = 1.0f; // right
        in.camLookX = look.x;
        in.camLookZ = look.z;
        if (sim.tick % sim.integrateEvery == 0) sim.Integrate();
        sm64_mario_tick(sim.id, &in, &sim.state, &sim.geo);
        ++sim.tick;
    }
    const DVec3 m = sim.Mario();
    const double screenRight = leftHanded ? m.x : -m.x; // world direction of screen right: +x (LH) / -x (RH)
    std::printf("  moved to (%.2f %.2f %.2f): %.2f m towards screen right, %.2f m forward\n", m.x, m.y, m.z, screenRight,
                m.z);
    CHECK(screenRight > 0.5);
    CHECK(std::fabs(m.z) < 0.5 * screenRight);
    sm64_mario_delete(sim.id);
}

// Floors the model has above the real surface (Mario's body seen as ground).
static int PhantomFloors(Sim& sim, double x0, double x1, double z0, double z1) {
    const bool body = sim.scene.hasBody;
    sim.scene.hasBody = false;
    int n = 0;
    for (double x = x0; x <= x1; x += 0.25)
        for (double z = z0; z <= z1; z += 0.25) {
            // Cell layers only (hole filling at the edge of what the camera has
            // seen borrows a neighbour's height on purpose).
            double h;
            if (!sim.world.CellFloorAt(DVec3(x, 0, z), 4.0, -1.0, h)) continue;
            const double realTop = 4.0 - sim.scene.Trace(DVec3(x, 4.0, z), DVec3(0, -1, 0));
            if (h > realTop + 0.3) {
                ++n;
                if (std::getenv("SM2MARIO_TEST_VERBOSE"))
                    std::printf("    phantom floor at (%.2f %.2f): model %.2f, real %.2f\n", x, z, h, realTop);
            }
        }
    sim.scene.hasBody = body;
    return n;
}

static void SprintIntoWall(const char* label, bool body, int delay, int every, int exclusion = 1) {
    std::printf("[sprint over the stairs into a wall: %s]\n", label);
    Sim sim;
    sim.fixedCam = true;
    sim.bodyInDepth = body;
    sim.exclusion = exclusion;
    sim.delay = delay;
    sim.integrateEvery = every;
    Scene& sc = sim.scene;
    for (int i = 0; i < 4; ++i) {
        const double h = 0.25 * (i + 1);
        sc.boxes.push_back({DVec3(-1.5, 0, 3.0 + 0.5 * i), DVec3(1.5, h, 3.5 + 0.5 * i)});
    }
    sc.boxes.push_back({DVec3(-1.5, 0, 5), DVec3(1.5, 1, 13)});              // platform
    sc.boxes.push_back({DVec3(-10, 0, 16), DVec3(10, 8, 17)});               // wall, face at z = 16
    sc.boxes.push_back({DVec3(0.675, 0, -1.775), DVec3(1.025, 4, -1.425)});  // pillar
    sc.boxes.push_back({DVec3(-5, 0, 1.9), DVec3(-3, 1.4, 6.1)});            // car
    CHECK(sim.Spawn(DVec3(0, 0, 0)));
    sim.Idle(30);
    double maxZ = -1e9, minY = 1e9, platformY = -1;
    int platformTicks = 0;
    const bool verbose = std::getenv("SM2MARIO_TEST_VERBOSE") != nullptr;
    auto run = [&](int ticks, bool forward) {
        for (int i = 0; i < ticks; ++i) {
            sim.Tick(forward ? DVec3(0, 0, 1) : DVec3(0, 0, 0));
            const DVec3 q = sim.Mario();
            maxZ = std::max(maxZ, q.z);
            minY = std::min(minY, q.y);
            if (q.z > 6.0 && q.z < 12.0 && Grounded(sim.state.action)) {
                platformY = q.y;
                ++platformTicks;
            }
            if (verbose && sim.tick % 3 == 0)
                std::printf("    t%d (%.2f %.2f %.2f) act %08X fwd %.1f | %d surfaces, %d wall cells\n", sim.tick, q.x,
                            q.y, q.z, sim.state.action, sim.state.forwardVelocity, sim.cstats.total,
                            sim.world.Stats().wallCells);
            if (const char* dt = std::getenv("SM2MARIO_TEST_DUMP_TICK"))
                if (sim.tick == std::atoi(dt)) {
                    std::printf("    -- world at tick %d around (%.2f %.2f):\n", sim.tick, q.x, q.z);
                    sim.world.DebugDump(q, 3);
                    const Vec3 l = sim.map.ToLocal(q);
                    std::printf("    -- libsm64 floor under Mario: %.1f (Mario at %.1f local)\n",
                                sm64_surface_find_floor_height(l.x, l.y + 100.0f, l.z), l.y);
                }
        }
    };
    run(90, true);   // like the e2e script: W for 3 s ...
    run(120, false); // ... released ...
    run(180, true);  // ... and held again for 6 s, into the wall
    const DVec3 m = sim.Mario();
    const int phantoms = PhantomFloors(sim, -1.25, 1.25, -1.0, 15.5);
    std::printf("  max z %.3f (wall face at 16), min y %.3f, platform y %.3f for %d ticks, now (%.2f %.2f %.2f), "
                "phantom floors %d, rescues %d%s\n",
                maxZ, minY, platformY, platformTicks, m.x, m.y, m.z, phantoms, sim.rescues,
                sim.outOfWorld ? ", FELL OUT OF THE WORLD" : "");
    if (exclusion == 0) { // the old way: just show what it did
        sm64_mario_delete(sim.id);
        return;
    }
    CHECK(phantoms == 0);                    // Mario's body never became ground
    CHECK(!sim.outOfWorld);
    CHECK(maxZ < 15.6);                      // never inside the wall (Mario's radius is 0.5 m)
    CHECK(m.z > 15.2);                       // but up against it
    CHECK(minY > -3.0);                      // never far through the ground (see GroundAboveFallingMario)
    CHECK(platformTicks > 3);                // went over the platform ...
    CHECK(std::fabs(platformY - 1.0) < 0.06); // ... on top of it
    CHECK(std::fabs(m.y) < 0.05);
    sm64_mario_delete(sim.id);
}

int main() {
    const char* romPath = std::getenv("SM2MARIO_TEST_ROM");
    if (!romPath) {
        std::printf("SM2MARIO_TEST_ROM not set - skipping (needs the user's own SM64 ROM)\n");
        return 0;
    }
    std::ifstream rf(romPath, std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    if (rom.size() < 8 * 1024 * 1024) {
        std::printf("ROM not readable: %s\n", romPath);
        return 1;
    }
    std::vector<uint8_t> texture(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
    sm64_global_init(rom.data(), texture.data());

    Sim sim;
    Scene& sc = sim.scene;
    sc.boxes.push_back({DVec3(6, 0, -12), DVec3(18, 15, 12)});         // 0 building, facade at x = 6
    sc.boxes.push_back({DVec3(-4.5, 0, 2.0), DVec3(-2.7, 1.45, 6.5)}); // 1 parked car
    sc.boxes.push_back({DVec3(-12, 0, -8), DVec3(-8, 1.0, -2)});       // 2 plaza (1 m up)
    for (int k = 1; k <= 4; ++k)                                       // 3-6 stairs up to it
        sc.boxes.push_back({DVec3(-8, 0, -8), DVec3(-8 + 0.35 * (5 - k), 0.2 * k, -2)});
    sc.boxes.push_back({DVec3(2.9, 0, -2.6), DVec3(3.1, 4.0, -2.4)});  // 7 lamp pole

    std::printf("[spawn]\n");
    sc.hasBody = true; // as if Spider-Man were still visible: must not become collision
    CHECK(sim.Spawn(DVec3(0, 0, 0)));
    sim.Idle(45);
    DVec3 m = sim.Mario();
    std::printf("  mario (%.2f %.2f %.2f) action %08X, %d surfaces, world: %d cells %d floor layers %d walls\n", m.x,
                m.y, m.z, sim.state.action, sim.cstats.total, sim.world.Stats().cells, sim.world.Stats().floorLayers,
                sim.world.Stats().wallCells);
    CHECK(std::fabs(m.y) < 0.05);
    CHECK(Grounded(sim.state.action));
    CHECK(sim.world.Stats().excluded > 0); // the body at Mario's position was seen and ignored
    sc.hasBody = false;

    std::printf("[walk into the facade at x = 6]\n");
    double maxX = -1e9;
    for (int i = 0; i < 200; ++i) {
        sim.Tick(DVec3(1, 0, 0));
        maxX = std::max(maxX, sim.Mario().x);
    }
    m = sim.Mario();
    std::printf("  stopped at x %.3f (max %.3f), y %.3f, action %08X\n", m.x, maxX, m.y, sim.state.action);
    CHECK(maxX < 5.75);          // never inside the wall (Mario's radius is 0.5 m)
    CHECK(m.x > 5.2);            // but right up against it
    CHECK(std::fabs(m.y) < 0.05);

    std::printf("[walk into the lamp pole]\n");
    CHECK(sim.GoTo(DVec3(1.0, 0, -2.5), 0.15, 300));
    double poleGap = 1e9, poleMax = -1e9;
    for (int i = 0; i < 90; ++i) {
        sim.Tick(DVec3(1, 0, 0));
        const DVec3 q = sim.Mario();
        // Distance from Mario's centre to the 20 cm pole's box.
        const double ex = std::max(0.0, std::max(2.9 - q.x, q.x - 3.1));
        const double ez = std::max(0.0, std::max(-2.6 - q.z, q.z + 2.4));
        poleGap = std::min(poleGap, std::sqrt(ex * ex + ez * ez));
        poleMax = std::max(poleMax, q.x);
    }
    std::printf("  closest approach to the pole %.3f m, max x %.2f\n", poleGap, poleMax);
    CHECK(poleGap > 0.3); // never inside it (Mario's radius is 0.5 m)
    if (std::getenv("SM2MARIO_TEST_VERBOSE")) sim.world.DebugDump(DVec3(3.0, 0, -2.5), 1);

    std::printf("[jump onto the car roof (1.45 m)]\n");
    CHECK(sim.GoTo(DVec3(-1.0, 0, 4.25), 0.15, 400));
    for (int i = 0; i < 20; ++i) sim.Tick(DVec3(-1, 0, 0)); // walk up against the car's side
    sim.Idle(10);
    double roofY = -1;
    int onRoofTicks = 0;
    for (int i = 0; i < 90; ++i) {
        // Standing jump towards the car: hold A for a full jump, steer onto the roof.
        const DVec3 p = sim.Mario();
        sim.Tick(p.x > -3.4 ? DVec3(-1, 0, 0) : DVec3(0, 0, 0), i < 10);
        const DVec3 q = sim.Mario();
        if (std::getenv("SM2MARIO_TEST_VERBOSE") && i % 3 == 0)
            std::printf("    car t%d (%.2f %.2f %.2f) act %08X fwd %.1f\n", i, q.x, q.y, q.z, sim.state.action,
                        sim.state.forwardVelocity);
        if (Grounded(sim.state.action) && q.x < -2.6 && q.x > -4.6 && q.y > 1.2) {
            roofY = q.y;
            ++onRoofTicks;
        }
    }
    m = sim.Mario();
    std::printf("  roof height %.3f for %d ticks, now (%.2f %.2f %.2f)\n", roofY, onRoofTicks, m.x, m.y, m.z);
    CHECK(onRoofTicks > 10);
    CHECK(std::fabs(roofY - 1.45) < 0.12);

    std::printf("[walk off the far side and land on the street]\n");
    bool fell = false;
    for (int i = 0; i < 120 && sim.Mario().x > -5.6; ++i) {
        sim.Tick(DVec3(-1, 0, 0));
        if (sm64::IsAirborne(sim.state.action)) fell = true;
    }
    sim.Idle(30);
    m = sim.Mario();
    std::printf("  now (%.2f %.2f %.2f) action %08X\n", m.x, m.y, m.z, sim.state.action);
    CHECK(fell);
    CHECK(m.x < -4.6);
    CHECK(std::fabs(m.y) < 0.05);

    std::printf("[walk round to the stairs and climb to the plaza (1 m)]\n");
    CHECK(sim.GoTo(DVec3(-5.2, 0, -0.5), 0.3, 300));
    CHECK(sim.GoTo(DVec3(-5.2, 0, -5.0), 0.3, 300));
    for (int i = 0; i < 70 && sim.Mario().x > -9.0; ++i) {
        sim.Tick(DVec3(-1, 0, 0));
        if (std::getenv("SM2MARIO_TEST_VERBOSE") && i % 4 == 0)
            std::printf("    stairs t%d (%.2f %.2f %.2f) act %08X fwd %.1f\n", i, sim.Mario().x, sim.Mario().y,
                        sim.Mario().z, sim.state.action, sim.state.forwardVelocity);
    }
    sim.Idle(15);
    m = sim.Mario();
    std::printf("  now (%.2f %.2f %.2f) action %08X\n", m.x, m.y, m.z, sim.state.action);
    if (std::getenv("SM2MARIO_TEST_VERBOSE")) {
        sim.world.DebugDump(DVec3(m.x - 0.5, 0, m.z), 2);
        sim.Rebuild();
        std::printf("  rebuilt: %d surfaces (%d floors, edge %d step %d probe %d walls, clipped %d)\n", sim.cstats.total,
                    sim.cstats.floors, sim.cstats.edgeWalls, sim.cstats.stepWalls, sim.cstats.probeWalls,
                    sim.cstats.clipped);
        for (double dx = 0; dx >= -1.0; dx -= 0.25) {
            Vec3 l = sim.map.ToLocal(DVec3(m.x + dx, m.y, m.z));
            float wx = l.x, wy = l.y, wz = l.z;
            const int walls = sm64_surface_find_wall_collision(&wx, &wy, &wz, 30.0f, 50.0f);
            const float fl = sm64_surface_find_floor_height(l.x, l.y + 100.0f, l.z);
            std::printf("    at x %.2f: floor %.1f units, %d walls at +30\n", m.x + dx, fl, walls);
        }
        int shown = 0;
        for (const SM64Surface& sf : sim.surfaces) {
            const Vec3 n = Normalize(Sm64SurfaceNormal(sf));
            const DVec3 g = sim.map.ToGame(Vec3(float(sf.vertices[0][0]), float(sf.vertices[0][1]), float(sf.vertices[0][2])));
            if (std::fabs(n.y) < 0.2f && std::fabs(g.x - m.x) < 1.5 && std::fabs(g.z - m.z) < 1.5 && shown++ < 10) {
                const DVec3 g1 = sim.map.ToGame(Vec3(float(sf.vertices[1][0]), float(sf.vertices[1][1]), float(sf.vertices[1][2])));
                const DVec3 g2 = sim.map.ToGame(Vec3(float(sf.vertices[2][0]), float(sf.vertices[2][1]), float(sf.vertices[2][2])));
                std::printf("    wall tri (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f) n_local (%.2f %.2f %.2f)\n", g.x,
                            g.y, g.z, g1.x, g1.y, g1.z, g2.x, g2.y, g2.z, n.x, n.y, n.z);
            }
        }
    }
    CHECK(m.x < -8.2);
    CHECK(std::fabs(m.y - 1.0) < 0.06);

    std::printf("[ground pound on the plaza]\n");
    bool pound = false, poundLand = false;
    for (int i = 0; i < 60; ++i) {
        const bool air = sm64::IsAirborne(sim.state.action);
        sim.Tick(DVec3(0, 0, 0), i < 3, false, air && i > 8 && i < 14);
        if (sim.state.action == sm64::ACT_GROUND_POUND) pound = true;
        if (sim.state.action == sm64::ACT_GROUND_POUND_LAND) poundLand = true;
    }
    m = sim.Mario();
    std::printf("  pound %d land %d, now y %.3f\n", int(pound), int(poundLand), m.y);
    CHECK(pound && poundLand);
    CHECK(std::fabs(m.y - 1.0) < 0.06);

    std::printf("[the car drives off: its collision must disappear]\n");
    CHECK(sim.GoTo(DVec3(-6.5, 0, -0.5), 0.3, 400)); // down the stairs, around the plaza
    CHECK(sim.GoTo(DVec3(1.5, 0, 0.5), 0.3, 400));
    CHECK(sim.GoTo(DVec3(1.5, 0, 4.6), 0.2, 400));
    for (int i = 0; i < 4; ++i) sim.Tick(DVec3(-1, 0, 0)); // face the car (camera behind Mario)
    sim.Idle(20);
    sc.boxes[1].enabled = false;                          // it drives off while he watches
    sim.Idle(40);
    if (std::getenv("SM2MARIO_TEST_VERBOSE")) {
        std::printf("  after watching it leave: mario (%.2f %.2f %.2f), carved %d\n", sim.Mario().x, sim.Mario().y,
                    sim.Mario().z, sim.world.Stats().carved);
        sim.world.DebugDump(DVec3(-3.6, 0, 4.6), 3);
    }
    int insideTicks = 0;
    for (int i = 0; i < 150 && sim.Mario().x > -5.0; ++i) {
        sim.Tick(DVec3(-1, 0, 0));
        const DVec3 q = sim.Mario();
        if (q.x < -2.8 && q.x > -4.4 && q.z > 2.1 && q.z < 6.4 && std::fabs(q.y) < 0.05) ++insideTicks;
    }
    m = sim.Mario();
    std::printf("  now (%.2f %.2f %.2f), %d ticks walking where the car stood\n", m.x, m.y, m.z, insideTicks);
    if (std::getenv("SM2MARIO_TEST_VERBOSE")) sim.world.DebugDump(DVec3(-2.7, 0, m.z), 2);
    CHECK(insideTicks > 3);
    CHECK(m.x < -4.8);           // walked straight through where the car was
    CHECK(std::fabs(m.y) < 0.05); // on the street, not on a ghost roof

    std::printf("\nperf: integrate %.2f ms/frame (480x200, stride 2), collision build %.2f ms\n",
                sim.integrateMs / std::max(1, sim.integrations), sim.buildMs / std::max(1, sim.builds));
    CHECK(sim.integrateMs / std::max(1, sim.integrations) < 25.0);
    sm64_mario_delete(sim.id);

    StrafeTest(true);
    StrafeTest(false);
    SprintIntoWall("ideal depth", false, 0, 2);
    SprintIntoWall("Mario in the depth, 3 frames late, 10 fps", true, 3, 3);
    SprintIntoWall("Mario in the depth, 2 frames late, 30 fps", true, 2, 1);
    SprintIntoWall("Mario in the depth, 4 frames late, 10 fps", true, 4, 3);
    SprintIntoWall("old exclusion (cylinder where Mario is now), 3 frames late, 10 fps", true, 3, 3, 0);
    sm64_global_terminate();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
