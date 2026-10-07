#include "mario.h"

#include <cstring>
#include <utility>

namespace sm2m {

void MarioGeometry::Allocate() {
    const size_t verts = size_t(SM64_GEO_MAX_TRIANGLES) * 3;
    position.assign(verts * 3, 0.0f);
    normal.assign(verts * 3, 0.0f);
    color.assign(verts * 3, 0.0f);
    uv.assign(verts * 2, 0.0f);
    triangles = 0;
}

MarioController::~MarioController() { Despawn(); }

bool MarioController::Spawn(const Vec3& localPos, float faceAngleRadians) {
    Despawn();
    prevGeo_.Allocate();
    curGeo_.Allocate();
    scratch_.Allocate();
    id_ = api_.mario_create(localPos.x, localPos.y, localPos.z);
    if (id_ < 0) return false;
    api_.set_mario_faceangle(id_, faceAngleRadians);
    std::memset(&prev_, 0, sizeof(prev_));
    std::memset(&cur_, 0, sizeof(cur_));
    cur_.position[0] = localPos.x;
    cur_.position[1] = localPos.y;
    cur_.position[2] = localPos.z;
    cur_.health = 0x880;
    prev_ = cur_;
    havePrev_ = false;
    return true;
}

void MarioController::Despawn() {
    if (id_ >= 0) {
        api_.mario_delete(id_);
        id_ = -1;
    }
    havePrev_ = false;
}

void MarioController::Tick(const SM64MarioInputs& inputs) {
    if (id_ < 0) return;
    // Current becomes previous, then libsm64 writes the new current.
    std::swap(prevGeo_, curGeo_);
    prev_ = cur_;

    SM64MarioGeometryBuffers buffers;
    buffers.position = curGeo_.position.data();
    buffers.normal = curGeo_.normal.data();
    buffers.color = curGeo_.color.data();
    buffers.uv = curGeo_.uv.data();
    buffers.numTrianglesUsed = 0;
    api_.mario_tick(id_, &inputs, &cur_, &buffers);
    curGeo_.triangles = buffers.numTrianglesUsed;
    if (!havePrev_) {
        // First tick: nothing to interpolate from.
        prevGeo_.triangles = curGeo_.triangles;
        std::memcpy(prevGeo_.position.data(), curGeo_.position.data(),
                    sizeof(float) * 9 * curGeo_.triangles);
        prev_ = cur_;
        havePrev_ = true;
    }
}

Vec3 MarioController::Position(float alpha) const {
    return {Lerp(prev_.position[0], cur_.position[0], alpha), Lerp(prev_.position[1], cur_.position[1], alpha),
            Lerp(prev_.position[2], cur_.position[2], alpha)};
}

void MarioController::Geometry(float alpha, MarioGeometry& out) const {
    const uint16_t tris = curGeo_.triangles;
    if (out.position.size() < size_t(SM64_GEO_MAX_TRIANGLES) * 9) out.Allocate();
    out.triangles = tris;
    const size_t n3 = size_t(tris) * 9;
    const size_t n2 = size_t(tris) * 6;
    if (havePrev_ && prevGeo_.triangles == tris) {
        const float* a = prevGeo_.position.data();
        const float* b = curGeo_.position.data();
        float* o = out.position.data();
        for (size_t i = 0; i < n3; ++i) o[i] = a[i] + (b[i] - a[i]) * alpha;
    } else {
        std::memcpy(out.position.data(), curGeo_.position.data(), n3 * sizeof(float));
    }
    std::memcpy(out.normal.data(), curGeo_.normal.data(), n3 * sizeof(float));
    std::memcpy(out.color.data(), curGeo_.color.data(), n3 * sizeof(float));
    std::memcpy(out.uv.data(), curGeo_.uv.data(), n2 * sizeof(float));
}

static void ShiftGeo(MarioGeometry& g, const Vec3& d) {
    const size_t n = size_t(g.triangles) * 3;
    for (size_t v = 0; v < n; ++v) {
        g.position[v * 3 + 0] -= d.x;
        g.position[v * 3 + 1] -= d.y;
        g.position[v * 3 + 2] -= d.z;
    }
}

void MarioController::ShiftOrigin(const Vec3& d) {
    if (id_ < 0) return;
    for (int i = 0; i < 3; ++i) {
        prev_.position[i] -= d[i];
        cur_.position[i] -= d[i];
    }
    ShiftGeo(prevGeo_, d);
    ShiftGeo(curGeo_, d);
    api_.set_mario_position(id_, cur_.position[0], cur_.position[1], cur_.position[2]);
}

void MarioController::Teleport(const Vec3& p) {
    if (id_ < 0) return;
    Vec3 d(cur_.position[0] - p.x, cur_.position[1] - p.y, cur_.position[2] - p.z);
    ShiftOrigin(d);
    // Same geometry shift, but also collapse interpolation so there is no smear.
    prev_ = cur_;
    prevGeo_.triangles = curGeo_.triangles;
    std::memcpy(prevGeo_.position.data(), curGeo_.position.data(), sizeof(float) * 9 * curGeo_.triangles);
}

} // namespace sm2m
