// Host-side unit tests for the platform independent parts of the mod.
// Build: see tests/CMakeLists.txt or run tests/run_tests.sh on Linux.
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <cstdio>
#include <fstream>
#include <map>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../src/common/seqlock.h"
#include "../src/common/ini.h"
#include "../src/common/rom.h"
#include "../src/common/sha1.h"
#include "../src/game/camera_finder.h"
#include "../src/game/combat.h"
#include "../src/game/pattern.h"
#include "../src/game/rtti.h"
#include "../src/mod/follow_monitor.h"
#include "../src/mod/camera_lead.h"
#include "../src/mod/camera_override.h"
#include "../src/mod/config.h"
#include "../src/mod/settings.h"
#include "../src/render/d3d12_parse.h"
#include "../src/render/frame_policy.h"
#include "../src/render/stencil_census.h"
#include "../src/render/inject_shaders.h"
#include "../src/render/inject_shaders_bin.h"
#include "../src/render/gbuffer_format.h"
#include "../src/render/view_constants.h"
#include "../src/world/world_model.h"
#include "../src/sm64/mario.h"
#include "../src/sm64/sm64_defs.h"
#include "../src/world/collision.h"
#include "../src/world/coords.h"
#include "../src/world/physics_world.h"
#include "../src/world/surface_types.h"
#include "physics_scene.h"

using namespace sm2m;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                             \
    } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= double(tol))

static void TestSha1() {
    std::printf("sha1\n");
    CHECK(Sha1::HexOf("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Sha1::HexOf("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(Sha1::HexOf(two.data(), two.size()) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    Sha1 s;
    std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.Update(a.data(), a.size());
    CHECK(Sha1::Hex(s.Final()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

static void TestRom() {
    std::printf("rom\n");
    std::vector<uint8_t> z64(8 * 1024 * 1024);
    std::mt19937 rng(1);
    for (auto& b : z64) b = uint8_t(rng());
    z64[0] = 0x80; z64[1] = 0x37; z64[2] = 0x12; z64[3] = 0x40;
    CHECK(DetectRomOrder(z64.data(), z64.size()) == RomOrder::Z64);

    std::vector<uint8_t> v64 = z64;
    for (size_t i = 0; i + 1 < v64.size(); i += 2) std::swap(v64[i], v64[i + 1]);
    CHECK(DetectRomOrder(v64.data(), v64.size()) == RomOrder::V64);
    std::vector<uint8_t> n64 = z64;
    for (size_t i = 0; i + 3 < n64.size(); i += 4) {
        std::swap(n64[i], n64[i + 3]);
        std::swap(n64[i + 1], n64[i + 2]);
    }
    CHECK(DetectRomOrder(n64.data(), n64.size()) == RomOrder::N64);

    RomCheck cv = CheckRom(v64);
    RomCheck cn = CheckRom(n64);
    CHECK(v64 == z64);
    CHECK(n64 == z64);
    CHECK(!cv.ok && cv.version == "unknown");
    CHECK(cv.sha1 == cn.sha1);
    std::vector<uint8_t> junk(100, 0x11);
    CHECK(!CheckRom(junk).ok);
    CHECK(std::string(kSm64UsSha1) == "9bef1128717f958171a4afac3ed78ee2bb4e86ce");
}

static void TestIni() {
    std::printf("ini\n");
    Ini ini;
    ini.LoadString(
        "\xEF\xBB\xBF; comment\n"
        "[Mario]\n"
        "Scale = 100 ; inline\n"
        "Enabled=yes\n"
        "Name = \"Mario Mode\"\n"
        "Pattern = 48 8B ?? ** ** ** ** C3\n"
        "[layout]\n"
        "health_max = 0xA0\n"
        "neg = -12\n"
        "list = a, b ,, c\n");
    CHECK(ini.GetInt("mario", "scale") == 100);
    CHECK(ini.GetBool("MARIO", "enabled") == true);
    CHECK(ini.GetString("Mario", "name") == "Mario Mode");
    CHECK(ini.GetString("Mario", "pattern") == "48 8B ?? ** ** ** ** C3");
    CHECK(ini.GetInt("Layout", "health_max") == 0xA0);
    CHECK(ini.GetInt("Layout", "neg") == -12);
    CHECK(ini.GetInt("Layout", "missing", 7) == 7);
    auto list = Ini::SplitList(ini.GetString("layout", "list"));
    CHECK(list.size() == 3 && list[2] == "c");
    Ini over;
    over.LoadString("[Mario]\nScale=50\n[New]\nx=1\n");
    ini.Merge(over);
    CHECK(ini.GetInt("Mario", "Scale") == 50);
    CHECK(ini.GetBool("Mario", "enabled"));
    CHECK(ini.GetInt("new", "x") == 1);
    double f;
    CHECK(Ini::ParseFloat(" 1.5 ", f) && f == 1.5);
    CHECK(!Ini::ParseFloat("1.5x", f));

    // In-place edits keep comments, spacing and line endings.
    const std::string text = "; settings\r\n[Render]\r\nGloss = 0.45             ; shine\r\nSpecular = 0.04\r\n\r\n"
                             "[Audio]\r\nVolume = 0.8\r\n";
    std::string v;
    CHECK(Ini::GetValueInText(text, "render", "GLOSS", v) && v == "0.45");
    std::string e = Ini::SetValueInText(text, "Render", "Gloss", "0.15");
    CHECK(e == "; settings\r\n[Render]\r\nGloss = 0.15             ; shine\r\nSpecular = 0.04\r\n\r\n"
               "[Audio]\r\nVolume = 0.8\r\n");
    e = Ini::SetValueInText(e, "Render", "Gloss", "0.123456789012345678901234");
    CHECK(Ini::GetValueInText(e, "Render", "Gloss", v) && v == "0.123456789012345678901234");
    CHECK(e.find("0.123456789012345678901234 ; shine\r\n") != std::string::npos);
    e = Ini::SetValueInText(text, "Render", "Shadows", "true"); // new key: after the section's last key
    CHECK(e == "; settings\r\n[Render]\r\nGloss = 0.45             ; shine\r\nSpecular = 0.04\r\nShadows = true\r\n"
               "\r\n[Audio]\r\nVolume = 0.8\r\n");
    e = Ini::SetValueInText(text, "Cheats", "MoonJump", "on"); // new section at the end
    CHECK(e == text + "[Cheats]\r\nMoonJump = on\r\n");
    e = Ini::SetValueInText("[A]\nx=1", "A", "y", "2"); // no final newline, \n endings
    CHECK(e == "[A]\nx=1\ny = 2\n");
    Ini r;
    r.LoadString(Ini::SetValueInText(text, "Audio", "Volume", "0.5"));
    CHECK(r.GetFloat("audio", "volume") == 0.5 && r.GetFloat("render", "gloss") == 0.45);
}

static void TestPattern() {
    std::printf("pattern\n");
    Pattern p = Pattern::Parse("48 8B ?? ** ** ** ** C3");
    CHECK(p.valid && p.size() == 8 && p.captureOffset == 3);
    CHECK(!Pattern::Parse("?? ??").valid);
    CHECK(!Pattern::Parse("4Z").valid);
    Pattern single = Pattern::Parse("E8 ? ? ? ? 90");
    CHECK(single.valid && single.size() == 6);

    std::vector<uint8_t> buf(4096, 0xCC);
    // Place: 48 8B 05 <disp32> C3 at offset 1000, disp pointing 200 bytes past the instruction end.
    const size_t at = 1000;
    buf[at] = 0x48; buf[at + 1] = 0x8B; buf[at + 2] = 0x05;
    int32_t disp = 200;
    std::memcpy(&buf[at + 3], &disp, 4);
    buf[at + 7] = 0xC3;
    int64_t off = FindPattern(buf.data(), buf.size(), p);
    CHECK(off == int64_t(at));
    uintptr_t match = uintptr_t(buf.data()) + uintptr_t(off);
    uintptr_t target = ResolveMatch(match, p, Resolve::Rip, 0);
    CHECK(target == uintptr_t(buf.data()) + at + 7 + 200);
    CHECK(CountPattern(buf.data(), buf.size(), p) == 1);
    // Leading wildcard anchor.
    Pattern lead = Pattern::Parse("?? 8B 05");
    CHECK(FindPattern(buf.data(), buf.size(), lead) == int64_t(at));
    // Call resolution: E8 rel32
    std::vector<uint8_t> code(64, 0x90);
    code[10] = 0xE8;
    int32_t rel = -5;
    std::memcpy(&code[11], &rel, 4);
    Pattern call = Pattern::Parse("E8 ** ** ** ** 90");
    int64_t co = FindPattern(code.data(), code.size(), call);
    CHECK(co == 10);
    CHECK(ResolveMatch(uintptr_t(code.data()) + 10, call, Resolve::Call, 0) == uintptr_t(code.data()) + 10);
}

static void TestFunctionStart() {
    std::printf("function start validation\n");
    uint8_t b[16];
    // Aligned: accepted as-is regardless of the bytes before it.
    std::memset(b, 0x11, 16);
    CHECK(ValidateFunctionStart(0x1000, b) == 0x1000);
    // The user's case: pattern matched at +2 after "40 53" (push rbx), int3 padding before.
    std::memset(b, 0xCC, 16);
    b[14] = 0x40;
    b[15] = 0x53;
    CHECK(ValidateFunctionStart(0x1002, b) == 0x1000);
    // Same gap but the 2 bytes are not a prologue (mid-function match): rejected.
    b[14] = 0x8B;
    b[15] = 0xC3;
    CHECK(ValidateFunctionStart(0x1002, b) == 0);
    // Prologue bytes, but no padding before the aligned start: rejected.
    std::memset(b, 0x11, 16);
    b[14] = 0x40;
    b[15] = 0x53;
    CHECK(ValidateFunctionStart(0x1002, b) == 0);
    // Previous function's ret right before the aligned start: accepted.
    b[13] = 0xC3;
    CHECK(ValidateFunctionStart(0x1002, b) == 0x1000);
    // Unaligned start straight after int3 padding (small functions): accepted.
    std::memset(b, 0xCC, 16);
    CHECK(ValidateFunctionStart(0x1004, b) == 0x1004);
    // Longer prologue: mov [rsp+8],rbx ; push rdi ; sub rsp,20h  (5 + 1 + 4 = 10 bytes)
    std::memset(b, 0xCC, 16);
    const uint8_t pro[10] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};
    std::memcpy(b + 6, pro, 10);
    CHECK(ValidateFunctionStart(0x100A, b) == 0x1000);
    CHECK(PrologueLength(pro, 10) == 10);
    CHECK(PrologueLength(pro, 9) == 0); // truncated instruction
    const uint8_t movrax[7] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08};
    CHECK(PrologueLength(movrax, 7) == 7);
}

static void TestMapping() {
    std::printf("mapping\n");
    WorldMapping m;
    m.Configure('Y', false, 100.0);
    m.SetOrigin(DVec3(1000.5, 20.0, -3000.25));
    DVec3 g(1001.5, 21.5, -2999.25);
    Vec3 l = m.ToLocal(g);
    CHECK_NEAR(l.x, 100, 1e-3);
    CHECK_NEAR(l.y, 150, 1e-3);
    CHECK_NEAR(l.z, 100, 1e-3);
    DVec3 back = m.ToGame(l);
    CHECK_NEAR(back.x, g.x, 1e-6);
    CHECK_NEAR(back.z, g.z, 1e-6);

    m.Configure('Y', true, 100.0);
    Vec3 lm = m.ToLocal(g);
    CHECK_NEAR(lm.z, -100, 1e-3);
    CHECK(m.Mirrored());
    Vec3 d = m.DirToLocal(Vec3(0, 0, 1));
    CHECK_NEAR(d.z, -1, 1e-6);
    Vec3 dg = m.DirToGame(d);
    CHECK_NEAR(dg.z, 1, 1e-6);

    WorldMapping z;
    z.Configure('Z', false, 100.0);
    Vec3 up = z.DirToLocal(Vec3(0, 0, 1));
    CHECK_NEAR(up.y, 1, 1e-6);
    // Proper rotation: det = +1.
    Vec3 r[3];
    z.Rows(r);
    CHECK_NEAR(Dot(Cross(r[0], r[1]), r[2]), 1.0, 1e-6);
    z.Configure('Z', true, 100.0);
    z.Rows(r);
    CHECK_NEAR(Dot(Cross(r[0], r[1]), r[2]), -1.0, 1e-6);
    DVec3 gz(3, 4, 5);
    CHECK_NEAR(z.ToGame(z.ToLocal(gz)).y, 4, 1e-6);

    OriginPolicy pol;
    Vec3 shift;
    CHECK(!ComputeOriginShift(Vec3(100, 50, -200), false, 0, pol, shift));
    CHECK(ComputeOriginShift(Vec3(3500.4f, 10, 0), false, 0, pol, shift));
    CHECK_NEAR(shift.x, 3500, 1e-3);
    CHECK_NEAR(shift.y, 0, 1e-3);
    CHECK(ComputeOriginShift(Vec3(0, -1200, 0), true, -40, pol, shift));
    CHECK_NEAR(shift.y, -1200, 1e-3);
    CHECK(!ComputeOriginShift(Vec3(0, -1200, 0), true, 10, pol, shift)); // still rising
}

// Analytic scene for collision tests: ground plane y=0, a 10 m tall building
// block, and a thin awning 3 m above the street.
struct Box {
    DVec3 lo, hi;
};
class SceneRaycaster final : public IRaycaster {
public:
    std::vector<Box> boxes;
    bool ground = true;
    int casts = 0;
    bool Cast(const DVec3& from, const DVec3& to, RayHit& hit) override {
        ++casts;
        double bestT = 2.0;
        Vec3 bestN;
        DVec3 d = to - from;
        if (ground && from.y >= 0 && to.y <= 0 && from.y != to.y) {
            double t = from.y / (from.y - to.y);
            if (t < bestT) {
                bestT = t;
                bestN = Vec3(0, 1, 0);
            }
        }
        for (const Box& b : boxes) {
            double tmin = 0, tmax = 1;
            int axis = -1;
            double sign = 0;
            bool miss = false;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a]) < 1e-12) {
                    if (from[a] < b.lo[a] || from[a] > b.hi[a]) miss = true;
                    continue;
                }
                double t1 = (b.lo[a] - from[a]) / d[a];
                double t2 = (b.hi[a] - from[a]) / d[a];
                double s = -1;
                if (t1 > t2) {
                    std::swap(t1, t2);
                    s = 1;
                }
                if (t1 > tmin) {
                    tmin = t1;
                    axis = a;
                    sign = s;
                }
                tmax = std::min(tmax, t2);
                if (tmin > tmax) miss = true;
            }
            if (miss || axis < 0) continue; // starts inside: ignore (like one-sided meshes)
            if (tmin < bestT) {
                bestT = tmin;
                bestN = Vec3(0, 0, 0);
                bestN[axis] = float(sign);
            }
        }
        if (bestT > 1.0) return false;
        hit.point = from + d * bestT;
        hit.normal = bestN;
        return true;
    }
    const char* Name() const override { return "scene"; }
};

// SM64-style floor query over generated surfaces: highest floor at or below y+0.78m.
static bool FindFloor(const std::vector<SM64Surface>& s, float x, float y, float z, float& h) {
    bool found = false;
    for (const auto& f : s) {
        Vec3 n = Sm64SurfaceNormal(f);
        float len = Length(n);
        if (len < 1) continue;
        n = n * (1.0f / len);
        if (n.y <= 0.01f) continue;
        float x1 = float(f.vertices[0][0]), z1 = float(f.vertices[0][2]);
        float x2 = float(f.vertices[1][0]), z2 = float(f.vertices[1][2]);
        float x3 = float(f.vertices[2][0]), z3 = float(f.vertices[2][2]);
        // Same-side tests (SM64 uses this winding for up-facing floors).
        if ((z1 - z) * (x2 - x1) - (x1 - x) * (z2 - z1) < 0) continue;
        if ((z2 - z) * (x3 - x2) - (x2 - x) * (z3 - z2) < 0) continue;
        if ((z3 - z) * (x1 - x3) - (x3 - x) * (z1 - z3) < 0) continue;
        float oo = -(n.x * f.vertices[0][0] + n.y * f.vertices[0][1] + n.z * f.vertices[0][2]);
        float height = -(x * n.x + n.z * z + oo) / n.y;
        if (y - (height - 78.0f) < 0) continue;
        if (!found || height > h) {
            h = height;
            found = true;
        }
    }
    return found;
}

// Fake sm64_surface_find_floor_height over a few horizontal rectangles.
struct FakeFloor {
    float x0, x1, z0, z1, y;
};
static std::vector<FakeFloor> g_fakeFloors;
static float FakeFindFloor(float x, float y, float z) {
    float best = -11000.0f;
    for (const FakeFloor& f : g_fakeFloors)
        if (x >= f.x0 && x <= f.x1 && z >= f.z0 && z <= f.z1 && f.y <= y + 78.0f) best = std::max(best, f.y);
    return best;
}

static void TestRescue() {
    std::printf("rescue from unseen ground\n");
    g_fakeFloors = {{-1000, 1000, -1000, 1000, 0.0f}}; // the street (it showed up late)
    float fy = 0;
    // Fell straight through it: from 50 above to 150 below.
    std::vector<Vec3> path;
    for (int i = 0; i <= 10; ++i) path.push_back(Vec3(0, 50.0f - 20.0f * float(i), 0));
    CHECK(GroundAboveFallingMario(FakeFindFloor, path.data(), int(path.size()), -20.0f, 400.0f, fy) && fy == 0.0f);
    // Only 70 below it: SM64 lands him itself.
    path = {Vec3(0, 10, 0), Vec3(0, -30, 0), Vec3(0, -70, 0)};
    CHECK(!GroundAboveFallingMario(FakeFindFloor, path.data(), int(path.size()), -20.0f, 400.0f, fy));
    // Rising (vy > 0): never.
    path = {Vec3(0, 50, 0), Vec3(0, -150, 0)};
    CHECK(!GroundAboveFallingMario(FakeFindFloor, path.data(), int(path.size()), 5.0f, 400.0f, fy));
    // Further below than maxClimb: not found (the floor isn't loaded that far up).
    path = {Vec3(0, 50, 0), Vec3(0, -600, 0)};
    CHECK(!GroundAboveFallingMario(FakeFindFloor, path.data(), int(path.size()), -20.0f, 400.0f, fy));
    // An awning at 300 over x 0..200 and the street at 0. He jumps off a roof
    // at 1000 (x -300), falls beside the awning and drifts under it: no rescue
    // onto the awning (he never went down through it).
    g_fakeFloors = {{-1000, 1000, -1000, 1000, 0.0f}, {0, 200, -100, 100, 300.0f}};
    path.clear();
    for (int i = 0; i <= 40; ++i) {
        const float t = float(i) / 40.0f;
        path.push_back(Vec3(-300.0f + 400.0f * t, 1000.0f - 950.0f * t, 0)); // ends at (100, 50), under the awning
    }
    CHECK(!GroundAboveFallingMario(FakeFindFloor, path.data(), int(path.size()), -30.0f, 1000.0f, fy));
    // ...but falling down through the awning itself (it appeared late) does.
    path.clear();
    for (int i = 0; i <= 10; ++i) path.push_back(Vec3(100, 400.0f - 30.0f * float(i), 0)); // 400 -> 100
    CHECK(GroundAboveFallingMario(FakeFindFloor, path.data(), int(path.size()), -30.0f, 1000.0f, fy) && fy == 300.0f);
}

static void TestCollision() {
    std::printf("collision\n");
    WorldMapping map;
    map.Configure('Y', false, 100.0);
    map.SetOrigin(DVec3(500, 0, 500)); // Mario stands at the origin, on the street
    SceneRaycaster scene;
    scene.boxes.push_back({DVec3(503, 0, 490), DVec3(520, 10, 520)});      // building: x>=503
    scene.boxes.push_back({DVec3(497, 3.0, 490), DVec3(499, 3.1, 520)});   // awning slab over x 497..499
    scene.boxes.push_back({DVec3(498, 0, 502), DVec3(502, 1.0, 504)});     // 1 m planter, 2 m to Mario's +z

    CollisionParams p;
    std::vector<SM64Surface> surfs;
    CollisionStats st;
    CollisionBuilder b;
    b.Build(scene, map, Vec3(0, 0, 0), p, surfs, st);
    std::printf("  rays=%d hits=%d floors=%d flat=%d edge=%d step=%d probe=%d clipped=%d total=%d\n", st.rays,
                st.hits, st.floors, st.flattened, st.edgeWalls, st.stepWalls, st.probeWalls, st.clipped, st.total);
    CHECK(st.total > 100);
    CHECK(st.edgeWalls > 0);
    CHECK(st.clipped > 0); // step/probe walls duplicated the edge walls and were clipped

    // No two coplanar walls may overlap (SM64 would push Mario once per wall).
    {
        struct W {
            Vec3 n;
            float d, u0, u1, lo, hi;
        };
        std::vector<W> ws;
        for (const auto& f : surfs) {
            Vec3 n = Normalize(Sm64SurfaceNormal(f));
            if (std::fabs(n.y) > 0.01f) continue;
            Vec3 t(-n.z, 0, n.x);
            float u0 = 1e30f, u1 = -1e30f, lo = 1e30f, hi = -1e30f;
            for (const auto& v : f.vertices) {
                float u = t.x * float(v[0]) + t.z * float(v[2]);
                u0 = std::min(u0, u);
                u1 = std::max(u1, u);
                lo = std::min(lo, float(v[1]));
                hi = std::max(hi, float(v[1]));
            }
            ws.push_back({n, n.x * float(f.vertices[0][0]) + n.z * float(f.vertices[0][2]), u0, u1, lo, hi});
        }
        int overlaps = 0;
        // Triangles come in pairs (quads); compare quads, i.e. even indices only.
        for (size_t a = 0; a + 1 < ws.size(); a += 2) {
            for (size_t b = a + 2; b + 1 < ws.size(); b += 2) {
                if (Dot(ws[a].n, ws[b].n) < 0.9f || std::fabs(ws[a].d - ws[b].d) > 30.0f) continue;
                const float ua0 = std::min(ws[a].u0, ws[a + 1].u0), ua1 = std::max(ws[a].u1, ws[a + 1].u1);
                const float ub0 = std::min(ws[b].u0, ws[b + 1].u0), ub1 = std::max(ws[b].u1, ws[b + 1].u1);
                const float overlap = std::min(ua1, ub1) - std::max(ua0, ub0);
                const float vOverlap = std::min(ws[a].hi, ws[b].hi) - std::max(ws[a].lo, ws[b].lo);
                if (overlap > 2.5f && vOverlap > 2.0f) ++overlaps;
            }
        }
        std::printf("  overlapping coplanar wall quads: %d\n", overlaps);
        CHECK(overlaps == 0);
    }

    // All walls must be vertical and all floors must face up.
    int walls = 0, floors = 0, bad = 0;
    for (const auto& s : surfs) {
        Vec3 n = Normalize(Sm64SurfaceNormal(s));
        if (n.y > 0.9f) ++floors;
        else if (std::fabs(n.y) < 0.01f) ++walls;
        else if (n.y < -0.01f) ++bad; // no ceilings expected
    }
    CHECK(bad == 0);
    CHECK(walls > 0 && floors > 0);

    float h = 0;
    CHECK(FindFloor(surfs, 0, 50, 0, h) && std::fabs(h) < 1.0f);           // street under Mario
    CHECK(FindFloor(surfs, -200, 50, 0, h) && std::fabs(h) < 1.0f);        // street under the awning
    CHECK(FindFloor(surfs, -200, 400, 0, h) && std::fabs(h - 310) < 2.0f); // standing on the awning
    CHECK(FindFloor(surfs, 0, 200, 300, h) && std::fabs(h - 100) < 2.0f);   // on top of the planter

    // Facade wall at the building edge (x = 300 local) facing -x (toward the street),
    // and a step wall on the planter's near side (z = 200) facing -z.
    bool faceWall = false, planterWall = false;
    for (const auto& s : surfs) {
        Vec3 n = Normalize(Sm64SurfaceNormal(s));
        if (std::fabs(n.y) >= 0.01f) continue;
        float wx = float(s.vertices[0][0]), wz = float(s.vertices[0][2]);
        if (n.x < -0.9f && wx > 250 && wx < 330) faceWall = true;
        if (n.z < -0.9f && wz > 150 && wz < 230) planterWall = true;
    }
    CHECK(faceWall);
    CHECK(planterWall);

    // Coplanar clipping works in the wall's plane, along it and in height: a
    // piece that starts higher up (a floor estimate lifted by a stray sample)
    // must not hide the lower part of the full-height piece next to it.
    {
        std::vector<SM64Surface> out;
        WallSet ws(out, 100, 30.0f);
        int clipped = 0;
        const Vec3 n(0, 0, -1);
        CHECK(ws.Emit(Vec3(-40, 0, 100), Vec3(40, 0, 100), 84, 1000, n, clipped) == 2); // raised piece
        CHECK(ws.Emit(Vec3(0, 0, 100), Vec3(80, 0, 100), -30, 1000, n, clipped) == 4);  // full piece next to it
        CHECK(clipped == 1);
        // Each point of the union is covered by exactly one triangle.
        auto covers = [&](float x, float y) {
            int c = 0;
            for (const SM64Surface& t : out) {
                const float ax = float(t.vertices[0][0]), ay = float(t.vertices[0][1]);
                const float bx = float(t.vertices[1][0]), by = float(t.vertices[1][1]);
                const float cx = float(t.vertices[2][0]), cy = float(t.vertices[2][1]);
                const float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
                if (std::fabs(d) < 1e-6f) continue;
                const float l1 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d;
                const float l2 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d;
                if (l1 > 1e-4f && l2 > 1e-4f && 1.0f - l1 - l2 > 1e-4f) ++c;
            }
            return c;
        };
        CHECK(covers(20, 0) == 1);   // below the raised piece: the new piece's lower part
        CHECK(covers(20, 500) == 1); // the raised piece only
        CHECK(covers(-20, 0) == 0);  // nothing was asked for there
        CHECK(covers(60, 0) == 1);
        CHECK(covers(60, 500) == 1);
        CHECK(ws.Emit(Vec3(0, 0, 100), Vec3(80, 0, 100), -30, 1000, n, clipped) == 0); // all covered now
        CHECK(ws.Emit(Vec3(0, 0, 120), Vec3(80, 0, 120), -30, 1000, n, clipped) == 0); // within the coplanar tolerance
        CHECK(ws.Emit(Vec3(0, 0, 140), Vec3(80, 0, 140), -30, 1000, n, clipped) == 2); // a different plane
    }

    // Flat fallback: floors only, at the requested height.
    FlatGroundRaycaster flat('Y', 2.0);
    map.SetOrigin(DVec3(0, 2.0, 0));
    b.Build(flat, map, Vec3(0, 0, 0), p, surfs, st);
    CHECK(st.stepWalls == 0 && st.probeWalls == 0 && st.edgeWalls == 0);
    CHECK(FindFloor(surfs, 123, 20, -77, h) && std::fabs(h) < 1.0f);

    // Rooftop edge: Mario on the roof must be able to walk off (a floor
    // exists beyond the edge at street level, not an out-of-bounds hole).
    map.SetOrigin(DVec3(504, 10, 500)); // 1 m in from the roof edge at x=503
    b.Build(scene, map, Vec3(0, 0, 0), p, surfs, st);
    CHECK(FindFloor(surfs, -200, 0, 0, h) && std::fabs(h + 1000) < 2.0f);
}

static void TestCameraFinder() {
    std::printf("camera finder\n");
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uni(-2.0f, 2.0f);
    std::vector<float> mem(1 << 16);
    for (auto& f : mem) f = uni(rng);

    const DVec3 hero(1200.0, 35.0, -800.0);
    const Vec3 up(0, 1, 0);
    // Camera 4 m behind (along -z) and 1.5 m above, looking at the hero's chest.
    DVec3 camPos = hero + DVec3(0.5, 1.5 + 1.0, -4.0);
    DVec3 tgt = hero + DVec3(0, 1.0, 0);
    Vec3 fwd = Normalize((tgt - camPos).ToFloat());
    Vec3 right = Normalize(Cross(up, fwd)); // LH convention
    Vec3 cup = Cross(fwd, right);
    const size_t camAt = 4096; // float index, 16-byte aligned
    float* c = &mem[camAt];
    c[0] = right.x; c[1] = right.y; c[2] = right.z; c[3] = 0;
    c[4] = cup.x; c[5] = cup.y; c[6] = cup.z; c[7] = 0;
    c[8] = fwd.x; c[9] = fwd.y; c[10] = fwd.z; c[11] = 0;
    c[12] = float(camPos.x); c[13] = float(camPos.y); c[14] = float(camPos.z); c[15] = 1;

    // The hero's own transform must not be picked.
    float* hm = &mem[8192];
    hm[0] = 1; hm[1] = 0; hm[2] = 0; hm[3] = 0;
    hm[4] = 0; hm[5] = 1; hm[6] = 0; hm[7] = 0;
    hm[8] = 0; hm[9] = 0; hm[10] = 1; hm[11] = 0;
    hm[12] = float(hero.x); hm[13] = float(hero.y); hm[14] = float(hero.z); hm[15] = 1;

    // Projection: 16:9, 60 deg vertical, reverse-Z infinite, with jitter; row-vector layout.
    const float aspect = 16.0f / 9.0f;
    const float ys = 1.0f / std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    float* pr = &mem[20000];
    std::memset(pr, 0, 64);
    pr[0] = ys / aspect; pr[5] = ys; pr[8] = 0.0003f; pr[9] = -0.0002f; pr[10] = 0; pr[11] = 1; pr[14] = 0.1f;
    // Column-vector (transposed) copy.
    float* pc = &mem[30000];
    std::memset(pc, 0, 64);
    pc[0] = ys / aspect; pc[5] = ys; pc[2] = 0.0001f; pc[10] = 0; pc[14] = 1; pc[11] = 0.1f;

    std::vector<CameraCandidate> cams;
    CameraSearchParams params;
    uintptr_t base = 0x10000000;
    ScanSpanForCameras(reinterpret_cast<const uint8_t*>(mem.data()), mem.size() * 4, base, hero, up, params, cams, 64);
    bool foundCam = false, foundHero = false;
    for (auto& cc : cams) {
        if (cc.address == base + camAt * 4) {
            foundCam = true;
            CHECK(cc.forwardAxis == 2 && cc.forwardSign == 1 && cc.upAxis == 1 && cc.rightAxis == 0);
            CHECK(CameraIsLeftHanded(cc, c));
            CameraPose pose;
            CHECK(PoseFromCandidate(c, cc, params, pose));
            CHECK_NEAR(pose.position.x, camPos.x, 1e-3);
        }
        if (cc.address == base + 8192 * 4) foundHero = true;
    }
    CHECK(foundCam);
    CHECK(!foundHero);
    std::printf("  camera candidates: %zu\n", cams.size());

    // Right-handed camera (looks down -Z of its basis): right = cross(fwd, up).
    float rh[16];
    Vec3 back = fwd * -1.0f;
    Vec3 rr = Normalize(Cross(fwd, up));
    Vec3 ru = Cross(rr, fwd);
    rh[0] = rr.x; rh[1] = rr.y; rh[2] = rr.z; rh[3] = 0;
    rh[4] = ru.x; rh[5] = ru.y; rh[6] = ru.z; rh[7] = 0;
    rh[8] = back.x; rh[9] = back.y; rh[10] = back.z; rh[11] = 0;
    rh[12] = float(camPos.x); rh[13] = float(camPos.y); rh[14] = float(camPos.z); rh[15] = 1;
    CameraCandidate rc;
    CameraPose rp;
    CHECK(EvaluateCameraMatrix(rh, MatrixLayout::RowsAxes, hero, up, params, rc, rp));
    CHECK(rc.forwardSign == -1);
    CHECK(!CameraIsLeftHanded(rc, rh));

    // Columns layout of the LH camera.
    float cl[16];
    for (int i = 0; i < 3; ++i) {
        cl[i * 4 + 0] = c[0 + i];
        cl[i * 4 + 1] = c[4 + i];
        cl[i * 4 + 2] = c[8 + i];
    }
    cl[3] = c[12]; cl[7] = c[13]; cl[11] = c[14];
    cl[12] = cl[13] = cl[14] = 0; cl[15] = 1;
    CameraCandidate cc2;
    CameraPose cp2;
    CHECK(EvaluateCameraMatrix(cl, MatrixLayout::ColumnsAxes, hero, up, params, cc2, cp2));
    CHECK(cc2.forwardAxis == 2 && CameraIsLeftHanded(cc2, cl));

    std::vector<ProjectionCandidate> projs;
    ScanSpanForProjections(reinterpret_cast<const uint8_t*>(mem.data()), mem.size() * 4, base, aspect, projs, 64);
    CHECK(projs.size() >= 2);
    float fov = MostCommonFov(projs);
    CHECK_NEAR(fov * 180.0f / 3.14159265f, 60.0f, 0.05f);
    ProjectionCandidate wrongAspect;
    CHECK(!EvaluateProjection(pr, 4.0f / 3.0f, wrongAspect));
}

static void TestCombat() {
    std::printf("combat\n");
    using namespace sm64;
    CHECK(ClassifyAttack(ACT_PUNCHING, MARIO_PUNCHING, 0) == AttackKind::Punch);
    CHECK(ClassifyAttack(ACT_JUMP_KICK, MARIO_KICKING, 5) == AttackKind::Kick);
    CHECK(ClassifyAttack(ACT_GROUND_POUND, 0, -50) == AttackKind::GroundPound);
    CHECK(ClassifyAttack(ACT_GROUND_POUND_LAND, 0, 0) == AttackKind::Shockwave);
    CHECK(ClassifyAttack(ACT_DIVE, 0, -3) == AttackKind::Dive);
    CHECK(ClassifyAttack(ACT_FREEFALL, 0, -10) == AttackKind::Stomp);
    CHECK(ClassifyAttack(ACT_FREEFALL, 0, 10) == AttackKind::None);
    CHECK(ClassifyAttack(ACT_IDLE, 0, 0) == AttackKind::None);

    CombatTuning t;
    const float u = 100;
    CHECK(AttackReaches(AttackKind::Punch, Vec3(0, 0, 0), 0, Vec3(80, 0, 0), t, u));
    CHECK(!AttackReaches(AttackKind::Punch, Vec3(0, 0, 0), 0, Vec3(200, 0, 0), t, u));
    CHECK(!AttackReaches(AttackKind::Punch, Vec3(0, 0, 0), 0, Vec3(80, 400, 0), t, u));
    CHECK(AttackReaches(AttackKind::Stomp, Vec3(10, 190, 0), -20, Vec3(0, 0, 0), t, u));
    CHECK(!AttackReaches(AttackKind::Stomp, Vec3(10, 30, 0), -20, Vec3(0, 0, 0), t, u));
    CHECK(AttackReaches(AttackKind::Shockwave, Vec3(0, 0, 0), 0, Vec3(200, 10, 100), t, u));
    CHECK(!AttackReaches(AttackKind::None, Vec3(0, 0, 0), 0, Vec3(0, 0, 0), t, u));

    CHECK(IncomingDamageWedges(0, 100, 1) == 0);
    CHECK(IncomingDamageWedges(5, 100, 1) == 1);
    CHECK(IncomingDamageWedges(20, 100, 1) == 2);
    CHECK(IncomingDamageWedges(90, 100, 1) == 4);
}

// Fake libsm64 for controller tests.
namespace fake {
static int ticks = 0;
static float pos[3];
int32_t create(float x, float y, float z) {
    pos[0] = x; pos[1] = y; pos[2] = z;
    ticks = 0;
    return 0;
}
void tick(int32_t, const SM64MarioInputs*, SM64MarioState* s, SM64MarioGeometryBuffers* g) {
    ++ticks;
    pos[0] += 10;
    for (int i = 0; i < 3; ++i) s->position[i] = pos[i];
    g->numTrianglesUsed = 2;
    for (int v = 0; v < 6; ++v) {
        g->position[v * 3 + 0] = pos[0] + float(v);
        g->position[v * 3 + 1] = pos[1];
        g->position[v * 3 + 2] = pos[2];
    }
}
void del(int32_t) {}
void setpos(int32_t, float x, float y, float z) {
    pos[0] = x; pos[1] = y; pos[2] = z;
}
void face(int32_t, float) {}
} // namespace fake

static void TestMarioController() {
    std::printf("mario controller\n");
    Sm64Api api;
    api.mario_create = fake::create;
    api.mario_tick = fake::tick;
    api.mario_delete = fake::del;
    api.set_mario_position = fake::setpos;
    api.set_mario_faceangle = fake::face;
    MarioController mc(api);
    CHECK(mc.Spawn(Vec3(0, 0, 0), 0));
    SM64MarioInputs in{};
    mc.Tick(in); // x = 10
    mc.Tick(in); // x = 20
    CHECK_NEAR(mc.Position(0.5f).x, 15, 1e-4);
    MarioGeometry g;
    mc.Geometry(0.25f, g);
    CHECK(g.triangles == 2);
    CHECK_NEAR(g.position[0], 12.5f, 1e-4);
    mc.ShiftOrigin(Vec3(20, 0, 0));
    CHECK_NEAR(mc.Position(1.0f).x, 0, 1e-4);
    CHECK_NEAR(mc.Position(0.0f).x, -10, 1e-4);
    mc.Geometry(0.5f, g);
    CHECK_NEAR(g.position[0], -5.0f, 1e-4);
    CHECK_NEAR(fake::pos[0], 0, 1e-4);
    mc.Teleport(Vec3(100, 0, 0));
    CHECK_NEAR(mc.Position(0.0f).x, 100, 1e-4);
    mc.Despawn();
    CHECK(!mc.Alive());
}

static void TestShippedConfig() {
    std::printf("shipped config\n");
    Ini ini;
    CHECK(ini.LoadFile("package/resources/sm2mario/sm2mario.ini"));
    ModConfig c = LoadModConfig(ini);
    ModConfig d; // struct defaults
    CHECK(c.fallDamage == "capped" && c.onDeath == "respawn");
    CHECK(c.upAxis == 'Y' && c.mirror == "auto" && c.unitsPerMetre == 100.0);
    CHECK(!c.hasWater);
    CHECK(c.cameraSource == "auto" && c.hideHero == "engine");
    CHECK(c.renderMode == "world" && c.shadows && c.collisionSource == "physics");
    // The game's physics: what the file says is what the struct defaults are.
    CHECK(c.physicsRaysPerFrame == d.physicsRaysPerFrame && c.physicsPoolHeadroom == d.physicsPoolHeadroom);
    CHECK_NEAR(c.physics.ringRadius, d.physics.ringRadius, 1e-6);
    CHECK(c.physics.ringDirs == d.physics.ringDirs);
    CHECK_NEAR(c.physics.castAbove, d.physics.castAbove, 1e-6);
    CHECK(c.physics.floorType == 3 && c.physics.wallType == 3 && c.physics.waterType == 17);
    CHECK(c.notSolidComponents == d.notSolidComponents && c.solidComponents == d.solidComponents);
    CHECK(c.pauseWithGame && c.onlyMarioControls && c.blockGameInput);
    CHECK_NEAR(c.followLead, d.followLead, 1e-6);
    CHECK_NEAR(c.followLeadMax, d.followLeadMax, 1e-6);
    CHECK_NEAR(c.specular, d.specular, 1e-6);
    CHECK(c.metalMaterial == d.metalMaterial);
    CHECK(c.gameDamage && c.hitPeople);
    for (int k = 1; k < int(AttackKind::Count); ++k) CHECK(c.tuning.reaction[k] == d.tuning.reaction[k]);
    CHECK(!c.wingCap && !c.metalCap && !c.vanishCap);
    CHECK(c.gbufferMarkers.size() == 2 && c.gbufferMarkers[0] == "GBuffer Dynamic");
    CHECK(c.gbufferFormats.size() == 4 && c.gbufferFormats[0] == 41 && c.gbufferFormats[3] == 17);
    CHECK_NEAR(c.gloss, 0.15, 1e-6);
    {
        // A user file from 0.2.0 still holds the old, too shiny defaults: they
        // move to the new ones; a value the user chose stays.
        const std::string userPath = "build-tests/migrate_user.ini";
        {
            std::ofstream f(userPath, std::ios::binary | std::ios::trunc);
            f << "[Render]\r\nMode = world\r\nGloss = 0.45             ; shine\r\nSpecular = 0.07\r\n";
        }
        const std::vector<std::string> changed =
            MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.4.0");
        CHECK(changed.size() == 1);
        Ini m;
        CHECK(m.LoadFile(userPath));
        CHECK_NEAR(m.GetFloat("Render", "Gloss"), 0.15, 1e-6);
        CHECK_NEAR(m.GetFloat("Render", "Specular"), 0.07, 1e-6);
        CHECK(m.GetString("General", "SettingsVersion") == "0.4.0");
        CHECK(MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.4.0").empty()); // idempotent
    }
    {
        // A 0.3.0 file: its rendered-world collision, punch-on-E and specular
        // defaults move on; its one cap still counts.
        const std::string userPath = "build-tests/migrate_user03.ini";
        {
            std::ofstream f(userPath, std::ios::binary | std::ios::trunc);
            f << "[Controls]\r\nKeyPunch = LMB, E        ; SM64 \"B\"\r\n[Collision]\r\nSource = world\r\n"
                 "[Cheats]\r\nCap = metal\r\n[Render]\r\nGloss = 0.15\r\nSpecular = 0.03\r\n";
        }
        const std::vector<std::string> changed =
            MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.4.0");
        CHECK(changed.size() == 4);
        Ini m;
        CHECK(m.LoadFile(userPath));
        CHECK(Ini::Lower(m.GetString("Collision", "Source")) == "physics");
        CHECK(m.GetString("Controls", "KeyPunch") == "LMB");
        CHECK_NEAR(m.GetFloat("Render", "Specular"), 0.02, 1e-6);
        CHECK_NEAR(m.GetFloat("Render", "Gloss"), 0.15, 1e-6);
        CHECK(!m.Has("Cheats", "Cap") && m.GetBool("Cheats", "MetalCap", false) && !m.GetBool("Cheats", "WingCap", true));
        const ModConfig u = LoadModConfig(m);
        CHECK(u.metalCap && !u.wingCap && !u.vanishCap && u.collisionSource == "physics");
        // As the mod loads it: the shipped file with the user's merged over it.
        Ini merged;
        CHECK(merged.LoadFile("package/resources/sm2mario/sm2mario.ini"));
        merged.Merge(m);
        const ModConfig um = LoadModConfig(merged);
        CHECK(um.metalCap && !um.wingCap && !um.vanishCap);
        CHECK(MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.4.0").empty());
        // The rendered world chosen again in 0.4: it stays (the file says it is from 0.4 now).
        CHECK(Ini::SetValuesInFile(userPath, {{"Collision", "Source", "world"}}));
        CHECK(MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.4.1").empty());
        Ini again;
        CHECK(again.LoadFile(userPath) && Ini::Lower(again.GetString("Collision", "Source")) == "world");
        CHECK(again.GetString("General", "SettingsVersion") == "0.4.1");
    }
    {
        // A 0.4.0 file: its 12 s fall limit moves to 30 s (a jump off a tower
        // falls for ~20 s); 12 chosen again in 0.5 stays.
        const std::string userPath = "build-tests/migrate_user04.ini";
        {
            std::ofstream f(userPath, std::ios::binary | std::ios::trunc);
            f << "[General]\r\nSettingsVersion = 0.4.0\r\n[Mario]\r\nRespawnAfterFallSeconds = 12\r\n"
                 "[Render]\r\nSpecular = 0.02\r\n";
        }
        const std::vector<std::string> changed =
            MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.5.0");
        CHECK(changed.size() == 1);
        Ini m;
        CHECK(m.LoadFile(userPath));
        CHECK_NEAR(m.GetFloat("Mario", "RespawnAfterFallSeconds"), 30.0, 1e-6);
        CHECK(m.GetString("General", "SettingsVersion") == "0.5.0");
        CHECK(Ini::SetValuesInFile(userPath, {{"Mario", "RespawnAfterFallSeconds", "12"}}));
        CHECK(MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.5.0").empty());
        Ini again;
        CHECK(again.LoadFile(userPath) && again.GetFloat("Mario", "RespawnAfterFallSeconds") == 12.0f);
    }
    {
        // A 0.5 file with 0.5's defaults written out: the rays start 8 m up
        // instead of 20, 32 a frame instead of 24, and the camera follows jumps
        // with 0.05 s of smoothing instead of 0.15. A value of the user's own stays.
        const std::string userPath = "build-tests/migrate_user05.ini";
        {
            std::ofstream f(userPath, std::ios::binary | std::ios::trunc);
            f << "[General]\r\nSettingsVersion = 0.5.0\r\n[Collision]\r\nRaysPerFrame = 24\r\nRoofSearchHeight = 20\r\n"
                 "[Camera]\r\nVerticalSmoothing = 0.15\r\n[Render]\r\nSpecular = 0.05\r\n";
        }
        const std::vector<std::string> changed =
            MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.6.0");
        CHECK(changed.size() == 3);
        Ini m;
        CHECK(m.LoadFile(userPath));
        CHECK(m.GetInt("Collision", "RaysPerFrame", 0) == 32);
        CHECK_NEAR(m.GetFloat("Collision", "RoofSearchHeight"), 8.0, 1e-6);
        CHECK_NEAR(m.GetFloat("Camera", "VerticalSmoothing"), 0.05, 1e-6);
        CHECK_NEAR(m.GetFloat("Render", "Specular"), 0.05, 1e-6);
        CHECK(m.GetString("General", "SettingsVersion") == "0.6.0");
        const ModConfig c6 = LoadModConfig(m);
        CHECK(c6.physicsRaysPerFrame == 32 && c6.marioStencil == "auto" && c6.stencilCensusFrames == 150);
        CHECK_NEAR(c6.physics.castAbove, 8.0, 1e-6);
        CHECK(MigrateUserIni("package/resources/sm2mario/sm2mario.ini", userPath, "0.6.0").empty());
    }
    CHECK(SettingsVersionNumber("0.4.0") > SettingsVersionNumber("0.3.0"));
    CHECK(SettingsVersionNumber("0.10.0") > SettingsVersionNumber("0.9.9"));
    CHECK(SettingsVersionNumber("1") == SettingsVersionNumber("1.0.0"));
    CHECK(SettingsVersionNumber("") == 0 && SettingsVersionNumber("abc") == 0);
    CHECK(c.captureWidth == 480 && c.frameReport);
    {
        // v0.1 configs: "scale" (and anything unknown) means the engine's hide now.
        Ini old;
        old.Set("Hero", "HideHero", "scale");
        old.Set("Render", "Mode", "bogus");
        old.Set("Collision", "Source", "bogus");
        const ModConfig o = LoadModConfig(old);
        CHECK(o.hideHero == "engine" && o.renderMode == "world" && o.collisionSource == "physics");
        old.Set("Hero", "HideHero", "None");
        old.Set("Render", "Mode", "Overlay");
        old.Set("Collision", "Source", "flat");
        old.Set("Render", "GBufferFormats", "41, 34, 17, 17, 42");
        const ModConfig o2 = LoadModConfig(old);
        CHECK(o2.hideHero == "none" && o2.renderMode == "overlay" && o2.collisionSource == "flat");
        CHECK(o2.gbufferFormats.size() == 5 && o2.gbufferFormats[4] == 42);
        old.Set("Collision", "Source", "Raycast"); // 0.3's name for the game's physics
        CHECK(LoadModConfig(old).collisionSource == "physics");
        old.Set("Collision", "Source", "World");
        CHECK(LoadModConfig(old).collisionSource == "world");
    }
    CHECK_NEAR(c.collision.probeUp, d.collision.probeUp, 1e-6);
    CHECK_NEAR(c.collision.minClearance, d.collision.minClearance, 1e-6);
    CHECK_NEAR(c.tuning.damageFraction[int(AttackKind::GroundPound)], 0.5, 1e-6);
    CHECK(c.enemyExclude.size() >= 3);
    CHECK(!c.debugOverlayOnStart && !c.calibrateOnStart);
    // Every [Combat] *Damage/*Knockback key in the file maps onto a tuning slot.
    for (const auto& key : ini.Keys("Combat")) {
        bool known = key.find("damage") == std::string::npos || key == "damagemultiplier" ||
                     key == "incomingdamagescale" || key == "usegamedamage";
        const char* kinds[] = {"punch", "kick", "trip", "slidekick", "dive", "stomp", "groundpound", "shockwave"};
        for (const char* k : kinds)
            if (key == std::string(k) + "damage") known = true;
        CHECK(known);
        // ... and every *Reaction key names a reaction.
        if (key.size() > 8 && key.compare(key.size() - 8, 8, "reaction") == 0) {
            bool slot = false;
            for (const char* k : kinds)
                if (key == std::string(k) + "reaction") slot = true;
            Reaction r;
            CHECK(slot && ParseReaction(ini.GetString("Combat", key.c_str(), ""), r));
        }
    }
    Ini b;
    CHECK(b.LoadFile("package/resources/sm2mario/bindings.ini"));
    const char* sections[] = {"HeroSystem", "GetActor", "TransformSetPosition", "TransformHide", "TransformUnhide",
                              "ComponentRegistry", "GetComponentInfo", "GetComponent1", "GetComponent2"};
    for (const char* sec : sections) CHECK(Pattern::Parse(b.GetString(sec, "pattern")).valid);
    CHECK(b.GetInt("Layout", "health_current") == 0xD0);
    // The game's physics and damage.
    const char* physics[] = {"PhysicsRaycast",         "PhysicsFrame",     "CollRequestInit",  "CollRequestRelease",
                             "CollRequestIgnoreActor", "QueryResultActor", "PhysicsQueryPool", "DamageSphere"};
    for (const char* sec : physics) CHECK(Pattern::Parse(b.GetString(sec, "pattern")).valid);
    CHECK(!b.Has("Raycast", "pattern")); // 0.3's placeholder: gone
}


static void TestViewConstants() {
    std::printf("view constants\n");
    float rows[kViewCbRows][4];
    const DVec3 cam(1234.5, 17.25, -876.0);
    const Vec3 fwd = Normalize(Vec3(0.3f, -0.4f, 1.0f));
    BuildViewConstants(cam, fwd, Vec3(0, 1, 0), 1.0f, 2293, 960, 0.1f, cam, nullptr, rows);
    ViewConstants v;
    std::string why;
    CHECK(ParseViewConstants(&rows[0][0], v, &why));
    CHECK(v.perspective);
    CHECK(v.leftHanded);
    CHECK(v.renderWidth == 2293 && v.renderHeight == 960);
    CHECK_NEAR(v.fovY, 1.0, 1e-4);
    CHECK_NEAR(v.camPos.x, 1234.5, 1e-3);
    // Project / unproject round trip, far from the origin.
    const DVec3 p(1240.25, 15.5, -860.75);
    double nx, ny, w;
    CHECK(v.Project(p, nx, ny, w));
    DVec3 back;
    CHECK(v.Unproject(nx, ny, w, back));
    CHECK(Length(back - p) < 1e-3);
    // Linear depth == distance along the forward axis.
    CHECK_NEAR(w, Dot((p - cam), DVec3(fwd)), 1e-3);
    // A point behind the camera doesn't project.
    CHECK(!v.Project(cam - DVec3(fwd) * 5.0, nx, ny, w));
    // TAA jitter (an x offset proportional to w) keeps the round trip exact.
    for (int k = 0; k < 3; ++k) rows[4 + k][0] += 0.0004f * fwd[k];
    CHECK(ParseViewConstants(&rows[0][0], v, &why));
    CHECK(v.Project(p, nx, ny, w) && v.Unproject(nx, ny, w, back) && Length(back - p) < 1e-3);
    // Handedness is what the screen shows: the same image with the stored
    // camera x axis pointing left (an engine flipping it in the projection)
    // is still left-handed, and a mirrored projection is right-handed.
    {
        float flipped[kViewCbRows][4];
        std::memcpy(flipped, rows, sizeof(flipped));
        for (int k = 0; k < 3; ++k) flipped[0][k] = -flipped[0][k]; // stored "right" points left
        ViewConstants f;
        CHECK(ParseViewConstants(&flipped[0][0], f, &why));
        CHECK(f.leftHanded && f.xScale < 0);
        for (int k = 0; k < 4; ++k) flipped[4 + k][0] = -flipped[4 + k][0]; // and the image mirrored too
        CHECK(ParseViewConstants(&flipped[0][0], f, &why));
        CHECK(!f.leftHanded && f.xScale > 0);
        double fx, fy, fw, ox, oy, ow;
        CHECK(f.Project(p, fx, fy, fw) && v.Project(p, ox, oy, ow));
        CHECK_NEAR(fx, -ox, 1e-6); // really the mirror image
    }
    // Garbage is rejected.
    float bad[kViewCbRows][4];
    std::memcpy(bad, rows, sizeof(bad));
    bad[0][0] = 2.0f;
    CHECK(!ParseViewConstants(&bad[0][0], v, &why));
    std::memcpy(bad, rows, sizeof(bad));
    bad[5][3] = 0.5f; // w column no longer the forward axis
    CHECK(!ParseViewConstants(&bad[0][0], v, &why));
    // Orthographic shadow view: w column (0,0,0,1).
    float ortho[kViewCbRows][4] = {};
    ortho[0][0] = 1; ortho[1][2] = 1; ortho[2][1] = -1; // looking straight down
    ortho[3][0] = 10; ortho[3][1] = 100; ortho[3][2] = 20; ortho[3][3] = 1;
    ortho[4][0] = 0.02f; ortho[6][1] = -0.02f; ortho[5][2] = -0.001f;
    ortho[7][2] = 0.5f; ortho[7][3] = 1.0f;
    CHECK(ParseViewConstants(&ortho[0][0], v, &why));
    CHECK(!v.perspective);
}

static void TestGBufferFormat() {
    std::printf("gbuffer format\n");
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst = 0;
    for (int i = 0; i < 20000; ++i) {
        Vec3 n = Normalize(Vec3(u(rng), u(rng), u(rng)));
        if (i < 6) n = Vec3(i == 0 ? 1.f : i == 1 ? -1.f : 0.f, i == 2 ? 1.f : i == 3 ? -1.f : 0.f, i == 4 ? 1.f : i == 5 ? -1.f : 0.f);
        const uint32_t enc = gbuf::PackNormal(n.x, n.y, n.z);
        CHECK((enc >> 25) == 0);
        float d[3];
        gbuf::UnpackNormal(enc, d);
        worst = std::max(worst, std::acos(std::min(1.0f, Dot(n, Normalize(Vec3(d[0], d[1], d[2]))))));
    }
    std::printf("  worst normal error %.4f deg\n", worst * 57.29578f);
    CHECK(worst < 0.15f / 57.3f * 10); // ~1 degree worst case incl. the poles
    float a[3] = {0.6f, 0.1f, 0.02f}, b[3];
    gbuf::UnpackAlbedo(gbuf::PackAlbedo(a[0], a[1], a[2]), b);
    CHECK_NEAR(b[0], 0.6, 0.01);
    CHECK_NEAR(b[1], 0.1, 0.01);
    CHECK_NEAR(b[2], 0.02, 0.01);
    const float alb[3] = {1, 1, 1}, nrm[3] = {0, 1, 0};
    const gbuf::Packed pk = gbuf::PackDefault(alb, nrm);
    CHECK(gbuf::ShadingModel(pk.g0y) == 1);
    CHECK(pk.g0y == 0x02400000u);   // what PS_DefaultMaterialGBuffer writes
    CHECK((pk.g1x >> 31) == 1 && ((pk.g1x >> 23) & 255) == 255);
    CHECK((pk.g1y >> 16) == 0xFFFF);
}

static void TestD3D12Parse() {
    std::printf("d3d12 parse\n");
    // Serialized root signature 1.1: [0] CBV b0 space0 (all), [1] constants b1 x 4 (VS), [2] table, [3] SRV t5 space1
    std::vector<uint8_t> rts(24 + 4 * 12 + 64, 0);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(&rts[off], &v, 4); };
    put(0, 2); put(4, 4); put(8, 24); put(12, 0); put(16, 0); put(20, 1);
    size_t payload = 24 + 4 * 12;
    const uint32_t types[4] = {2, 1, 0, 3}, vis[4] = {0, 1, 5, 0};
    for (int i = 0; i < 4; ++i) {
        put(24 + 12 * i, types[i]);
        put(24 + 12 * i + 4, vis[i]);
        put(24 + 12 * i + 8, uint32_t(payload));
        if (i == 0) { put(payload, 0); put(payload + 4, 0); put(payload + 8, 0); payload += 12; }
        if (i == 1) { put(payload, 1); put(payload + 4, 0); put(payload + 8, 4); payload += 12; }
        if (i == 2) { put(payload, 0); put(payload + 4, 0); payload += 8; }
        if (i == 3) { put(payload, 5); put(payload + 4, 1); put(payload + 8, 0); payload += 12; }
    }
    // Wrap it in a DXBC container like D3D12SerializeVersionedRootSignature does.
    std::vector<uint8_t> blob(36 + 8 + rts.size(), 0);
    std::memcpy(&blob[0], "DXBC", 4);
    uint32_t v32 = 1; std::memcpy(&blob[20], &v32, 4);
    v32 = uint32_t(blob.size()); std::memcpy(&blob[24], &v32, 4);
    v32 = 1; std::memcpy(&blob[28], &v32, 4);
    v32 = 36; std::memcpy(&blob[32], &v32, 4);
    std::memcpy(&blob[36], "RTS0", 4);
    v32 = uint32_t(rts.size()); std::memcpy(&blob[40], &v32, 4);
    std::memcpy(&blob[44], rts.data(), rts.size());
    d3d12p::RootSigInfo rs;
    std::string why;
    CHECK(d3d12p::ParseRootSignature(blob.data(), blob.size(), rs, &why));
    CHECK(rs.params.size() == 4);
    CHECK(rs.FindRootCbv(0, 0) == 0);
    CHECK(rs.FindRootCbv(1, 0) == -1);
    CHECK(rs.params[1].type == d3d12p::RootParamType::Constants && rs.params[1].num32BitValues == 4);
    CHECK(rs.params[3].type == d3d12p::RootParamType::Srv && rs.params[3].registerSpace == 1);
    CHECK(!d3d12p::ParseRootSignature(blob.data(), 30, rs, &why));

    // Pixel shader container with an OSG1 output signature: SV_Depth.
    std::vector<uint8_t> ps(32 + 4 + 8 + 8 + 32 + 16, 0);
    std::memcpy(&ps[0], "DXBC", 4);
    for (int i = 0; i < 16; ++i) ps[4 + i] = uint8_t(i * 7 + 1);
    v32 = 1; std::memcpy(&ps[20], &v32, 4);
    v32 = uint32_t(ps.size()); std::memcpy(&ps[24], &v32, 4);
    v32 = 1; std::memcpy(&ps[28], &v32, 4);
    v32 = 36; std::memcpy(&ps[32], &v32, 4);
    std::memcpy(&ps[36], "OSG1", 4);
    v32 = 8 + 32 + 16; std::memcpy(&ps[40], &v32, 4);
    uint8_t* part = &ps[44];
    v32 = 1; std::memcpy(part, &v32, 4);       // count
    v32 = 8; std::memcpy(part + 4, &v32, 4);   // offset
    v32 = 40; std::memcpy(part + 8 + 4, &v32, 4);   // name offset
    v32 = 65; std::memcpy(part + 8 + 12, &v32, 4);  // SV_Depth
    std::memcpy(part + 40, "SV_Depth", 8);
    d3d12p::ShaderInfo si;
    CHECK(d3d12p::ParseShader(ps.data(), ps.size(), si));
    CHECK(si.present && si.writesDepth && si.numTargets == 0 && si.digest[1] == 8);

    // Pipeline state stream: root sig, PS, RT formats (4), DSV format, depth stencil, rasterizer.
    std::vector<uint8_t> st(512, 0);
    size_t off = 0;
    auto sub = [&](uint32_t type, const void* inner, size_t size, size_t align) {
        std::memcpy(&st[off], &type, 4);
        const size_t in = (off + 4 + align - 1) & ~(align - 1);
        std::memcpy(&st[in], inner, size);
        off = (in + size + 7) & ~size_t(7);
    };
    void* fakeRs = reinterpret_cast<void*>(uintptr_t(0x12345678));
    sub(0, &fakeRs, 8, 8);
    struct { const void* p; uint64_t n; } psb = {ps.data(), ps.size()};
    sub(2, &psb, 16, 8);
    struct { uint32_t f[8]; uint32_t n; } rtf = {{41, 34, 17, 17, 0, 0, 0, 0}, 4};
    sub(15, &rtf, 36, 4);
    uint32_t dsv = 20;
    sub(16, &dsv, 4, 4);
    uint32_t ds[13] = {1, 1, 7, 0};
    sub(11, ds, 52, 4);
    uint32_t rast[11] = {3, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    int32_t bias = -5; std::memcpy(&rast[3], &bias, 4);
    sub(10, rast, 44, 4);
    d3d12p::PsoInfo pi;
    CHECK(d3d12p::ParsePipelineStream(st.data(), off, pi, &why));
    CHECK(pi.rootSignature == fakeRs);
    CHECK(pi.numRenderTargets == 4 && pi.rtvFormats[0] == 41 && pi.rtvFormats[3] == 17);
    CHECK(pi.dsvFormat == 20 && pi.depthEnable && pi.depthWrite && pi.depthFunc == 7);
    CHECK(d3d12p::IsReverseZFunc(pi.depthFunc));
    CHECK(pi.depthBias == -5 && pi.cullMode == 1);
    CHECK(pi.ps.present && pi.ps.writesDepth);
    uint32_t junk = 99;
    off = 0;
    sub(99, &junk, 4, 4);
    CHECK(!d3d12p::ParsePipelineStream(st.data(), off, pi, &why));
}

// Serialized RTS0 shaped like Spider-Man 2's graphics root signature
// (142cd9750 in Spider-Man2.exe): [0] 24 root constants at b15, [1] SRV table
// t57.. space1 (all stages), then per stage (VS, PS) SRV / CBV / UAV / sampler
// tables. version 1 = 1.0 ranges (20 bytes), 2 = 1.1 (24 bytes, APPEND offsets).
static std::vector<uint8_t> GameLikeRootSig(uint32_t version) {
    struct Range { uint32_t type, num, base, space; };
    struct Param { uint32_t type, vis; std::vector<Range> ranges; uint32_t reg, space, n; };
    std::vector<Param> params;
    params.push_back({1, 0, {}, 15, 0, 24});
    params.push_back({0, 0, {{0, 70, 57, 1}}, 0, 0, 0});
    for (uint32_t vis : {1u, 5u}) {
        params.push_back({0, vis, {{0, 57, 0, 0}}, 0, 0, 0});
        params.push_back({0, vis, {{2, 14, 0, 0}}, 0, 0, 0});
        params.push_back({0, vis, {{1, 16, 0, 0}}, 0, 0, 0});
        params.push_back({0, vis, {{3, 16, 0, 0}}, 0, 0, 0});
    }
    const size_t rangeSize = version == 1 ? 20 : 24;
    std::vector<uint8_t> b(4096, 0);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(&b[off], &v, 4); };
    put(0, version);
    put(4, uint32_t(params.size()));
    put(8, 24);
    put(20, 0xC1D);
    size_t payload = 24 + 12 * params.size();
    for (size_t i = 0; i < params.size(); ++i) {
        const Param& p = params[i];
        put(24 + 12 * i, p.type);
        put(24 + 12 * i + 4, p.vis);
        put(24 + 12 * i + 8, uint32_t(payload));
        if (p.type == 0) {
            put(payload, uint32_t(p.ranges.size()));
            put(payload + 4, uint32_t(payload + 8));
            size_t r = payload + 8;
            for (const Range& rg : p.ranges) {
                put(r, rg.type);
                put(r + 4, rg.num);
                put(r + 8, rg.base);
                put(r + 12, rg.space);
                put(r + rangeSize - 4, version == 1 ? 0u : 0xFFFFFFFFu);
                r += rangeSize;
            }
            payload = r;
        } else {
            put(payload, p.reg);
            put(payload + 4, p.space);
            put(payload + 8, p.n);
            payload += 12;
        }
    }
    b.resize(payload);
    return b;
}

static void TestRootSigTables() {
    std::printf("root signature tables\n");
    for (uint32_t version : {1u, 2u}) {
        std::vector<uint8_t> blob = GameLikeRootSig(version);
        frame::RootSigLayout l;
        std::string why;
        CHECK(d3d12p::ParseRootSignature(blob.data(), blob.size(), l.info, &why));
        CHECK(l.info.params.size() == 10);
        frame::BuildLayout(l);
        CHECK(l.usable);
        CHECK(l.viewVs.param == 3 && l.viewVs.offset == 0); // VS CBV table, b0 first
        CHECK(l.viewRootCbv == -1);
        CHECK(l.constTotal == 24 && l.constOffset[0] == 0 && l.constOffset[1] == 24);
        const d3d12p::TableSlot ps = l.info.FindTableDescriptor(d3d12p::kRangeCbv, 6, 0, d3d12p::kVisPixel);
        CHECK(ps.param == 7 && ps.offset == 6);
        const d3d12p::TableSlot g = l.info.FindTableDescriptor(d3d12p::kRangeSrv, 116, 1, d3d12p::kVisPixel);
        CHECK(g.param == 1 && g.offset == 59); // g_GlobalSrv_GBufferDepth_116
        CHECK(l.info.FindTableDescriptor(d3d12p::kRangeCbv, 14, 0, d3d12p::kVisVertex).param == -1);
    }
    // Appended ranges: [CBV b0 x2][SRV t0 x3 append][UAV u4 x1 at 10]
    std::vector<uint8_t> b(256, 0);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(&b[off], &v, 4); };
    put(0, 2); put(4, 1); put(8, 24);
    put(24, 0); put(28, 0); put(32, 36);
    put(36, 3); put(40, 44);
    const uint32_t rg[3][6] = {{2, 2, 0, 0, 0, 0xFFFFFFFFu}, {0, 3, 0, 0, 0, 0xFFFFFFFFu}, {1, 1, 4, 0, 0, 10}};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 6; ++k) put(44 + 24 * i + 4 * k, rg[i][k]);
    d3d12p::RootSigInfo rs;
    CHECK(d3d12p::ParseRootSignature(b.data(), 44 + 72, rs));
    CHECK(rs.params[0].ranges.size() == 3);
    CHECK(rs.params[0].ranges[1].offset == 2 && rs.params[0].ranges[2].offset == 10);
    CHECK(rs.FindTableDescriptor(d3d12p::kRangeSrv, 2, 0, d3d12p::kVisVertex).offset == 4);
    CHECK(rs.FindTableDescriptor(d3d12p::kRangeUav, 4, 0, d3d12p::kVisPixel).offset == 10);
}

static void TestGraphicsPsoDesc() {
    std::printf("graphics pso desc\n");
    std::vector<uint8_t> d(d3d12p::kGraphicsPsoDescSize, 0);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(&d[off], &v, 4); };
    uint64_t rsPtr = 0xABCD0000;
    std::memcpy(&d[0], &rsPtr, 8);
    put(120 + 4, 1);                  // independent blend
    d[120 + 8 + 40 * 0 + 36] = 15;
    d[120 + 8 + 40 * 4 + 36] = 0;     // RT4 masked
    put(452 + 4, 2);                  // cull front
    int32_t bias = 1000; std::memcpy(&d[452 + 12], &bias, 4);
    float slope = 2.5f; std::memcpy(&d[452 + 20], &slope, 4);
    put(452 + 24, 1);
    put(496 + 0, 1); put(496 + 4, 1); put(496 + 8, 7); put(496 + 12, 1);
    d[496 + 16] = 0x0F; d[496 + 17] = 0x30;
    put(496 + 20 + 8, 3);             // front pass: REPLACE
    put(572, 3);
    put(576, 5);
    const uint32_t f[5] = {41, 34, 17, 17, 42};
    for (int i = 0; i < 5; ++i) put(580 + 4 * i, f[i]);
    put(612, 20);
    put(616, 1);
    d3d12p::PsoInfo p;
    CHECK(d3d12p::ParseGraphicsPsoDesc(d.data(), p));
    CHECK(p.rootSignature == reinterpret_cast<void*>(uintptr_t(rsPtr)));
    CHECK(p.numRenderTargets == 5 && p.rtvFormats[4] == 42 && p.dsvFormat == 20);
    CHECK(p.depthEnable && p.depthWrite && p.depthFunc == 7 && p.stencilEnable);
    CHECK(p.stencilReadMask == 0x0F && p.stencilWriteMask == 0x30 && p.front.pass == 3);
    CHECK(p.cullMode == 2 && p.depthBias == 1000 && p.slopeScaledDepthBias == 2.5f && p.depthClipEnable);
    CHECK(p.rtWriteMask[0] == 15 && p.rtWriteMask[4] == 0);
    CHECK(frame::Classify(p, frame::DefaultClassifyConfig()) == frame::PsoKind::GBuffer);
    put(580, 28);
    CHECK(d3d12p::ParseGraphicsPsoDesc(d.data(), p));
    CHECK(frame::Classify(p, frame::DefaultClassifyConfig()) == frame::PsoKind::Other);
    std::string hex = "57e678a45ff1fe17d8b60a31cc5c940d";
    uint8_t dg[16];
    CHECK(d3d12p::ParseDigest(hex, dg) && d3d12p::DigestHex(dg) == hex);
    CHECK(!d3d12p::ParseDigest("57e678a45ff1fe17d8b60a31cc5c94", dg));
}

// A list that draws the G-buffer, then shadow regions, as the game does.
static void TestFramePolicy() {
    std::printf("frame policy\n");
    // The precompiled injection shaders must come from the current source
    // (tools/compile_shaders.sh regenerates them).
    CHECK(InjectShaderSourceHash() == shaderbin::kSourceHash);
    CHECK(sizeof(shaderbin::kVSGBuffer) > 100 && std::memcmp(shaderbin::kPSGBuffer, "DXBC", 4) == 0);
    frame::Registry reg;
    frame::PolicyConfig pcfg;
    frame::Policy pol(&reg, pcfg);
    std::vector<uint8_t> blob = GameLikeRootSig(1);
    frame::RootSigLayout layout;
    CHECK(d3d12p::ParseRootSignature(blob.data(), blob.size(), layout.info));
    frame::BuildLayout(layout);

    frame::PsoClass gb, caster, copy, move, depthOnly, other;
    gb.info.numRenderTargets = 4;
    const uint32_t gf[4] = {41, 34, 17, 17};
    std::memcpy(gb.info.rtvFormats, gf, sizeof(gf));
    gb.info.dsvFormat = 20;
    gb.info.depthEnable = gb.info.depthWrite = true;
    gb.info.depthFunc = d3d12p::kCmpGreaterEqual;
    gb.kind = frame::Classify(gb.info, frame::DefaultClassifyConfig());
    CHECK(gb.kind == frame::PsoKind::GBuffer);
    caster.info.dsvFormat = 40;
    caster.info.depthEnable = caster.info.depthWrite = true;
    caster.info.vs.present = true;
    d3d12p::ParseDigest("05a36ebf39ff6d35941e48b710a404d9", caster.info.vs.digest);
    caster.kind = frame::Classify(caster.info, frame::DefaultClassifyConfig());
    CHECK(caster.kind == frame::PsoKind::ShadowCaster);
    copy.info = caster.info;
    copy.info.vs.present = false;
    copy.info.ps.present = true;
    d3d12p::ParseDigest("57e678a45ff1fe17d8b60a31cc5c940d", copy.info.ps.digest);
    copy.kind = frame::Classify(copy.info, frame::DefaultClassifyConfig());
    CHECK(copy.kind == frame::PsoKind::CacheCopy);
    move.info = copy.info;
    d3d12p::ParseDigest("445fce1a23ad66df1d1b392ad58dbc5f", move.info.ps.digest);
    move.kind = frame::Classify(move.info, frame::DefaultClassifyConfig());
    CHECK(move.kind == frame::PsoKind::CacheMove);
    depthOnly.info = caster.info;
    depthOnly.info.vs.present = false;
    depthOnly.kind = frame::Classify(depthOnly.info, frame::DefaultClassifyConfig());
    CHECK(depthOnly.kind == frame::PsoKind::Other);
    other.kind = frame::PsoKind::Other;

    const uint32_t stride = 32;
    const uint64_t vsTable = 0x9000000000ull;
    auto bindGBuffer = [&](frame::ListState& s, uint32_t w, uint32_t h, uint64_t base) {
        s.numRt = 4;
        for (int i = 0; i < 4; ++i) {
            s.rt[i] = frame::ViewInfo();
            s.rt[i].valid = true;
            s.rt[i].resource = base + uint64_t(i);
            s.rt[i].format = gf[i];
            s.rt[i].width = w;
            s.rt[i].height = h;
            s.rtv[i] = 0x100 + uint64_t(i);
        }
        s.ds = frame::ViewInfo();
        s.ds.valid = true;
        s.ds.resource = base + 10;
        s.ds.format = 20;
        s.ds.width = w;
        s.ds.height = h;
        s.dsv = 0x200;
        s.numVp = 1;
        s.vp[0].w = float(w);
        s.vp[0].h = float(h);
    };
    auto setRoot = [&](frame::ListState& s) {
        s.rootSig = 0x77;
        s.layout = &layout;
        s.args[3].kind = frame::kArgTable;
        s.args[3].value = vsTable;
    };
    std::vector<frame::Decision> out;
    std::vector<frame::Skip> skips;

    // Frame 1: the main view's size is unknown yet -> nothing.
    reg.BeginFrame(1);
    frame::ListState s;
    s.Reset(1);
    setRoot(s);
    bindGBuffer(s, 1920, 1080, 0x1000);
    s.pso = 1;
    s.psoClass = &gb;
    for (int i = 0; i < 5; ++i) pol.OnDraw(s, stride);
    pol.AtBoundary(s, frame::Boundary::Barrier, out, &skips);
    CHECK(out.empty() && skips.size() == 1);

    // Frame 2: main view known; a small probe G-buffer is skipped.
    reg.BeginFrame(2);
    s.Reset(2);
    setRoot(s);
    bindGBuffer(s, 1920, 1080, 0x1000);
    s.psoClass = &gb;
    pol.OnDraw(s, stride);
    pol.OnDraw(s, stride);
    pol.AtBoundary(s, frame::Boundary::RenderTargets, out, &skips);
    CHECK(out.size() == 1 && out[0].act == frame::Act::GBuffer);
    CHECK(out[0].g.viewTable == vsTable && out[0].g.draws == 2 && out[0].g.rt[0].resource == 0x1000);
    CHECK(reg.IsMainDepthTarget(0x1000, 0) && !reg.IsMainDepthTarget(0x1001, 0));
    out.clear();
    bindGBuffer(s, 256, 256, 0x5000);
    pol.OnDraw(s, stride);
    skips.clear();
    pol.AtBoundary(s, frame::Boundary::Close, out, &skips);
    CHECK(out.empty() && skips.size() == 1 && std::string(skips[0].why) == "not the main view");

    // A segment without depth writes (EQUAL after a pre-pass) is skipped.
    frame::PsoClass gbEq = gb;
    gbEq.info.depthWrite = false;
    bindGBuffer(s, 1920, 1080, 0x1000);
    s.psoClass = &gbEq;
    pol.OnDraw(s, stride);
    skips.clear();
    pol.AtBoundary(s, frame::Boundary::Barrier, out, &skips);
    CHECK(out.empty() && skips.size() == 1);
    // ...and one where b0 isn't reachable (no table set).
    s.psoClass = &gb;
    s.args[3] = frame::RootArg();
    pol.OnDraw(s, stride);
    pol.AtBoundary(s, frame::Boundary::Barrier, out, &skips);
    CHECK(out.empty());

    // PIX markers: once a segment inside "GBuffer Dynamic" was seen, only
    // such segments get Mario.
    setRoot(s);
    std::strcpy(s.markers[0], "GBuffer Dynamic");
    s.markerDepth = 1;
    pol.OnDraw(s, stride);
    pol.AtBoundary(s, frame::Boundary::Barrier, out, &skips);
    CHECK(out.size() == 1 && out[0].g.markerMatch);
    out.clear();
    std::strcpy(s.markers[0], "GBuffer Static");
    pol.OnDraw(s, stride);
    skips.clear();
    pol.AtBoundary(s, frame::Boundary::Barrier, out, &skips);
    CHECK(out.empty() && skips.size() == 1);
    s.markerDepth = 0;

    // Shadows. Region A: cache copied, then casters -> inject at the
    // viewport change. Region B: casters without a copy/clear -> skip (could
    // be a cache being rebuilt). Region C: in a cache texture -> skip.
    auto bindShadow = [&](frame::ListState& st, uint64_t res, float x, float y, float size) {
        st.numRt = 0;
        st.ds = frame::ViewInfo();
        st.ds.valid = true;
        st.ds.resource = res;
        st.ds.format = 40;
        st.ds.width = st.ds.height = 4096;
        st.dsv = 0x300;
        st.numVp = 1;
        st.vp[0] = frame::Viewport();
        st.vp[0].x = x;
        st.vp[0].y = y;
        st.vp[0].w = st.vp[0].h = size;
    };
    reg.BeginFrame(3);
    frame::ListState sh;
    sh.Reset(3);
    setRoot(sh);
    bindShadow(sh, 0xA000, 0, 0, 1024);
    sh.psoClass = &copy;
    pol.OnDraw(sh, stride);
    CHECK(reg.Kind(0xA000) == frame::Registry::MapKind::Working);
    sh.psoClass = &caster;
    for (int i = 0; i < 3; ++i) pol.OnDraw(sh, stride);
    sh.psoClass = &depthOnly; // a material-specific caster in the same region
    pol.OnDraw(sh, stride);
    out.clear();
    skips.clear();
    pol.AtBoundary(sh, frame::Boundary::Viewports, out, &skips);
    CHECK(out.size() == 1 && out[0].act == frame::Act::Shadow && out[0].s.byCopy && out[0].s.casterDraws == 4);
    CHECK(out[0].s.viewTable == vsTable && out[0].s.vp.x == 0 && out[0].s.ds.resource == 0xA000);
    CHECK(!out[0].s.viewFromCopy && out[0].s.pso == &depthOnly);
    out.clear();
    bindShadow(sh, 0xA000, 1024, 0, 1024);
    sh.psoClass = &caster;
    pol.OnDraw(sh, stride);
    pol.AtBoundary(sh, frame::Boundary::Viewports, out, &skips);
    CHECK(out.empty() && skips.size() == 1);
    // A region refreshed from the cache with no dynamic caster at all (the
    // hero is hidden) still gets Mario, with the copy's view and the last
    // known caster pipeline.
    bindShadow(sh, 0xA000, 0, 1024, 1024);
    sh.psoClass = &copy;
    pol.OnDraw(sh, stride);
    pol.AtBoundary(sh, frame::Boundary::Viewports, out, &skips);
    CHECK(out.size() == 1 && out[0].s.viewFromCopy && out[0].s.casterDraws == 0 && out[0].s.pso == &caster);
    CHECK(out[0].s.vp.y == 1024);
    out.clear();
    // A clear of the working map makes a region eligible too.
    const frame::Rect rc{2048, 0, 3072, 1024};
    bindShadow(sh, 0xA000, 2048, 0, 1024);
    pol.AtBoundary(sh, frame::Boundary::ClearDepth, out, &skips);
    pol.OnClearDepth(sh, sh.ds, &rc, 1);
    sh.psoClass = &caster;
    pol.OnDraw(sh, stride);
    pol.AtBoundary(sh, frame::Boundary::Close, out, &skips);
    CHECK(out.size() == 1 && !out[0].s.byCopy);
    out.clear();
    // A cache texture: something moves cache contents into it.
    frame::ListState cs;
    cs.Reset(3);
    setRoot(cs);
    bindShadow(cs, 0xC000, 0, 0, 2048);
    cs.psoClass = &move;
    pol.OnDraw(cs, stride);
    CHECK(reg.Kind(0xC000) == frame::Registry::MapKind::Cache);
    pol.OnClearDepth(cs, cs.ds, nullptr, 0);
    cs.psoClass = &caster;
    pol.OnDraw(cs, stride);
    skips.clear();
    pol.AtBoundary(cs, frame::Boundary::Close, out, &skips);
    CHECK(out.empty() && skips.size() == 1 && std::string(skips[0].why) == "shadow cache texture");
    // The main depth buffer, cleared, with depth-only draws (a pre-pass) is
    // never treated as a shadow map.
    frame::ListState pre;
    pre.Reset(3);
    setRoot(pre);
    bindShadow(pre, 0x1010, 0, 0, 1920);
    pol.OnClearDepth(pre, pre.ds, nullptr, 0);
    pre.psoClass = &depthOnly;
    pol.OnDraw(pre, stride);
    pol.AtBoundary(pre, frame::Boundary::Close, out, &skips);
    CHECK(out.empty());

    // The real game's sun shadows: an 8192 atlas whose regions get casters
    // every frame, refreshed in a way the tracker doesn't see. Skipped while
    // only refreshed regions are used; with steady regions on, the ones
    // redrawn (nearly) every frame qualify - a region that only gets casters
    // now and then (a cache being built) never does.
    {
        frame::Registry r2;
        frame::Policy p2(&r2, pcfg);
        auto frameOf = [&](uint64_t f, bool sparse, std::vector<frame::Decision>& o, std::vector<frame::Skip>& k) {
            r2.BeginFrame(f);
            frame::ListState a;
            a.Reset(f);
            setRoot(a);
            bindShadow(a, 0xD000, 0, 0, 2048);
            a.psoClass = &caster;
            for (int i = 0; i < 20; ++i) p2.OnDraw(a, stride);
            p2.AtBoundary(a, frame::Boundary::Viewports, o, &k);
            if (sparse) {
                bindShadow(a, 0xD000, 2048, 0, 2048);
                for (int i = 0; i < 20; ++i) p2.OnDraw(a, stride);
                p2.AtBoundary(a, frame::Boundary::Close, o, &k);
            }
        };
        std::vector<frame::Decision> o;
        std::vector<frame::Skip> k;
        for (uint64_t f = 10; f < 30; ++f) frameOf(f, f % 5 == 0, o, k);
        CHECK(o.empty() && !k.empty() && k[0].shadow && k[0].casters == 20);
        CHECK(std::string(k[0].why) == "shadow region not refreshed in this list (cached?)");
        p2.SetSteadyRegions(true);
        CHECK(p2.SteadyRegions());
        int every = 0, sparse = 0;
        for (uint64_t f = 30; f < 60; ++f) {
            o.clear();
            frameOf(f, f % 5 == 0, o, k);
            for (const frame::Decision& d : o) (d.s.vp.x == 0 ? every : sparse)++;
        }
        CHECK(every == 30 && sparse == 0);
        // A brand-new region isn't steady until it has 12 frames of history.
        o.clear();
        frame::Registry r3;
        frame::Policy p3(&r3, pcfg);
        p3.SetSteadyRegions(true);
        int first = -1;
        for (uint64_t f = 1; f <= 16 && first < 0; ++f) {
            r3.BeginFrame(f);
            frame::ListState a;
            a.Reset(f);
            setRoot(a);
            bindShadow(a, 0xD100, 0, 0, 1024);
            a.psoClass = &caster;
            p3.OnDraw(a, stride);
            p3.AtBoundary(a, frame::Boundary::Close, o, &k);
            if (!o.empty()) first = int(f);
        }
        CHECK(first == frame::Registry::kSteadyFrames);
    }
    // A region reset by a depth-only quad with depth test ALWAYS (instead of a
    // clear): prepared; used once the map is known to be redrawn every frame.
    {
        frame::Registry r4;
        frame::Policy p4(&r4, pcfg);
        frame::PsoClass quad = depthOnly;
        quad.info.depthFunc = d3d12p::kCmpAlways;
        int injectedAt = -1, refreshes = 0;
        for (uint64_t f = 1; f <= 20; ++f) {
            r4.BeginFrame(f);
            frame::ListState a;
            a.Reset(f);
            setRoot(a);
            bindShadow(a, 0xE000, 0, 0, 1024);
            a.numScissor = 1;
            a.scissor[0] = frame::Rect{1, 1, 1023, 1023}; // inset by a texel
            a.psoClass = &quad;
            p4.OnDraw(a, stride);
            a.psoClass = &caster;
            for (int i = 0; i < 5; ++i) p4.OnDraw(a, stride);
            std::vector<frame::Decision> o;
            std::vector<frame::Skip> k;
            p4.AtBoundary(a, frame::Boundary::Close, o, &k);
            refreshes += int(a.refreshDraws);
            CHECK(a.casterDraws == 5);
            if (!o.empty() && injectedAt < 0) injectedAt = int(f);
            if (o.empty() && !k.empty() && f == 1)
                CHECK(std::string(k[0].why) == "depth target that is not a known shadow map (not redrawn every frame)");
        }
        CHECK(refreshes == 20 && injectedAt == frame::Registry::kSteadyFrames);
        // Clear rects inset from the viewport still count.
        frame::Prepared pr;
        pr.rc = frame::Rect{2, 2, 1022, 1022};
        CHECK(frame::Covers(pr, frame::Rect{0, 0, 1024, 1024}));
        pr.rc = frame::Rect{0, 0, 1024, 512};
        CHECK(!frame::Covers(pr, frame::Rect{0, 0, 1024, 1024}));
    }

    // 0.3.0's pixelated trail: with steady regions on, Mario's shadow was
    // drawn into the game's screen-space depth buffers too (a full-size
    // depth-stencil target redrawn every frame by depth-only draws, a
    // half-size occlusion depth, a 64x64 one, targets with colour bound).
    // None of them is a shadow map.
    {
        frame::Registry r5;
        frame::Policy p5(&r5, pcfg);
        p5.SetSteadyRegions(true);
        std::map<std::string, int> why;
        int injected = 0;
        auto screenDepth = [&](frame::ListState& a, uint64_t res, uint32_t w, uint32_t h, uint32_t fmt, int rts) {
            a.numRt = uint32_t(rts);
            for (int i = 0; i < rts; ++i) {
                a.rt[i] = frame::ViewInfo();
                a.rt[i].valid = true;
                a.rt[i].resource = res + 100 + uint64_t(i);
                a.rt[i].width = w;
                a.rt[i].height = h;
            }
            a.ds = frame::ViewInfo();
            a.ds.valid = true;
            a.ds.resource = res;
            a.ds.format = fmt;
            a.ds.width = w;
            a.ds.height = h;
            a.dsv = 0x400;
            a.numVp = 1;
            a.vp[0] = frame::Viewport();
            a.vp[0].w = float(w);
            a.vp[0].h = float(h);
        };
        for (uint64_t f = 1; f <= 30; ++f) {
            r5.BeginFrame(f);
            frame::ListState a;
            a.Reset(f);
            setRoot(a);
            bindGBuffer(a, 2544, 1440, 0x7000); // the main view
            a.psoClass = &gb;
            p5.OnDraw(a, stride);
            std::vector<frame::Decision> o;
            std::vector<frame::Skip> k;
            p5.AtBoundary(a, frame::Boundary::Barrier, o, &k);
            o.clear();
            k.clear();
            const struct {
                uint64_t res;
                uint32_t w, h, fmt;
                int rts;
            } targets[] = {{0x045ca, 2544, 1440, 20, 0}, {0x044fc, 1272, 720, 40, 0}, {0x93600, 64, 64, 40, 0},
                           {0x92a52, 1024, 1024, 20, 2}, {0x0d32, 2540, 1440, 40, 0}};
            for (const auto& t : targets) {
                screenDepth(a, t.res, t.w, t.h, t.fmt, t.rts);
                if (f % 2) pol.OnClearDepth(a, a.ds, nullptr, 0);
                a.psoClass = &depthOnly;
                for (int i = 0; i < 6; ++i) p5.OnDraw(a, stride);
                p5.AtBoundary(a, frame::Boundary::Viewports, o, &k);
            }
            // The real shadow atlas next to them still gets Mario.
            bindShadow(a, 0xF432C, 0, 0, 2048);
            a.ds.format = 55;
            a.ds.width = a.ds.height = 8192;
            a.psoClass = &depthOnly;
            for (int i = 0; i < 6; ++i) p5.OnDraw(a, stride);
            p5.AtBoundary(a, frame::Boundary::Close, o, &k);
            for (const frame::Decision& d : o) {
                CHECK(d.s.ds.resource == 0xF432C);
                ++injected;
            }
            for (const frame::Skip& sk : k) why[sk.why]++;
        }
        CHECK(injected == 30 - frame::Registry::kSteadyFrames + 1);
        // (from frame 2 on: the main view's size is learnt from the frame before)
        CHECK(why["screen-sized depth target (not a shadow map)"] == 29 * 3);
        CHECK(why["depth target too small for a shadow map"] == 30);
        CHECK(why["colour targets bound with the depth (not a shadow map)"] == 30);
    }

    // Capture once per frame.
    CHECK(reg.TryClaimCapture(3) && !reg.TryClaimCapture(3) && reg.TryClaimCapture(4));

    // PIX payloads.
    const char ansi[] = "GBuffer Dynamic";
    CHECK(frame::DecodePixEvent(1, ansi, sizeof(ansi)) == "GBuffer Dynamic");
    const wchar_t* wide = L"Create CSM";
    std::vector<uint8_t> w16;
    for (const wchar_t* c = wide; *c; ++c) {
        w16.push_back(uint8_t(*c));
        w16.push_back(0);
    }
    w16.push_back(0);
    w16.push_back(0);
    CHECK(frame::DecodePixEvent(0, w16.data(), uint32_t(w16.size())) == "Create CSM");
    std::vector<uint8_t> pix3(24, 0x11);
    pix3[0] = 2;
    const char body[] = "Sun Shadows";
    pix3.insert(pix3.end(), body, body + sizeof(body));
    pix3.resize(pix3.size() + 8, 0);
    pix3.push_back(0x7F);
    CHECK(frame::DecodePixEvent(2, pix3.data(), uint32_t(pix3.size())) == "Sun Shadows");
}

static void TestWorldModel() {
    std::printf("world model\n");
    // Ground at y = 0 and a wall face at x = 5 (facing -x), seen from a camera
    // at (0, 2, 0) looking along +x.
    float rows[kViewCbRows][4];
    const DVec3 cam(0, 2, 0);
    BuildViewConstants(cam, Normalize(Vec3(1, -0.35f, 0)), Vec3(0, 1, 0), 1.0f, 320, 180, 0.1f, cam, nullptr, rows);
    DepthFrame f;
    CHECK(ParseViewConstants(&rows[0][0], f.view));
    f.width = 320;
    f.height = 180;
    f.depth.assign(size_t(320) * 180, 0.0f);
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) {
            double nx, ny;
            ViewConstants::PixelToNdc(x, y, 320, 180, nx, ny);
            DVec3 p1;
            f.view.Unproject(nx, ny, 1.0, p1);
            const DVec3 d = p1 - cam;
            double t = 1e30;
            if (d.y < 0) t = std::min(t, -cam.y / d.y);
            if (d.x > 0) {
                const double tw = (5.0 - cam.x) / d.x;
                const DVec3 h = cam + d * tw;
                if (h.y >= 0 && h.y <= 3 && std::fabs(h.z) < 4) t = std::min(t, tw);
            }
            f.depth[size_t(y) * 320 + x] = t < 1e29 ? float(t) : 0.0f;
        }
    WorldModel wm(1);
    std::vector<Cylinder> none;
    wm.Integrate(f, DVec3(0, 0, 0), none);
    wm.Integrate(f, DVec3(0, 0, 0), none);
    CHECK(wm.Stats().floorSamples > 100 && wm.Stats().wallSamples > 50);
    RayHit hit;
    CHECK(wm.Cast(DVec3(2, 1, 0), DVec3(2, -5, 0), hit));
    CHECK_NEAR(hit.point.y, 0.0, 0.02);
    CHECK(hit.normal.y > 0.99f);
    CHECK(wm.Cast(DVec3(3, 1, 0.5), DVec3(7, 1, 0.5), hit));
    CHECK_NEAR(hit.point.x, 5.0, 0.15);
    CHECK(hit.normal.x < -0.9f);
    CHECK(!wm.Cast(DVec3(3, 3.6, 0.5), DVec3(7, 3.6, 0.5), hit)); // above the 3 m wall
    CHECK(!wm.Cast(DVec3(-3, 1, 0.5), DVec3(-1, 1, 0.5), hit));  // nothing seen behind the camera
    // Rays starting inside the wall pass through (builder convention).
    CHECK(!wm.Cast(DVec3(5.05, 1, 0.5), DVec3(7, 1, 0.5), hit));
    // Fallback ground only near its centre.
    CHECK(!wm.Cast(DVec3(-20, 1, 0), DVec3(-20, -5, 0), hit));
    wm.SetFallbackGround(DVec3(-20, 0, 0), -0.5, 3.0);
    CHECK(wm.Cast(DVec3(-20, 1, 0), DVec3(-20, -5, 0), hit) && std::fabs(hit.point.y + 0.5) < 1e-6);
    // An excluded body is not integrated.
    WorldModel wm2(1);
    std::vector<Cylinder> body(1);
    body[0].base = DVec3(5, 0, 0);
    body[0].radius = 10;
    body[0].height = 10; // the whole 3 m wall is inside the body
    wm2.Integrate(f, DVec3(0, 0, 0), body);
    CHECK(wm2.Stats().excluded > 0 && wm2.Stats().wallSamples == 0);
    // A shorter body only hides what is inside it: the wall above 2 m stays.
    WorldModel wm3(1);
    body[0].height = 2.0f;
    wm3.Integrate(f, DVec3(0, 0, 0), body);
    CHECK(wm3.Stats().excluded > 0 && wm3.Stats().wallSamples > 0);
    // Box exclusions (Mario's bounds as drawn): only what is inside the box.
    WorldModel wm4(1);
    std::vector<ExcludeBox> boxes(1);
    boxes[0].lo = DVec3(4.5, -0.2, -1.0);
    boxes[0].hi = DVec3(5.5, 1.5, 1.0);
    wm4.Integrate(f, DVec3(0, 0, 0), std::vector<Cylinder>(), boxes);
    wm4.Integrate(f, DVec3(0, 0, 0), std::vector<Cylinder>(), boxes);
    CHECK(wm4.Stats().excluded > 0 && wm4.Stats().wallSamples > 0);
    CHECK(wm4.Cast(DVec3(3, 1, 2.0), DVec3(7, 1, 2.0), hit) && std::fabs(hit.point.x - 5.0) < 0.15); // beside the box
    CHECK(wm4.Cast(DVec3(3, 2.0, 0.0), DVec3(7, 2.0, 0.0), hit) && std::fabs(hit.point.x - 5.0) < 0.15); // above it
}

// A street seen from a camera, with a car (a 2 x 1.4 x 4 m box) that may move
// between frames; depth by ray casting, motion vectors the game's way.
struct StreetScene {
    DVec3 cam{0, 2, -6}, prevCam{0, 2, -6};
    Vec3 fwd = Normalize(Vec3(0.2f, -0.3f, 1.0f));
    DVec3 car{3, 0, 6}, prevCar{3, 0, 6};
    bool haveCar = true;
    static constexpr int W = 320, H = 180;

    static bool HitBox(const DVec3& o, const DVec3& d, const DVec3& lo, const DVec3& hi, double& t) {
        double t0 = 0, t1 = 1e30;
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(d[k]) < 1e-12) {
                if (o[k] < lo[k] || o[k] > hi[k]) return false;
                continue;
            }
            double a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
        }
        if (t0 > t1) return false;
        t = t0;
        return true;
    }
    DepthFrame Render(bool withMotion, bool garbageMotion = false) const {
        float prevRows[kViewCbRows][4], rows[kViewCbRows][4];
        BuildViewConstants(prevCam, fwd, Vec3(0, 1, 0), 1.0f, W, H, 0.1f, prevCam, nullptr, prevRows);
        ViewConstants pv;
        ParseViewConstants(&prevRows[0][0], pv);
        BuildViewConstants(cam, fwd, Vec3(0, 1, 0), 1.0f, W, H, 0.1f, prevCam, pv.vp, rows);
        DepthFrame f;
        ParseViewConstants(&rows[0][0], f.view);
        f.width = W;
        f.height = H;
        f.depth.assign(size_t(W) * H, 0.0f);
        if (withMotion) f.motion.assign(size_t(W) * H * 2, 0.0f);
        const DVec3 lo = car + DVec3(-1, 0, -2), hi = car + DVec3(1, 1.4, 2);
        std::mt19937 rng(7);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                double nx, ny;
                ViewConstants::PixelToNdc(x, y, W, H, nx, ny);
                DVec3 p1;
                f.view.Unproject(nx, ny, 1.0, p1);
                const DVec3 d = p1 - cam; // w = 1 at t = 1
                double t = 1e30, tb;
                bool onCar = false;
                if (d.y < 0) t = -cam.y / d.y;
                if (haveCar && HitBox(cam, d, lo, hi, tb) && tb < t) {
                    t = tb;
                    onCar = true;
                }
                if (t > 1e29) continue;
                const size_t i = size_t(y) * W + x;
                f.depth[i] = float(t);
                if (!withMotion) continue;
                if (garbageMotion) {
                    f.motion[i * 2] = float(int(rng() % 200) - 100) / 100.0f;
                    f.motion[i * 2 + 1] = float(int(rng() % 200) - 100) / 100.0f;
                    continue;
                }
                const DVec3 P = cam + d * t;
                const DVec3 prevP = onCar ? P - (car - prevCar) : P;
                double px, py, pw;
                pv.Project(prevP, px, py, pw);
                const double uPrev = px * 0.5 + 0.5, vPrev = py * -0.5 + 0.5;
                f.motion[i * 2] = float(((x + 0.5) / W - uPrev) * f.view.raw[33][0]);
                f.motion[i * 2 + 1] = float(((y + 0.5) / H - vPrev) * f.view.raw[33][1]);
            }
        return f;
    }
};

static void TestWorldMotion() {
    std::printf("world model: moving things\n");
    const std::vector<Cylinder> none;
    const DVec3 focus(0, 0, 0);
    RayHit hit;
    auto carSolid = [&](const WorldModel& wm, const DVec3& car) {
        // into the car's front (the face the camera sees) at 0.7 m
        return wm.Cast(car + DVec3(0, 0.7, -4), car + DVec3(0, 0.7, 0), hit);
    };
    // A parked car (no motion) becomes solid, with or without motion vectors.
    for (int mv = 0; mv < 2; ++mv) {
        StreetScene sc;
        WorldModel wm(1);
        for (int k = 0; k < 4; ++k) wm.Integrate(sc.Render(mv == 1), focus, none);
        CHECK(carSolid(wm, sc.car));
        if (mv) CHECK(wm.Stats().motionUsed && wm.Stats().moving == 0);
    }
    // A car driving by (0.4 m a frame along x) never becomes solid with motion
    // vectors - while the street stays a floor; without them it leaves a trail.
    for (int mv = 0; mv < 2; ++mv) {
        StreetScene sc;
        WorldModel wm(1);
        sc.car = DVec3(-4, 0, 6);
        for (int k = 0; k < 20; ++k) {
            sc.prevCar = sc.car;
            sc.car.x += 0.4;
            wm.Integrate(sc.Render(mv == 1), focus, none);
            if (mv == 1) CHECK(wm.Stats().motionUsed);
        }
        const DVec3 mid(0, 0, 6); // the car passed here 10 frames ago
        if (mv == 1) {
            CHECK(wm.Stats().moving > 50);
            CHECK(!carSolid(wm, mid) && !carSolid(wm, sc.car));
        } else {
            CHECK(carSolid(wm, sc.car));
        }
        CHECK(wm.Cast(DVec3(0, 1, 2), DVec3(0, -3, 2), hit) && std::fabs(hit.point.y) < 0.05);
    }
    // A parked car that drives off: unsettled as soon as it moves.
    {
        StreetScene sc;
        WorldModel wm(1);
        for (int k = 0; k < 6; ++k) wm.Integrate(sc.Render(true), focus, none);
        CHECK(carSolid(wm, sc.car));
        const DVec3 parked = sc.car;
        sc.prevCar = sc.car;
        sc.car.x += 0.3;
        wm.Integrate(sc.Render(true), focus, none);
        CHECK(wm.Stats().unsettled > 0);
        CHECK(!carSolid(wm, parked));
    }
    // The camera moving: the street's own motion is the camera's - static.
    {
        StreetScene sc;
        WorldModel wm(1);
        for (int k = 0; k < 4; ++k) {
            sc.prevCam = sc.cam;
            sc.cam.x += 0.3;
            sc.cam.z += 0.2;
            wm.Integrate(sc.Render(true), focus, none);
            CHECK(wm.Stats().motionUsed && wm.Stats().moving == 0);
        }
        CHECK(carSolid(wm, sc.car));
    }
    // Vectors that don't match the camera at all: ignored, depth still used.
    {
        StreetScene sc;
        WorldModel wm(1);
        for (int k = 0; k < 3; ++k) wm.Integrate(sc.Render(true, true), focus, none);
        CHECK(!wm.Stats().motionUsed && wm.Stats().motionRejected == 3);
        CHECK(carSolid(wm, sc.car));
    }
}

static void TestRtti() {
    std::printf("rtti\n");
    // A hand-made image: type descriptor, two locators (primary and a
    // secondary vtable at offset 8), their vtables, and a decoy class.
    alignas(16) static uint8_t img[0x1000];
    std::memset(img, 0, sizeof(img));
    const uintptr_t base = reinterpret_cast<uintptr_t>(img);
    auto put32 = [&](size_t off, uint32_t v) { std::memcpy(img + off, &v, 4); };
    auto put64 = [&](size_t off, uint64_t v) { std::memcpy(img + off, &v, 8); };
    std::strcpy(reinterpret_cast<char*>(img + 0x110), ".?AVCameraTarget@Camera2@@");
    std::strcpy(reinterpret_cast<char*>(img + 0x150), ".?AVCameraTargetManual@Camera2@@");
    // decoy locator for the other class first
    put32(0x180, 1); put32(0x184, 0); put32(0x18C, 0x140); put32(0x194, 0x180);
    put64(0x1F8, base + 0x180);
    // secondary (offset 8) locator before the primary one
    put32(0x200, 1); put32(0x204, 8); put32(0x20C, 0x100); put32(0x214, 0x200);
    put64(0x2F8, base + 0x200);
    put32(0x240, 1); put32(0x244, 0); put32(0x24C, 0x100); put32(0x254, 0x240);
    put64(0x3F8, base + 0x240);
    put64(0x400, 0x1111); // slot 0
    std::vector<rtti::Span> spans{{base, sizeof(img)}};
    CHECK(rtti::FindVtable(base, spans, ".?AVCameraTarget@Camera2@@") == base + 0x400);
    CHECK(rtti::FindVtable(base, spans, ".?AVCameraTarget@Camera2@@", 8) == base + 0x300);
    CHECK(rtti::FindVtable(base, spans, ".?AVCameraTargetManual@Camera2@@") == base + 0x200);
    CHECK(rtti::FindVtable(base, spans, ".?AVNothing@@") == 0);
    // A locator whose self offset is wrong isn't one.
    put32(0x254, 0x123);
    CHECK(rtti::FindVtable(base, spans, ".?AVCameraTarget@Camera2@@") == 0);

    // Optional: the real game executable, mapped section by section like the
    // loader would (no relocations: slots hold preferred-base addresses).
    if (const char* exe = std::getenv("SM2MARIO_TEST_EXE")) {
        std::ifstream f(exe, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(file.size() > 0x1000);
        if (file.size() > 0x1000) {
            uint32_t lfanew, sizeOfImage;
            std::memcpy(&lfanew, &file[0x3C], 4);
            std::memcpy(&sizeOfImage, &file[lfanew + 24 + 56], 4);
            uint16_t nsec, optSize;
            std::memcpy(&nsec, &file[lfanew + 6], 2);
            std::memcpy(&optSize, &file[lfanew + 20], 2);
            std::vector<uint8_t> mem(size_t(sizeOfImage) + 0x10000, 0);
            uint8_t* image = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(mem.data()) + 0xFFFF) & ~uintptr_t(0xFFFF));
            std::memcpy(image, file.data(), 0x1000);
            for (uint16_t i = 0; i < nsec; ++i) {
                const size_t sh = lfanew + 24 + optSize + size_t(i) * 40;
                uint32_t va, raw, ptr;
                std::memcpy(&va, &file[sh + 12], 4);
                std::memcpy(&raw, &file[sh + 16], 4);
                std::memcpy(&ptr, &file[sh + 20], 4);
                if (raw && ptr + raw <= file.size()) std::memcpy(image + va, &file[ptr], raw);
            }
            const uintptr_t ib = reinterpret_cast<uintptr_t>(image);
            // Base relocations (data directory 5), as the loader applies them.
            uint64_t preferred;
            uint32_t relRva, relSize;
            std::memcpy(&preferred, &file[lfanew + 24 + 24], 8);
            std::memcpy(&relRva, &file[lfanew + 24 + 112 + 5 * 8], 4);
            std::memcpy(&relSize, &file[lfanew + 24 + 112 + 5 * 8 + 4], 4);
            const uint64_t delta = uint64_t(ib) - preferred;
            for (uint32_t off = 0; off + 8 <= relSize;) {
                uint32_t page, size;
                std::memcpy(&page, image + relRva + off, 4);
                std::memcpy(&size, image + relRva + off + 4, 4);
                if (size < 8) break;
                for (uint32_t k = 8; k + 2 <= size; k += 2) {
                    uint16_t e;
                    std::memcpy(&e, image + relRva + off + k, 2);
                    if ((e >> 12) != 10) continue; // IMAGE_REL_BASED_DIR64
                    uint64_t v;
                    std::memcpy(&v, image + page + (e & 0xFFF), 8);
                    v += delta;
                    std::memcpy(image + page + (e & 0xFFF), &v, 8);
                }
                off += size;
            }
            const auto spans2 = rtti::ImageDataSpans(ib);
            CHECK(spans2.size() >= 2);
            const uintptr_t vt = rtti::FindVtable(ib, spans2, ".?AVCameraTarget@Camera2@@");
            std::printf("  Spider-Man2.exe: CameraTarget vtable at rva 0x%llx\n", static_cast<unsigned long long>(vt - ib));
            CHECK(vt - ib == 0x538a670);
            uint64_t slot10 = 0;
            if (vt) std::memcpy(&slot10, reinterpret_cast<const void*>(vt + 80), 8);
            CHECK(slot10 - ib == 0x2986b20ull);
            // The shipped bindings fit this exe: each hooked virtual starts the
            // way its check_ pattern says.
            Ini b;
            CHECK(b.LoadFile("package/resources/sm2mario/bindings.ini"));
            auto slotFn = [&](uintptr_t vtable, int64_t slot) {
                uint64_t fn = 0;
                if (vtable && slot >= 0) std::memcpy(&fn, reinterpret_cast<const void*>(vtable + uintptr_t(slot) * 8), 8);
                return uintptr_t(fn);
            };
            auto startsLike = [&](uintptr_t fn, const std::string& check, size_t window) {
                const Pattern pat = Pattern::Parse(check);
                return fn > ib && pat.valid && FindPattern(reinterpret_cast<const uint8_t*>(fn), window, pat) >= 0;
            };
            for (const char* key : {"get_position", "get_matrix", "get_track_position"}) {
                const uintptr_t fn = slotFn(vt, b.GetInt("CameraTarget", key, -1));
                CHECK(startsLike(fn, b.GetString("CameraTarget", std::string("check_") + key, ""), 48));
            }
            const uintptr_t pvt = rtti::FindVtable(ib, spans2, b.GetString("PedestrianInteract", "class", "").c_str());
            std::printf("  Spider-Man2.exe: BehaviorPedestrianInteract vtable at rva 0x%llx\n",
                        static_cast<unsigned long long>(pvt ? pvt - ib : 0));
            const uintptr_t enter = slotFn(pvt, b.GetInt("PedestrianInteract", "enter", -1));
            CHECK(pvt != 0 && enter - ib == 0x55aec0);
            CHECK(startsLike(enter, b.GetString("PedestrianInteract", "check_enter", ""), 64));
        }
    }
}

static void TestFollowMonitor() {
    std::printf("follow monitor\n");
    // Mario jumps 2 m at t = 1 s (parabola), lands at 1.8 s; 60 samples a second.
    auto marioAt = [](double t) {
        if (t < 1.0 || t > 1.8) return 5.0;
        const double u = (t - 1.0) / 0.8;
        return 5.0 + 8.0 * u * (1.0 - u);
    };
    for (int follows = 0; follows < 2; ++follows) {
        FollowMonitor fm;
        for (int i = 0; i < 180; ++i) {
            const double t = i / 60.0, m = marioAt(t);
            const bool air = t > 1.0 && t < 1.8;
            fm.Sample(t, m, follows ? m + 2.2 : 7.2, air);
            fm.SampleResidual(follows ? 0.0 : m - 5.0, 0.01, air);
        }
        const auto jumps = fm.TakeJumps();
        CHECK(jumps.size() == 1);
        CHECK(fm.Jumps() == 1);
        if (!jumps.empty()) {
            CHECK_NEAR(jumps[0].marioRise, 2.0, 0.05);
            CHECK_NEAR(jumps[0].cameraRise, follows ? 2.0 : 0.0, 0.05);
        }
        CHECK_NEAR(fm.FollowRatio(), follows ? 1.0 : 0.0, 0.03);
        CHECK(fm.TakeJumps().empty());
        CHECK(fm.AirFrames() > 40);
        CHECK_NEAR(fm.MeanAirVertical(), follows ? 0.0 : 1.33, 0.1);
        CHECK(!fm.Summary().empty());
    }
    // A hop under the threshold isn't counted; two jumps in a row are two.
    FollowMonitor fm;
    double t = 0;
    auto run = [&](double rise, double secs) {
        for (int i = 0; i < int(secs * 60); ++i, t += 1.0 / 60) {
            const double u = double(i) / (secs * 60);
            fm.Sample(t, 5.0 + 4 * rise * u * (1 - u), 7.2, true);
        }
        fm.Sample(t, 5.0, 7.2, false);
        t += 1.0 / 60;
    };
    run(0.3, 0.5);
    for (int i = 0; i < 60; ++i, t += 1.0 / 60) fm.Sample(t, 5.0, 7.2, false);
    CHECK(fm.Jumps() == 0);
    run(1.5, 0.7);
    run(1.5, 0.7); // airborne again before the camera's settle time ran out
    for (int i = 0; i < 60; ++i, t += 1.0 / 60) fm.Sample(t, 5.0, 7.2, false);
    CHECK(fm.Jumps() == 2);
    CHECK(fm.TakeJumps().size() == 2);
    fm.Reset();
    CHECK(fm.Jumps() == 0 && fm.Summary().empty());
}

static void TestSettings() {
    std::printf("live settings\n");
    Ini ini;
    CHECK(ini.LoadFile("package/resources/sm2mario/sm2mario.ini"));
    const ModConfig c = LoadModConfig(ini);
    LiveSettings s = LiveFromConfig(c);
    CHECK_NEAR(s.volume, 0.8, 1e-6);
    CHECK_NEAR(s.attackStrength, 1.0, 1e-6);
    CHECK(!s.infiniteHealth && !s.wingCap && !s.metalCap && !s.vanishCap && !s.moonJump && !s.bljAnywhere);
    CHECK(s.followHeight && s.photoPoses);
    CHECK_NEAR(s.shine, c.gloss, 1e-6);

    // Every setting appears once, with a label and a description.
    const auto& items = SettingItems();
    int seen[kSetCount] = {};
    for (const SettingItem& it : items) {
        if (it.kind == SettingItem::Header) {
            CHECK(it.id == -1 && std::strlen(it.label) > 0);
            continue;
        }
        CHECK(it.id >= 0 && it.id < kSetCount);
        if (it.id >= 0 && it.id < kSetCount) ++seen[it.id];
        CHECK(std::strlen(it.label) > 0 && std::strlen(it.desc) > 0);
        if (it.kind == SettingItem::Slider) CHECK(it.max > it.min && it.displayMax > it.displayMin);
        if (it.kind == SettingItem::Choice) CHECK(it.choices.size() >= 2);
    }
    for (int i = 0; i < kSetCount; ++i) CHECK(seen[i] == 1);
    CHECK(items.front().kind == SettingItem::Header);

    // Values snap to what the menus can show, and stay in range.
    LiveSettings v;
    SetSetting(v, kSetVolume, 0.83f);
    CHECK_NEAR(v.volume, 0.85, 1e-5);
    SetSetting(v, kSetVolume, 7.0f);
    CHECK_NEAR(v.volume, 1.0, 1e-6);
    SetSetting(v, kSetVolume, std::nanf(""));
    CHECK_NEAR(v.volume, 1.0, 1e-6);
    SetSetting(v, kSetAttack, 1.1f);
    CHECK_NEAR(v.attackStrength, 1.0, 1e-5);
    SetSetting(v, kSetAttack, -3.0f);
    CHECK_NEAR(v.attackStrength, 0.25, 1e-6);
    // The caps are independent: any combination, as in SM64.
    SetSetting(v, kSetWingCap, 1.0f);
    SetSetting(v, kSetMetalCap, 1.0f);
    CHECK(v.wingCap && v.metalCap && !v.vanishCap);
    SetSetting(v, kSetWingCap, 0.0f);
    CHECK(!v.wingCap && v.metalCap);
    SetSetting(v, kSetMoonJump, 1.0f);
    CHECK(v.moonJump);
    CHECK(FormatSetting(v, kSetMoonJump) == "ON" && FormatSetting(v, kSetBlj) == "OFF");
    CHECK(FormatSetting(v, kSetMetalCap) == "ON" && FormatSetting(v, kSetVanishCap) == "OFF");
    CHECK(FormatSetting(v, kSetVolume) == "100%");
    CHECK(FormatSetting(v, kSetAttack) == "0.25x");
    // The camera's distance: 0.5-3x the game's own, in tenths.
    CHECK_NEAR(LiveSettings().cameraDistance, 1.5, 1e-6);
    SetSetting(v, kSetCameraDistance, 1.47f);
    CHECK_NEAR(v.cameraDistance, 1.5, 1e-5);
    CHECK(FormatSetting(v, kSetCameraDistance) == "1.5x");
    CHECK_NEAR(StepSetting(v, kSetCameraDistance, +1), 1.6, 1e-5);
    SetSetting(v, kSetCameraDistance, 9.0f);
    CHECK_NEAR(v.cameraDistance, 3.0, 1e-6);
    SetSetting(v, kSetCameraDistance, 0.1f);
    CHECK_NEAR(v.cameraDistance, 0.5, 1e-6);
    // Steps: toggles flip, choices wrap both ways, sliders clamp.
    v = LiveSettings();
    CHECK(StepSetting(v, kSetInfiniteHealth, +1) == 1.0f);
    CHECK(StepSetting(v, kSetVanishCap, -1) == 1.0f && StepSetting(v, kSetVanishCap, +1) == 1.0f);
    CHECK_NEAR(StepSetting(v, kSetVolume, +1), 0.85, 1e-5);
    v.volume = 1.0f;
    CHECK_NEAR(StepSetting(v, kSetVolume, +1), 1.0, 1e-6);
    CHECK_NEAR(StepSetting(v, kSetAttack, +1), 1.25, 1e-5);

    // The F8 menu: the cursor skips headers and wraps; left/right change the
    // selected value, Enter only switches toggles and choices.
    SettingsMenu m;
    LiveSettings live;
    m.Show(live);
    CHECK(m.Open() && items[size_t(m.Cursor())].kind != SettingItem::Header);
    const int first = m.Cursor();
    m.Up();
    CHECK(m.Cursor() == int(items.size()) - 1); // wrapped to the last item
    m.Down();
    CHECK(m.Cursor() == first);
    CHECK(items[size_t(first)].id == kSetVolume);
    CHECK(m.Right(live) && std::fabs(live.volume - 0.85f) < 1e-5f);
    CHECK(!m.Accept(live)); // a slider
    for (int i = 0; i < 40; ++i) m.Right(live);
    CHECK_NEAR(live.volume, 1.0, 1e-6);
    CHECK(!m.Right(live)); // at the end: nothing changes
    int guard = 0;
    while (items[size_t(m.Cursor())].id != kSetInfiniteHealth && guard++ < 50) m.Down();
    CHECK(items[size_t(m.Cursor())].id == kSetInfiniteHealth);
    CHECK(m.Accept(live) && live.infiniteHealth);
    CHECK(m.Left(live) && !live.infiniteHealth);
    m.Down(); // WING CAP
    CHECK(items[size_t(m.Cursor())].id == kSetWingCap);
    CHECK(m.Accept(live) && live.wingCap);
    m.Down(); // METAL CAP: on top of the wing cap
    CHECK(items[size_t(m.Cursor())].id == kSetMetalCap);
    CHECK(m.Accept(live) && live.metalCap && live.wingCap);
    m.Down();
    CHECK(items[size_t(m.Cursor())].id == kSetVanishCap);
    CHECK(m.Right(live) && live.vanishCap && live.metalCap && live.wingCap);
    CHECK(m.AtOpen() == LiveSettings() && !(live == LiveSettings()));
    m.Hide();
    CHECK(!m.Open());
    m.Show(live);
    CHECK(items[size_t(m.Cursor())].id == kSetVanishCap); // keeps its place

    // 0.3's single cap still loads.
    {
        Ini old;
        old.Set("Cheats", "Cap", "Vanish");
        const ModConfig oc = LoadModConfig(old);
        CHECK(oc.vanishCap && !oc.wingCap && !oc.metalCap);
        old.Set("Cheats", "WingCap", "true"); // the new keys win
        const ModConfig nc = LoadModConfig(old);
        CHECK(nc.wingCap && !nc.vanishCap);
    }

    // Saving: into the user's file, comments kept, missing sections added.
    {
        const std::string path = "build-tests/live_user.ini";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << "[Audio]\r\nVolume = 0.8             ; 0..1 (also in the F8 menu)\r\n[Render]\r\nGloss = 0.15\r\n"
                 "[Cheats]\r\nCap = metal\r\n";
        }
        LiveSettings w;
        w.volume = 0.35f;
        w.attackStrength = 2.5f;
        w.infiniteHealth = true;
        w.wingCap = true;
        w.vanishCap = true;
        w.moonJump = true;
        w.bljAnywhere = true;
        w.shine = 0.4f;
        w.followHeight = false;
        w.photoPoses = false;
        w.cameraDistance = 2.3f;
        std::string err;
        CHECK(SaveLiveSettings(path, w, &err));
        Ini back;
        CHECK(back.LoadFile(path));
        const LiveSettings r = LiveFromConfig(LoadModConfig(back));
        CHECK(r == w);
        std::ifstream f(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(text.find("Volume = 0.35") != std::string::npos);
        CHECK(text.find("; 0..1 (also in the F8 menu)") != std::string::npos);
        CHECK(text.find("[Cheats]") != std::string::npos && text.find("WingCap = true") != std::string::npos);
        CHECK(text.find("MetalCap = false") != std::string::npos && text.find("VanishCap = true") != std::string::npos);
        CHECK(text.find("\nCap =") == std::string::npos); // 0.3's single cap is gone
        CHECK(text.find("\r\n") != std::string::npos);
        CHECK(!SaveLiveSettings("build-tests/no/such/dir/x.ini", w, &err) && !err.empty());
    }
}

// ---------------------------------------------------------------- physics world

// Runs the ray scheduler against a synthetic world for `frames` frames.
static void PumpPhysics(PhysicsWorld& w, phys_test::PhysScene& sc, const DVec3& feet, double& now, int frames,
                        int budget = 24, bool answer = true) {
    std::vector<GameRay> rays;
    for (int f = 0; f < frames; ++f) {
        rays.clear();
        w.Schedule(feet, Vec3(0, 0, 0), now, budget, rays);
        CHECK(int(rays.size()) <= budget);
        if (answer)
            for (const GameRay& r : rays) {
                GameRayResult res;
                sc.Cast(r, res);
                w.Accept(res, now);
            }
        now += 1.0 / 60.0;
    }
}

// Does some wall (|normal.y| small) facing `n` (local, horizontal) cover the
// local point p (within `tol` of its plane)?
static bool WallCovers(const std::vector<SM64Surface>& surfs, const Vec3& p, const Vec3& n, float tol = 6.0f) {
    for (const SM64Surface& f : surfs) {
        const Vec3 fn = Normalize(Sm64SurfaceNormal(f));
        if (std::fabs(fn.y) > 0.05f || Dot(fn, n) < 0.95f) continue;
        const Vec3 a(float(f.vertices[0][0]), float(f.vertices[0][1]), float(f.vertices[0][2]));
        if (std::fabs(Dot(p - a, fn)) > tol) continue;
        // inside the triangle (2D in the wall plane: tangent t, up)
        const Vec3 t(-fn.z, 0, fn.x);
        float u[3], v[3];
        for (int k = 0; k < 3; ++k) {
            const Vec3 q(float(f.vertices[k][0]), float(f.vertices[k][1]), float(f.vertices[k][2]));
            u[k] = Dot(q, t);
            v[k] = q.y;
        }
        const float pu = Dot(p, t), pv = p.y;
        auto cross = [](float ax, float ay, float bx, float by) { return ax * by - ay * bx; };
        const float d1 = cross(u[1] - u[0], v[1] - v[0], pu - u[0], pv - v[0]);
        const float d2 = cross(u[2] - u[1], v[2] - v[1], pu - u[1], pv - v[1]);
        const float d3 = cross(u[0] - u[2], v[0] - v[2], pu - u[2], pv - v[2]);
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        if (!(neg && pos)) return true;
    }
    return false;
}

// The floor surface under local (x, z) closest below y + 10.
static const SM64Surface* FloorUnder(const std::vector<SM64Surface>& surfs, float x, float y, float z) {
    const SM64Surface* best = nullptr;
    float bestY = -1e30f;
    for (const SM64Surface& f : surfs) {
        const Vec3 n = Normalize(Sm64SurfaceNormal(f));
        if (n.y < 0.5f) continue;
        float xs[3], zs[3], ys[3];
        for (int k = 0; k < 3; ++k) {
            xs[k] = float(f.vertices[k][0]);
            ys[k] = float(f.vertices[k][1]);
            zs[k] = float(f.vertices[k][2]);
        }
        auto cross = [](float ax, float ay, float bx, float by) { return ax * by - ay * bx; };
        const float d1 = cross(xs[1] - xs[0], zs[1] - zs[0], x - xs[0], z - zs[0]);
        const float d2 = cross(xs[2] - xs[1], zs[2] - zs[1], x - xs[1], z - zs[1]);
        const float d3 = cross(xs[0] - xs[2], zs[0] - zs[2], x - xs[2], z - zs[2]);
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        if (neg && pos) continue;
        const float fy = (ys[0] + ys[1] + ys[2]) / 3.0f;
        if (fy <= y + 10.0f && fy > bestY) {
            bestY = fy;
            best = &f;
        }
    }
    return best;
}

static void TestPhysicsWorld() {
    std::printf("physics world\n");
    using phys_test::PBox;
    using phys_test::PhysScene;
    // Materials and footsteps.
    CHECK(std::string(PhysicsMaterialName(9)) == "kConcrete");
    CHECK(std::string(PhysicsMaterialName(89)) == "kWoodThin");
    CHECK(std::string(PhysicsMaterialName(-1)) == "none");
    CHECK(SurfaceForMaterial(28).terrain == sm64s::TERRAIN_GRASS);
    CHECK(FootstepSound(SurfaceForMaterial(28)) == sm64s::SOUND_TERRAIN_GRASS);
    CHECK(FootstepSound(SurfaceForMaterial(9)) == sm64s::SOUND_TERRAIN_STONE);
    CHECK(FootstepSound(SurfaceForMaterial(2)) == sm64s::SOUND_TERRAIN_STONE);
    CHECK(FootstepSound(SurfaceForMaterial(87)) == sm64s::SOUND_TERRAIN_SPOOKY);
    CHECK(FootstepSound(SurfaceForMaterial(64)) == sm64s::SOUND_TERRAIN_SAND);
    CHECK(FootstepSound(SurfaceForMaterial(66)) == sm64s::SOUND_TERRAIN_SNOW);
    CHECK(FootstepSound(SurfaceForMaterial(13)) == sm64s::SOUND_TERRAIN_DEFAULT);
    CHECK(SurfaceForMaterial(34).type == sm64s::SURFACE_ICE);
    CHECK(SurfaceForMaterial(38).type == sm64s::SURFACE_BURNING);
    CHECK(SurfaceForMaterial(-1).terrain == sm64s::TERRAIN_STONE && SurfaceForMaterial(500).type == 0);

    for (int sided = 0; sided < 2; ++sided) {
        PhysScene sc;
        sc.twoSided = sided == 1;
        const DVec3 o(500, 10, 500); // Mario's feet (the street is at y = 10)
        sc.groundY = 10;
        sc.boxes.push_back({o + DVec3(3, 0, -6), o + DVec3(13, 30, 6), 4});           // building, facade at x = +3
        sc.boxes.push_back({o + DVec3(-3, 0, -2), o + DVec3(-2.8, 1.2, 2), 9});       // 1.2 m wall at x = -2.8
        sc.boxes.push_back({o + DVec3(-2, 0, -4.5), o + DVec3(0.5, 0.02, -3), 28});   // grass patch
        sc.boxes.push_back({o + DVec3(-1, 0, 3), o + DVec3(1, 0.15, 3.3), 9});        // kerb (15 cm)
        PBox person{o + DVec3(1.2, 0, 1.2), o + DVec3(1.6, 1.8, 1.6), 15};
        person.kind = HitKind::Character;
        sc.boxes.push_back(person);
        sc.waterY = 2; // the river, 8 m below

        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        CHECK(!w.Ready(o));
        PumpPhysics(w, sc, o, now, 1);
        CHECK(w.Ready(o)); // the ground under Mario is asked first
        PumpPhysics(w, sc, o, now, 120);
        const PhysicsWorldStats& st0 = w.Stats();
        CHECK(st0.pending == 0);
        CHECK(st0.walls > 10);
        double water = 0;
        CHECK(w.Water(water) && std::fabs(water - 2.0) < 1e-6);
        CHECK(w.FloorMaterialAt(o) == 2);
        CHECK(w.FloorMaterialAt(o + DVec3(-1, 0, -3.75)) == 28);

        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        std::printf("  %s: %d surfaces, floors %d, ring walls %d, step walls %d, clipped %d | %s\n",
                    sided ? "two-sided" : "one-sided", cs.total, cs.floors, cs.probeWalls, cs.stepWalls, cs.clipped,
                    w.Debug(o).c_str());
        CHECK(cs.total > 200);
        // The street under Mario.
        const SM64Surface* f = FloorUnder(surfs, 0, 0, 0);
        CHECK(f && std::abs(f->vertices[0][1]) <= 1);
        CHECK(f && f->terrain == sm64s::TERRAIN_STONE);
        // The grass patch is grass.
        f = FloorUnder(surfs, -100, 5, -375);
        CHECK(f && f->terrain == sm64s::TERRAIN_GRASS && std::abs(f->vertices[0][1] - 2) <= 1);
        // The facade blocks Mario all along, from the street up past his head.
        for (float z = -350; z <= 350; z += 50)
            for (float y : {30.0f, 90.0f, 150.0f}) {
                const bool ok = WallCovers(surfs, Vec3(300, y, z), Vec3(-1, 0, 0));
                if (!ok) std::printf("    facade gap: sided=%d y=%g z=%g\n", sided, y, z);
                CHECK(ok);
            }
        // ... and well over anything Mario jumps (its top isn't known: the
        // ring finds more of it as he goes up).
        CHECK(WallCovers(surfs, Vec3(300, 700, 0), Vec3(-1, 0, 0)));
        CHECK(!WallCovers(surfs, Vec3(300, 1500, 0), Vec3(-1, 0, 0)));
        // The 1.2 m wall: as tall as it is (Mario can jump onto it).
        CHECK(WallCovers(surfs, Vec3(-280, 60, 0), Vec3(1, 0, 0)));
        CHECK(!WallCovers(surfs, Vec3(-280, 160, 0), Vec3(1, 0, 0)));
        // The kerb is a step, not a wall.
        CHECK(!WallCovers(surfs, Vec3(0, 7, 300), Vec3(0, 0, -1)));
        // People aren't solid.
        CHECK(!WallCovers(surfs, Vec3(120, 60, 140), Vec3(-1, 0, 0)));
        // A safety floor far below.
        CHECK(cs.safetyFloorY < -10000.0f);
    }

    // Rays the game never answers are asked again.
    {
        PhysScene sc;
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        const DVec3 o(0, 0, 0);
        PumpPhysics(w, sc, o, now, 1, 24, false);
        CHECK(w.Stats().pending > 0);
        CHECK(!w.Ready(o));
        PumpPhysics(w, sc, o, now, 120, 24, true); // 2 s later: lost ones re-asked
        CHECK(w.Ready(o));
        CHECK(w.Stats().lost > 0);
    }
    // A car whose body says kAcid (the game's cars do: 0.4 threw Mario off
    // every car roof like off lava) is plain ground; the world's own lava burns.
    {
        PhysScene sc;
        PBox car{DVec3(-1, 0, -1), DVec3(1, 1.4, 1), 1};
        car.kind = HitKind::Movable;
        car.actor = true;
        sc.boxes.push_back(car);
        sc.boxes.push_back({DVec3(3, 0, -1), DVec3(4.5, 0.05, 1), 38}); // a lava pool (kLavaActive)
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        const DVec3 feet(0, 1.4, 0); // on the car's roof
        PumpPhysics(w, sc, feet, now, 120);
        CHECK(w.Stats().actorBurning > 0);
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(DVec3(0, 0, 0));
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 140, 0), cp, surfs, cs);
        const SM64Surface* roof = FloorUnder(surfs, 0, 140, 0);
        CHECK(roof && std::abs(roof->vertices[0][1] - 140) <= 2 && roof->type != sm64s::SURFACE_BURNING);
        const SM64Surface* lava = FloorUnder(surfs, 375, 20, 0);
        CHECK(lava && lava->type == sm64s::SURFACE_BURNING);
        CHECK(BurnsMario(1) && BurnsMario(38) && BurnsMario(39) && !BurnsMario(2) && !BurnsMario(-1));
    }
    // A fence whose faces the game answers from behind (wound the other way)
    // still stops Mario, from whichever side he is on.
    {
        PhysScene sc;
        sc.twoSided = true;
        PBox fence{DVec3(2.1, 0, -3), DVec3(2.15, 1.0, 3), 43};
        fence.flip = true;
        sc.boxes.push_back(fence);
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        const DVec3 o(0, 0, 0);
        PumpPhysics(w, sc, o, now, 60);
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(210, 50, 0), Vec3(-1, 0, 0)));
        CHECK(WallCovers(surfs, Vec3(210, 20, 150), Vec3(-1, 0, 0)));
        CHECK(!WallCovers(surfs, Vec3(215, 50, 0), Vec3(1, 0, 0)));
        // Over it: the side he is on now stops him, the other side is gone.
        const DVec3 o2(4, 0, 0);
        PumpPhysics(w, sc, o2, now, 60);
        map.SetOrigin(o2);
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(-185, 50, 0), Vec3(1, 0, 0)));
        CHECK(!WallCovers(surfs, Vec3(-190, 50, 0), Vec3(-1, 0, 0)));
    }
    // Under a stack of decks (scaffolding): every slab answers twice, the
    // column ray runs out of hits before the street - the rest of the column
    // is asked by follow-up rays, and the street is still there. (Within the
    // 8 m the rays start above him since 0.6: four decks 1.9 m apart, room for
    // Mario between them.)
    for (int sided = 0; sided < 2; ++sided) {
        PhysScene sc;
        sc.twoSided = sided == 1;
        const DVec3 o(0, 0, 0);
        for (int k = 0; k < 4; ++k)
            sc.boxes.push_back({DVec3(-3, 2.0 + 1.9 * k, -3), DVec3(3, 2.1 + 1.9 * k, 3), 51}); // metal decks
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        PumpPhysics(w, sc, o, now, 2);
        PumpPhysics(w, sc, o, now, 120);
        CHECK(w.Ready(o));
        if (sided) CHECK(w.Stats().partials > 0); // (it did run out of hits)
        CHECK(w.FloorMaterialAt(o) == 2); // the street, not a deck
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        const SM64Surface* f = FloorUnder(surfs, 0, 50, 0);
        CHECK(f && std::abs(f->vertices[0][1]) <= 1);
        // ... and on the first deck, the street below it is too.
        const DVec3 deck(0, 2.1, 0);
        PumpPhysics(w, sc, deck, now, 120);
        map.SetOrigin(deck);
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        f = FloorUnder(surfs, 0, 50, 0);
        CHECK(f && std::abs(f->vertices[0][1]) <= 1); // the deck under him
        f = FloorUnder(surfs, 400, 50, 0);             // past the deck's edge (x = 3)
        CHECK(f && std::abs(f->vertices[0][1] + 210) <= 2); // the street, 2.1 m down
    }
    // People crowding the way to a wall: rays that run out of hits on them
    // don't know what's behind, so they don't clear the wall away.
    {
        PhysScene sc;
        sc.twoSided = true;
        const DVec3 o(0, 0, 0);
        sc.boxes.push_back({DVec3(3, 0, -4), DVec3(4, 6, 4), 4}); // a wall at x = 3
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        PumpPhysics(w, sc, o, now, 60);
        for (double x : {0.8, 1.3, 1.8, 2.3}) { // a crowd walks in between
            PBox person{DVec3(x, 0, -4), DVec3(x + 0.3, 1.8, 4), 15};
            person.kind = HitKind::Character;
            sc.boxes.push_back(person);
        }
        PumpPhysics(w, sc, o, now, 30); // half a second
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(300, 60, 0), Vec3(-1, 0, 0)));
    }
    // A car that drives away is forgotten quickly.
    {
        PhysScene sc;
        const DVec3 o(0, 0, 0);
        PBox car{DVec3(1, 0, -1), DVec3(3, 1.5, 1), 45};
        car.kind = HitKind::Movable;
        sc.boxes.push_back(car);
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        PumpPhysics(w, sc, o, now, 90);
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(100, 60, 0), Vec3(-1, 0, 0)));
        const SM64Surface* roof = FloorUnder(surfs, 200, 200, 0);
        CHECK(roof && std::abs(roof->vertices[0][1] - 150) <= 2); // he can stand on its roof
        sc.boxes[0].enabled = false;                              // it drives off
        PumpPhysics(w, sc, o, now, 60);
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(!WallCovers(surfs, Vec3(100, 60, 0), Vec3(-1, 0, 0)));
        const SM64Surface* street = FloorUnder(surfs, 200, 200, 0);
        CHECK(street && std::abs(street->vertices[0][1]) <= 1);
    }
    // In the air, the edge of what the game has answered for isn't a wall: a
    // safety floor lies under all of it and well past the grid (SM64 bonks
    // Mario off any spot without a floor - 0.4's invisible walls when flying
    // and long jumping). On the ground it is only under what is known.
    {
        PhysScene sc;
        const DVec3 o(0, 0, 0);
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        PumpPhysics(w, sc, o, now, 2); // only the columns right under him are known
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.SetAirborne(false);
        w.Build(map, Vec3(0, 300, 0), cp, surfs, cs);
        CHECK(FloorUnder(surfs, 0, 300, 0) != nullptr);        // the street under him
        CHECK(FloorUnder(surfs, 400, 300, 0) == nullptr);      // 4 m away: not asked yet - he waits at the edge
        w.SetAirborne(true);
        w.Build(map, Vec3(0, 300, 0), cp, surfs, cs);
        CHECK(FloorUnder(surfs, 400, 300, 0) != nullptr);      // flying over it: no edge
        CHECK(FloorUnder(surfs, 2500, 300, 1800) != nullptr);  // ... even far past the grid
        CHECK(FloorUnder(surfs, 0, 300, 0) != nullptr);
    }
    // A post whose top no ray finds isn't a wall up into the sky: Mario's
    // ring passes over it when he flies or jumps above it, and it ends there.
    for (int tall = 0; tall < 2; ++tall) {
        PhysScene sc;
        sc.twoSided = true;
        const DVec3 o(0, 0, 0);
        const double h = tall ? 6.0 : 1.0; // a lamp post, or a 1 m bollard ...
        sc.boxes.push_back({DVec3(2.0, 0, -0.1), DVec3(2.2, h, 0.1), 45});
        sc.noTopRays = true; // ... whose top the game never answers
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        PumpPhysics(w, sc, o, now, 60);
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(200, 50, 0), Vec3(-1, 0, 0))); // it stops him
        if (!tall) {
            // The ring's head-height ray went over the bollard: it ends below it.
            CHECK(!WallCovers(surfs, Vec3(200, 200, 0), Vec3(-1, 0, 0)));
            continue;
        }
        CHECK(WallCovers(surfs, Vec3(200, 400, 0), Vec3(-1, 0, 0)));   // its top unknown: taken as tall ...
        CHECK(!WallCovers(surfs, Vec3(200, 1100, 0), Vec3(-1, 0, 0))); // ... but not for ever (0.4: 20 m)
        // Mario jumps (or flies) up beside it: his ring at 7 m passes over it.
        const DVec3 up7(0, 7.0, 0);
        PumpPhysics(w, sc, up7, now, 30);
        map.SetOrigin(up7);
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(!WallCovers(surfs, Vec3(200, 50, 0), Vec3(-1, 0, 0)));     // nothing at his height now (local: 7.5 m)
        CHECK(WallCovers(surfs, Vec3(200, -600, 0), Vec3(-1, 0, 0)));    // the post itself (1 m) still stops him
    }
    // ... but a ring ray going past a post (not over it) doesn't end it: 0.5's
    // first cut took the patch between two ring rays for the post's width.
    {
        PhysScene sc;
        sc.twoSided = true;
        const DVec3 o(0, 0, 0);
        sc.boxes.push_back({DVec3(4.0, 0, -0.1), DVec3(4.2, 6.0, 0.1), 45}); // a lamp post, 4 m away
        sc.noTopRays = true;
        PhysicsWorld w;
        PhysicsWorldParams pp;
        w.Configure(pp, 1);
        double now = 0;
        PumpPhysics(w, sc, o, now, 60);
        // He steps aside (and up a step): two of his rings now pass 0.2 and
        // 0.4 m beside it, none hits it.
        PumpPhysics(w, sc, DVec3(0, 0.6, 0.3), now, 40);
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(o);
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        CollisionParams cp;
        w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(400, 50, 0), Vec3(-1, 0, 0)));  // it stops him ...
        CHECK(WallCovers(surfs, Vec3(400, 300, 0), Vec3(-1, 0, 0))); // ... above where the rays went past it
        // A long low wall, from beside it as well: it ends below them, all
        // along it (its ends too: more of it is seen along its face).
        PhysScene wall;
        wall.twoSided = true;
        wall.boxes.push_back({DVec3(2.0, 0, -3.0), DVec3(2.2, 1.0, 3.0), 45}); // 1 m high, 6 m long
        wall.noTopRays = true;
        PhysicsWorld w2;
        w2.Configure(pp, 1);
        now = 0;
        PumpPhysics(w2, wall, DVec3(0, -0.6, 0), now, 60); // (lower: his rings all hit it)
        w2.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(200, 200, 0), Vec3(-1, 0, 0))); // its top unknown yet
        PumpPhysics(w2, wall, DVec3(0, 0.6, 0.25), now, 40);
        w2.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
        CHECK(WallCovers(surfs, Vec3(200, 50, 0), Vec3(-1, 0, 0)));
        CHECK(!WallCovers(surfs, Vec3(200, 200, 0), Vec3(-1, 0, 0)));
        CHECK(!WallCovers(surfs, Vec3(200, 200, 290), Vec3(-1, 0, 0)));  // at its ends
        CHECK(!WallCovers(surfs, Vec3(200, 200, -290), Vec3(-1, 0, 0)));
    }

    // Columns under a fire escape: each ray uses up its hits on the slats
    // overhead and stops before the street (a follow-up ray continues it).
    {
        PhysicsWorldParams pp;
        WorldMapping map;
        map.Configure('Y', false, 100.0);
        map.SetOrigin(DVec3(0, 0, 0));
        CollisionParams cp;
        std::vector<SM64Surface> surfs;
        CollisionStats cs;
        auto slats = [](double x0) {
            PhysScene sc;
            sc.twoSided = true; // every slat answers twice: 8 hits before the street
            for (double y : {2.0, 4.0, 6.0, 7.0})
                sc.boxes.push_back({DVec3(x0, y, -3), DVec3(3, y + 0.05, 3), 52}); // kMetalFloorHollow
            return sc;
        };
        // Answers this frame's rays except the ones `skip` holds back
        // (answered: true; held back: unanswered, or `empty` - answered with nothing).
        auto pump = [&](PhysicsWorld& w, PhysScene& sc, const DVec3& feet, double& now, int frames, auto skip) {
            std::vector<GameRay> rays;
            for (int f = 0; f < frames; ++f) {
                rays.clear();
                w.Schedule(feet, Vec3(0, 0, 0), now, 24, rays);
                for (const GameRay& r : rays) {
                    GameRayResult res;
                    const int how = skip(r); // 0 answer, 1 never, 2 answered empty
                    if (how == 1) continue;
                    if (how == 2) res.tag = r.tag;
                    else sc.Cast(r, res);
                    w.Accept(res, now);
                }
                now += 1.0 / 60.0;
            }
        };
        const DVec3 feet(0, 0, 0);
        auto isFollowUp = [&](const GameRay& r) {
            return r.type != 17 && std::fabs(r.from.x - r.to.x) < 1e-6 && std::fabs(r.from.z - r.to.z) < 1e-6 &&
                   r.from.y - r.to.y > 3.5 && r.from.y < feet.y + pp.castAbove - 0.01;
        };
        auto all = [](const GameRay&) { return 0; };
        {
            PhysScene sc = slats(-3);
            PhysicsWorld w;
            w.Configure(pp, 1);
            double now = 0;
            pump(w, sc, feet, now, 120, all);
            CHECK(w.Ready(feet));
            CHECK(w.Stats().partials > 0);
            w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
            const SM64Surface* f = FloorUnder(surfs, 0, 10, 0);
            CHECK(f && std::abs(f->vertices[0][1]) <= 1); // the street, under the slats
            // Refreshed, the follow-ups never answered: the street stays (0.5
            // dropped it until they did - a hole under Mario).
            pump(w, sc, feet, now, 240, [&](const GameRay& r) { return isFollowUp(r) ? 1 : 0; });
            CHECK(w.Ready(feet));
            w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
            f = FloorUnder(surfs, 0, 10, 0);
            CHECK(f && std::abs(f->vertices[0][1]) <= 1);
            CHECK(w.Stats().keptLayers > 0);
            CHECK(w.DescribeColumn(feet, now).find("(kept)") != std::string::npos);
            // ... and once they answer, the street is the follow-up's again.
            pump(w, sc, feet, now, 240, all);
            bool fresh = false;
            for (int i = 0; i < 30 && !fresh; ++i) {
                fresh = w.DescribeColumn(feet, now).find("(kept)") == std::string::npos;
                if (!fresh) pump(w, sc, feet, now, 1, all);
            }
            CHECK(fresh);
        }
        // A column answered once with nothing (a ray through a seam): its
        // floor is kept that once, gone the second time.
        {
            PhysScene sc; // the open street
            PhysicsWorld w;
            w.Configure(pp, 1);
            double now = 0;
            pump(w, sc, feet, now, 120, all);
            auto mine = [&](const GameRay& r) {
                return r.type != 17 && std::fabs(r.from.x) < 1e-6 && std::fabs(r.from.z) < 1e-6 &&
                       r.from.y - r.to.y > 3.5;
            };
            int misses = 0;
            for (int i = 0; i < 400 && misses < 1; ++i)
                pump(w, sc, feet, now, 1, [&](const GameRay& r) { return mine(r) ? (++misses, 2) : 0; });
            CHECK(misses == 1);
            w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
            const SM64Surface* f = FloorUnder(surfs, 0, 10, 0);
            CHECK(f && std::abs(f->vertices[0][1]) <= 1);
            for (int i = 0; i < 400 && misses < 2; ++i)
                pump(w, sc, feet, now, 1, [&](const GameRay& r) { return mine(r) ? (++misses, 2) : 0; });
            CHECK(misses == 2);
            CHECK(w.DescribeColumn(feet, now).find("nothing") != std::string::npos);
        }
        // Slats over half of the area, their follow-ups not answered yet: the
        // street under them isn't known - but the lowest slat found so far is
        // no step up from the street beside it (0.5: a 3 m invisible wall
        // along the edge of a fire escape, an awning, a tree).
        {
            PhysScene sc = slats(0.25);
            PhysicsWorld w;
            w.Configure(pp, 1);
            double now = 0;
            pump(w, sc, DVec3(-1, 0, 0), now, 120, [&](const GameRay& r) { return isFollowUp(r) ? 1 : 0; });
            map.SetOrigin(DVec3(-1, 0, 0));
            w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
            for (float z : {-200.0f, 0.0f, 200.0f})
                for (float y : {50.0f, 150.0f}) CHECK(!WallCovers(surfs, Vec3(125, y, z), Vec3(-1, 0, 0), 30.0f));
            // (Answered, the street goes on under them: no wall either.)
            pump(w, sc, DVec3(-1, 0, 0), now, 120, all);
            w.Build(map, Vec3(0, 0, 0), cp, surfs, cs);
            for (float z : {-200.0f, 0.0f, 200.0f}) CHECK(!WallCovers(surfs, Vec3(125, 50, z), Vec3(-1, 0, 0), 30.0f));
            const SM64Surface* f = FloorUnder(surfs, 200, 10, 0);
            CHECK(f && std::abs(f->vertices[0][1]) <= 1);
            map.SetOrigin(DVec3(0, 0, 0));
        }
    }
}

// The game's camera following its target (Mario + the lead + its resting
// height) as the game does it: frame n's camera comes from the target the mod
// set at the Present before (it is rendered, then seen by the mod at the next
// Present), through the camera's own smoothing (time constant; 0: none) and
// `extra` more frames of delay. Mario is drawn where he was at that Present.
struct CamModel {
    double tau;
    int extra;
    double fps;
};
struct JumpFollow {
    double rise = 0;     // the camera's rise in the last jump (Mario: 2.4 m)
    double worst = 0;    // ... its worst distance from where it should be, during and after it
    double lowest = 0;   // ... its lowest after landing (rest = 0)
    double settled = 0;  // distance from rest at the end
    double lag = 0;      // the lag the lead learnt
    float lastLead = 0;
};
static JumpFollow SimulateJumps(const CamModel& cm, bool lead, int jumps) {
    CameraLead cl;
    CameraLead::Params p;
    if (!lead) p.gain = 0;
    const double rest = 2.0, dt = 1.0 / cm.fps;
    double cam = rest;
    std::vector<double> targets(size_t(cm.extra) + 1, 0.0), drawn(1, 0.0);
    double prevM = 0;
    JumpFollow r;
    r.lowest = 1e9;
    const double last = 2.0 * (jumps - 1) + 1.0; // the last jump's start
    const int n = int((2.0 * jumps + 2.0) / dt);
    float l = 0;
    for (int i = 0; i < n; ++i) {
        const double t = i * dt;
        const int k = int(t / 2.0); // jump k: in the air from 2k+1 s to 2k+2 s
        const double u = t - (2.0 * k + 1.0);
        const bool air = k < jumps && u >= 0 && u < 1.0;
        const double m = air ? 4.0 * 2.4 * u * (1.0 - u) : 0.0;
        const double v = air ? 4.0 * 2.4 * (1.0 - 2.0 * u) : 0.0;
        // The game renders a frame with the target set at the last Present
        // (`extra` frames older still), and the mod sees that camera now.
        const double target = targets.front(), with = drawn.front();
        if (cm.tau > 0) cam += (target + rest - cam) * (1.0 - std::exp(-dt / cm.tau));
        else cam = target + rest;
        const double ul = t - last;
        if (ul >= 0 && ul < 1.6) {
            r.rise = std::max(r.rise, cam - rest);
            r.worst = std::max(r.worst, std::fabs(cam - (with + rest))); // vs Mario in that frame
            if (ul >= 1.0) r.lowest = std::min(r.lowest, cam - rest);
        }
        l = cl.Update(t, with, v, cam, air, p);
        targets.erase(targets.begin());
        targets.push_back(m + l);
        drawn.erase(drawn.begin());
        drawn.push_back(m);
        prevM = m;
    }
    (void)prevM;
    r.settled = std::fabs(cam - rest);
    r.lag = cl.Lag();
    r.lastLead = l;
    return r;
}


// The seqlock the hooks and the Present thread share values through: real
// threads, a reader never sees half of one write and half of another.
static void TestSeqWords() {
    std::printf("seqlock\n");
    SeqWords<21> sw;
    uint32_t v0[21];
    uint32_t seq0 = 1;
    CHECK(sw.Read(v0, &seq0) && seq0 == 0 && v0[0] == 0); // never written
    std::atomic<int> writersDone{0};
    std::atomic<uint64_t> reads{0}, torn{0}, backwards{0}, dropped{0};
    constexpr uint32_t kWrites = 150000;
    auto writer = [&](uint32_t tag) {
        uint32_t v[21];
        for (uint32_t k = 1; k <= kWrites; ++k) {
            v[0] = tag;
            for (int i = 1; i < 21; ++i) v[i] = k * 2u + tag;
            if (!sw.Write(v)) ++dropped;
        }
        ++writersDone;
    };
    std::thread reader([&] {
        uint32_t v[21], last[3] = {0, 0, 0};
        while (writersDone.load() < 2) {
            uint32_t sq = 0;
            if (!sw.Read(v, &sq) || sq == 0) continue; // (busy, or nothing written yet)
            ++reads;
            const uint32_t tag = v[0];
            bool ok = tag == 1 || tag == 2;
            for (int i = 2; i < 21 && ok; ++i) ok = v[i] == v[1];
            if (!ok || (v[1] & 1u) != (tag & 1u)) {
                ++torn;
                continue;
            }
            if (v[1] < last[tag]) ++backwards; // each writer's own values only grow
            last[tag] = v[1];
        }
    });
    std::thread w1(writer, 1u), w2(writer, 2u);
    w1.join();
    w2.join();
    reader.join();
    std::printf("  2 writers x %u writes, %llu consistent reads, %llu torn, %llu out of order, %llu writes dropped\n",
                kWrites, static_cast<unsigned long long>(reads.load()), static_cast<unsigned long long>(torn.load()),
                static_cast<unsigned long long>(backwards.load()), static_cast<unsigned long long>(dropped.load()));
    CHECK(reads.load() > 100);
    CHECK(torn.load() == 0);
    CHECK(backwards.load() == 0);
    uint32_t v[21], seq = 0;
    CHECK(sw.Read(v, &seq) && seq > 0 && (seq & 1u) == 0);
    CHECK(v[0] == 1 || v[0] == 2);
}

static void TestCameraLead() {
    std::printf("camera lead\n");
    // A heavily smoothed camera (like the game's for Spider-Man), at 60 fps.
    const CamModel heavy{0.5, 1, 60.0};
    const JumpFollow plain = SimulateJumps(heavy, false, 4), led = SimulateJumps(heavy, true, 4);
    std::printf("  smoothed camera: a 2.40 m jump, the camera rises %.2f m without the lead, %.2f m with it (lag "
                "learnt %.2f s; worst %.2f / %.2f m off, lowest after landing %.2f)\n",
                plain.rise, led.rise, led.lag, plain.worst, led.worst, led.lowest);
    CHECK(plain.rise < 1.9);              // the game's own smoothing trails the jump
    CHECK(led.rise > 2.2);                // the lead keeps up
    CHECK(led.rise < 2.4 * 1.1);          // without swinging past
    CHECK(led.lowest > -0.3);             // nor far below after landing
    CHECK(led.worst < plain.worst * 0.5); // much closer to Mario all through the jump
    CHECK(led.settled < 0.05 && plain.settled < 0.05 && std::fabs(led.lastLead) < 0.01f);
    // A lighter camera.
    const JumpFollow light = SimulateJumps(CamModel{0.2, 0, 60.0}, true, 4);
    std::printf("  lighter camera: rises %.2f m (lag learnt %.2f s, worst %.2f m off)\n", light.rise, light.lag,
                light.worst);
    CHECK(light.rise > 2.25 && light.rise < 2.4 * 1.1 && light.worst < 0.3);
    // A camera that follows its target exactly (the end-to-end test's, at 10
    // frames a second): nothing to make up - the lead learns that and stays
    // out of the way.
    const JumpFollow exact = SimulateJumps(CamModel{0.0, 0, 10.0}, true, 6);
    std::printf("  an exact camera at 10 fps: worst %.2f m off with the lead (lag learnt %.3f s)\n", exact.worst,
                exact.lag);
    CHECK(exact.worst < 0.1 && exact.lag < 0.02);
    // ... or one a frame later than that: a frame's worth of lag, made up.
    const JumpFollow late0 = SimulateJumps(CamModel{0.0, 1, 10.0}, false, 6),
                     late1 = SimulateJumps(CamModel{0.0, 1, 10.0}, true, 6);
    std::printf("  a frame late at 10 fps: worst %.2f m off without the lead, %.2f m with it (lag learnt %.2f s)\n",
                late0.worst, late1.worst, late1.lag);
    CHECK(late1.worst < late0.worst && late1.lag > 0.05 && late1.lag < 0.16 && late1.settled < 0.05);
    // Standing still, tilting the camera: no lead, the resting height follows.
    CameraLead cl;
    CameraLead::Params p;
    for (int i = 0; i < 120; ++i) CHECK(cl.Update(i / 60.0, 5.0, 0.0, 7.0 + i * 0.01, false, p) == 0.0f);
    CHECK(std::fabs(cl.RestHeight() - 3.0) < 0.5);
}

static void TestCameraOverride() {
    std::printf("camera override\n");
    // The game's camera transform: rows right / up / forward, yawed.
    const double th = 0.6;
    Vec3 rows[3] = {Vec3(float(std::cos(th)), 0, float(-std::sin(th))), Vec3(0, 1, 0),
                    Vec3(float(std::sin(th)), 0, float(std::cos(th)))};
    auto at = [&](const DVec3& m, double r, double u, double f) {
        DVec3 c = m;
        for (int k = 0; k < 3; ++k) {
            const double w = k == 0 ? r : (k == 1 ? u : f);
            c = c + DVec3(rows[k].x, rows[k].y, rows[k].z) * w;
        }
        return c;
    };
    const DVec3 m0(100, 10, 200);
    const DVec3 c0 = at(m0, 0.3, 1.5, -4.0); // behind, above, a little to the side
    // Finding it: written where the view is, aimed like it.
    float sign = 0;
    const Vec3 fwd = rows[2];
    CHECK(CameraCandidateRow(c0, rows, c0 + DVec3(0.2, 0, 0.1), fwd, 1.5f, 0.9f, sign) == 2 && sign > 0);
    CHECK(CameraCandidateRow(c0, rows, c0, fwd * -1.0f, 1.5f, 0.9f, sign) == 2 && sign < 0);
    CHECK(CameraCandidateRow(c0 + DVec3(3, 0, 0), rows, c0, fwd, 1.5f, 0.9f, sign) == -1); // elsewhere
    CHECK(CameraCandidateRow(c0, rows, c0, Normalize(Vec3(1, 1, 1)), 1.5f, 0.9f, sign) == -1); // aimed elsewhere

    CameraOverride co;
    CameraOverride::Params p;
    co.SetCamera(2, 1.0f);
    CameraOverride::GameSample g;
    g.valid = true;
    for (int k = 0; k < 3; ++k) g.rows[k] = rows[k];
    double t = 0;
    const double dt = 1.0 / 60.0;
    CameraOverride::Plan plan;
    // Writes not aimed like the view rendered (its memory something else's):
    // nothing learnt from them.
    {
        CameraOverride other;
        other.SetCamera(2, 1.0f);
        CameraOverride::GameSample h = g;
        h.haveView = true;
        h.viewForward = Normalize(Vec3(1, 0, 0) - rows[2]); // 90+ degrees off
        for (int i = 0; i < 90; ++i) {
            h.pos = c0;
            h.seq = uint32_t(i + 1);
            other.Update(i * dt, m0, false, 0.0f, 1, h, true, p);
        }
        CHECK(!other.Learnt());
        h.viewForward = Normalize(rows[2] + Vec3(0.05f, 0, 0)); // aimed like the view (a frame behind)
        for (int i = 90; i < 180; ++i) {
            h.pos = c0;
            h.seq = uint32_t(i + 1);
            other.Update(i * dt, m0, false, 0.0f, 1, h, true, p);
        }
        CHECK(other.Learnt());
    }
    // Writes of a camera the game only just switched to rendering from (one
    // of its others: GameSample::learn false) teach nothing either.
    {
        CameraOverride other;
        other.SetCamera(2, 1.0f);
        CameraOverride::GameSample h = g;
        h.learn = false;
        for (int i = 0; i < 90; ++i) {
            h.pos = c0 + DVec3(0, 0.5, 0);
            h.seq = uint32_t(i + 1);
            other.Update(i * dt, m0, false, 0.0f, 1, h, true, p);
        }
        CHECK(!other.Learnt());
        h.learn = true;
        for (int i = 90; i < 180; ++i) {
            h.pos = c0;
            h.seq = uint32_t(i + 1);
            other.Update(i * dt, m0, false, 0.0f, 1, h, true, p);
        }
        CHECK(other.Learnt());
        CHECK_NEAR(other.Offset().y, 1.5, 0.01);
    }
    // Mario stands; the game's camera rests behind him: its framing is learnt.
    for (int i = 0; i < 90; ++i, t += dt) {
        g.pos = c0;
        g.seq = uint32_t(i + 1);
        plan = co.Update(t, m0, false, 0.0f, 1, g, true, p);
    }
    CHECK(co.Learnt());
    const Vec3 o = co.Offset();
    CHECK_NEAR(o.x, 0.3, 0.01);
    CHECK_NEAR(o.y, 1.5, 0.01);
    CHECK_NEAR(o.z, -4.0, 0.01);
    CHECK(plan.active && plan.blend > 0.99f);
    CHECK(Length(CameraOverride::Place(plan, rows, c0) - c0) < 0.01); // at rest: where the game had it

    // Mario runs off at 8 m/s; the game's camera trails him (a 0.5 s spring).
    DVec3 cam = c0, m = m0;
    double worst = 0, gameWorst = 0;
    for (int i = 0; i < 90; ++i, t += dt) {
        m = m + DVec3(8.0 * dt, 0, 0);
        const DVec3 want = at(m, 0.3, 1.5, -4.0);
        cam = cam + (want - cam) * (1.0 - std::exp(-dt / 0.5));
        g.pos = cam;
        g.seq += 1;
        plan = co.Update(t, m, false, 8.0f, 1, g, true, p);
        const DVec3 placed = CameraOverride::Place(plan, rows, cam);
        worst = std::max(worst, Length(placed - want));
        gameWorst = std::max(gameWorst, Length(cam - want));
    }
    std::printf("  running at 8 m/s: the game's camera trails by up to %.2f m, the placed one by %.3f m\n", gameWorst,
                worst);
    CHECK(gameWorst > 1.0);
    CHECK(worst < 0.05);

    // He stops; the game's camera, still catching up, is closer along the view
    // than learnt (or a wall pulled it in): the placed one comes closer too,
    // never further than learnt.
    {
        const DVec3 closer = at(m, 0.3, 1.5, -2.0);
        const DVec3 placed = CameraOverride::Place(plan, rows, closer);
        CHECK(Length(placed - closer) < 0.01);
        const DVec3 further = at(m, 0.3, 1.5, -7.0);
        CHECK(Length(CameraOverride::Place(plan, rows, further) - at(m, 0.3, 1.5, -4.0)) < 0.01);
    }

    // A 2.4 m jump: the placed camera goes up with him (eased a little), the
    // game's lagging one wouldn't.
    {
        double top = 0, gameTop = 0;
        const DVec3 base = m;
        DVec3 gc = at(base, 0.3, 1.5, -4.0);
        for (int i = 0; i < 60; ++i, t += dt) {
            const double u = i / 60.0;
            DVec3 mj = base;
            mj.y += 4.0 * 2.4 * u * (1.0 - u);
            const DVec3 want = at(mj, 0.3, 1.5, -4.0);
            gc = gc + (want - gc) * (1.0 - std::exp(-dt / 0.5));
            g.pos = gc;
            g.seq += 1;
            plan = co.Update(t, mj, true, 0.0f, 1, g, true, p);
            const DVec3 placed = CameraOverride::Place(plan, rows, gc);
            top = std::max(top, placed.y - (base.y + 1.5));
            gameTop = std::max(gameTop, gc.y - (base.y + 1.5));
        }
        std::printf("  a 2.40 m jump: the placed camera rises %.2f m, the game's %.2f m\n", top, gameTop);
        CHECK(top > 2.0 && top < 2.45);
        CHECK(gameTop < 1.6);
    }

    // The game's camera somewhere else (a cinematic far away): eased out.
    for (int i = 0; i < 60; ++i, t += dt) {
        g.pos = m + DVec3(60, 20, 0);
        g.seq += 1;
        plan = co.Update(t, m, false, 0.0f, 1, g, true, p);
    }
    CHECK(!plan.active || plan.blend == 0.0f);
    CHECK(Length(CameraOverride::Place(plan, rows, g.pos) - g.pos) < 1e-6);
    // ... back behind him: eased in again (still learnt).
    for (int i = 0; i < 10; ++i, t += dt) {
        g.pos = at(m, 0.3, 1.5, -4.0);
        g.seq += 1;
        plan = co.Update(t, m, false, 0.0f, 1, g, true, p);
    }
    CHECK(plan.active && plan.blend > 0.2f && plan.blend < 1.0f);
    // Paused / not allowed: eased out.
    for (int i = 0; i < 60; ++i, t += dt) plan = co.Update(t, m, false, 0.0f, 1, g, false, p);
    CHECK(plan.blend == 0.0f);
    // Lost: nothing placed - the framing is kept for the next one found
    // (0.5 learnt it afresh each time: the game's lagging camera until Mario
    // stood still again).
    co.LoseCamera();
    CHECK(!co.HasCamera() && co.Learnt());
    plan = co.Update(t, m, false, 0.0f, 1, g, true, p);
    CHECK(!plan.active);
    // ... another camera, its rows laid out differently (along the view
    // backwards first, then across - pointing left -, then up; a rotation like
    // the first): the same spot behind Mario, on the same side, at once - he
    // doesn't have to stand still first.
    {
        Vec3 rows2[3] = {rows[2] * -1.0f, rows[0] * -1.0f, rows[1]};
        CameraOverride::GameSample g2 = g;
        for (int k = 0; k < 3; ++k) g2.rows[k] = rows2[k];
        co.SetCamera(0, -1.0f);
        DVec3 mr = m;
        for (int i = 0; i < 40; ++i, t += dt) {
            mr = mr + DVec3(8.0 * dt, 0, 0);
            g2.pos = mr + DVec3(-14.0, 2.0, 1.0); // the game's own camera, far behind
            g2.seq += 1;
            plan = co.Update(t, mr, false, 8.0f, 1, g2, true, p);
        }
        CHECK(plan.active && plan.blend > 0.99f);
        CHECK(Length(CameraOverride::Place(plan, rows2, g2.pos) - at(mr, 0.3, 1.5, -4.0)) < 0.05);
        co.LoseCamera();
    }
    // A camera never seen at rest: the usual framing (4.7 m back, 1.35 m up)
    // while Mario runs; a rest where the game's camera is pulled in to 1.5 m
    // (a wall behind it) isn't learnt from.
    {
        CameraOverride fresh;
        fresh.SetCamera(2, 1.0f);
        CameraOverride::GameSample gf = g;
        DVec3 mf = m0;
        CameraOverride::Plan pf;
        for (int i = 0; i < 40; ++i, t += dt) {
            mf = mf + DVec3(6.0 * dt, 0, 0);
            gf.pos = mf + DVec3(-14.0, 1.0, 0.0);
            gf.seq += 1;
            pf = fresh.Update(t, mf, false, 6.0f, 1, gf, true, p);
        }
        CHECK(pf.active && pf.blend > 0.99f && !fresh.Learnt());
        CHECK(Length(CameraOverride::Place(pf, rows, gf.pos) - at(mf, 0.0, p.defaultUp, -p.defaultBack)) < 0.05);
        for (int i = 0; i < 90; ++i, t += dt) {
            gf.pos = at(mf, 0.0, 1.2, -1.5);
            gf.seq += 1;
            pf = fresh.Update(t, mf, false, 0.0f, 1, gf, true, p);
        }
        CHECK(!fresh.Learnt());
        CHECK_NEAR(fresh.Framing().z, p.defaultBack, 1e-4);
    }

    // Walls, from the game's physics: rays from Mario's chest to the camera's spot.
    {
        CameraOverride cw;
        cw.SetCamera(2, 1.0f);
        double tw = 100.0;
        CameraOverride::Plan pw;
        for (int i = 0; i < 90; ++i, tw += dt) {
            g.pos = c0;
            g.seq += 1;
            pw = cw.Update(tw, m0, false, 0.0f, 1, g, true, p);
        }
        CHECK(pw.active && !pw.collide && pw.reach == 1.0f);
        DVec3 from[CameraOverride::kCollisionRays], to[CameraOverride::kCollisionRays];
        CHECK(CameraOverride::CollisionRays(pw, rows, p, from, to));
        const DVec3 look = m0 + DVec3(0, p.lookHeight, 0);
        const double span = Length(c0 - look); // chest to the camera's spot
        CHECK(Length(from[0] - look) < 1e-6 && Length(from[1] - look) < 1e-6);
        CHECK_NEAR(Length(to[0] - look), span + p.collisionMargin, 1e-4);
        CHECK(Length(c0 + (c0 - look) * (p.collisionMargin / span) - to[0]) < 1e-4); // straight through the spot
        // the side rays: level, across the view, either side of the spot
        const DVec3 across = to[1] - to[2];
        CHECK(std::fabs(across.y) < 1e-6 && std::fabs(Dot(across, c0 - look)) < 1e-6);
        CHECK(Length(across) > 2.0 * p.collisionSide * 0.95);
        double len[3];
        for (int i = 0; i < 3; ++i) len[i] = Length(to[i] - from[i]);
        auto answer = [&](double a, double b, double c) { // hit distances, < 0: none
            const double h[3] = {a, b, c};
            for (int i = 0; i < 3; ++i) cw.AcceptCollision(tw, i, float(h[i]), float(len[i]), p);
        };
        auto step = [&](int frames, const DVec3& gamePos) {
            for (int i = 0; i < frames; ++i, tw += dt) {
                g.pos = gamePos;
                g.seq += 1;
                pw = cw.Update(tw, m0, false, 0.0f, 1, g, true, p);
            }
        };
        // a wall 2 m back from his chest, across all three rays: in front of it at once
        answer(2.0, 2.05, 2.05);
        step(1, c0);
        CHECK(pw.collide);
        CHECK_NEAR(pw.reach, (2.0 - p.collisionMargin) / span, 0.02);
        const DVec3 placed = CameraOverride::Place(pw, rows, c0);
        CHECK_NEAR(Length(placed - look), 2.0 - p.collisionMargin, 0.05);
        CHECK(Length(Normalize((placed - look).ToFloat()) - Normalize((c0 - look).ToFloat())) < 1e-3); // same line
        // only the middle ray hits (a post): the camera sees past it - back out, gently
        answer(1.0, -1, -1);
        step(1, c0);
        const float after1 = pw.reach;
        CHECK(after1 > (2.0 - p.collisionMargin) / span - 0.01 && after1 < 0.9f);
        for (int i = 0; i < 60; ++i) {
            answer(1.0, -1, -1);
            step(1, c0);
        }
        CHECK(pw.reach > 0.93f); // (a second: most of the way)
        // a corner: two rays hit - the further of the two decides
        answer(1.5, 2.5, -1);
        step(1, c0);
        CHECK_NEAR(pw.reach, (2.5 - p.collisionMargin) / (len[1] - p.collisionMargin), 0.01);
        // right behind him: never closer than collisionMinDistance to his chest
        answer(0.05, 0.05, 0.05);
        step(1, c0);
        CHECK_NEAR(Length(CameraOverride::Place(pw, rows, c0) - look), p.collisionMinDistance, 0.02);
        // all clear again
        for (int i = 0; i < 120; ++i) {
            answer(-1, -1, -1);
            step(1, c0);
        }
        CHECK(pw.reach > 0.99f);
        // with the rays answering, the game's own camera coming closer (its lag
        // as Mario walks towards it) doesn't pull the camera in
        answer(-1, -1, -1);
        step(1, c0);
        const DVec3 lagging = at(m0, 0.3, 1.5, -2.0);
        CHECK(Length(CameraOverride::Place(pw, rows, lagging) - c0) < 0.03);
        // ... once they stop answering, the game's camera distance limits it again
        step(int(p.collisionFresh / dt) + 5, c0);
        CHECK(!pw.collide && pw.reach == 1.0f);
        CHECK(Length(CameraOverride::Place(pw, rows, lagging) - lagging) < 0.01);
        // nothing to cast without a plan
        CameraOverride::Plan none;
        CHECK(!CameraOverride::CollisionRays(none, rows, p, from, to));
    }
}

// [Camera] Distance, and the pivot carried on to when the game writes its
// camera (on its own thread, a moment after the mod's frame - or just before
// it, a frame on: 0.6 used the plan as it was, and the camera jumped a frame's
// worth of Mario's motion back and forth when he flew).
static void TestCameraDistanceAndTiming() {
    std::printf("camera distance and timing\n");
    const Vec3 rows[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)}; // right, up, forward
    CameraOverride::Params p;
    CameraOverride::GameSample g;
    g.valid = true;
    for (int k = 0; k < 3; ++k) g.rows[k] = rows[k];
    const DVec3 m(10, 0, 10);
    auto settle = [&](CameraOverride& co, float distance) {
        p.distance = distance;
        co.SetCamera(2, 1.0f);
        CameraOverride::Plan plan;
        double t = 1.0;
        for (int i = 0; i < 60; ++i, t += 1.0 / 60.0) {
            g.pos = m + DVec3(0, 1.35, -4.7); // the game's own camera: the usual framing, at rest
            g.seq += 1;
            plan = co.Update(t, m, false, 0.0f, 1, g, true, p);
            // (The wall rays answered: nothing in the way. Without them the
            // camera goes no further back than the game's own.)
            for (int k = 0; k < CameraOverride::kCollisionRays; ++k) co.AcceptCollision(t, k, -1.0f, 12.0f, p);
        }
        return plan;
    };
    {
        CameraOverride co;
        const CameraOverride::Plan plan = settle(co, 1.0f);
        const DVec3 at1 = CameraOverride::Place(plan, rows, g.pos);
        CHECK(plan.active && Length(at1 - (m + DVec3(0, 1.35, -4.7))) < 0.02);
        CameraOverride co2;
        const CameraOverride::Plan plan2 = settle(co2, 2.0f);
        const DVec3 at2 = CameraOverride::Place(plan2, rows, g.pos);
        // Twice as far, along the same line from Mario: he stays where he is on screen.
        CHECK(Length(at2 - (m + DVec3(0, 2.7, -9.4))) < 0.03);
        CHECK_NEAR(co2.Framing().z, 4.7, 0.02); // (the framing itself is the game's)
        CameraOverride co3;
        p.collisionMinDistance = 0.3f;
        const CameraOverride::Plan plan3 = settle(co3, 0.1f); // clamped to 0.3x
        CHECK(Length(CameraOverride::Place(plan3, rows, g.pos) - (m + DVec3(0, 0.405, -1.41))) < 0.03);
        p.distance = 1.0f;
    }
    {
        // Writes on another thread: the pivot as it was a frame before them.
        CameraOverride co;
        settle(co, 1.0f);
        CameraOverride::Plan plan;
        double t = 3.0;
        DVec3 mm = m;
        for (int i = 0; i < 30; ++i, t += 1.0 / 60.0) {
            mm = mm + DVec3(0, 0, 12.0 / 60.0); // flying away from the camera at 12 m/s
            g.pos = mm + DVec3(0, 1.35, -4.7);
            g.seq += 1;
            plan = co.Update(t, mm, true, 12.0f, 1, g, true, p);
        }
        CHECK(std::fabs(plan.time - (t - 1.0 / 60.0)) < 1e-9 && std::fabs(plan.frameTime - 1.0f / 60.0f) < 1e-4f);
        CHECK(Length(plan.pivot - plan.prevPivot - DVec3(0, 0, 0.2)) < 1e-6); // (a frame's 20 cm)
        // At the mod's frame: the frame before's; 10 ms after it: 12 cm on from there.
        CHECK(Length(CameraOverride::PivotAt(plan, plan.time) - plan.prevPivot) < 1e-9);
        CHECK_NEAR(CameraOverride::PivotAt(plan, plan.time + 0.010).z - plan.prevPivot.z, 0.12, 1e-6);
        // ... never before the frame before, nor past the latest (a write late by a hitch).
        CHECK(Length(CameraOverride::PivotAt(plan, plan.time - 0.010) - plan.prevPivot) < 1e-9);
        CHECK(Length(CameraOverride::PivotAt(plan, plan.time + 0.5) - plan.pivot) < 1e-9);
        // The race 0.6 lost: a write just before the mod's next frame (with
        // this plan) and one just after it (with the next) put the camera in
        // the same place.
        const DVec3 before = CameraOverride::PivotAt(plan, t - 0.0005);
        mm = mm + DVec3(0, 0, 12.0 / 60.0);
        g.pos = mm + DVec3(0, 1.35, -4.7);
        g.seq += 1;
        const CameraOverride::Plan next = co.Update(t, mm, true, 12.0f, 1, g, true, p);
        const DVec3 after = CameraOverride::PivotAt(next, t + 0.0005);
        CHECK(Length(after - before) < 0.02);  // (12 m/s x 1 ms)
        CHECK(Length(next.pivot - plan.pivot) > 0.19); // (the plans themselves: a frame apart)
        // Not across a jump of a few metres in one frame (a respawn).
        t += 1.0 / 60.0;
        mm = mm + DVec3(0, 0, 5.0);
        g.seq += 1;
        const CameraOverride::Plan jump = co.Update(t, mm, false, 0.0f, 1, g, true, p);
        CHECK(Length(CameraOverride::PivotAt(jump, jump.time + 0.004) - jump.pivot) < 1e-9);
        // No frame before (a pause): as it is.
        CameraOverride::Plan old = jump;
        old.frameTime = 0;
        CHECK(Length(CameraOverride::PivotAt(old, old.time + 0.004) - old.pivot) < 1e-12);
        old = jump;
        old.time = 0;
        CHECK(Length(CameraOverride::PivotAt(old, 100.0) - old.pivot) < 1e-12);
        // After a pause (no Update for a second): no frame before.
        t += 1.0;
        g.seq += 1;
        const CameraOverride::Plan resumed = co.Update(t, mm, false, 0.0f, 1, g, true, p);
        CHECK(resumed.frameTime == 0.0f);
    }
    {
        // The game's own camera far behind Mario (he flies faster than its
        // spring): still following - it moves on from its last write. A jump
        // to somewhere far (a cinematic shot) isn't. (0.6: 25 m and no more.)
        CameraOverride co;
        settle(co, 1.0f);
        double t = 5.0;
        DVec3 mm = m;
        DVec3 gp = m + DVec3(0, 1.35, -4.7);
        CameraOverride::Plan plan;
        bool lost = false;
        for (int i = 0; i < 180; ++i, t += 1.0 / 60.0) {
            mm = mm + DVec3(0, 0, 30.0 / 60.0);  // 30 m/s ...
            gp = gp + DVec3(0, 0, 18.0 / 60.0);  // ... the game's camera 18 m/s: 36 m behind after 3 s
            g.pos = gp;
            g.seq += 1;
            plan = co.Update(t, mm, true, 30.0f, 1, g, true, p);
            if (!co.Following() || !(plan.blend > 0.99f)) lost = true;
        }
        CHECK(!lost && Length(gp - mm) > 30.0);
        // A cut far away: no longer following, eases out.
        g.pos = mm + DVec3(80, 20, 0);
        g.seq += 1;
        co.Update(t, mm, true, 30.0f, 1, g, true, p);
        CHECK(!co.Following());
        // ... and coming back near Mario: following again.
        t += 1.0 / 60.0;
        g.pos = mm + DVec3(0, 1.35, -4.7);
        g.seq += 1;
        co.Update(t, mm, true, 30.0f, 1, g, true, p);
        CHECK(co.Following());
        // Far, and never near before: not following.
        CameraOverride fresh;
        fresh.SetCamera(2, 1.0f);
        g.pos = m + DVec3(0, 0, -60);
        g.seq += 1;
        fresh.Update(9.0, m, false, 0.0f, 1, g, true, p);
        CHECK(!fresh.Following());
    }
    {
        // Which writes to the camera's transform are the camera's (the hooks
        // place only those): Mario at z 100, the view rendered from behind him.
        CameraOverride::Plan pl;
        pl.active = true;
        pl.checkView = true;
        pl.forwardRow = 2;
        pl.forwardSign = 1.0f;
        pl.pivot = DVec3(0, 0, 100);
        pl.viewPos = DVec3(0, 1.35, 95.3);
        pl.viewForward = Vec3(0, 0, 1);
        const Vec3 ahead[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
        // Near the view, aimed like it.
        CHECK(CameraOverride::AcceptWrite(pl, DVec3(0, 1.35, 94.0), ahead, true, DVec3(0, 1.35, 93.8)));
        // The game's own camera 50 m behind a flying Mario, looking at him,
        // moving on from its last write: the camera's (0.6 left it to the
        // game, and the view snapped back and forth).
        CHECK(CameraOverride::AcceptWrite(pl, DVec3(0, 2, 50), ahead, true, DVec3(0, 2, 49.6)));
        // ... not if it jumped there, or nothing was written before it.
        CHECK(!CameraOverride::AcceptWrite(pl, DVec3(0, 2, 50), ahead, true, DVec3(0, 2, 30)));
        CHECK(!CameraOverride::AcceptWrite(pl, DVec3(0, 2, 50), ahead, false, DVec3()));
        // ... nor if it doesn't look at him (40 m to the side: 39 degrees off).
        CHECK(!CameraOverride::AcceptWrite(pl, DVec3(40, 2, 50), ahead, true, DVec3(40, 2, 49.6)));
        // Something else's transform (its memory reused): turned away, far.
        const Vec3 aside[3] = {Vec3(0, 0, 1), Vec3(0, 1, 0), Vec3(-1, 0, 0)};
        CHECK(!CameraOverride::AcceptWrite(pl, DVec3(30, 0, 60), aside, true, DVec3(30, 0, 60)));
        // ... a fast turn near the view is still the camera's (0.6's rule).
        CHECK(CameraOverride::AcceptWrite(pl, DVec3(1, 1.35, 95.0), aside, true, DVec3(0, 1.35, 95.2)));
        // Not a rotation.
        const Vec3 scaled[3] = {Vec3(5, 0, 0), Vec3(0, 5, 0), Vec3(0, 0, 5)};
        CHECK(!CameraOverride::AcceptWrite(pl, DVec3(0, 1.35, 94.0), scaled, true, DVec3(0, 1.35, 93.8)));
        // Without the view to compare with: every write.
        pl.checkView = false;
        CHECK(CameraOverride::AcceptWrite(pl, DVec3(1000, 0, 0), aside, false, DVec3()));
    }
}

static void TestStencilCensus() {
    std::printf("stencil census\n");
    StencilCensus c;
    const uint32_t hero = StencilCensus::Key(0x80, 0x80, 3, false);    // replace 0x80: only with Spider-Man
    const uint32_t hair = StencilCensus::Key(0x25, 0x3F, 3, true);     // someone's hair, now and then
    const uint32_t water = StencilCensus::Key(0x40, 0x40, 3, false);   // the river, always
    CHECK(StencilCensus::Ref(hair) == 0x25 && StencilCensus::WriteMask(hair) == 0x3F && StencilCensus::PassOp(hair) == 3 &&
          StencilCensus::GBuffer(hair) && !StencilCensus::MarioPass(hair));
    CHECK(StencilCensus::Ref(StencilCensus::Key(0xFF, 0x80, 3, false)) == 0x80); // (only the bits written)
    CHECK(StencilCensus::MarioPass(StencilCensus::Key(1, 1, 3, true, true)) &&
          !StencilCensus::MarioPass(StencilCensus::Key(1, 1, 3, false, true))); // (G-buffer draws only)
    for (int i = 0; i < 200; ++i) {
        c.Note(hero);
        c.Note(hero); // (twice in a frame: one frame)
        if (i % 2) c.Note(hair);
        c.Note(water);
        c.EndFrame(true);
    }
    CHECK(!c.Enough(150) && c.Switches() == 0); // (no switch yet)
    for (int i = 0; i < 50; ++i) {
        c.Note(hero);
        c.EndFrame(false, true); // a menu: not counted
    }
    for (int i = 0; i < 200; ++i) {
        if (i % 50 == 0) c.Note(hair);
        c.Note(water);
        c.EndFrame(false);
    }
    // One switch: the 150 frames before it against the 150 after.
    CHECK(c.Enough(150) && c.Switches() == 1);
    CHECK(c.WindowFramesWithHero() == 150 && c.WindowFramesWithout() == 150);
    CHECK(c.FramesWithHero() == 200 && c.FramesWithout() == 200);
    const std::vector<StencilCensus::Finding> f = c.HeroMarks(150);
    CHECK(f.size() == 1 && f[0].key == hero);
    CHECK(f.size() == 1 && f[0].withHero > 0.99f && f[0].without < 0.01f);
    CHECK(c.Describe().find("replace 0x80") != std::string::npos);
    CHECK(c.Describe().find("(around M: 100% / 0%)") != std::string::npos);
    const std::vector<StencilCensus::Finding> common = c.CommonMarks(150);
    CHECK(common.size() == 1 && common[0].key == water && common[0].draws == 400);
    {
        // His own mark, and none of the G-buffer's (the river's is a depth-only draw's).
        const StencilCensus::Decision d = c.Decide(150);
        CHECK(d.ready && d.ref == 0x80 && d.mask == 0x80 && d.shared.empty());
        CHECK(d.own.find("0x80 under mask 0x80 (100% of frames with him, 0% without)") != std::string::npos);
    }
    c.Reset();
    CHECK(!c.Enough(1) && c.HeroMarks(0).empty() && c.Switches() == 0);
    CHECK(c.Describe() == "no stencil writes into the main view");

    // What Mario's pixels get: Spider-Man's own marks, and for the other bits
    // what every G-buffer draw writes (0.6's first cut gave Mario only his own
    // mark: in front of the sky the G-buffer's bit was then missing).
    const uint32_t own = StencilCensus::Key(0x80, 0x80, 3, false);  // a pass of his own (ModelStencilWrite)
    const uint32_t wide = StencilCensus::Key(0xC0, 0xC0, 3, false); // overlapping it, less often
    const uint32_t lit = StencilCensus::Key(0x01, 0x01, 3, true);   // every G-buffer draw
    const uint32_t count = StencilCensus::Key(0x00, 0x0C, 7, true); // an increment, everywhere: a count, not a mark
    {
        StencilCensus d;
        d.SetWindow(100);
        CHECK(!d.Decide(100).ready);
        for (int i = 0; i < 120; ++i) {
            d.Note(own);
            if (i % 10) d.Note(wide);
            d.Note(lit);
            d.Note(count);
            d.EndFrame(true);
        }
        CHECK(!d.Decide(100).ready); // (none with Mario yet)
        for (int i = 0; i < 120; ++i) {
            d.Note(lit);
            d.Note(count);
            d.EndFrame(false);
        }
        StencilCensus::Decision x = d.Decide(100);
        CHECK(x.ready && x.ref == 0x81 && x.mask == 0x81);
        CHECK(x.own.find("0x80 under mask 0x80") != std::string::npos && x.own.find("0xc0") == std::string::npos);
        CHECK(x.shared == "0x01 under mask 0x01");
        // Spider-Man again, twice, without that mark: it was something that
        // only happened to be on screen with him the first time (a fountain),
        // and drops out.
        for (int round = 0; round < 2; ++round) {
            for (int i = 0; i < 200; ++i) {
                d.Note(lit);
                d.EndFrame(true);
            }
            for (int i = 0; i < 200; ++i) {
                d.Note(lit);
                d.EndFrame(false);
            }
        }
        CHECK(d.Switches() == 5);
        x = d.Decide(300);
        CHECK(x.ready && x.ref == 0x01 && x.mask == 0x01 && x.own.empty() && x.shared == "0x01 under mask 0x01");
    }
    {
        // The river was on screen for most of the time Spider-Man was (80% of
        // all frames with him) but not where M was pressed: the frames around
        // the switch say it isn't his (0.6's first cut compared all frames).
        StencilCensus d;
        d.SetWindow(100);
        const uint32_t river = StencilCensus::Key(0x40, 0x40, 3, true);
        for (int i = 0; i < 400; ++i) {
            d.Note(own);
            d.Note(river);
            d.EndFrame(true);
        }
        for (int i = 0; i < 100; ++i) {
            d.Note(own);
            d.EndFrame(true);
        }
        for (int i = 0; i < 100; ++i) d.EndFrame(false);
        const std::vector<StencilCensus::Finding> h = d.HeroMarks(100);
        CHECK(h.size() == 1 && h[0].key == own);
        CHECK(d.FramesWithHero() == 500 && d.WindowFramesWithHero() == 100);
    }
    {
        // A switch right back (a quick double press of M): too little either
        // side to count.
        StencilCensus d;
        d.SetWindow(100);
        for (int i = 0; i < 100; ++i) {
            d.Note(own);
            d.EndFrame(true);
        }
        for (int i = 0; i < 10; ++i) d.EndFrame(false);
        for (int i = 0; i < 100; ++i) {
            d.Note(own);
            d.EndFrame(true);
        }
        CHECK(d.Switches() == 0 && !d.Enough(1)); // (10 frames of Mario: neither switch counts)
    }
    {
        // A mark written in the passes Mario is drawn in comes before the
        // static world's for the same bits, though the world has more draws.
        StencilCensus d;
        d.SetWindow(50);
        const uint32_t world = StencilCensus::Key(0x00, 0x0C, 3, true);       // the static world: 0
        const uint32_t movers = StencilCensus::Key(0x04, 0x0C, 3, true, true); // characters and cars: 4
        for (int i = 0; i < 120; ++i) {
            d.Note(world, 3000);
            d.Note(movers, 40);
            d.EndFrame(i < 60);
        }
        const StencilCensus::Decision x = d.Decide(50);
        CHECK(x.ready && x.ref == 0x04 && x.mask == 0x0C && x.own.empty());
        CHECK(d.Describe().find("G-buffer, Mario's passes") != std::string::npos);
    }
    {
        // His mark covers the G-buffer's bit: his value for it stands.
        StencilCensus d;
        d.SetWindow(100);
        const uint32_t full = StencilCensus::Key(0x82, 0xFF, 3, true);
        for (int i = 0; i < 100; ++i) {
            d.Note(lit);
            d.Note(full);
            d.EndFrame(true);
        }
        for (int i = 0; i < 100; ++i) {
            d.Note(lit);
            d.EndFrame(false);
        }
        const StencilCensus::Decision x = d.Decide(100);
        CHECK(x.ready && x.ref == 0x82 && x.mask == 0xFF && x.shared.empty());
    }
    {
        // Nothing but depth-only marks and counts: no mark at all.
        StencilCensus d;
        d.SetWindow(50);
        for (int i = 0; i < 120; ++i) {
            d.Note(water);
            d.Note(count);
            d.EndFrame(i < 60);
        }
        CHECK(!d.Decide(100).ready && d.Decide(50).ready);
        const StencilCensus::Decision x = d.Decide(50);
        CHECK(x.mask == 0 && x.ref == 0 && x.own.empty() && x.shared.empty());
    }
    {
        // A game writing an id per object: the first kMaxKeys kinds are kept,
        // the rest counted; the log line stays short.
        StencilCensus d;
        for (int i = 0; i < 600; ++i) d.Note(StencilCensus::Key(uint8_t(i % 256), 0xFF, uint8_t(3 + i / 256), true));
        d.EndFrame(true);
        CHECK(d.Overflow() == 600 - StencilCensus::kMaxKeys);
        const std::string all = d.Describe();
        CHECK(all.find("more") != std::string::npos && all.find("not kept") != std::string::npos && all.size() < 8000);
    }
}

static void TestMarioMaterial() {
    std::printf("mario material\n");
    // SHINE 0 is the game's default material (no gloss, no specular colour);
    // the occlusion never drops to where the lighting takes the sun off him.
    const gbuf::ShineLook l0 = gbuf::LookForShine(0.0f, 0.02f);
    CHECK(l0.gloss == 0.0f && l0.f0 == 0.0f);
    CHECK(l0.occlusion >= gbuf::kMinOcclusion);
    CHECK(3.0f * gbuf::kMinOcclusion - 0.75f >= 1.0f); // saturate(3 * occlusion - 0.75): the sun untouched
    float lastGloss = -1, lastF0 = -1, lastOcc = -1;
    for (int i = 0; i <= 20; ++i) {
        const gbuf::ShineLook l = gbuf::LookForShine(i / 20.0f, 0.02f);
        CHECK(l.gloss >= lastGloss && l.f0 >= lastF0 && l.occlusion >= lastOcc);
        CHECK(l.occlusion >= gbuf::kMinOcclusion && l.occlusion <= 1.0f);
        lastGloss = l.gloss;
        lastF0 = l.f0;
        lastOcc = l.occlusion;
    }
    CHECK_NEAR(gbuf::LookForShine(0.5f, 0.02f).f0, 0.02, 1e-6); // [Render] Specular is F0 at 50%
    CHECK(gbuf::LookForShine(1.0f, 0.02f).occlusion == 1.0f);
    // The depth and stencil tests of Mario's draw, whatever the segment's pipeline.
    using namespace d3d12p;
    CHECK(MarioDepthFunc(kCmpEqual) == kCmpGreaterEqual);  // after a depth pre-pass (0.4: Mario vanished)
    CHECK(MarioDepthFunc(kCmpAlways) == kCmpGreaterEqual); // (would show him through walls)
    CHECK(MarioDepthFunc(kCmpGreater) == kCmpGreaterEqual);
    CHECK(MarioDepthFunc(kCmpGreaterEqual) == kCmpGreaterEqual);
    CHECK(MarioDepthFunc(kCmpLess) == kCmpLessEqual);
    CHECK(MarioDepthFunc(kCmpLessEqual) == kCmpLessEqual);
    // EQUAL / ALWAYS in a game with ordinary depth (its other tests LESS*).
    CHECK(MarioDepthFunc(kCmpEqual, false) == kCmpLessEqual);
    CHECK(MarioDepthFunc(kCmpAlways, false) == kCmpLessEqual);
    CHECK(MarioDepthFunc(kCmpGreater, false) == kCmpGreaterEqual);
    StencilOps game;
    game.func = kCmpEqual;
    game.pass = 3; // REPLACE
    game.fail = 2; // ZERO
    const StencilOps mine = MarioStencilOps(game);
    CHECK(mine.func == kCmpAlways && mine.pass == 3 && mine.fail == 1 && mine.depthFail == 1);
}

int main() {
    TestSha1();
    TestRom();
    TestIni();
    TestPattern();
    TestFunctionStart();
    TestMapping();
    TestCollision();
    TestRescue();
    TestCameraFinder();
    TestCombat();
    TestMarioController();
    TestShippedConfig();
    TestViewConstants();
    TestGBufferFormat();
    TestD3D12Parse();
    TestRootSigTables();
    TestGraphicsPsoDesc();
    TestFramePolicy();
    TestWorldModel();
    TestWorldMotion();
    TestRtti();
    TestFollowMonitor();
    TestSettings();
    TestPhysicsWorld();
    TestCameraLead();
    TestSeqWords();
    TestCameraOverride();
    TestCameraDistanceAndTiming();
    TestStencilCensus();
    TestMarioMaterial();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
