// A synthetic world answered like Spider-Man 2's physics ray casts: boxes and
// a ground plane, every hit along a ray (nearest first) with its normal and
// physics material. Shared by the unit tests and the libsm64 walking test.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "../src/world/physics_world.h"

namespace phys_test {

using sm2m::DVec3;
using sm2m::GameHit;
using sm2m::GameRay;
using sm2m::GameRayResult;
using sm2m::HitKind;
using sm2m::Vec3;

struct PBox {
    DVec3 lo, hi;
    int16_t material = 9; // kConcrete
    HitKind kind = HitKind::World;
    bool enabled = true;
    bool flip = false; // its faces wound the other way: normals reported inwards
    bool actor = false; // an actor's body (a car, a person), not the static world
};

struct PhysScene {
    std::vector<PBox> boxes;
    double groundY = 0;
    int16_t groundMaterial = 2; // kAsphalt
    bool twoSided = false;      // rays also hit faces from behind (inside a box)
    double waterY = -1e30;      // a water plane (only for water queries)
    bool noTopRays = false;     // short rays straight down (a wall's top ray) find nothing
    int answered = 0;

    void Cast(const GameRay& r, GameRayResult& out) {
        ++answered;
        out = GameRayResult();
        out.tag = r.tag;
        const DVec3 d = r.to - r.from;
        const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-9) return;
        const DVec3 dir = d * (1.0 / len);
        if (noTopRays && r.type != 17 && dir.y < -0.99 && len < 3.5) return;
        struct H {
            double t;
            GameHit h;
        };
        std::vector<H> hits;
        if (r.type == 17) { // water only
            if (waterY > -1e29 && dir.y < -1e-9) {
                const double t = (waterY - r.from.y) / dir.y;
                if (t >= 0 && t <= len) {
                    GameHit h;
                    h.pos = r.from + dir * t;
                    h.normal = Vec3(0, 1, 0);
                    h.material = 79; // kWaterOcean
                    hits.push_back({t, h});
                }
            }
        } else {
            if (std::fabs(dir.y) > 1e-9) {
                const double t = (groundY - r.from.y) / dir.y;
                if (t >= 0 && t <= len && (dir.y < 0 || twoSided)) {
                    GameHit h;
                    h.pos = r.from + dir * t;
                    h.normal = Vec3(0, dir.y < 0 ? 1.0f : -1.0f, 0);
                    h.material = groundMaterial;
                    hits.push_back({t, h});
                }
            }
            for (const PBox& b : boxes) {
                if (!b.enabled) continue;
                // Each face of the box as a plane hit (entering faces, and
                // leaving faces too if two-sided).
                for (int a = 0; a < 3; ++a) {
                    if (std::fabs(dir[a]) < 1e-12) continue;
                    for (int side = 0; side < 2; ++side) {
                        const double plane = side ? b.hi[a] : b.lo[a];
                        const double t = (plane - r.from[a]) / dir[a];
                        if (t < 0 || t > len) continue;
                        const DVec3 p = r.from + dir * t;
                        bool inside = true;
                        for (int q = 0; q < 3; ++q)
                            if (q != a && (p[q] < b.lo[q] - 1e-9 || p[q] > b.hi[q] + 1e-9)) inside = false;
                        if (!inside) continue;
                        Vec3 n(0, 0, 0);
                        n[a] = side ? 1.0f : -1.0f;
                        const bool front = (dir[a] * n[a]) < 0;
                        if (!front && !twoSided) continue;
                        if (b.flip) n = n * -1.0f;
                        GameHit h;
                        h.pos = p;
                        h.normal = n;
                        h.material = b.material;
                        h.kind = b.kind;
                        h.actor = b.actor;
                        hits.push_back({t, h});
                    }
                }
            }
        }
        std::sort(hits.begin(), hits.end(), [](const H& x, const H& y) { return x.t < y.t; });
        const int n = std::min<int>(int(hits.size()), std::min<int>(r.maxHits, sm2m::kMaxGameHits));
        for (int i = 0; i < n; ++i) out.hits[i] = hits[size_t(i)].h;
        out.count = n;
    }
};

} // namespace phys_test
