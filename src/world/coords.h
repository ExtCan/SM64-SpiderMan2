// Mapping between game world space (metres, doubles) and libsm64's local
// space (SM64 units, floats) with a floating origin.
//
//   local = scale * M * (game - origin)
//
// M is a signed axis permutation chosen from the game's up axis and
// handedness. SM64 is right-handed and Y-up; if the game is left-handed the
// mapping mirrors one axis (otherwise Mario renders mirrored and the stick
// directions come out swapped).
#pragma once

#include "../common/vec.h"

namespace sm2m {

class WorldMapping {
public:
    // upAxis: 'Y' or 'Z' (game axis that points up). mirror: add a reflection.
    void Configure(char upAxis, bool mirror, double unitsPerGameUnit);

    Vec3 ToLocal(const DVec3& game) const;
    DVec3 ToGame(const Vec3& local) const;
    Vec3 DirToLocal(const Vec3& gameDir) const; // no scale, no origin
    Vec3 DirToGame(const Vec3& localDir) const;

    const DVec3& Origin() const { return origin_; }
    void SetOrigin(const DVec3& o) { origin_ = o; }
    double Scale() const { return scale_; }
    bool Mirrored() const { return mirror_; }
    char UpAxis() const { return up_; }
    Vec3 GameUp() const;

    // Rows of M as game-space unit vectors (local axis i = dot(row[i], gameVec)).
    void Rows(Vec3 rows[3]) const;

private:
    // local[i] = sign[i] * game[src[i]]
    int src_[3] = {0, 1, 2};
    float sign_[3] = {1, 1, 1};
    double scale_ = 100.0;
    DVec3 origin_;
    bool mirror_ = false;
    char up_ = 'Y';
};

struct OriginPolicy {
    float horizontalLimit = 3000.0f;  // local units before re-centring X/Z
    float groundedVerticalLimit = 3000.0f;
    // While falling, keep Mario within this many units below the origin. This
    // keeps positions inside libsm64's floor search range (FLOOR_LOWER_LIMIT is
    // -11000) and stops SM64's own fall damage from accumulating over
    // skyscraper-height drops (the mod applies its own fall damage instead).
    float airborneFallLimit = 1000.0f;
};

// Decides whether to move the origin. On true, `shift` is the local-space
// vector to move the origin by (Mario's new local position = old - shift).
bool ComputeOriginShift(const Vec3& localPos, bool airborne, float verticalVelocity, const OriginPolicy& policy,
                        Vec3& shift);

} // namespace sm2m
