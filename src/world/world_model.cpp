#include "world_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sm2m {

void WorldModel::SetUpAxis(int upIndex) {
    up_ = upIndex == 2 ? 2 : 1;
    h0_ = 0;
    h1_ = up_ == 2 ? 1 : 2;
}

void WorldModel::Clear() {
    cells_.clear();
    stats_ = WorldModelStats{};
}

void WorldModel::CellIndex(const DVec3& p, int64_t& i, int64_t& j) const {
    i = int64_t(std::floor(p[h0_] / p_.cell));
    j = int64_t(std::floor(p[h1_] / p_.cell));
}

DVec3 WorldModel::CellCentre(int64_t i, int64_t j, double up) const {
    DVec3 c;
    c[h0_] = (double(i) + 0.5) * p_.cell;
    c[h1_] = (double(j) + 0.5) * p_.cell;
    c[up_] = up;
    return c;
}

void WorldModel::AddFloor(Cell& c, float h, double ox, double oz) {
    for (int k = 0; k < c.nFloor; ++k) {
        Layer& l = c.floor[k];
        if (std::fabs(l.h - h) < p_.layerMerge) {
            const float w = float(std::min<int>(l.hits, 8));
            l.h = (l.h * w + h) / (w + 1.0f);
            if (l.hits < kMaxConfidence) ++l.hits;
            if (l.misses) --l.misses;
            if (l.sw > 256.0f) {
                l.sx *= 0.5f;
                l.sz *= 0.5f;
                l.sw *= 0.5f;
            }
            l.sx += float(ox);
            l.sz += float(oz);
            l.sw += 1.0f;
            return;
        }
    }
    Layer nl;
    nl.h = h;
    nl.hits = 1;
    nl.sx = float(ox);
    nl.sz = float(oz);
    nl.sw = 1.0f;
    if (c.nFloor < 4) {
        c.floor[c.nFloor++] = nl;
    } else {
        int weakest = 0;
        for (int k = 1; k < 4; ++k)
            if (c.floor[k].hits < c.floor[weakest].hits) weakest = k;
        if (c.floor[weakest].hits > 2) return;
        c.floor[weakest] = nl;
    }
    std::sort(c.floor, c.floor + c.nFloor, [](const Layer& a, const Layer& b) { return a.h < b.h; });
}

void WorldModel::AddWall(Cell& c, float h, const Vec3& n, double ox, double oz) {
    c.wallLo = std::min(c.wallLo, h);
    c.wallHi = std::max(c.wallHi, h);
    if (c.wallSamples > 4096.0f) {
        // Keep the running averages bounded (and responsive).
        c.wallSamples *= 0.5f;
        c.nx *= 0.5f;
        c.nz *= 0.5f;
        c.cx *= 0.5f;
        c.cz *= 0.5f;
    }
    const float hn = std::sqrt(n[h0_] * n[h0_] + n[h1_] * n[h1_]);
    if (hn > 1e-3f) {
        c.nx += n[h0_] / hn;
        c.nz += n[h1_] / hn;
    }
    c.cx += float(ox);
    c.cz += float(oz);
    c.wallSamples += 1.0f;
    if (c.wallHits < kMaxConfidence) ++c.wallHits;
    if (c.wallMisses) --c.wallMisses;
}

void WorldModel::Seed(const DVec3& feet, float radius) {
    int64_t i0, j0;
    CellIndex(feet, i0, j0);
    const int r = int(std::ceil(radius / p_.cell));
    for (int64_t i = i0 - r; i <= i0 + r; ++i)
        for (int64_t j = j0 - r; j <= j0 + r; ++j) {
            const DVec3 c = CellCentre(i, j, feet[up_]);
            const double dx = c[h0_] - feet[h0_], dz = c[h1_] - feet[h1_];
            if (dx * dx + dz * dz > double(radius) * radius) continue;
            Cell& cell = cells_[KeyOf(i, j)];
            for (int k = 0; k < p_.minFloorHits; ++k) AddFloor(cell, float(feet[up_]), 0.5 * p_.cell, 0.5 * p_.cell);
        }
}

bool WorldModel::Near(const DVec3& p, const DVec3& focus) const {
    const double dx = p[h0_] - focus[h0_], dz = p[h1_] - focus[h1_];
    return dx * dx + dz * dz < double(p_.changeRadius) * p_.changeRadius;
}

bool WorldModel::MovingMask(const DepthFrame& f, std::vector<uint8_t>& mask) {
    const int W = f.width, H = f.height, s = std::max(1, p_.stride);
    if (int(f.motion.size()) < W * H * 2) return false;
    const ViewConstants& v = f.view;
    const double sx = v.raw[33][0], sy = v.raw[33][1];
    const double rw = v.renderWidth > 0 ? v.renderWidth : W, rh = v.renderHeight > 0 ? v.renderHeight : H;
    const double eps = std::max(1e-6, double(v.raw[35][0]));
    if (std::fabs(sx) < 1e-9 || std::fabs(sy) < 1e-9) return false;
    mask.assign(size_t(W) * H, 0);
    int valid = 0, moving = 0;
    for (int y = 0; y < H; y += s) {
        for (int x = 0; x < W; x += s) {
            const size_t i = size_t(y) * W + x;
            const float w = f.depth[i];
            if (!(w > 0.05f && w < p_.maxViewDepth)) continue;
            const float mx = f.motion[i * 2], my = f.motion[i * 2 + 1];
            if (!std::isfinite(mx) || !std::isfinite(my)) continue;
            double nx, ny;
            ViewConstants::PixelToNdc(x, y, W, H, nx, ny);
            DVec3 P;
            if (!v.Unproject(nx, ny, w, P)) continue;
            // Where the camera alone would have moved this point on screen:
            // last frame's clip position of the same world point.
            const DVec3 rel = P - v.prevCamPos;
            double c[4];
            for (int k = 0; k < 4; ++k)
                c[k] = rel.x * v.prevVp[0][k] + rel.y * v.prevVp[1][k] + rel.z * v.prevVp[2][k] + v.prevVp[3][k];
            const double pw = std::max(c[3], eps);
            const double uPrev = c[0] / pw * 0.5 + 0.5, vPrev = c[1] / pw * -0.5 + 0.5;
            const double uCur = (x + 0.5) / W, vCur = (y + 0.5) / H;
            const double camX = (uCur - uPrev) * rw, camY = (vCur - vPrev) * rh;      // render pixels
            const double gameX = mx / sx * rw, gameY = my / sy * rh;
            const double dx = gameX - camX, dy = gameY - camY;
            const double lim = p_.movingPixels + p_.movingRelative * std::sqrt(camX * camX + camY * camY);
            ++valid;
            if (dx * dx + dy * dy > lim * lim) {
                mask[i] = 1;
                ++moving;
            }
        }
    }
    ++stats_.motionFrames;
    if (valid == 0 || moving > p_.maxMovingFraction * valid) {
        ++stats_.motionRejected;
        return false;
    }
    return true;
}

void WorldModel::Unsettle(Cell& c, const DVec3& P, float nu, bool near) {
    const float h = float(P[up_]);
    if (nu > 0.5f) {
        for (int k = 0; k < c.nFloor; ++k) {
            Layer& l = c.floor[k];
            if (std::fabs(l.h - h) >= p_.layerMerge || l.hits <= 1) continue;
            l.hits = 1;
            ++stats_.unsettled;
            if (near) ++stats_.nearChanges;
        }
    } else if (c.wallHits > 1 && h >= c.wallLo - 0.5f && h <= c.wallHi + 0.5f) {
        c.wallHits = 1;
        ++stats_.unsettled;
        if (near) ++stats_.nearChanges;
    }
}

void WorldModel::Integrate(const DepthFrame& f, const DVec3& focus, const std::vector<Cylinder>& exclude,
                           const std::vector<ExcludeBox>& boxes) {
    WorldModelStats& st = stats_;
    st.samples = st.floorSamples = st.wallSamples = st.excluded = st.edges = st.carved = 0;
    st.moving = st.unsettled = st.nearChanges = 0;
    st.motionUsed = false;
    if (!f.view.valid || f.width < 4 || f.height < 4 || int(f.depth.size()) < f.width * f.height) return;
    ++st.frames;
    std::vector<uint8_t>& moving = movingMask_;
    st.motionUsed = !f.motion.empty() && MovingMask(f, moving);
    const int W = f.width, H = f.height, s = std::max(1, p_.stride);
    const double range2 = double(p_.range) * p_.range;
    const DVec3 cam = f.view.camPos;

    auto pointAt = [&](int x, int y, DVec3& out, float& w) -> bool {
        if (x < 0 || y < 0 || x >= W || y >= H) return false;
        w = f.depth[size_t(y) * W + x];
        if (!(w > 0.05f && w < p_.maxViewDepth)) return false;
        double nx, ny;
        ViewConstants::PixelToNdc(x, y, W, H, nx, ny);
        return f.view.Unproject(nx, ny, w, out);
    };
    auto excluded = [&](const DVec3& p) {
        for (const Cylinder& c : exclude) {
            const double dx = p[h0_] - c.base[h0_], dz = p[h1_] - c.base[h1_];
            const double dy = p[up_] - c.base[up_];
            if (dx * dx + dz * dz < double(c.radius) * c.radius && dy > -c.below && dy < c.height) return true;
        }
        for (const ExcludeBox& b : boxes)
            if (p.x >= b.lo.x && p.x <= b.hi.x && p.y >= b.lo.y && p.y <= b.hi.y && p.z >= b.lo.z && p.z <= b.hi.z)
                return true;
        return false;
    };

    for (int y = 0; y < H; y += s) {
        for (int x = 0; x < W; x += s) {
            DVec3 P;
            float w;
            if (!pointAt(x, y, P, w)) continue;
            const double fx = P[h0_] - focus[h0_], fz = P[h1_] - focus[h1_];
            if (fx * fx + fz * fz > range2) continue;
            if (excluded(P)) {
                ++st.excluded;
                continue;
            }
            // Neighbours on the same surface (no silhouette jump) for the normal.
            const float jump = p_.edgeJump * w + 0.02f;
            DVec3 A, B;
            float wa, wb;
            Vec3 dx, dy;
            if (pointAt(x + s, y, A, wa) && std::fabs(wa - w) < jump) dx = (A - P).ToFloat();
            else if (pointAt(x - s, y, A, wa) && std::fabs(wa - w) < jump) dx = (P - A).ToFloat();
            else {
                ++st.edges;
                continue;
            }
            if (pointAt(x, y + s, B, wb) && std::fabs(wb - w) < jump) dy = (B - P).ToFloat();
            else if (pointAt(x, y - s, B, wb) && std::fabs(wb - w) < jump) dy = (P - B).ToFloat();
            else {
                ++st.edges;
                continue;
            }
            Vec3 n = Normalize(Cross(dx, dy));
            if (Length(n) < 0.5f) continue;
            if (Dot(n, (cam - P).ToFloat()) < 0.0f) n = -n;
            const float nu = n[up_];
            int64_t ci, cj;
            CellIndex(P, ci, cj);
            if (st.motionUsed && moving[size_t(y) * W + x]) {
                ++st.moving;
                auto it = cells_.find(KeyOf(ci, cj));
                if (it != cells_.end()) Unsettle(it->second, P, nu, Near(P, focus));
                continue;
            }
            ++st.samples;
            const double ox = P[h0_] - double(ci) * p_.cell, oz = P[h1_] - double(cj) * p_.cell;
            if (nu > 0.5f) {
                AddFloor(cells_[KeyOf(ci, cj)], float(P[up_]), ox, oz);
                ++st.floorSamples;
            } else if (std::fabs(nu) <= 0.5f) {
                AddWall(cells_[KeyOf(ci, cj)], float(P[up_]), n, ox, oz);
                ++st.wallSamples;
            }
            // Ceilings (undersides of awnings, bridges) aren't needed by SM64.
        }
    }
    Carve(f, focus, exclude, boxes);

    // Forget cells far behind Mario.
    if ((st.frames & 15) == 0) {
        const double forget2 = double(p_.forgetDistance) * p_.forgetDistance;
        for (auto it = cells_.begin(); it != cells_.end();) {
            const int64_t i = int64_t(int32_t(uint32_t(it->first >> 32))), j = int64_t(int32_t(uint32_t(it->first)));
            const DVec3 c = CellCentre(i, j, 0);
            const double dx = c[h0_] - focus[h0_], dz = c[h1_] - focus[h1_];
            if (dx * dx + dz * dz > forget2) it = cells_.erase(it);
            else ++it;
        }
    }
    st.cells = int(cells_.size());
    st.floorLayers = st.wallCells = 0;
    for (const auto& kv : cells_) {
        st.floorLayers += kv.second.nFloor;
        if (WallSolid(kv.second)) ++st.wallCells;
    }
}

void WorldModel::Carve(const DepthFrame& f, const DVec3& focus, const std::vector<Cylinder>& exclude,
                       const std::vector<ExcludeBox>& boxes) {
    const int W = f.width, H = f.height;
    int64_t i0, j0;
    CellIndex(focus, i0, j0);
    const int r = int(std::ceil(p_.carveRadius / p_.cell));
    // Bodies in the frame (Mario, a visible hero) need no special case here: in
    // front of a cell they hide it (no evidence either way), and a cell they
    // stand next to is judged like any other - 0.2 skipped them, so a phantom
    // wall Mario was pushing against could never be seen through and removed.
    (void)exclude;
    (void)boxes;
    // >0: seen through it, 0: seen on it, <0: hidden or off screen.
    auto test = [&](const DVec3& q) -> int {
        double nx, ny, w;
        if (!f.view.Project(q, nx, ny, w) || w < 0.3) return -1;
        const double px = (nx + 1.0) * 0.5 * W, py = (1.0 - ny) * 0.5 * H;
        if (px < 1 || py < 1 || px >= W - 1 || py >= H - 1) return -1;
        // Nearest surface around the pixel: a point on a silhouette edge must
        // not count as "seen through" because the pixel next to it shows the
        // background.
        float d = 1e30f;
        const int cx = int(px), cy = int(py);
        for (int yy = cy - 1; yy <= cy + 1; ++yy)
            for (int xx = cx - 1; xx <= cx + 1; ++xx) {
                const float v = f.depth[size_t(yy) * W + size_t(xx)];
                d = std::min(d, v > 0.05f ? v : 1e30f); // nothing rendered = infinitely far
            }
        const double tol = p_.carveTolerance + 0.03 * w;
        if (d > w + tol) return 1; // sky or something further away
        if (std::fabs(d - w) <= tol) return 0;
        return -1;
    };
    for (int64_t i = i0 - r; i <= i0 + r; ++i) {
        for (int64_t j = j0 - r; j <= j0 + r; ++j) {
            auto it = cells_.find(KeyOf(i, j));
            if (it == cells_.end()) continue;
            Cell& c = it->second;
            for (int k = 0; k < c.nFloor;) {
                Layer& l = c.floor[k];
                DVec3 q = CellCentre(i, j, l.h + 0.02);
                if (l.sw > 0) {
                    q[h0_] = double(i) * p_.cell + l.sx / l.sw;
                    q[h1_] = double(j) * p_.cell + l.sz / l.sw;
                }
                const int t = test(q);
                if (t > 0) {
                    ++l.misses;
                    if (l.misses >= p_.forgetMisses && l.misses * 2 > l.hits) {
                        for (int m = k; m + 1 < c.nFloor; ++m) c.floor[m] = c.floor[m + 1];
                        --c.nFloor;
                        ++stats_.carved;
                        if (Near(q, focus)) ++stats_.nearChanges;
                        continue;
                    }
                } else if (t == 0) {
                    if (l.misses) --l.misses;
                }
                ++k;
            }
            if (c.wallHits > 0 && c.wallSamples > 0) {
                const double cx = double(i) * p_.cell + c.cx / c.wallSamples;
                const double cz = double(j) * p_.cell + c.cz / c.wallSamples;
                // Look at the column every 0.25 m: any height where the wall
                // is seen confirms it; otherwise two heights seen through (or
                // all of them) are a miss. Parts hidden behind something (a
                // ledge Mario stands on, Mario himself) don't count either way
                // - 0.2 judged the middle and both ends only, so a phantom
                // whose lower half was hidden by the platform in front of it
                // could never go. The bottom 0.35 m is left out: the ground
                // right behind a wall's foot is within the depth tolerance of
                // it, so it would always look "seen". (Kerbs and other short
                // columns: their middle, as before.)
                const double lo = c.wallLo, hi = std::min(double(c.wallHi), double(c.wallLo) + 3.0);
                auto at = [&](double h) {
                    DVec3 q;
                    q[h0_] = cx;
                    q[h1_] = cz;
                    q[up_] = h;
                    return test(q);
                };
                int on = 0, through = 0, hidden = 0;
                const double from = lo + 0.35, top = hi - 0.05;
                if (top - from < 0.1) {
                    const int r = at(0.5 * (lo + hi));
                    on = r == 0;
                    through = (r > 0) * 2;
                } else {
                    const int steps = std::min(10, int(std::ceil((top - from) / 0.25)));
                    for (int k = 0; k <= steps; ++k) {
                        const int r = at(from + (top - from) * double(k) / steps);
                        if (r == 0) ++on;
                        else if (r > 0) ++through;
                        else ++hidden;
                    }
                }
                int t = -1;
                if (on > 0) t = 0;
                else if (through >= 2 || (through >= 1 && hidden == 0)) t = 1;
                if (t > 0) {
                    if (++c.wallMisses >= p_.forgetMisses && c.wallMisses * 2 > c.wallHits) {
                        c.wallHits = c.wallMisses = 0;
                        c.wallLo = 1e30f;
                        c.wallHi = -1e30f;
                        c.wallSamples = 0;
                        c.nx = c.nz = c.cx = c.cz = 0;
                        ++stats_.carved;
                        if (Near(CellCentre(i, j, 0), focus)) ++stats_.nearChanges;
                    }
                } else if (t == 0 && c.wallMisses) {
                    --c.wallMisses;
                }
            }
        }
    }
}

std::string WorldModel::DebugString(const DVec3& p, int r) const {
    std::string out;
    char line[256];
    int64_t i0, j0;
    CellIndex(p, i0, j0);
    for (int64_t i = i0 - r; i <= i0 + r; ++i)
        for (int64_t j = j0 - r; j <= j0 + r; ++j) {
            auto it = cells_.find(KeyOf(i, j));
            if (it == cells_.end()) continue;
            const Cell& c = it->second;
            const DVec3 cc = CellCentre(i, j, 0);
            std::snprintf(line, sizeof(line), "    cell (%.3f %.3f): floors", cc[h0_], cc[h1_]);
            out += line;
            for (int k = 0; k < c.nFloor; ++k) {
                std::snprintf(line, sizeof(line), " %.2f(h%d m%d)", c.floor[k].h, c.floor[k].hits, c.floor[k].misses);
                out += line;
            }
            if (c.wallHits) {
                std::snprintf(line, sizeof(line), " | wall %.2f..%.2f hits %d miss %d n (%.2f %.2f) at (%.3f %.3f)",
                              c.wallLo, c.wallHi, c.wallHits, c.wallMisses, c.nx / std::max(1.0f, c.wallSamples),
                              c.nz / std::max(1.0f, c.wallSamples),
                              double(i) * p_.cell + c.cx / std::max(1e-6f, c.wallSamples),
                              double(j) * p_.cell + c.cz / std::max(1e-6f, c.wallSamples));
                out += line;
            }
            out += "\n";
        }
    return out;
}

void WorldModel::DebugDump(const DVec3& p, int r) const { std::fputs(DebugString(p, r).c_str(), stdout); }

bool WorldModel::CellFloor(const Cell& c, double top, double bottom, double& h) const {
    for (int k = c.nFloor - 1; k >= 0; --k) {
        const Layer& l = c.floor[k];
        if (l.hits < p_.minFloorHits) continue;
        if (l.h <= top && l.h >= bottom) {
            h = l.h;
            return true;
        }
    }
    return false;
}

bool WorldModel::CellFloorAt(const DVec3& p, double top, double bottom, double& height) const {
    int64_t ci, cj;
    CellIndex(p, ci, cj);
    auto c = cells_.find(KeyOf(ci, cj));
    return c != cells_.end() && CellFloor(c->second, top, bottom, height);
}

bool WorldModel::FloorBelow(const DVec3& p, double& height) const {
    RayHit hit;
    DVec3 to = p;
    to[up_] -= 200.0;
    if (!Cast(p, to, hit)) return false;
    height = hit.point[up_];
    return true;
}

bool WorldModel::Cast(const DVec3& from, const DVec3& to, RayHit& hit) const {
    const DVec3 d = to - from;
    const double hlen = std::sqrt(d[h0_] * d[h0_] + d[h1_] * d[h1_]);
    if (hlen < 1e-6) {
        if (d[up_] >= 0) return false; // no ceilings
        const double top = from[up_], bottom = to[up_];
        int64_t ci, cj;
        CellIndex(from, ci, cj);
        double h;
        bool found = false;
        auto c = cells_.find(KeyOf(ci, cj));
        if (c != cells_.end()) found = CellFloor(c->second, top, bottom, h);
        if (!found) {
            // Fill small holes (under Mario, behind thin poles) from the nearest neighbours.
            const int r = std::max(1, int(std::ceil(p_.holeFill / p_.cell)));
            double bestD = 1e30;
            for (int64_t i = ci - r; i <= ci + r; ++i)
                for (int64_t j = cj - r; j <= cj + r; ++j) {
                    if (i == ci && j == cj) continue;
                    auto n = cells_.find(KeyOf(i, j));
                    if (n == cells_.end()) continue;
                    double nh;
                    if (!CellFloor(n->second, top, bottom, nh)) continue;
                    const double dd = double((i - ci) * (i - ci) + (j - cj) * (j - cj)) - nh * 1e-6;
                    if (dd < bestD) {
                        bestD = dd;
                        h = nh;
                        found = true;
                    }
                }
        }
        if (!found && fbRadius_ > 0) {
            const double dx = from[h0_] - fbCentre_[h0_], dz = from[h1_] - fbCentre_[h1_];
            if (dx * dx + dz * dz < fbRadius_ * fbRadius_ && fbHeight_ <= top && fbHeight_ >= bottom) {
                h = fbHeight_;
                found = true;
            }
        }
        if (!found) return false;
        hit.point = from;
        hit.point[up_] = h;
        hit.normal = Vec3(0, 0, 0);
        hit.normal[up_] = 1.0f;
        return true;
    }

    // Horizontal / oblique: walk the cells the segment crosses (2D DDA).
    const double cell = p_.cell;
    const double sx = from[h0_] / cell, sz = from[h1_] / cell;
    const double dx = d[h0_] / cell, dz = d[h1_] / cell;
    int64_t i = int64_t(std::floor(sx)), j = int64_t(std::floor(sz));
    const int stepI = dx > 0 ? 1 : (dx < 0 ? -1 : 0), stepJ = dz > 0 ? 1 : (dz < 0 ? -1 : 0);
    const double tdI = stepI ? std::fabs(1.0 / dx) : 1e30, tdJ = stepJ ? std::fabs(1.0 / dz) : 1e30;
    double tI = stepI > 0 ? (double(i + 1) - sx) / dx : (stepI < 0 ? (sx - double(i)) / -dx : 1e30);
    double tJ = stepJ > 0 ? (double(j + 1) - sz) / dz : (stepJ < 0 ? (sz - double(j)) / -dz : 1e30);
    double tEnter = 0;
    int enteredAxis = -1; // 0: crossed an i boundary, 1: j boundary
    bool first = true;
    for (int guard = 0; guard < 4096 && tEnter <= 1.0; ++guard) {
        const double tExit = std::min(std::min(tI, tJ), 1.0);
        auto it = cells_.find(KeyOf(i, j));
        if (it != cells_.end() && WallSolid(it->second)) {
            const Cell& c = it->second;
            const double lo = double(c.wallLo) - p_.wallBottomMargin, hi = double(c.wallHi) + p_.wallTopMargin;
            const double ya = from[up_] + d[up_] * tEnter, yb = from[up_] + d[up_] * tExit;
            const bool overlaps = std::max(ya, yb) >= lo && std::min(ya, yb) <= hi;
            if (overlaps) {
                const double nl = std::sqrt(double(c.nx) * c.nx + double(c.nz) * c.nz);
                if (nl > 0.5 * c.wallSamples) {
                    // A facade: hit its plane (through the samples' centroid).
                    const double nhx = c.nx / nl, nhz = c.nz / nl;
                    const double cxw = double(i) * cell + c.cx / c.wallSamples;
                    const double czw = double(j) * cell + c.cz / c.wallSamples;
                    const double s0 = (from[h0_] - cxw) * nhx + (from[h1_] - czw) * nhz; // >0: in front
                    const double den = d[h0_] * nhx + d[h1_] * nhz;
                    // Rays that start behind the face (inside the solid) or move
                    // away from it pass through (the builder's convention).
                    if (den < -1e-9 && !(first && s0 < 0)) {
                        const double tp = s0 / -den;
                        if (tp <= tExit) {
                            hit.point = from + d * std::max(tp, tEnter);
                            hit.normal = Vec3(0, 0, 0);
                            hit.normal[h0_] = float(nhx);
                            hit.normal[h1_] = float(nhz);
                            return true;
                        }
                    }
                } else if (!first) {
                    // Thin or round things (poles): solid from the edge we crossed.
                    hit.point = from + d * tEnter;
                    hit.normal = Vec3(0, 0, 0);
                    if (enteredAxis == 0) hit.normal[h0_] = float(-stepI);
                    else hit.normal[h1_] = float(-stepJ);
                    return true;
                }
            }
        }
        first = false;
        if (tExit >= 1.0) break;
        tEnter = tExit;
        if (tI < tJ) {
            i += stepI;
            tI += tdI;
            enteredAxis = 0;
        } else {
            j += stepJ;
            tJ += tdJ;
            enteredAxis = 1;
        }
    }
    return false;
}

} // namespace sm2m
