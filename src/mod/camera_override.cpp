#include "camera_override.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sm2m {

namespace {
double Dot3(const DVec3& a, const Vec3& b) { return a.x * double(b.x) + a.y * double(b.y) + a.z * double(b.z); }
double Comp(const Vec3& v, int k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }
void SetComp(Vec3& v, int k, float x) {
    if (k == 0) v.x = x;
    else if (k == 1) v.y = x;
    else v.z = x;
}
} // namespace

void CameraOverride::Reset() { *this = CameraOverride(); }

void CameraOverride::SetCamera(int forwardRow, float forwardSign) {
    const int row = std::max(0, std::min(2, forwardRow));
    const float sign = forwardSign < 0 ? -1.0f : 1.0f;
    // (The framing is kept in the camera's own terms - back, up, side - so it
    // holds for another camera whose rows are laid out differently.)
    if (row != forwardRow_) upRow_ = -1; // seen again with its next write
    haveCamera_ = true;
    forwardRow_ = row;
    forwardSign_ = sign;
}

void CameraOverride::LoseCamera() {
    haveCamera_ = false;
    blend_ = 0;
    haveLastGame_ = false;
}

bool CameraOverride::OffsetFor(Vec3& out) const {
    if (upRow_ < 0 || upRow_ == forwardRow_) return false;
    const int side = 3 - forwardRow_ - upRow_;
    const Vec3 f = framing_ * distance_;
    SetComp(out, forwardRow_, -f.z * forwardSign_);
    SetComp(out, upRow_, f.y * upSign_);
    SetComp(out, side, f.x * sideSign_);
    return true;
}

DVec3 CameraOverride::PivotAt(const Plan& plan, double t) {
    if (!(plan.frameTime > 0.0f) || !(plan.time > 0.0)) return plan.pivot;
    // One frame before t, between the last two frames: as far past the
    // previous pivot as t is past the latest frame (a write later than a
    // frame after it - a hitch - gets the latest).
    double f = (t - plan.time) / double(plan.frameTime);
    if (!(f == f)) return plan.pivot; // (NaN)
    f = std::max(0.0, std::min(1.0, f));
    // (Not across a jump: a respawn, an origin shift.)
    const DVec3 d = plan.pivot - plan.prevPivot;
    if (!(Length(d) < 3.0)) return plan.pivot;
    return plan.prevPivot + d * f;
}

CameraOverride::Plan CameraOverride::Update(double t, const DVec3& mario, bool airborne, float groundSpeed, int up,
                                            const GameSample& game, bool allowed, const Params& p) {
    double dt = started_ ? t - lastT_ : 0.0;
    if (dt < 0 || dt > 0.5) dt = 0; // a pause or a hitch: no smoothing step across it
    started_ = true;
    lastT_ = t;
    up = std::max(0, std::min(2, up));

    // The pivot: Mario, his height eased a little (more in the air).
    if (!havePivot_ || dt == 0) {
        if (!havePivot_) pivotUp_ = mario[up];
        havePivot_ = true;
    } else {
        const double tau = std::max(0.005, double(airborne ? p.verticalTauAir : p.verticalTauGround));
        pivotUp_ += (mario[up] - pivotUp_) * (1.0 - std::exp(-dt / tau));
    }
    // Never far behind (a long drop, a respawn).
    pivotUp_ = std::max(mario[up] - 3.0, std::min(mario[up] + 3.0, pivotUp_));
    DVec3 pivot = mario;
    pivot[up] = pivotUp_;
    // Where it was at the frame before (for writes on another thread; none
    // across a pause or a hitch).
    const bool havePrev = lastPivotT_ >= 0 && dt > 0.002;
    const DVec3 prevPivot = havePrev ? lastPivot_ : pivot;
    lastPivot_ = pivot;
    lastPivotT_ = t;
    distance_ = std::max(0.3f, std::min(4.0f, p.distance));

    // Is the game's camera following Mario at all? Near him, yes; further
    // than that only while it chases him: it was following, and moves on
    // from its last write without a jump (a cut to a cinematic shot jumps).
    bool following = following_;
    if (game.valid) {
        const double d = Length(game.pos - mario);
        const bool fresh = game.seq != lastSeq_;
        if (d <= double(p.minGameDistance)) {
            following = false;
        } else if (d < double(p.maxGameDistance)) {
            following = true;
        } else if (fresh) {
            const double step = sourceSwitched_ ? 0.0 : (haveLastGame_ ? Length(game.pos - lastGamePos_) : 1e9);
            following = following_ && step < double(p.maxChaseStep) && d < double(p.maxChaseDistance);
        }
        if (fresh) sourceSwitched_ = false;
    } else {
        following = false;
    }
    following_ = following;

    if (!framingSet_) {
        framing_ = Vec3(0.0f, p.defaultUp, p.defaultBack);
        framingSet_ = true;
    }
    // Which of the camera's rows points up (the others: along the view and across it).
    if (game.valid) {
        int best = -1;
        float bestUp = 0.3f;
        for (int k = 0; k < 3; ++k) {
            if (k == forwardRow_) continue;
            const float len = Length(game.rows[k]);
            if (!(len > 0.5f && len < 2.0f)) continue;
            const float u = float(Comp(game.rows[k], up)) / len;
            if (std::fabs(u) > bestUp) {
                bestUp = std::fabs(u);
                best = k;
                upSign_ = u > 0 ? 1.0f : -1.0f;
            }
        }
        if (best >= 0) {
            upRow_ = best;
            // Which way the across row points, against up x forward: so a
            // framing to one side stays on that side for a camera whose rows
            // are laid out the other way round.
            const int side = 3 - forwardRow_ - upRow_;
            const Vec3 across = Cross(game.rows[upRow_] * upSign_, game.rows[forwardRow_] * forwardSign_);
            const float d = Dot(across, game.rows[side]);
            if (std::fabs(d) > 0.1f) sideSign_ = d > 0 ? 1.0f : -1.0f;
        }
    }

    // Learn the game's framing while Mario stands still and its camera has settled.
    const bool resting = !airborne && groundSpeed < p.restSpeed;
    if (!resting) restSince_ = -1;
    else if (restSince_ < 0) restSince_ = t;
    const bool fresh = game.valid && game.seq != lastSeq_;
    const bool settled = haveLastGame_ && Length(game.pos - lastGamePos_) < 0.03;
    const bool aimed =
        !game.haveView || Dot(Normalize(game.rows[forwardRow_]), game.viewForward) * forwardSign_ > 0.9f;
    // (Not while paused or in photo mode: whatever camera is written then
    // isn't the one that follows him.)
    if (allowed && haveCamera_ && game.learn && fresh && following && resting && restSince_ >= 0 &&
        t - restSince_ >= double(p.restTime) && settled && aimed) {
        const DVec3 rel = game.pos - mario;
        Vec3 e;
        for (int k = 0; k < 3; ++k) SetComp(e, k, float(Dot3(rel, game.rows[k])));
        if (upRow_ >= 0 && std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z)) {
            // In the camera's terms: to the side, up, back.
            const Vec3 f(float(Comp(e, 3 - forwardRow_ - upRow_)) * sideSign_, float(Comp(e, upRow_)) * upSign_,
                         -float(Comp(e, forwardRow_)) * forwardSign_);
            // Only a framing a follow camera has (not one pulled in by a wall,
            // or pushed out by a fight).
            const bool plausible = f.z >= p.minBack && f.z <= p.maxBack && f.y >= p.minUp && f.y <= p.maxUp &&
                                   std::fabs(f.x) <= p.maxSide;
            if (!plausible) {
                // nothing learnt from this one
            } else if (!learnt_) {
                framing_ = f;
                learnt_ = true;
            } else {
                for (int k = 0; k < 3; ++k) {
                    double step = (Comp(f, k) - Comp(framing_, k)) * double(p.learnRate);
                    step = std::max(-double(p.maxLearnStep), std::min(double(p.maxLearnStep), step));
                    SetComp(framing_, k, float(Comp(framing_, k) + step));
                }
            }
        }
    }
    if (game.valid) {
        if (fresh) {
            lastGamePos_ = game.pos;
            haveLastGame_ = true;
        }
        lastSeq_ = game.seq;
    }

    // The framing in this camera's rows.
    const bool haveOffset = OffsetFor(offset_);
    // Ease in and out.
    const bool want = allowed && haveCamera_ && haveOffset && following;
    if (dt > 0) {
        const float step = float(dt / std::max(0.01, double(p.blendTime)));
        blend_ = want ? std::min(1.0f, blend_ + step) : std::max(0.0f, blend_ - step);
    } else if (!want && !allowed) {
        blend_ = 0;
    }

    // Walls: as far as two of the rays allow (one alone may be a post), in
    // at once, back out gently. With only one answer so far, hold.
    float answered[kCollisionRays];
    int nFresh = 0;
    for (int i = 0; i < kCollisionRays; ++i)
        if (t - rayTime_[i] <= double(p.collisionFresh)) answered[nFresh++] = rayReach_[i];
    const bool collide = nFresh > 0;
    if (!collide) {
        reach_ = 1.0f;
    } else if (nFresh >= 2) {
        for (int i = 1; i < nFresh; ++i) // (three at most)
            for (int j = i; j > 0 && answered[j] < answered[j - 1]; --j) std::swap(answered[j], answered[j - 1]);
        const float target = answered[1];
        if (target < reach_) reach_ = target;
        else if (dt > 0)
            reach_ += (target - reach_) * float(1.0 - std::exp(-dt / std::max(0.01, double(p.collisionRelease))));
    }

    Plan plan;
    plan.active = haveCamera_ && haveOffset && blend_ > 0.0f;
    plan.pivot = pivot;
    plan.time = t;
    plan.prevPivot = prevPivot;
    plan.frameTime = havePrev ? float(dt) : 0.0f;
    plan.offset = offset_;
    plan.forwardRow = forwardRow_;
    plan.forwardSign = forwardSign_;
    plan.blend = blend_;
    plan.collide = collide;
    plan.reach = reach_;
    plan.lookHeight = p.lookHeight;
    plan.up = up;
    return plan;
}

void CameraOverride::AcceptCollision(double t, int index, float hit, float length, const Params& p) {
    if (index < 0 || index >= kCollisionRays || !(length > 0.0f)) return;
    // The camera's spot is `margin` short of the ray's end.
    const float full = length - p.collisionMargin;
    if (full < 1e-3f) return;
    float r = 1.0f;
    if (hit >= 0.0f) r = (hit - p.collisionMargin) / full;
    const float lo = std::min(1.0f, p.collisionMinDistance / full);
    rayReach_[index] = std::max(lo, std::min(1.0f, r));
    rayTime_[index] = t;
}

bool CameraOverride::CollisionRays(const Plan& plan, const Vec3 rows[3], const Params& p, DVec3 from[kCollisionRays],
                                   DVec3 to[kCollisionRays]) {
    if (!plan.active) return false;
    const int up = std::max(0, std::min(2, plan.up));
    DVec3 look = plan.pivot;
    look[up] += double(plan.lookHeight);
    DVec3 spot = plan.pivot;
    for (int k = 0; k < 3; ++k)
        spot = spot + DVec3(rows[k].x, rows[k].y, rows[k].z) * Comp(plan.offset, k);
    const DVec3 d = spot - look;
    const double len = Length(d);
    if (!(len > 0.2)) return false;
    // Across the view, level: up x the way to the camera (or, looking
    // straight down, the most level of the other rows).
    DVec3 upv;
    upv[up] = 1.0;
    DVec3 side(upv.y * d.z - upv.z * d.y, upv.z * d.x - upv.x * d.z, upv.x * d.y - upv.y * d.x);
    double sl = Length(side);
    if (sl < 1e-3 * len) {
        int best = -1;
        double bestUp = 2.0;
        for (int k = 0; k < 3; ++k) {
            if (k == plan.forwardRow) continue;
            const double u = std::fabs(Comp(rows[k], up)) / std::max(1e-6f, Length(rows[k]));
            if (u < bestUp) {
                bestUp = u;
                best = k;
            }
        }
        if (best < 0) return false;
        side = DVec3(rows[best].x, rows[best].y, rows[best].z);
        sl = Length(side);
        if (sl < 1e-6) return false;
    }
    side = side * (1.0 / sl);
    for (int i = 0; i < kCollisionRays; ++i) {
        const double o = i == 0 ? 0.0 : (i == 1 ? double(p.collisionSide) : -double(p.collisionSide));
        const DVec3 e = spot + side * o - look;
        const double el = Length(e);
        from[i] = look;
        to[i] = look + e * ((el + double(p.collisionMargin)) / el);
    }
    return true;
}

DVec3 CameraOverride::Place(const Plan& plan, const Vec3 rows[3], const DVec3& gamePos) {
    if (!plan.active || plan.blend <= 0.0f) return gamePos;
    Vec3 o = plan.offset;
    if (!plan.collide) {
        // No rays: along the view, as far back as the game's camera, at most
        // as far as learnt (closer when a wall pulls it in - or Mario walks
        // towards it - never further when it lags behind him).
        const int f = std::max(0, std::min(2, plan.forwardRow));
        const double eF = Dot3(gamePos - plan.pivot, rows[f]);
        const double oF = Comp(o, f);
        if (oF * eF > 0 && std::fabs(eF) < std::fabs(oF)) SetComp(o, f, float(eF));
    }
    DVec3 mine = plan.pivot;
    for (int k = 0; k < 3; ++k)
        mine = mine + DVec3(rows[k].x, rows[k].y, rows[k].z) * Comp(o, k);
    if (plan.collide && plan.reach < 1.0f) {
        // In front of the wall the rays found: part of the way from his chest.
        DVec3 look = plan.pivot;
        look[std::max(0, std::min(2, plan.up))] += double(plan.lookHeight);
        mine = look + (mine - look) * double(std::max(0.0f, plan.reach));
    }
    const double b = std::max(0.0f, std::min(1.0f, plan.blend));
    return gamePos + (mine - gamePos) * b;
}

bool CameraOverride::AcceptWrite(const Plan& plan, const DVec3& pos, const Vec3 rows[3], bool havePrev,
                                 const DVec3& prevGame) {
    if (!plan.checkView) return true;
    const int f = plan.forwardRow < 0 || plan.forwardRow > 2 ? 2 : plan.forwardRow;
    const float len = Length(rows[f]);
    if (!(len > 0.5f && len < 2.0f)) return false; // not a rotation
    const double away = Length(pos - plan.viewPos);
    const bool turned = !(Dot(rows[f], plan.viewForward) * plan.forwardSign >= 0.5f * len);
    if (!(away > 40.0) && !(turned && away > 15.0)) return true;
    if (turned || !havePrev) return false;
    // Chasing Mario: on from its last write, looking at him.
    const DVec3 toMario = plan.pivot - pos;
    const double dist = Length(toMario);
    if (!(dist > 1e-3) || !(dist < 200.0)) return false;
    const double facing =
        Dot(toMario, DVec3(rows[f].x, rows[f].y, rows[f].z)) * double(plan.forwardSign) / (dist * double(len));
    return Length(pos - prevGame) < 10.0 && facing > 0.8;
}

int CameraCandidateRow(const DVec3& pos, const Vec3 rows[3], const DVec3& camPos, const Vec3& camForward, float radius,
                       float aim, float& sign) {
    if (Length(pos - camPos) > double(radius)) return -1;
    int best = -1;
    float bestDot = aim;
    for (int k = 0; k < 3; ++k) {
        const float len = Length(rows[k]);
        if (len < 0.5f || len > 2.0f) return -1; // not a rotation
        const float d = Dot(rows[k], camForward) / len;
        if (std::fabs(d) > bestDot) {
            bestDot = std::fabs(d);
            best = k;
            sign = d > 0 ? 1.0f : -1.0f;
        }
    }
    return best;
}

} // namespace sm2m
