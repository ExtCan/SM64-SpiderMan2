#include "collision.h"

#include <algorithm>
#include <cmath>

#include "../sm64/sm64_defs.h"

namespace sm2m {

bool FlatGroundRaycaster::Cast(const DVec3& from, const DVec3& to, RayHit& hit) {
    const double a = from[up_] - h_;
    const double b = to[up_] - h_;
    if ((a > 0 && b > 0) || (a < 0 && b < 0) || a == b) return false;
    const double t = a / (a - b);
    hit.point = from + (to - from) * t;
    hit.normal = Vec3(0, 0, 0);
    hit.normal[up_] = a >= 0 ? 1.0f : -1.0f;
    return true;
}

Vec3 Sm64SurfaceNormal(const SM64Surface& s) {
    const float x1 = float(s.vertices[0][0]), y1 = float(s.vertices[0][1]), z1 = float(s.vertices[0][2]);
    const float x2 = float(s.vertices[1][0]), y2 = float(s.vertices[1][1]), z2 = float(s.vertices[1][2]);
    const float x3 = float(s.vertices[2][0]), y3 = float(s.vertices[2][1]), z3 = float(s.vertices[2][2]);
    return {(y2 - y1) * (z3 - z2) - (z2 - z1) * (y3 - y2), (z2 - z1) * (x3 - x2) - (x2 - x1) * (z3 - z2),
            (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2)};
}

bool GroundAboveFallingMario(float (*findFloor)(float, float, float), const Vec3* path, int count, float vy,
                             float maxClimb, float& floorY) {
    if (!findFloor || !path || count < 2 || vy >= 0.0f) return false;
    const Vec3& pos = path[count - 1];
    float top = -1e30f;
    for (int i = 0; i < count; ++i) top = std::max(top, path[i].y);
    const float climb = std::min(top - pos.y, maxClimb);
    if (climb < 80.0f) return false; // SM64 lands him on anything within 78 units itself
    // find_floor(y) returns the highest floor at or below y + 78.
    const float f = findFloor(pos.x, pos.y + climb - 78.0f, pos.z);
    if (f < pos.y + 78.0f || f > pos.y + climb + 1.0f) return false;
    auto over = [&](const Vec3& p) { return std::fabs(findFloor(p.x, f + 1.0f - 78.0f, p.z) - f) < 3.0f; };
    for (int i = 0; i + 1 < count; ++i) {
        if (path[i].y >= f - 1.0f && path[i + 1].y < f && over(path[i]) && over(path[i + 1])) {
            floorY = f;
            return true;
        }
    }
    return false;
}

static int32_t RoundI(float v) { return int32_t(std::lround(v)); }

bool CollisionBuilder::AddTri(std::vector<SM64Surface>& out, const Vec3& a, const Vec3& b, const Vec3& c,
                              const Vec3& desired, SurfaceKind kind) {
    SM64Surface s{};
    s.type = kind.type;
    s.force = 0;
    s.terrain = kind.terrain;
    const Vec3* v[3] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        s.vertices[i][0] = RoundI(v[i]->x);
        s.vertices[i][1] = RoundI(v[i]->y);
        s.vertices[i][2] = RoundI(v[i]->z);
    }
    Vec3 n = Sm64SurfaceNormal(s);
    const float len = Length(n);
    if (len < 1.0f) return false; // degenerate after rounding
    if (Dot(n, desired) < 0) {
        for (int k = 0; k < 3; ++k) std::swap(s.vertices[1][k], s.vertices[2][k]);
    }
    out.push_back(s);
    return true;
}

int WallSet::Emit(const Vec3& a0, const Vec3& b0, float lo, float hi, const Vec3& nIn, int& clipped, SurfaceKind kind) {
    const Vec3 n = Normalize(Vec3(nIn.x, 0, nIn.z));
    const Vec3 a(a0.x, 0, a0.z), b(b0.x, 0, b0.z);
    const float L = Length(b - a);
    if (L < 2.0f || hi - lo < 2.0f || Length(n) < 0.5f) return 0;
    const Vec3 t = (b - a) * (1.0f / L);
    const Vec3 mid = (a + b) * 0.5f;
    struct Cover {
        float u0, u1, y0, y1;
    };
    std::vector<Cover> cover;
    std::vector<float> cuts = {0.0f, L};
    for (const Seg& s : segs_) {
        if (Dot(s.n, n) < 0.9f) continue;
        const Vec3 smid = (s.a + s.b) * 0.5f;
        if (std::fabs(Dot(n, smid - a)) > tol_ || std::fabs(Dot(s.n, mid - s.a)) > tol_) continue;
        float u1 = Dot(s.a - a, t), u2 = Dot(s.b - a, t);
        if (u1 > u2) std::swap(u1, u2);
        u1 = std::max(u1, 0.0f);
        u2 = std::min(u2, L);
        const float y0 = std::max(lo, s.lo), y1 = std::min(hi, s.hi);
        if (u2 - u1 < 1e-3f || y1 - y0 < 1e-3f) continue;
        cover.push_back({u1, u2, y0, y1});
        cuts.push_back(u1);
        cuts.push_back(u2);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    // Free height ranges in each stretch between cuts; neighbouring
    // stretches with the same free ranges are merged into one quad.
    struct Piece {
        float u0, u1, y0, y1;
    };
    std::vector<Piece> pieces;
    bool anyCovered = false;
    std::vector<std::pair<float, float>> ys, freeYs;
    for (size_t c = 0; c + 1 < cuts.size(); ++c) {
        const float u0 = cuts[c], u1 = cuts[c + 1];
        if (u1 - u0 < 1e-3f) continue;
        const float um = 0.5f * (u0 + u1);
        ys.clear();
        for (const Cover& cv : cover)
            if (cv.u0 <= um && um <= cv.u1) ys.push_back({cv.y0, cv.y1});
        std::sort(ys.begin(), ys.end());
        freeYs.clear();
        float cur = lo;
        for (const auto& y : ys) {
            if (y.first > cur) freeYs.push_back({cur, y.first});
            cur = std::max(cur, y.second);
        }
        if (cur < hi) freeYs.push_back({cur, hi});
        if (!ys.empty()) anyCovered = true;
        for (const auto& fy : freeYs) {
            if (fy.second - fy.first < 2.0f) continue;
            bool merged = false;
            for (Piece& pc : pieces)
                if (pc.u1 == u0 && pc.y0 == fy.first && pc.y1 == fy.second) {
                    pc.u1 = u1;
                    merged = true;
                    break;
                }
            if (!merged) pieces.push_back({u0, u1, fy.first, fy.second});
        }
    }
    if (anyCovered) ++clipped;
    int tris = 0;
    for (const Piece& pc : pieces) {
        if (pc.u1 - pc.u0 < 2.0f || int(out_.size()) + 2 > max_) continue;
        const Vec3 p0 = a + t * pc.u0, p1 = a + t * pc.u1;
        const Vec3 q0(p0.x, pc.y0, p0.z), q1(p1.x, pc.y0, p1.z), q2(p1.x, pc.y1, p1.z), q3(p0.x, pc.y1, p0.z);
        if (CollisionBuilder::AddTri(out_, q0, q1, q2, n, kind)) ++tris;
        if (CollisionBuilder::AddTri(out_, q0, q2, q3, n, kind)) ++tris;
        segs_.push_back({p0, p1, n, pc.y0, pc.y1});
    }
    return tris;
}

namespace {

// Closest height in `v` to `target`; returns false if none within `tol`.
bool Closest(const GridVertex& v, float target, float tol, float& out, SurfaceKind* kind = nullptr) {
    float best = 0, bestD = tol + 1;
    SurfaceKind bestKind;
    for (int i = 0; i < v.count; ++i) {
        float d = std::fabs(v.h[i] - target);
        if (d < bestD) {
            bestD = d;
            best = v.h[i];
            bestKind = v.kind[i];
        }
    }
    if (bestD > tol) return false;
    out = best;
    if (kind) *kind = bestKind;
    return true;
}

// Top of the wall whose face is at `p` (normal n): the lowest upward-facing
// surface just behind the face, above the height the wall was found at. If
// none (the ray started inside a tall building and passed through), the wall
// is treated as tall.
float FindWallTop(IRaycaster& rays, const WorldMapping& map, const Vec3& p, const Vec3& n, float foundAtY, float maxTop,
                  float gap, float s, CollisionStats& st) {
    const Vec3 behind = p - n * (0.1f * s);
    // A probe that grazes the top edge of a wall (a step exactly at probe
    // height) hits it at its top: look a little below the hit too.
    const float lowest = foundAtY - 0.1f * s;
    float startY = maxTop;
    float best = maxTop;
    for (int i = 0; i < 3 && startY > lowest; ++i) {
        RayHit hit;
        ++st.rays;
        if (!rays.Cast(map.ToGame(Vec3(behind.x, startY, behind.z)), map.ToGame(Vec3(behind.x, lowest, behind.z)), hit))
            break;
        ++st.hits;
        const Vec3 hl = map.ToLocal(hit.point);
        const Vec3 nl = map.DirToLocal(hit.normal);
        if (nl.y > 0.5f && hl.y > lowest) best = hl.y; // later (lower) candidates win
        startY = hl.y - gap;
    }
    return best;
}

} // namespace

void CollisionBuilder::EmitFloors(const FloorGrid& g, const CollisionParams& p, float s, std::vector<SM64Surface>& out,
                                  CollisionStats& stats) {
    const float step = p.stepHeight * s;
    const Vec3 up(0, 1, 0);
    const int N = g.N();
    auto full = [&]() { return int(out.size()) >= p.maxSurfaces; };
    // For each grid triangle, connect heights that agree within a step.
    auto floorTri = [&](int ia, int ja, int ib, int jb, int ic, int jc) {
        const GridVertex& A = g.At(ia, ja);
        const GridVertex& B = g.At(ib, jb);
        const GridVertex& C = g.At(ic, jc);
        if (A.count == 0 || B.count == 0 || C.count == 0) return;
        const GridVertex* corners[3] = {&A, &B, &C};
        float emitted[12];
        int emittedCount = 0;
        bool lowestEmitted = false;
        const float lowA = A.h[A.count - 1], lowB = B.h[B.count - 1], lowC = C.h[C.count - 1];
        for (int seed = 0; seed < 3; ++seed) {
            const GridVertex& S = *corners[seed];
            for (int k = 0; k < S.count; ++k) {
                float ha, hb, hc;
                SurfaceKind ka, kb, kc;
                const float t = S.h[k];
                if (!Closest(A, t, step, ha, &ka) || !Closest(B, t, step, hb, &kb) || !Closest(C, t, step, hc, &kc))
                    continue;
                if (std::fabs(ha - hb) > step || std::fabs(hb - hc) > step || std::fabs(ha - hc) > step) continue;
                const float key = ha + hb * 3.1f + hc * 7.3f;
                bool dup = false;
                for (int e = 0; e < emittedCount; ++e)
                    if (std::fabs(emitted[e] - key) < 0.5f) dup = true;
                if (dup || emittedCount >= 12) continue;
                emitted[emittedCount++] = key;
                if (ha == lowA && hb == lowB && hc == lowC) lowestEmitted = true;
                // The triangle is what most of its corners are made of.
                const SurfaceKind kind = (kb.type == kc.type && kb.terrain == kc.terrain) ? kb : ka;
                if (!full() && AddTri(out, Vec3(g.X(ia), ha, g.Z(ja)), Vec3(g.X(ib), hb, g.Z(jb)),
                                      Vec3(g.X(ic), hc, g.Z(jc)), up, kind))
                    ++stats.floors;
            }
        }
        // Ledge / cliff: keep a floor at the lowest level so Mario can walk
        // (or fall) off edges instead of hitting SM64's out-of-bounds wall.
        if (!lowestEmitted) {
            const float lo = std::min(lowA, std::min(lowB, lowC));
            const SurfaceKind kind = lo == lowA ? A.kind[A.count - 1] : lo == lowB ? B.kind[B.count - 1] : C.kind[C.count - 1];
            if (!full() && AddTri(out, Vec3(g.X(ia), lo, g.Z(ja)), Vec3(g.X(ib), lo, g.Z(jb)),
                                  Vec3(g.X(ic), lo, g.Z(jc)), up, kind))
                ++stats.flattened;
        }
    };
    for (int j = 0; j < N - 1; ++j) {
        for (int i = 0; i < N - 1; ++i) {
            floorTri(i, j, i + 1, j, i + 1, j + 1);
            floorTri(i, j, i + 1, j + 1, i, j + 1);
        }
    }
}

void CollisionBuilder::EmitStepWalls(const FloorGrid& g, const CollisionParams& p, float s, WallSet& walls,
                                     CollisionStats& stats) {
    const float step = p.stepHeight * s;
    const int N = g.N();
    // The wall sits halfway between the neighbours and faces the low side.
    auto stepWall = [&](int ia, int ja, int ib, int jb) {
        const GridVertex& A = g.At(ia, ja);
        const GridVertex& B = g.At(ib, jb);
        if (A.count == 0 || B.count == 0) return;
        // (A column not known all the way down: its lowest floor so far may
        // be an awning or a branch over the street - not a step.)
        if (!A.lowKnown || !B.lowKnown) return;
        const float ha = A.h[A.count - 1], hb = B.h[B.count - 1];
        if (std::fabs(ha - hb) <= step) return;
        const float lo = std::min(ha, hb) - 0.3f * s;
        const float hi = std::max(ha, hb);
        const float mx = 0.5f * (g.X(ia) + g.X(ib));
        const float mz = 0.5f * (g.Z(ja) + g.Z(jb));
        Vec3 n = ha < hb ? Vec3(g.X(ia) - g.X(ib), 0, g.Z(ja) - g.Z(jb)) : Vec3(g.X(ib) - g.X(ia), 0, g.Z(jb) - g.Z(ja));
        n = Normalize(n);
        const Vec3 t(-n.z, 0, n.x);
        const float half = 0.5f * g.cell + 1.0f;
        const SurfaceKind kind = ha > hb ? A.kind[A.count - 1] : B.kind[B.count - 1];
        stats.stepWalls += walls.Emit(Vec3(mx - t.x * half, 0, mz - t.z * half), Vec3(mx + t.x * half, 0, mz + t.z * half),
                                      lo, hi, n, stats.clipped, kind);
    };
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            if (i + 1 < N) stepWall(i, j, i + 1, j);
            if (j + 1 < N) stepWall(i, j, i, j + 1);
        }
    }
}

void CollisionBuilder::EmitSafetyFloor(const FloorGrid& g, float y, std::vector<SM64Surface>& out,
                                       CollisionStats& stats, float apron) {
    // A floor below everything so Mario never enters SM64's "no floor = out
    // of bounds" state inside the sampled area.
    const float ext = (float(g.R) + 3.0f) * g.cell + std::max(0.0f, apron);
    stats.safetyFloorY = y;
    const Vec3 up(0, 1, 0);
    Vec3 a(g.cx - ext, y, g.cz - ext), b(g.cx + ext, y, g.cz - ext), c(g.cx + ext, y, g.cz + ext),
        d(g.cx - ext, y, g.cz + ext);
    AddTri(out, a, b, c, up);
    AddTri(out, a, c, d, up);
}

void CollisionBuilder::EmitSafetyFloor(const FloorGrid& g, const std::vector<uint8_t>& known, float y,
                                       std::vector<SM64Surface>& out, CollisionStats& stats) {
    stats.safetyFloorY = y;
    const int N = g.N();
    if (known.size() != size_t(N) * size_t(N)) return;
    auto k = [&](int i, int j) { return known[size_t(j) * size_t(N) + size_t(i)] != 0; };
    auto cell = [&](int i, int j) { return k(i, j) && k(i + 1, j) && k(i, j + 1) && k(i + 1, j + 1); };
    const Vec3 up(0, 1, 0);
    // One quad per run of known cells along each row.
    for (int j = 0; j + 1 < N; ++j) {
        int i = 0;
        while (i + 1 < N) {
            if (!cell(i, j)) {
                ++i;
                continue;
            }
            int e = i + 1; // cells i .. e-1, vertices i .. e
            while (e + 1 < N && cell(e, j)) ++e;
            const Vec3 a(g.X(i), y, g.Z(j)), b(g.X(e), y, g.Z(j)), c(g.X(e), y, g.Z(j + 1)), d(g.X(i), y, g.Z(j + 1));
            AddTri(out, a, b, c, up);
            AddTri(out, a, c, d, up);
            i = e;
        }
    }
}

void CollisionBuilder::Build(IRaycaster& rays, const WorldMapping& map, const Vec3& m, const CollisionParams& p,
                             std::vector<SM64Surface>& out, CollisionStats& stats) {
    out.clear();
    stats = CollisionStats{};
    const float s = float(map.Scale());
    const float cell = p.cellSize * s;
    const int R = std::max(1, p.gridRadiusCells);
    const float step = p.stepHeight * s;
    const float top = m.y + p.probeUp * s;
    const float bottom = m.y - p.probeDown * s;
    const float gap = p.layerGap * s;

    // Snap the grid to whole cells so floors don't swim between rebuilds.
    FloorGrid g;
    g.R = R;
    g.cell = cell;
    g.cx = std::round(m.x / cell) * cell;
    g.cz = std::round(m.z / cell) * cell;
    g.v.assign(size_t(g.N()) * size_t(g.N()), GridVertex());
    const int N = g.N();

    // 1) Multi-layer downward probes.
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            GridVertex& v = g.At(i, j);
            float startY = top;
            for (int layer = 0; layer < std::min(p.maxLayers, 4); ++layer) {
                if (startY <= bottom) break;
                RayHit hit;
                ++stats.rays;
                DVec3 from = map.ToGame(Vec3(g.X(i), startY, g.Z(j)));
                DVec3 to = map.ToGame(Vec3(g.X(i), bottom, g.Z(j)));
                if (!rays.Cast(from, to, hit)) break;
                ++stats.hits;
                Vec3 local = map.ToLocal(hit.point);
                Vec3 nl = map.DirToLocal(hit.normal);
                if (nl.y > 0.05f || layer == 0) {
                    // Keep upward-facing hits (and the first hit, even if it is
                    // an edge, so building footprints still produce a height).
                    // A lower layer only counts if Mario fits underneath the one
                    // above it; otherwise it is the ground *inside* a solid
                    // object the ray passed through (planter, car, kiosk) or a
                    // crawlspace, and keeping it would erase that object's walls.
                    const bool fits = v.count == 0 || (v.h[v.count - 1] - local.y) >= p.minClearance * s;
                    if (fits) {
                        v.kind[v.count] = SurfaceKind{};
                        v.h[v.count++] = local.y;
                    }
                }
                startY = local.y - gap;
            }
        }
    }

    auto full = [&]() { return int(out.size()) >= p.maxSurfaces; };

    // 2) Floors.
    EmitFloors(g, p, s, out, stats);

    WallSet walls(out, p.maxSurfaces, p.coplanarTolerance * s);
    const float maxTop = m.y + (p.probeUp + p.tallWallExtra) * s;

    // 3) Edge probes: short horizontal rays along grid edges near Mario,
    // walking outward. They find facades exactly (and contiguously), even
    // where the downward rays started inside a tall building. Emitted first so
    // the grid-quantised step walls get clipped against them.
    const int Re = std::min(R, std::max(0, p.edgeProbeRadiusCells));
    auto edgeProbe = [&](int ia, int ja, int ib, int jb) {
        const float da = (g.X(ia) - m.x) * (g.X(ia) - m.x) + (g.Z(ja) - m.z) * (g.Z(ja) - m.z);
        const float db = (g.X(ib) - m.x) * (g.X(ib) - m.x) + (g.Z(jb) - m.z) * (g.Z(jb) - m.z);
        if (db < da) {
            std::swap(ia, ib);
            std::swap(ja, jb);
        }
        float floorA;
        if (!Closest(g.At(ia, ja), m.y, p.edgeProbeMaxDrop * s, floorA)) return;
        const Vec3 dir = Normalize(Vec3(g.X(ib) - g.X(ia), 0, g.Z(jb) - g.Z(ja)));
        for (float h : p.edgeProbeHeights) {
            if (h <= 0 || full()) continue;
            const float y = floorA + h * s;
            RayHit hit;
            ++stats.rays;
            if (!rays.Cast(map.ToGame(Vec3(g.X(ia), y, g.Z(ja))), map.ToGame(Vec3(g.X(ib), y, g.Z(jb))), hit)) continue;
            ++stats.hits;
            const Vec3 nl = map.DirToLocal(hit.normal);
            if (std::fabs(nl.y) > 0.6f) continue;
            const Vec3 nh = Normalize(Vec3(nl.x, 0, nl.z));
            if (Dot(nh, dir) > -0.2f) continue;
            const Vec3 P = map.ToLocal(hit.point);
            const float wallTop = FindWallTop(rays, map, P, nh, y, maxTop, gap, s, stats);
            // Low rises (kerbs, stairs) stay walkable: the floor grid already
            // ramps up them, and a riser wall would stop Mario in front of
            // human-sized steps that SM64's physics treats as walls.
            if (wallTop - floorA < step) continue;
            const Vec3 t(-nh.z, 0, nh.x);
            const float half = 0.75f * cell; // overlap with neighbours is clipped
            stats.edgeWalls += walls.Emit(P - t * half, P + t * half, floorA - 0.3f * s, wallTop, nh, stats.clipped);
            return; // one wall per edge
        }
    };
    for (int j = R - Re; j <= R + Re; ++j) {
        for (int i = R - Re; i <= R + Re; ++i) {
            if (i + 1 <= R + Re) edgeProbe(i, j, i + 1, j);
            if (j + 1 <= R + Re) edgeProbe(i, j, i, j + 1);
        }
    }

    // 4) Step walls.
    if (!full()) EmitStepWalls(g, p, s, walls, stats);

    // 5) Radial probes for thin things the grid can miss (poles, railings).
    const float probeLen = p.wallProbeDist * s;
    for (float hMetres : p.wallProbeHeights) {
        const float y = m.y + hMetres * s;
        for (int k = 0; k < p.wallProbeDirs && !full(); ++k) {
            const float ang = 6.2831853f * float(k) / float(p.wallProbeDirs);
            const Vec3 dir(std::cos(ang), 0, std::sin(ang));
            RayHit hit;
            ++stats.rays;
            DVec3 from = map.ToGame(Vec3(m.x, y, m.z));
            DVec3 to = map.ToGame(Vec3(m.x + dir.x * probeLen, y, m.z + dir.z * probeLen));
            if (!rays.Cast(from, to, hit)) continue;
            ++stats.hits;
            Vec3 nl = map.DirToLocal(hit.normal);
            if (std::fabs(nl.y) > 0.6f) continue; // floor/ceiling, not a wall
            Vec3 nh = Normalize(Vec3(nl.x, 0, nl.z));
            if (Dot(nh, dir) > -0.2f) continue;   // grazing or back-facing
            const Vec3 P = map.ToLocal(hit.point);
            const float wallTop = FindWallTop(rays, map, P, nh, y, maxTop, gap, s, stats);
            if (wallTop - m.y < step) continue; // walkable rise (see the edge probes)
            const Vec3 t(-nh.z, 0, nh.x);
            const float half = 0.5f * p.wallSlabWidth * s;
            stats.probeWalls +=
                walls.Emit(P - t * half, P + t * half, m.y - p.wallSlabBelow * s, wallTop, nh, stats.clipped);
        }
    }

    // 6) Safety floor.
    EmitSafetyFloor(g, bottom - 1.0f * s, out, stats);
    stats.total = int(out.size());
}

} // namespace sm2m
