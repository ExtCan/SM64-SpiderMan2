#include "physics_world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sm2m {

namespace {
constexpr float kPi = 3.14159265f;
constexpr size_t kMaxWalls = 4096;
constexpr size_t kMaxColumns = 6000;
constexpr double kLostAfter = 1.5; // s without an answer: asked again

double Dist2(const DVec3& a, const DVec3& b) {
    const DVec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

// Distance from p to the segment a..b.
double SegmentDistance(const DVec3& p, const DVec3& a, const DVec3& b) {
    const DVec3 ab = b - a;
    const double len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    double t = 0;
    if (len2 > 1e-12) {
        const DVec3 ap = p - a;
        t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
        t = std::max(0.0, std::min(1.0, t));
    }
    return std::sqrt(Dist2(p, a + ab * t));
}
} // namespace

void PhysicsWorld::Configure(const PhysicsWorldParams& p, int upAxis) {
    p_ = p;
    p_.cell = std::max(0.1f, p_.cell);
    p_.radius = std::max(2, std::min(40, p_.radius));
    p_.columnHits = std::max(1, std::min(kMaxGameHits, p_.columnHits));
    p_.ringDirs = std::max(8, std::min(128, p_.ringDirs));
    p_.ringHits = std::max(1, std::min(kMaxGameHits, p_.ringHits));
    up_ = upAxis == 2 ? 2 : 1;
    ha_ = 0;
    hb_ = up_ == 1 ? 2 : 1;
    Reset();
}

void PhysicsWorld::Reset() {
    columns_.clear();
    walls_.clear();
    pending_.clear();
    ringTime_.assign(size_t(p_.ringDirs) * 3, -1e9);
    waterAsked_ = -1e9;
    waterTime_ = -1e9;
    waterKnown_ = false;
    changed_ = true;
    stats_ = PhysicsWorldStats{};
}

uint64_t PhysicsWorld::Key(int64_t ix, int64_t iz) {
    return (uint64_t(uint32_t(int32_t(ix))) << 32) | uint64_t(uint32_t(int32_t(iz)));
}

int64_t PhysicsWorld::ColumnIndex(double v) const { return int64_t(std::floor(v / double(p_.cell) + 0.5)); }

void PhysicsWorld::Horizontal(const DVec3& v, double& a, double& b) const {
    a = v[ha_];
    b = v[hb_];
}

DVec3 PhysicsWorld::Point(double a, double b, double up) const {
    DVec3 p;
    p[ha_] = a;
    p[hb_] = b;
    p[up_] = up;
    return p;
}

const PhysicsWorld::Column* PhysicsWorld::FindColumn(int64_t ix, int64_t iz) const {
    auto it = columns_.find(Key(ix, iz));
    return it == columns_.end() ? nullptr : &it->second;
}

void PhysicsWorld::Expire(const DVec3& feet, double now) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.time > kLostAfter) {
            if (it->second.purpose == Purpose::Column) {
                auto c = columns_.find(it->second.column);
                if (c != columns_.end()) c->second.asked = -1;
            } else if (it->second.purpose == Purpose::WallTop) {
                for (Wall& w : walls_)
                    if (w.id == it->second.wall) w.topAsked = -1;
            }
            ++stats_.lost;
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    const double keep = double(p_.radius) * p_.cell + 8.0;
    walls_.erase(std::remove_if(walls_.begin(), walls_.end(),
                                [&](const Wall& w) {
                                    const double life = w.kind == HitKind::Movable ? 0.6 : p_.wallLife;
                                    if (now - w.time > life) return true;
                                    double a, b, fa, fb;
                                    Horizontal(w.pos, a, b);
                                    Horizontal(feet, fa, fb);
                                    return (a - fa) * (a - fa) + (b - fb) * (b - fb) > keep * keep;
                                }),
                 walls_.end());
    if (columns_.size() > kMaxColumns) {
        double fa, fb;
        Horizontal(feet, fa, fb);
        const int64_t cx = ColumnIndex(fa), cz = ColumnIndex(fb);
        const int64_t far = p_.radius + 16;
        for (auto it = columns_.begin(); it != columns_.end();) {
            const int64_t ix = int64_t(int32_t(uint32_t(it->first >> 32)));
            const int64_t iz = int64_t(int32_t(uint32_t(it->first & 0xFFFFFFFFu)));
            if (std::llabs(ix - cx) > far || std::llabs(iz - cz) > far) it = columns_.erase(it);
            else ++it;
        }
    }
}

void PhysicsWorld::Schedule(const DVec3& feet, const Vec3& velocity, double now, int budget, std::vector<GameRay>& out) {
    lastFeet_ = feet;
    Expire(feet, now);
    if (budget <= 0) return;
    const double feetUp = Up(feet);
    DVec3 upDir;
    upDir[up_] = 1;

    auto emit = [&](const DVec3& from, const DVec3& to, uint8_t type, int maxHits, const Pending& pd) {
        // (Tags with the top bit set are other rays': the camera's.)
        if (nextTag_ == 0 || nextTag_ >= 0x80000000u) nextTag_ = 1;
        const uint32_t tag = nextTag_++;
        Pending p = pd;
        p.from = from;
        p.to = to;
        p.time = now;
        pending_[tag] = p;
        GameRay r;
        r.from = from;
        r.to = to;
        r.tag = tag;
        r.type = type;
        r.maxHits = uint8_t(std::max(1, std::min(kMaxGameHits, maxHits)));
        out.push_back(r);
        ++stats_.raysSent;
        --budget;
    };
    auto castColumn = [&](int64_t ix, int64_t iz) {
        Column& c = columns_[Key(ix, iz)];
        const double a = double(ix) * p_.cell, b = double(iz) * p_.cell;
        Pending pd;
        pd.purpose = Purpose::Column;
        pd.column = Key(ix, iz);
        c.asked = now;
        if (c.partial && c.time >= 0 && c.below > c.lo) {
            // On down from where the last ray stopped.
            pd.followUp = true;
            pd.columnTime = c.time;
            emit(Point(a, b, c.below), Point(a, b, c.lo), p_.floorType, p_.columnHits, pd);
            return;
        }
        emit(Point(a, b, feetUp + p_.castAbove), Point(a, b, feetUp - p_.castBelow), p_.floorType, p_.columnHits, pd);
    };
    auto needs = [&](const Column* c, double dist) -> int {
        // 0: no, 1: refresh, 2: never asked / doesn't cover Mario's height / not known all the way down
        if (c && c->asked >= 0 && now - c->asked < kLostAfter) return 0; // in flight
        if (!c || c->time < 0) return 2;
        if (feetUp + 3.0 > c->hi || feetUp - 6.0 < c->lo) return 2;
        if (c->partial) return 2;
        const double age = now - c->time;
        if (c->movable && dist < double(p_.radius) * p_.cell && age > p_.movableRefresh) return 1;
        if (dist < p_.nearRadius && age > p_.nearRefresh) return 1;
        if (age > p_.farRefresh) return 1;
        return 0;
    };

    double fa, fb;
    Horizontal(feet, fa, fb);

    // 1) The ground right under Mario, before anything else.
    {
        const int64_t ix = ColumnIndex(fa), iz = ColumnIndex(fb);
        for (int dz = -1; dz <= 1 && budget > 0; ++dz)
            for (int dx = -1; dx <= 1 && budget > 0; ++dx)
                if (needs(FindColumn(ix + dx, iz + dz), 0.0) == 2) castColumn(ix + dx, iz + dz);
    }

    // 2) Water under Mario.
    bool waterPending = false;
    for (const auto& kv : pending_)
        if (kv.second.purpose == Purpose::Water) waterPending = true;
    if (budget > 0 && !waterPending && now - waterAsked_ > p_.waterRefresh) {
        Pending pd;
        pd.purpose = Purpose::Water;
        emit(feet + upDir * 8.0, feet - upDir * 40.0, p_.waterType, 1, pd);
        waterAsked_ = now;
    }

    // 3) The ring: the oldest slots first, at most half the budget.
    {
        const int slots = int(ringTime_.size());
        std::vector<int> due;
        due.reserve(size_t(slots));
        for (int i = 0; i < slots; ++i)
            if (now - ringTime_[size_t(i)] > p_.ringRefresh) due.push_back(i);
        std::sort(due.begin(), due.end(), [&](int x, int y) { return ringTime_[size_t(x)] < ringTime_[size_t(y)]; });
        int ringBudget = std::max(1, budget / 2);
        for (int slot : due) {
            if (budget <= 0 || ringBudget <= 0) break;
            const int dir = slot / 3, level = slot % 3;
            const double ang = 2.0 * kPi * double(dir) / double(p_.ringDirs);
            const double ca = std::cos(ang), sa = std::sin(ang);
            const double h = p_.ringHeights[level];
            const DVec3 from = Point(fa, fb, feetUp + h);
            const DVec3 to = Point(fa + ca * p_.ringRadius, fb + sa * p_.ringRadius, feetUp + h);
            Pending pd;
            pd.purpose = Purpose::Ring;
            pd.ringSlot = slot;
            pd.base = feetUp;
            emit(from, to, p_.wallType, p_.ringHits, pd);
            ringTime_[size_t(slot)] = now;
            --ringBudget;
        }
    }

    // 4) The tops of new walls: a ray down just behind the face (a railing's
    //    top rail, a planter's rim); none = a tall wall.
    {
        int tops = 0;
        for (Wall& w : walls_) {
            if (budget <= 0 || tops >= 4) break;
            if (w.topKnown || w.topAsked >= 0 || w.kind == HitKind::Character) continue;
            const double up = Up(w.pos);
            double wa, wb;
            Horizontal(w.pos, wa, wb);
            const double na = w.n[ha_], nb = w.n[hb_];
            const double ba = wa - na * 0.04, bb = wb - nb * 0.04;
            Pending pd;
            pd.purpose = Purpose::WallTop;
            pd.wall = w.id;
            w.topAsked = now;
            emit(Point(ba, bb, up + 3.0), Point(ba, bb, up - 0.1), p_.wallType, 2, pd);
            ++tops;
        }
    }

    // 5) The columns of the floor grid, where Mario is heading first.
    if (budget > 0) {
        double va = 0, vb = 0;
        Horizontal(DVec3(velocity.x, velocity.y, velocity.z), va, vb);
        double la = va * 0.3, lb = vb * 0.3;
        const double ll = std::sqrt(la * la + lb * lb);
        if (ll > 3.0) {
            la *= 3.0 / ll;
            lb *= 3.0 / ll;
        }
        const double ca = fa + la, cb = fb + lb;
        const int64_t cx = ColumnIndex(ca), cz = ColumnIndex(cb);
        const int R = p_.radius;
        struct Cand {
            double score;
            int64_t ix, iz;
        };
        std::vector<Cand> cands;
        cands.reserve(size_t(2 * R + 1) * size_t(2 * R + 1));
        for (int64_t iz = cz - R; iz <= cz + R; ++iz) {
            for (int64_t ix = cx - R; ix <= cx + R; ++ix) {
                const double a = double(ix) * p_.cell, b = double(iz) * p_.cell;
                const double d = std::sqrt((a - fa) * (a - fa) + (b - fb) * (b - fb));
                const int need = needs(FindColumn(ix, iz), d);
                if (!need) continue;
                const bool nearM = d < p_.nearRadius;
                const double tier = need == 2 ? (nearM ? 0 : 2) : (nearM ? 1 : 3);
                cands.push_back({tier * 1000.0 + d, ix, iz});
            }
        }
        const size_t take = std::min(cands.size(), size_t(budget));
        std::partial_sort(cands.begin(), cands.begin() + long(take), cands.end(),
                          [](const Cand& x, const Cand& y) { return x.score < y.score; });
        for (size_t i = 0; i < take; ++i) castColumn(cands[i].ix, cands[i].iz);
    }
    stats_.pending = pending_.size();
}

void PhysicsWorld::Carve(const DVec3& from, const DVec3& to, double now) {
    (void)now;
    walls_.erase(std::remove_if(walls_.begin(), walls_.end(),
                                [&](const Wall& w) { return SegmentDistance(w.pos, from, to) < 0.12; }),
                 walls_.end());
}

void PhysicsWorld::CapWalls(const DVec3& from, const DVec3& dir, double clear, double up) {
    if (clear <= 0.05) return;
    double fa, fb, da, db;
    Horizontal(from, fa, fb);
    Horizontal(dir, da, db);
    const double dl = std::sqrt(da * da + db * db);
    if (dl < 1e-6) return;
    da /= dl;
    db /= dl;
    for (Wall& w : walls_) {
        const double hitUp = Up(w.pos);
        if (hitUp >= up - 0.05) continue; // found at or above this height: not under the ray
        if (w.topKnown && w.top <= up - 0.1) continue;
        // The wall's face: centre (pa, pb), along t = (-nb, na), +-half.
        double pa, pb;
        Horizontal(w.pos, pa, pb);
        const double ta = -w.n[hb_], tb = w.n[ha_];
        // from + s*d = p + u*t  ->  s*d - u*t = p - from
        const double ra = pa - fa, rb = pb - fb;
        const double det = da * (-tb) - db * (-ta);
        if (std::fabs(det) < 1e-6) continue; // parallel
        const double sRay = (ra * (-tb) - rb * (-ta)) / det;
        const double uFace = (da * rb - db * ra) / det;
        if (sRay < 0.0 || sRay > clear || std::fabs(uFace) > double(w.half) + 0.05) continue;
        // A wall seen at one point alone (a lamp post: narrower than the patch
        // between two ring rays) is only known to be there: a ray going past
        // beside that point says nothing about its top. (A long wall has more
        // of it seen along its face, even at its ends.)
        if (std::fabs(uFace) > 0.2 && WallAlone(w)) continue;
        w.top = up - 0.1;
        w.topKnown = true;
        changed_ = true;
    }
}

// No other piece of the same face (same facing, same plane, the same kind
// of thing) seen beside it, as far along it as the next ring ray would meet
// it.
bool PhysicsWorld::WallAlone(const Wall& w) const {
    double pa, pb;
    Horizontal(w.pos, pa, pb);
    const double ta = -w.n[hb_], tb = w.n[ha_];
    for (const Wall& o : walls_) {
        if (&o == &w || o.kind != w.kind || Dot(o.n, w.n) < 0.9f) continue;
        double oa, ob;
        Horizontal(o.pos, oa, ob);
        const double along = std::fabs((oa - pa) * ta + (ob - pb) * tb);
        const double off = (oa - pa) * double(w.n[ha_]) + (ob - pb) * double(w.n[hb_]);
        if (std::fabs(off) <= 0.25 && along > 0.15 && along < double(std::max(w.gap, o.gap)) + 0.3) return false;
    }
    return true;
}

void PhysicsWorld::AddWall(const GameHit& h, const DVec3& dir, double dist, double base, double now) {
    // Horizontal normal, facing back along the ray.
    double na = h.normal[ha_], nb = h.normal[hb_];
    const double len = std::sqrt(na * na + nb * nb);
    if (len < 0.3) return;
    na /= len;
    nb /= len;
    double da, db;
    Horizontal(dir, da, db);
    const double facing = na * da + nb * db;
    if (std::fabs(facing) < 0.2) return; // grazing
    if (facing > 0) {
        // The back of a face (the game's meshes answer from both sides): the
        // wall Mario meets is the side towards him.
        na = -na;
        nb = -nb;
    }
    Vec3 n;
    n[ha_] = float(na);
    n[hb_] = float(nb);
    // The other side of the same wall, seen before from over there (Mario got
    // past it): SM64 walls push Mario out to their front, so it must go.
    walls_.erase(std::remove_if(walls_.begin(), walls_.end(),
                                [&](const Wall& w) { return Dist2(w.pos, h.pos) < 0.3 * 0.3 && Dot(w.n, n) < -0.5f; }),
                 walls_.end());
    // Neighbouring ring rays are this far apart at this distance (across
    // them; along a face they meet at an angle, further apart).
    const double spacing = dist * 2.0 * kPi / double(p_.ringDirs);
    const float half = float(std::max(0.2, std::min(0.8, 0.5 * spacing + 0.1)));
    const float gap = float(std::min(4.0, spacing / std::max(0.2, std::fabs(facing))));
    for (Wall& w : walls_) {
        if (Dist2(w.pos, h.pos) < 0.2 * 0.2 && Dot(w.n, n) > 0.9f) {
            // (A top found for the old point still holds within 20 cm.)
            w.pos = h.pos;
            w.n = n;
            w.time = now;
            w.base = std::min(w.base, base);
            w.half = std::max(w.half, half);
            w.gap = std::max(w.gap, gap);
            w.material = h.material;
            w.kind = h.kind;
            w.actor = h.actorId;
            return;
        }
    }
    if (walls_.size() >= kMaxWalls) {
        auto oldest = std::min_element(walls_.begin(), walls_.end(),
                                       [](const Wall& x, const Wall& y) { return x.time < y.time; });
        walls_.erase(oldest);
    }
    Wall w;
    w.id = nextWall_++;
    w.pos = h.pos;
    w.n = n;
    w.half = half;
    w.gap = gap;
    w.base = base;
    w.material = h.material;
    w.kind = h.kind;
    w.actor = h.actorId;
    w.time = now;
    w.found = now;
    walls_.push_back(w);
}

void PhysicsWorld::Accept(const GameRayResult& answer, double now) {
    auto it = pending_.find(answer.tag);
    if (it == pending_.end()) return;
    // Only the world's own lava and acid burn Mario: actors' bodies can say
    // kAcid (0.4: every car roof threw him off like lava).
    GameRayResult r = answer;
    for (int i = 0; i < std::max(0, std::min(kMaxGameHits, r.count)); ++i)
        if (r.hits[i].actor && BurnsMario(r.hits[i].material)) {
            r.hits[i].material = -1;
            ++stats_.actorBurning;
        }
    const Pending pd = it->second;
    pending_.erase(it);
    ++stats_.results;
    const int count = std::max(0, std::min(kMaxGameHits, r.count));
    stats_.hits += uint64_t(count);

    switch (pd.purpose) {
    case Purpose::Water:
        // Only water counts (should the query ever answer with the ground,
        // Mario must not swim in the street).
        waterKnown_ = false;
        for (int i = 0; i < count; ++i) {
            // (Puddles and ankle-deep water are floors with wet footsteps.)
            if (r.hits[i].kind == HitKind::Character || !IsSwimmableWater(r.hits[i].material)) continue;
            waterKnown_ = true;
            waterLevel_ = Up(r.hits[i].pos);
            break;
        }
        waterTime_ = now;
        break;
    case Purpose::Ring: {
        const DVec3 d = pd.to - pd.from;
        const double len = std::sqrt(Dist2(pd.to, pd.from));
        const DVec3 dir = len > 1e-9 ? d * (1.0 / len) : DVec3(1, 0, 0);
        const GameHit* wall = nullptr;
        for (int i = 0; i < count; ++i) {
            if (r.hits[i].kind == HitKind::Character) continue;
            wall = &r.hits[i];
            break;
        }
        // Nothing is there any more between Mario and what the ray hit. (A
        // ray that ran out of hits on people doesn't know past the last one.)
        double clear = len;
        if (wall) clear = std::sqrt(Dist2(wall->pos, pd.from)) - 0.15;
        else if (count >= p_.ringHits && count > 0) clear = std::sqrt(Dist2(r.hits[count - 1].pos, pd.from)) - 0.15;
        if (clear > 0.05) {
            Carve(pd.from, pd.from + dir * clear, now);
            // Walls found lower down that this ray passed over end below it.
            CapWalls(pd.from, dir, clear, Up(pd.from));
        }
        if (wall) {
            const double dist = std::sqrt(Dist2(wall->pos, pd.from));
            const size_t before = walls_.size();
            AddWall(*wall, dir, dist, pd.base, now);
            if (walls_.size() != before) changed_ = true;
        }
        break;
    }
    case Purpose::WallTop: {
        for (Wall& w : walls_) {
            if (w.id != pd.wall) continue;
            w.topAsked = now;
            w.topKnown = false;
            for (int i = 0; i < count; ++i) {
                const GameHit& h = r.hits[i];
                if (h.kind == HitKind::Character || std::fabs(h.normal[up_]) < 0.3f) continue;
                w.top = Up(h.pos);
                w.topKnown = true;
                break;
            }
            // No top within 3 m: tall (a building), which is the default.
            changed_ = true;
            break;
        }
        break;
    }
    case Purpose::Column: {
        Column& c = columns_[pd.column];
        // The ray collected as many hits as it could: what is below the last
        // one is still to be asked (a follow-up ray, next).
        const bool full = count >= p_.columnHits;
        double lowest = 1e30;
        for (int i = 0; i < count; ++i) lowest = std::min(lowest, Up(r.hits[i].pos));
        auto byHeight = [](const Layer& x, const Layer& y) { return x.y > y.y; };
        if (pd.followUp) {
            // On down from an earlier answer (unless a fresh one replaced it).
            if (!c.partial || c.time != pd.columnTime) break;
            c.asked = -1;
            // What this ray finds below where the last one stopped replaces
            // what was kept there from the answer before - as far down as it
            // got (it may stop at its hit limit again).
            const double reached = full ? lowest : c.lo - 1.0;
            {
                int w = 0;
                for (int i = 0; i < c.count; ++i) {
                    const Layer& l = c.layers[i];
                    if (l.kept && l.y < c.below + 0.005 && l.y >= reached - 0.005) continue;
                    c.layers[w++] = l;
                }
                c.count = w;
            }
            for (int i = 0; i < count && c.count < kMaxColumnLayers; ++i) {
                const GameHit& h = r.hits[i];
                if (Up(h.pos) >= c.below + 0.005) continue; // (the hit it started at)
                Layer l;
                l.y = float(Up(h.pos));
                l.up = h.normal[up_];
                l.material = h.material;
                l.kind = h.kind;
                if (h.kind == HitKind::Movable) c.movable = true;
                c.layers[c.count++] = l;
            }
            std::sort(c.layers, c.layers + c.count, byHeight);
            ++c.followUps;
            const bool wasFilled = c.filled;
            c.partial = full && lowest > c.lo + 0.05 && c.count < kMaxColumnLayers && c.followUps < 3;
            if (c.partial) {
                c.below = lowest - 0.02;
                ++stats_.partials;
            }
            c.filled = c.partial && wasFilled;
            changed_ = true;
            break;
        }
        Column next;
        next.time = now;
        next.asked = -1;
        next.lo = std::min(Up(pd.from), Up(pd.to));
        next.hi = std::max(Up(pd.from), Up(pd.to));
        for (int i = 0; i < count; ++i) {
            const GameHit& h = r.hits[i];
            Layer l;
            l.y = float(Up(h.pos));
            l.up = h.normal[up_];
            l.material = h.material;
            l.kind = h.kind;
            if (h.kind == HitKind::Movable) next.movable = true;
            next.layers[next.count++] = l;
        }
        std::sort(next.layers, next.layers + next.count, byHeight);
        if (full && lowest > next.lo + 0.05) {
            next.partial = true;
            next.below = lowest - 0.02;
            ++stats_.partials;
        }
        // What the answer before had and this one doesn't. Only for the
        // static world (cars and props come and go):
        //  - below where this ray stopped at its hit limit: kept until its
        //    follow-up says (0.5 dropped it: a hole under Mario for a frame or
        //    two each time a column under an awning or a tree was refreshed);
        //  - anywhere else it covered: kept once - a ray can slip through a
        //    seam between two pieces of floor - and gone if the next answer
        //    doesn't have it either.
        if (c.time >= 0 && !c.movable && !next.movable) {
            bool filled = next.partial && c.Known();
            for (int i = 0; i < c.count && next.count < kMaxColumnLayers; ++i) {
                const Layer& o = c.layers[i];
                if (o.kind != HitKind::World || o.y < next.lo - 0.001 || o.y > next.hi + 0.001) continue;
                bool matched = false;
                for (int k = 0; k < next.count && !matched; ++k)
                    matched = !next.layers[k].kept && std::fabs(next.layers[k].y - o.y) < 0.05f;
                if (matched) continue;
                const bool unasked = next.partial && o.y < next.below + 0.005;
                if (!unasked && o.kept) continue; // missed twice: it's gone
                Layer k = o;
                k.kept = true;
                next.layers[next.count++] = k;
                ++stats_.keptLayers;
            }
            std::sort(next.layers, next.layers + next.count, byHeight);
            next.filled = filled;
        }
        bool same = c.time >= 0 && c.count == next.count && next.partial == c.partial;
        for (int i = 0; same && i < next.count; ++i)
            same = std::fabs(c.layers[i].y - next.layers[i].y) < 0.02f && c.layers[i].material == next.layers[i].material;
        if (!same) changed_ = true;
        c = next;
        break;
    }
    }
    stats_.pending = pending_.size();
}

bool PhysicsWorld::TakeChanged() {
    const bool c = changed_;
    changed_ = false;
    return c;
}

bool PhysicsWorld::Ready(const DVec3& feet) const {
    double fa, fb;
    Horizontal(feet, fa, fb);
    const Column* c = FindColumn(ColumnIndex(fa), ColumnIndex(fb));
    return c && c->Known();
}

bool PhysicsWorld::Water(double& level) const {
    if (!waterKnown_) return false;
    level = waterLevel_;
    return true;
}

int PhysicsWorld::FloorMaterialAt(const DVec3& feet) const {
    double fa, fb;
    Horizontal(feet, fa, fb);
    const Column* c = FindColumn(ColumnIndex(fa), ColumnIndex(fb));
    if (!c || c->time < 0) return -2;
    const double up = Up(feet);
    for (int i = 0; i < c->count; ++i)
        if (std::fabs(c->layers[i].up) > 0.3f && c->layers[i].kind != HitKind::Character && c->layers[i].y <= up + 0.3)
            return c->layers[i].material;
    return -2;
}

const PhysicsWorldStats& PhysicsWorld::Stats() const {
    stats_.columns = columns_.size();
    stats_.walls = walls_.size();
    stats_.pending = pending_.size();
    return stats_;
}

void PhysicsWorld::Build(const WorldMapping& map, const Vec3& m, const CollisionParams& cp, std::vector<SM64Surface>& out,
                         CollisionStats& stats, std::vector<SurfaceOrigin>* origins) const {
    out.clear();
    if (origins) origins->clear();
    // Every surface added since the last call came from `o`.
    auto tag = [&](SurfaceOrigin::Kind kind, uint32_t wall = 0) {
        if (!origins) return;
        SurfaceOrigin o;
        o.kind = kind;
        o.wall = wall;
        origins->resize(out.size(), o);
    };
    stats = CollisionStats{};
    const float s = float(map.Scale());
    const float minClear = cp.minClearance;
    const float step = cp.stepHeight;

    // The floor grid: vertices on the world columns nearest Mario.
    const DVec3 mg = map.ToGame(m);
    double fa, fb;
    Horizontal(mg, fa, fb);
    const int64_t cx = ColumnIndex(fa), cz = ColumnIndex(fb);
    FloorGrid g;
    g.R = p_.radius;
    g.cell = p_.cell * s;
    {
        const Vec3 c = map.ToLocal(Point(double(cx) * p_.cell, double(cz) * p_.cell, Up(mg)));
        g.cx = c.x;
        g.cz = c.z;
    }
    g.v.assign(size_t(g.N()) * size_t(g.N()), GridVertex());
    std::vector<uint8_t> known(g.v.size(), 0); // the game has answered for this vertex's column
    const double top = Up(mg) + double(cp.probeUp);
    const double bottom = Up(mg) - double(cp.probeDown);
    for (int j = 0; j < g.N(); ++j) {
        for (int i = 0; i < g.N(); ++i) {
            // Which world column this vertex sits on (whatever the mapping's
            // axes and mirroring).
            const DVec3 vg = map.ToGame(Vec3(g.X(i), m.y, g.Z(j)));
            double va, vb;
            Horizontal(vg, va, vb);
            const Column* c = FindColumn(ColumnIndex(va), ColumnIndex(vb));
            if (!c || c->time < 0) continue;
            if (c->Known()) known[size_t(j) * size_t(g.N()) + size_t(i)] = 1;
            GridVertex& v = g.At(i, j);
            // Its lowest floor is only the ground there if the column is known
            // all the way down: otherwise it is whatever the ray reached before
            // its hit limit (an awning, a branch) - no step wall from that.
            v.lowKnown = c->Known();
            for (int k = 0; k < c->count && v.count < 4; ++k) {
                const Layer& l = c->layers[k];
                // Not a floor: a person, or a steep face. (Either side of a
                // flat face counts: the game's meshes answer from both sides,
                // and what Mario can't fit under is skipped next.)
                if (l.kind == HitKind::Character || std::fabs(l.up) <= 0.3f) continue;
                if (IsDeepWater(l.material)) continue; // he swims in it (the water level), not on it
                if (l.y > top || l.y < bottom) continue;
                // Mario must fit under whatever is above it (a car's roof,
                // a table): otherwise this floor is inside something.
                float above = 1e30f;
                for (int q = 0; q < k; ++q)
                    if (c->layers[q].kind != HitKind::Character && !IsDeepWater(c->layers[q].material))
                        above = std::min(above, c->layers[q].y);
                if (above - l.y < minClear) continue;
                const Vec3 lp = map.ToLocal(Point(va, vb, l.y));
                v.h[v.count] = lp.y;
                v.kind[v.count] = SurfaceForMaterial(l.material);
                ++v.count;
            }
        }
    }

    CollisionBuilder::EmitFloors(g, cp, s, out, stats);
    tag(SurfaceOrigin::Floor);

    // Walls the ring found, within the grid.
    WallSet walls(out, cp.maxSurfaces, cp.coplanarTolerance * s);
    const double reach = double(p_.radius) * p_.cell + 0.5;
    for (const Wall& w : walls_) {
        if (w.kind == HitKind::Character || IsDeepWater(w.material)) continue;
        double wa, wb;
        Horizontal(w.pos, wa, wb);
        if ((wa - fa) * (wa - fa) + (wb - fb) * (wb - fb) > reach * reach) continue;
        // Its top: the top of whatever is just behind the face (a low wall,
        // a planter, a building's roof); unknown = tall.
        double na = w.n[ha_], nb = w.n[hb_];
        const Column* behind = FindColumn(ColumnIndex(wa - na * 0.3), ColumnIndex(wb - nb * 0.3));
        const double hitUp = Up(w.pos);
        // Unknown: tall, but not for ever - the ring finds the rest of it as
        // Mario goes up (0.4 made it 20 m: a lamp post or a tree whose top no
        // ray found was an invisible wall high over the street).
        double wallTop = hitUp + double(p_.unknownTopAbove);
        if (w.topKnown) {
            wallTop = w.top;
        } else if (w.topAsked >= 0 && !behind) {
            // asked, nothing found within 3 m above the hit: tall
        } else if (behind && behind->time >= 0) {
            double best = 1e30;
            for (int k = 0; k < behind->count; ++k) {
                const Layer& l = behind->layers[k];
                if (l.kind == HitKind::Character || std::fabs(l.up) <= 0.3f || IsDeepWater(l.material)) continue;
                if (l.y >= hitUp - 0.05 && l.y < best) best = l.y;
            }
            if (best < 1e29) wallTop = best;
        }
        const Vec3 P = map.ToLocal(w.pos);
        Vec3 n = map.DirToLocal(w.n);
        n.y = 0;
        if (Length(n) < 0.5f) continue;
        n = Normalize(n);
        const Vec3 t(-n.z, 0, n.x);
        const float half = w.half * s;
        // Its foot: the floor in front of it (else where Mario's feet were).
        double foot = w.base;
        if (const Column* front = FindColumn(ColumnIndex(wa + na * 0.3), ColumnIndex(wb + nb * 0.3))) {
            for (int k = 0; k < front->count; ++k) {
                const Layer& l = front->layers[k];
                if (l.kind == HitKind::Character || std::fabs(l.up) <= 0.3f || l.y > hitUp || IsDeepWater(l.material))
                    continue;
                foot = std::min(foot, double(l.y));
                break;
            }
        }
        if (wallTop - foot < step) continue; // a kerb or a step: the floor grid ramps up it
        const float lo = map.ToLocal(Point(wa, wb, foot - 0.3)).y;
        const float hi = map.ToLocal(Point(wa, wb, wallTop)).y;
        stats.probeWalls +=
            walls.Emit(P - t * half, P + t * half, lo, hi, n, stats.clipped, SurfaceForMaterial(w.material));
        tag(SurfaceOrigin::RingWall, w.id);
    }

    CollisionBuilder::EmitStepWalls(g, cp, s, walls, stats);
    tag(SurfaceOrigin::StepWall);
    // On the ground: under what the game has answered for only - Mario can't
    // run into a place it hasn't been asked about yet (SM64 stops him at its
    // edge for the few frames until it has). In the air: everywhere and well
    // past the grid, or the edge of what is known is an invisible wall that
    // flying and long jumps bonk off.
    if (airborne_)
        CollisionBuilder::EmitSafetyFloor(g, m.y - (cp.probeDown + 1.0f) * s, out, stats, p_.airApron * s);
    else
        CollisionBuilder::EmitSafetyFloor(g, known, m.y - (cp.probeDown + 1.0f) * s, out, stats);
    tag(SurfaceOrigin::SafetyFloor);
    stats.total = int(out.size());
}

std::string PhysicsWorld::DescribeWall(uint32_t id, double now, uintptr_t* actor) const {
    if (actor) *actor = 0;
    for (const Wall& w : walls_) {
        if (w.id != id) continue;
        if (actor) *actor = w.actor;
        char buf[320];
        const char* kind = w.kind == HitKind::Movable ? "something that moves" : w.kind == HitKind::Character ? "a person" : "the world";
        char top[64];
        if (w.topKnown) std::snprintf(top, sizeof(top), "top at %.2f", w.top);
        else if (w.topAsked >= 0) std::snprintf(top, sizeof(top), "no top found within 3 m: tall");
        else std::snprintf(top, sizeof(top), "top not asked yet: %.0f m tall for now", double(p_.unknownTopAbove));
        std::snprintf(buf, sizeof(buf),
                      "a wall the ring found (%s, %s) at (%.2f %.2f %.2f) facing (%.2f %.2f %.2f), %.2f m wide, %s; "
                      "seen %.2f s ago, first %.1f s ago",
                      kind, PhysicsMaterialName(w.material), w.pos.x, w.pos.y, w.pos.z, w.n.x, w.n.y, w.n.z,
                      double(w.half) * 2.0, top, now - w.time, now - w.found);
        return buf;
    }
    return "a wall the ring found (gone since)";
}

std::string PhysicsWorld::DescribeColumn(const DVec3& at, double now) const {
    double fa, fb;
    Horizontal(at, fa, fb);
    const int64_t ix = ColumnIndex(fa), iz = ColumnIndex(fb);
    const Column* c = FindColumn(ix, iz);
    char buf[640];
    if (!c || c->time < 0) {
        std::snprintf(buf, sizeof(buf), "column (%lld %lld): never answered%s", static_cast<long long>(ix),
                      static_cast<long long>(iz), c && c->asked >= 0 ? " (asked)" : "");
        return buf;
    }
    std::string layers;
    for (int i = 0; i < c->count; ++i) {
        const Layer& l = c->layers[i];
        char one[96];
        std::snprintf(one, sizeof(one), "%s%.2f%s %s%s%s", i ? ", " : "", l.y, l.up > 0.3f ? "^" : (l.up < -0.3f ? "v" : "|"),
                      PhysicsMaterialName(l.material),
                      l.kind == HitKind::Movable ? " (moves)" : (l.kind == HitKind::Character ? " (person)" : ""),
                      l.kept ? " (kept)" : "");
        layers += one;
    }
    std::snprintf(buf, sizeof(buf), "column (%lld %lld) answered %.2f s ago%s%s, %.1f..%.1f: %s",
                  static_cast<long long>(ix), static_cast<long long>(iz), now - c->time,
                  c->partial ? (c->filled ? ", stopped at its hit limit (older answer kept below)" : ", stopped at its hit limit")
                             : "",
                  c->asked >= 0 ? ", asked again" : "", c->lo, c->hi, layers.empty() ? "nothing" : layers.c_str());
    return buf;
}

std::string PhysicsWorld::Debug(const DVec3& feet) const {
    double fa, fb;
    Horizontal(feet, fa, fb);
    const int64_t cx = ColumnIndex(fa), cz = ColumnIndex(fb);
    int known = 0, total = 0;
    for (int64_t iz = cz - p_.radius; iz <= cz + p_.radius; ++iz)
        for (int64_t ix = cx - p_.radius; ix <= cx + p_.radius; ++ix) {
            ++total;
            const Column* c = FindColumn(ix, iz);
            if (c && c->time >= 0) ++known;
        }
    char buf[256];
    const Column* here = FindColumn(cx, cz);
    std::snprintf(buf, sizeof(buf), "columns %d/%d known, walls %zu, rays in flight %zu, floor here %s, water %s", known,
                  total, walls_.size(), pending_.size(),
                  here && here->time >= 0 ? (here->count ? PhysicsMaterialName(here->layers[0].material) : "none")
                                          : "unknown",
                  waterKnown_ ? "yes" : "no");
    return buf;
}

} // namespace sm2m
