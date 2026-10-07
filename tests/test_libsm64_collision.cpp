// Feeds CollisionBuilder output to the real libsm64 collision code (no ROM
// needed for surface queries) and checks floors, ledges and walls behave the
// way SM64 will see them in game.
//
//   tests/run_libsm64_test.sh <path to a native libsm64 build dir>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/world/collision.h"
#include "../src/world/coords.h"
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
};

// Same analytic street scene as the unit tests: ground at y=0, a 10 m
// building from x=503, a thin awning at 3 m, a 1 m planter.
class Scene final : public IRaycaster {
public:
    std::vector<Box> boxes;
    bool Cast(const DVec3& from, const DVec3& to, RayHit& hit) override {
        double bestT = 2.0;
        Vec3 bestN;
        const DVec3 d = to - from;
        if (from.y >= 0 && to.y <= 0 && from.y != to.y) {
            bestT = from.y / (from.y - to.y);
            bestN = Vec3(0, 1, 0);
        }
        for (const Box& b : boxes) {
            double tmin = 0, tmax = 1;
            int axis = -1;
            double sign = 0;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                if (std::fabs(d[a]) < 1e-12) {
                    if (from[a] < b.lo[a] || from[a] > b.hi[a]) miss = true;
                    continue;
                }
                double t1 = (b.lo[a] - from[a]) / d[a], t2 = (b.hi[a] - from[a]) / d[a], s = -1;
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
            if (miss || axis < 0 || tmin >= bestT) continue;
            bestT = tmin;
            bestN = Vec3(0, 0, 0);
            bestN[axis] = float(sign);
        }
        if (bestT > 1.0) return false;
        hit.point = from + d * bestT;
        hit.normal = bestN;
        return true;
    }
    const char* Name() const override { return "scene"; }
};

int main() {
    Scene scene;
    scene.boxes.push_back({DVec3(503, 0, 490), DVec3(520, 10, 520)});
    scene.boxes.push_back({DVec3(497, 3.0, 490), DVec3(499, 3.1, 520)});
    scene.boxes.push_back({DVec3(498, 0, 502), DVec3(502, 1.0, 504)});

    for (int mirrored = 0; mirrored < 2; ++mirrored) {
        std::printf("mapping %s\n", mirrored ? "mirrored (left-handed game)" : "direct");
        WorldMapping map;
        map.Configure('Y', mirrored != 0, 100.0);
        map.SetOrigin(DVec3(500, 0, 500));
        CollisionBuilder b;
        std::vector<SM64Surface> surfs;
        CollisionStats st;
        b.Build(scene, map, Vec3(0, 0, 0), CollisionParams(), surfs, st);
        sm64_static_surfaces_load(surfs.data(), uint32_t(surfs.size()));
        std::printf("  %d surfaces\n", st.total);

        // Local coordinates for game points.
        auto L = [&](double x, double y, double z) { return map.ToLocal(DVec3(x, y, z)); };

        Vec3 street = L(500, 0.5, 500);
        CHECK(std::fabs(sm64_surface_find_floor_height(street.x, street.y, street.z) - 0.0f) < 1.0f);
        Vec3 under = L(498, 0.5, 500); // under the awning
        CHECK(std::fabs(sm64_surface_find_floor_height(under.x, under.y, under.z) - 0.0f) < 1.0f);
        Vec3 onAwning = L(498, 4.0, 500);
        CHECK(std::fabs(sm64_surface_find_floor_height(onAwning.x, onAwning.y, onAwning.z) - 310.0f) < 2.0f);
        Vec3 planter = L(500, 2.0, 503);
        CHECK(std::fabs(sm64_surface_find_floor_height(planter.x, planter.y, planter.z) - 100.0f) < 2.0f);

        // Facade at x=503 (local x = 300), taller than the downward probes'
        // start: points 20 units inside Mario's 50-unit radius, anywhere along
        // the facade near Mario, must be pushed out to exactly 50 units (a
        // bigger push means overlapping walls pushed twice).
        for (double gz = 497.0; gz <= 503.0; gz += 0.75) {
            Vec3 w = L(502.8, 0.5, gz);
            float wx = w.x, wy = w.y, wz = w.z;
            const int walls = sm64_surface_find_wall_collision(&wx, &wy, &wz, 60.0f, 50.0f);
            const bool ok = walls > 0 && std::fabs(wx - 250.0f) < 2.5f;
            if (!ok) std::printf("  facade at z=%.2f: %d wall(s), x %.1f -> %.1f\n", gz, walls, w.x, wx);
            CHECK(ok);
        }
        // Tall: a point 6 m up the facade (a triple jump) is still blocked.
        {
            Vec3 w = L(502.8, 6.0, 500);
            float wx = w.x, wy = w.y, wz = w.z;
            CHECK(sm64_surface_find_wall_collision(&wx, &wy, &wz, 60.0f, 50.0f) > 0 && wx < 252.5f);
        }

        // Planter side (game z=502, local z=+/-200): pushed away from the planter.
        Vec3 pw = L(500, 0.0, 501.8); // feet on the street; SM64 checks walls 30/60 units up
        float px = pw.x, py = pw.y, pz = pw.z;
        const int pwalls = sm64_surface_find_wall_collision(&px, &py, &pz, 60.0f, 50.0f);
        const float planterZ = L(500, 0, 502).z;
        std::printf("  planter: %d wall(s), z %.1f -> %.1f (planter face at %.1f)\n", pwalls, pw.z, pz, planterZ);
        CHECK(pwalls > 0);
        CHECK(std::fabs(std::fabs(pz - planterZ) - 50.0f) < 2.5f);
        // The planter's wall stops at its top (1 m): Mario's wall check 1.6 m
        // up must not hit it, so he can jump onto it.
        Vec3 pa = L(500, 1.6, 501.8);
        float ax = pa.x, ay = pa.y, az = pa.z;
        CHECK(sm64_surface_find_wall_collision(&ax, &ay, &az, 0.0f, 50.0f) == 0);

        // Open street: no walls.
        Vec3 open = L(495, 0.5, 495);
        float ox = open.x, oy = open.y, oz = open.z;
        CHECK(sm64_surface_find_wall_collision(&ox, &oy, &oz, 60.0f, 50.0f) == 0);

        // Roof edge: from the roof, beyond the edge there is street-level floor
        // (Mario can walk off and fall) rather than out-of-bounds.
        map.SetOrigin(DVec3(504, 10, 500));
        b.Build(scene, map, Vec3(0, 0, 0), CollisionParams(), surfs, st);
        sm64_static_surfaces_load(surfs.data(), uint32_t(surfs.size()));
        Vec3 roof = map.ToLocal(DVec3(505, 10.5, 500));
        CHECK(std::fabs(sm64_surface_find_floor_height(roof.x, roof.y, roof.z) - 0.0f) < 2.0f);
        Vec3 beyond = map.ToLocal(DVec3(502, 10.5, 500));
        CHECK(std::fabs(sm64_surface_find_floor_height(beyond.x, beyond.y, beyond.z) + 1000.0f) < 2.0f);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
