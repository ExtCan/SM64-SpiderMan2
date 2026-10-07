// Owns one libsm64 Mario: fixed 30 Hz ticks, previous/current snapshots for
// render interpolation, and origin shifts for the floating-origin world.
#pragma once

#include <cstdint>
#include <vector>

#include "../common/vec.h"
#include "sm64_api.h"

namespace sm2m {

struct MarioGeometry {
    std::vector<float> position; // 9 floats per triangle
    std::vector<float> normal;
    std::vector<float> color;
    std::vector<float> uv;       // 6 floats per triangle
    uint16_t triangles = 0;

    void Allocate();
};

class MarioController {
public:
    explicit MarioController(const Sm64Api& api) : api_(api) {}
    ~MarioController();

    // Creates Mario at `localPos` (SM64 units). libsm64 requires a floor below
    // the spawn point, so collision must be loaded first.
    bool Spawn(const Vec3& localPos, float faceAngleRadians);
    void Despawn();
    bool Alive() const { return id_ >= 0; }
    int32_t Id() const { return id_; }

    // Runs one SM64 frame and rotates the snapshot buffers.
    void Tick(const SM64MarioInputs& inputs);

    const SM64MarioState& State() const { return cur_; }
    const SM64MarioState& PrevState() const { return prev_; }

    // Interpolated values between the previous and current tick, alpha in [0,1].
    Vec3 Position(float alpha) const;
    void Geometry(float alpha, MarioGeometry& out) const;

    // Moves Mario and all stored snapshots by -delta (local units) after the
    // world origin moved by +delta.
    void ShiftOrigin(const Vec3& delta);

    // Teleport inside the current local frame (no interpolation smear).
    void Teleport(const Vec3& localPos);

private:
    const Sm64Api& api_;
    int32_t id_ = -1;
    SM64MarioState prev_{};
    SM64MarioState cur_{};
    MarioGeometry prevGeo_;
    MarioGeometry curGeo_;
    MarioGeometry scratch_;
    bool havePrev_ = false;
};

} // namespace sm2m
