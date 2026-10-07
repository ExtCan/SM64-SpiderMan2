// Cheats and poses (src/mod/cheats.cpp) against the real libsm64 with the
// user's ROM: moon jump, BLJ anywhere, infinite health, caps, and the photo
// poses.
//
//   SM2MARIO_TEST_ROM=/path/to/sm64.us.z64 tests/run_cheats_test.sh <libsm64 build dir>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "../src/mod/cheats.h"
#include "../src/sm64/sm64_defs.h"
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

static Sm64Api Api() {
    Sm64Api a;
    a.set_mario_action = sm64_set_mario_action;
    a.set_mario_velocity = sm64_set_mario_velocity;
    a.set_mario_forward_velocity = sm64_set_mario_forward_velocity;
    a.set_mario_faceangle = sm64_set_mario_faceangle;
    a.set_mario_health = sm64_set_mario_health;
    a.set_mario_animation = sm64_set_mario_animation;
    a.set_mario_anim_frame = sm64_set_mario_anim_frame;
    a.set_mario_state = sm64_set_mario_state;
    a.mario_interact_cap = sm64_mario_interact_cap;
    a.mario_extend_cap = sm64_mario_extend_cap;
    a.play_sound_global = sm64_play_sound_global;
    return a;
}

struct Sim {
    int32_t id = -1;
    SM64MarioState s{};
    std::vector<float> pos, nrm, col, uv;
    SM64MarioGeometryBuffers geo{};
    Sim() : pos(9 * SM64_GEO_MAX_TRIANGLES), nrm(9 * SM64_GEO_MAX_TRIANGLES), col(9 * SM64_GEO_MAX_TRIANGLES),
            uv(6 * SM64_GEO_MAX_TRIANGLES) {
        geo.position = pos.data();
        geo.normal = nrm.data();
        geo.color = col.data();
        geo.uv = uv.data();
    }
    void Spawn() {
        if (id >= 0) sm64_mario_delete(id);
        id = sm64_mario_create(0, 100, 0);
        SM64MarioInputs in{};
        in.camLookZ = 1;
        for (int i = 0; i < 40; ++i) sm64_mario_tick(id, &in, &s, &geo);
    }
    void Tick(SM64MarioInputs in) {
        in.camLookZ = 1;
        sm64_mario_tick(id, &in, &s, &geo);
    }
};

int main() {
    const char* romPath = std::getenv("SM2MARIO_TEST_ROM");
    if (!romPath) {
        std::printf("set SM2MARIO_TEST_ROM\n");
        return 2;
    }
    std::ifstream f(romPath, std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> tex(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
    sm64_global_init(rom.data(), tex.data());
    // A 200 m floor.
    SM64Surface floor[2] = {{0, 0, 0, {{-20000, 0, -20000}, {-20000, 0, 20000}, {20000, 0, 20000}}},
                            {0, 0, 0, {{-20000, 0, -20000}, {20000, 0, 20000}, {20000, 0, -20000}}}};
    sm64_static_surfaces_load(floor, 2);
    const Sm64Api api = Api();
    Sim sim;
    LiveSettings set;

    std::printf("[moon jump]\n");
    for (int on = 0; on < 2; ++on) {
        sim.Spawn();
        Cheats ch;
        set = LiveSettings();
        set.moonJump = on != 0;
        float top = 0;
        SM64MarioInputs in{};
        in.buttonA = 1;
        for (int i = 0; i < 60; ++i) {
            ch.BeforeTick(api, sim.id, sim.s, in, set);
            sim.Tick(in);
            ch.AfterTick(api, sim.id, sim.s, set);
            top = std::max(top, sim.s.position[1]);
        }
        std::printf("  moon jump %s: highest %.0f units\n", on ? "on" : "off", top);
        if (on) CHECK(top > 1200.0f); // still rising after 2 s of holding jump
        else CHECK(top < 400.0f && top > 100.0f);
    }

    std::printf("[BLJ anywhere]\n");
    for (int on = 0; on < 2; ++on) {
        sim.Spawn();
        Cheats ch;
        set = LiveSettings();
        set.bljAnywhere = on != 0;
        // A backwards long jump.
        sm64_set_mario_forward_velocity(sim.id, -20);
        sm64_set_mario_action(sim.id, sm64::ACT_LONG_JUMP);
        SM64MarioInputs in{};
        in.buttonZ = 1;
        sim.Tick(in);
        float fastest = 0;
        for (int i = 0; i < 40; ++i) {
            in.buttonA = (i % 4) == 0; // tap jump
            ch.BeforeTick(api, sim.id, sim.s, in, set);
            sim.Tick(in);
            ch.AfterTick(api, sim.id, sim.s, set);
            fastest = std::min(fastest, sim.s.forwardVelocity);
        }
        std::printf("  BLJ %s: fastest %.0f units/frame\n", on ? "on" : "off", fastest);
        if (on) CHECK(fastest <= -Cheats::kMaxBljSpeed + 5 && fastest >= -Cheats::kMaxBljSpeed - 1);
        else CHECK(fastest > -40.0f);
    }

    std::printf("[infinite health]\n");
    {
        sim.Spawn();
        Cheats ch;
        set = LiveSettings();
        set.infiniteHealth = true;
        sm64_mario_take_damage(sim.id, 3, 0, 0, 0, 100);
        SM64MarioInputs in{};
        int16_t lowest = sim.s.health;
        for (int i = 0; i < 60; ++i) {
            ch.BeforeTick(api, sim.id, sim.s, in, set);
            sim.Tick(in);
            ch.AfterTick(api, sim.id, sim.s, set);
            lowest = std::min(lowest, sim.s.health);
        }
        std::printf("  after 3 wedges of damage: health 0x%x (lowest seen 0x%x)\n", sim.s.health, lowest);
        CHECK(sim.s.health == sm64::HEALTH_FULL);
        set.infiniteHealth = false;
        for (int i = 0; i < 90; ++i) sim.Tick(in);
        sm64_set_mario_invincibility(sim.id, 0);
        sm64_mario_take_damage(sim.id, 3, 0, 0, 0, 100);
        for (int i = 0; i < 60; ++i) {
            ch.BeforeTick(api, sim.id, sim.s, in, set);
            sim.Tick(in);
            ch.AfterTick(api, sim.id, sim.s, set);
        }
        CHECK(sim.s.health < sm64::HEALTH_FULL);
    }

    std::printf("[caps]\n");
    {
        sim.Spawn();
        Cheats ch;
        set = LiveSettings();
        SM64MarioInputs in{};
        auto run = [&](int ticks) {
            for (int i = 0; i < ticks; ++i) {
                ch.BeforeTick(api, sim.id, sim.s, in, set);
                sim.Tick(in);
                ch.AfterTick(api, sim.id, sim.s, set);
            }
        };
        set.wingCap = true;
        run(90);
        std::printf("  wing cap: flags 0x%x\n", sim.s.flags);
        CHECK((sim.s.flags & sm64::MARIO_SPECIAL_CAPS) == sm64::MARIO_WING_CAP);
        // Caps combine, as in SM64: metal on top of wings, then vanish too.
        set.metalCap = true;
        run(90);
        std::printf("  wing + metal cap: flags 0x%x\n", sim.s.flags);
        CHECK((sim.s.flags & sm64::MARIO_SPECIAL_CAPS) == (sm64::MARIO_WING_CAP | sm64::MARIO_METAL_CAP));
        set.vanishCap = true;
        run(30);
        CHECK((sim.s.flags & sm64::MARIO_SPECIAL_CAPS) == sm64::MARIO_SPECIAL_CAPS);
        // ... and come off one at a time.
        set.wingCap = false;
        run(10);
        CHECK((sim.s.flags & sm64::MARIO_SPECIAL_CAPS) == (sm64::MARIO_METAL_CAP | sm64::MARIO_VANISH_CAP));
        set.metalCap = set.vanishCap = false;
        run(10);
        CHECK((sim.s.flags & sm64::MARIO_SPECIAL_CAPS) == 0);
        // A new Mario gets the cap again.
        set.wingCap = true;
        run(5);
        sim.Spawn();
        ch.Reset();
        run(90);
        CHECK((sim.s.flags & sm64::MARIO_SPECIAL_CAPS) == sm64::MARIO_WING_CAP);
    }

    std::printf("[model colours]\n");
    {
        // The overalls' bib (the triangles textured with the buttons, atlas
        // cell 1) is blue like the rest of the overalls, whatever was drawn
        // last the frame before - libsm64 used to skip Mario's first display
        // list (his butt, which sets the blue light the torso shares), so the
        // bib took the shoes' brown or the wing cap's white
        // (third_party/libsm64-patches/0002-draw-mario-butt.patch).
        const uint32_t capsList[] = {0, sm64::MARIO_WING_CAP, sm64::MARIO_VANISH_CAP};
        for (uint32_t cap : capsList) {
            sim.Spawn();
            if (cap) sm64_mario_interact_cap(sim.id, cap, 600, 0);
            SM64MarioInputs in{};
            for (int i = 0; i < 40; ++i) sim.Tick(in);
            int bib = 0, blue = 0;
            const float u0 = 64.0f / float(SM64_TEXTURE_WIDTH), u1 = 96.0f / float(SM64_TEXTURE_WIDTH);
            for (int t = 0; t < sim.geo.numTrianglesUsed; ++t) {
                const float u = sim.uv[size_t(t) * 6];
                if (u < u0 - 1e-4f || u > u1 + 1e-4f) continue;
                ++bib;
                const float* c = &sim.col[size_t(t) * 9];
                if (c[0] < 0.05f && c[1] < 0.05f && c[2] > 0.95f) ++blue;
            }
            std::printf("  cap 0x%x: %d triangles, bib %d (%d blue)\n", cap, sim.geo.numTrianglesUsed, bib, blue);
            CHECK(bib >= 8 && blue == bib);
            CHECK(sim.geo.numTrianglesUsed >= 740); // the butt's 72 triangles are there
        }
    }

    std::printf("[poses]\n");
    {
        const Pose poses[] = {Pose::Wave, Pose::PeaceSign, Pose::StarDance};
        const int32_t anims[] = {sm64::MARIO_ANIM_CREDITS_WAVING, sm64::MARIO_ANIM_CREDITS_PEACE_SIGN,
                                 sm64::MARIO_ANIM_STAR_DANCE};
        for (int k = 0; k < 3; ++k) {
            sim.Spawn();
            PoseController pc;
            SM64MarioInputs in{};
            const float face = 1.2f;
            CHECK(pc.Start(api, sim.id, sim.s, poses[k], face));
            int ticks = 0, animOk = 0;
            const float x0 = sim.s.position[0], z0 = sim.s.position[2];
            std::string frames;
            while (pc.Active() && ticks < 200) {
                pc.Tick(api, sim.id, sim.s, in);
                sim.Tick(in);
                if (sim.s.animID == anims[k]) ++animOk;
                ++ticks;
                if (const char* dir = std::getenv("POSE_DUMP")) {
                    // the mesh (positions relative to the feet, colours) at this tick
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/%s_%02d.txt", dir, PoseName(poses[k]), ticks);
                    if (FILE* f = std::fopen(path, "w")) {
                        std::fprintf(f, "%f\n", sim.s.faceAngle);
                        for (int v = 0; v < sim.geo.numTrianglesUsed * 3; ++v)
                            std::fprintf(f, "%f %f %f %f %f %f\n", sim.geo.position[v * 3] - sim.s.position[0],
                                         sim.geo.position[v * 3 + 1] - sim.s.position[1],
                                         sim.geo.position[v * 3 + 2] - sim.s.position[2], sim.geo.color[v * 3],
                                         sim.geo.color[v * 3 + 1], sim.geo.color[v * 3 + 2]);
                        std::fclose(f);
                    }
                }
                if (std::getenv("POSE_FRAMES")) {
                    // the mesh's top (above the feet) and reach (along his facing), units
                    float top = -1e9f, reach = -1e9f;
                    const float fx = std::sin(sim.s.faceAngle), fz = std::cos(sim.s.faceAngle);
                    for (int v = 0; v < sim.geo.numTrianglesUsed * 3; ++v) {
                        const float* q = sim.geo.position + v * 3;
                        top = std::max(top, q[1] - sim.s.position[1]);
                        reach = std::max(reach, (q[0] - sim.s.position[0]) * fx + (q[2] - sim.s.position[2]) * fz);
                    }
                    char b[48];
                    std::snprintf(b, sizeof(b), " %d:%.0f/%.0f", int(sim.s.animFrame), top, reach);
                    frames += b;
                }
            }
            if (!frames.empty()) std::printf("  %s frames:%s\n", PoseName(poses[k]), frames.c_str());
            std::printf("  %s: %d ticks, animation right in %d, face %.2f, action 0x%08x after\n", PoseName(poses[k]),
                        ticks, animOk, sim.s.faceAngle, sim.s.action);
            CHECK(ticks == PoseController::DurationTicks(poses[k]));
            CHECK(animOk >= ticks - 2);
            CHECK(std::fabs(sim.s.faceAngle - face) < 0.01f);
            CHECK(std::fabs(sim.s.position[0] - x0) < 1 && std::fabs(sim.s.position[2] - z0) < 1); // stood still
            sim.Tick(in);
            CHECK(sim.s.action == sm64::ACT_IDLE);
        }
        // Photo mode holds a pose where it shows best: still in it there.
        for (int k = 0; k < 3; ++k) {
            sim.Spawn();
            PoseController pc;
            SM64MarioInputs in{};
            CHECK(pc.Start(api, sim.id, sim.s, poses[k], 0.5f));
            const int hold = PoseController::HoldTicks(poses[k]);
            CHECK(hold > 0 && hold < PoseController::DurationTicks(poses[k]));
            for (int t = 0; t < hold; ++t) {
                pc.Tick(api, sim.id, sim.s, in);
                sim.Tick(in);
            }
            CHECK(pc.Active() && pc.Ticks() == hold);
            CHECK(sim.s.action == sm64::ACT_END_WAVING_CUTSCENE && sim.s.animID == anims[k]);
            std::printf("  %s held at frame %d\n", PoseName(poses[k]), int(sim.s.animFrame));
        }
        // Moving the stick ends a pose early; Mario can't pose in the air.
        sim.Spawn();
        PoseController pc;
        SM64MarioInputs in{};
        CHECK(pc.Start(api, sim.id, sim.s, Pose::Wave, 0));
        for (int i = 0; i < 10; ++i) {
            pc.Tick(api, sim.id, sim.s, in);
            sim.Tick(in);
        }
        in.stickY = -1;
        pc.Tick(api, sim.id, sim.s, in);
        CHECK(!pc.Active());
        sim.Tick(in);
        for (int i = 0; i < 10; ++i) sim.Tick(in);
        CHECK(sim.s.action != sm64::ACT_END_WAVING_CUTSCENE && sm64::ActionGroup(sim.s.action) == sm64::ACT_GROUP_MOVING);
        in = SM64MarioInputs();
        in.buttonA = 1;
        for (int i = 0; i < 3; ++i) sim.Tick(in);
        CHECK(sm64::IsAirborne(sim.s.action));
        CHECK(!pc.Start(api, sim.id, sim.s, Pose::Wave, 0));
    }

    sm64_global_terminate();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
