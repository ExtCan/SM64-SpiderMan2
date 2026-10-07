#include "coords.h"

#include <cmath>

namespace sm2m {

void WorldMapping::Configure(char upAxis, bool mirror, double unitsPerGameUnit) {
    up_ = (upAxis == 'Z' || upAxis == 'z') ? 'Z' : 'Y';
    mirror_ = mirror;
    scale_ = unitsPerGameUnit > 0 ? unitsPerGameUnit : 100.0;
    if (up_ == 'Y') {
        // (x, y, z) -> (x, y, z); mirror flips z.
        src_[0] = 0; sign_[0] = 1;
        src_[1] = 1; sign_[1] = 1;
        src_[2] = 2; sign_[2] = mirror ? -1.0f : 1.0f;
    } else {
        // Z-up game: (x, y, z) -> (x, z, -y) is a proper rotation; mirror -> (x, z, y).
        src_[0] = 0; sign_[0] = 1;
        src_[1] = 2; sign_[1] = 1;
        src_[2] = 1; sign_[2] = mirror ? 1.0f : -1.0f;
    }
}

Vec3 WorldMapping::ToLocal(const DVec3& g) const {
    DVec3 d = g - origin_;
    Vec3 r;
    for (int i = 0; i < 3; ++i) r[i] = float(double(sign_[i]) * d[src_[i]] * scale_);
    return r;
}

DVec3 WorldMapping::ToGame(const Vec3& l) const {
    DVec3 d;
    for (int i = 0; i < 3; ++i) d[src_[i]] = double(l[i]) * double(sign_[i]) / scale_;
    return origin_ + d;
}

Vec3 WorldMapping::DirToLocal(const Vec3& g) const {
    Vec3 r;
    for (int i = 0; i < 3; ++i) r[i] = sign_[i] * g[src_[i]];
    return r;
}

Vec3 WorldMapping::DirToGame(const Vec3& l) const {
    Vec3 r;
    for (int i = 0; i < 3; ++i) r[src_[i]] = l[i] * sign_[i];
    return r;
}

Vec3 WorldMapping::GameUp() const { return up_ == 'Y' ? Vec3(0, 1, 0) : Vec3(0, 0, 1); }

void WorldMapping::Rows(Vec3 rows[3]) const {
    for (int i = 0; i < 3; ++i) {
        rows[i] = Vec3(0, 0, 0);
        rows[i][src_[i]] = sign_[i];
    }
}

bool ComputeOriginShift(const Vec3& p, bool airborne, float vy, const OriginPolicy& policy, Vec3& shift) {
    shift = Vec3(0, 0, 0);
    bool any = false;
    if (std::fabs(p.x) > policy.horizontalLimit || std::fabs(p.z) > policy.horizontalLimit) {
        // Whole units keep collision vertices integral.
        shift.x = std::round(p.x);
        shift.z = std::round(p.z);
        any = true;
    }
    if (!airborne) {
        if (std::fabs(p.y) > policy.groundedVerticalLimit) {
            shift.y = std::round(p.y);
            any = true;
        }
    } else if (vy < 0.0f && p.y < -policy.airborneFallLimit) {
        shift.y = std::round(p.y);
        any = true;
    } else if (p.y > policy.groundedVerticalLimit * 2.0f) {
        // Something launched Mario very high; keep inside libsm64's range.
        shift.y = std::round(p.y);
        any = true;
    }
    return any;
}

} // namespace sm2m
