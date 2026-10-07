#include "camera_finder.h"

#include <cmath>
#include <cstring>
#include <map>

namespace sm2m {
namespace {

inline bool Near(float v, float target, float tol) { return std::fabs(v - target) < tol; }

void ReadAxes(const float* f, MatrixLayout layout, Vec3 a[3], DVec3& t) {
    if (layout == MatrixLayout::RowsAxes) {
        for (int i = 0; i < 3; ++i) a[i] = Vec3(f[4 * i], f[4 * i + 1], f[4 * i + 2]);
        t = DVec3(f[12], f[13], f[14]);
    } else {
        for (int i = 0; i < 3; ++i) a[i] = Vec3(f[i], f[4 + i], f[8 + i]);
        t = DVec3(f[3], f[7], f[11]);
    }
}

bool Orthonormal(const Vec3 a[3], float tol) {
    for (int i = 0; i < 3; ++i) {
        if (!IsFinite(a[i])) return false;
        if (!Near(Dot(a[i], a[i]), 1.0f, 2.0f * tol)) return false;
    }
    return std::fabs(Dot(a[0], a[1])) < tol && std::fabs(Dot(a[0], a[2])) < tol && std::fabs(Dot(a[1], a[2])) < tol;
}

} // namespace

bool EvaluateCameraMatrix(const float* f, MatrixLayout layout, const DVec3& heroPos, const Vec3& worldUp,
                          const CameraSearchParams& p, CameraCandidate& cand, CameraPose& pose) {
    Vec3 a[3];
    DVec3 t;
    ReadAxes(f, layout, a, t);
    if (!Orthonormal(a, p.orthoTolerance)) return false;
    if (!IsFinite(t)) return false;
    // Rows-layout 4x4s normally carry (0,0,0,1) in the last column; tolerate
    // 3x4 + padding but reject obvious garbage there.
    const DVec3 target = heroPos + DVec3(worldUp) * double(p.lookAtHeight);
    const DVec3 d = target - t;
    const double dist = Length(d);
    if (!(dist >= p.minDistance && dist <= p.maxDistance)) return false;
    const Vec3 dir = (d * (1.0 / dist)).ToFloat();

    int fi = -1;
    float best = 0;
    for (int i = 0; i < 3; ++i) {
        float dd = std::fabs(Dot(a[i], dir));
        if (dd > best) {
            best = dd;
            fi = i;
        }
    }
    if (fi < 0 || best < p.minLookDot) return false;
    const int fs = Dot(a[fi], dir) >= 0 ? 1 : -1;

    int ui = -1;
    float bestUp = 0;
    for (int i = 0; i < 3; ++i) {
        if (i == fi) continue;
        float u = std::fabs(Dot(a[i], worldUp));
        if (u > bestUp) {
            bestUp = u;
            ui = i;
        }
    }
    if (ui < 0 || bestUp < p.minUpDot) return false;
    const int us = Dot(a[ui], worldUp) >= 0 ? 1 : -1;
    const int ri = 3 - fi - ui;

    cand.layout = layout;
    cand.forwardAxis = fi;
    cand.forwardSign = fs;
    cand.upAxis = ui;
    cand.upSign = us;
    cand.rightAxis = ri;
    cand.distance = dist;
    cand.score = best + ((dist > 1.5 && dist < 8.0) ? 0.2f : 0.0f) + 0.1f * bestUp;
    cand.lastPos = t;

    pose.position = t;
    pose.forward = a[fi] * float(fs);
    pose.up = a[ui] * float(us);
    pose.right = a[ri];
    pose.valid = true;
    return true;
}

bool PoseFromCandidate(const float* f, const CameraCandidate& c, const CameraSearchParams& p, CameraPose& pose) {
    Vec3 a[3];
    DVec3 t;
    ReadAxes(f, c.layout, a, t);
    if (!Orthonormal(a, p.orthoTolerance * 2.0f) || !IsFinite(t)) return false;
    pose.position = t;
    pose.forward = a[c.forwardAxis] * float(c.forwardSign);
    pose.up = a[c.upAxis] * float(c.upSign);
    pose.right = a[c.rightAxis];
    pose.valid = true;
    return true;
}

bool CameraIsLeftHanded(const CameraCandidate& c, const float* f) {
    Vec3 a[3];
    DVec3 t;
    ReadAxes(f, c.layout, a, t);
    const Vec3 F = a[c.forwardAxis] * float(c.forwardSign);
    const Vec3 U = a[c.upAxis] * float(c.upSign);
    const Vec3 R = a[c.rightAxis];
    return Dot(Cross(U, F), R) > 0.0f;
}

void ScanSpanForCameras(const uint8_t* data, size_t size, uintptr_t base, const DVec3& heroPos, const Vec3& worldUp,
                        const CameraSearchParams& p, std::vector<CameraCandidate>& out, size_t maxOut) {
    if (size < 64) return;
    size_t start = (16 - (base & 15)) & 15;
    float f[16];
    for (size_t off = start; off + 64 <= size; off += 16) {
        std::memcpy(f, data + off, 64);
        // Cheap pre-filter: a unit axis in either layout.
        const float r0 = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
        const float c0 = f[0] * f[0] + f[4] * f[4] + f[8] * f[8];
        const bool rowOk = std::fabs(r0 - 1.0f) < 0.01f;
        const bool colOk = std::fabs(c0 - 1.0f) < 0.01f;
        if (!rowOk && !colOk) continue;
        CameraCandidate cand;
        CameraPose pose;
        if (rowOk && EvaluateCameraMatrix(f, MatrixLayout::RowsAxes, heroPos, worldUp, p, cand, pose)) {
            cand.address = base + off;
            out.push_back(cand);
        } else if (colOk && EvaluateCameraMatrix(f, MatrixLayout::ColumnsAxes, heroPos, worldUp, p, cand, pose)) {
            cand.address = base + off;
            out.push_back(cand);
        }
        if (out.size() >= maxOut) return;
    }
}

bool EvaluateProjection(const float* f, float aspect, ProjectionCandidate& out) {
    const float zt = 1e-6f;
    const float jt = 0.02f; // TAA jitter tolerance
    auto zero = [&](int i) { return std::fabs(f[i]) < zt; };
    const float xs = f[0], ys = f[5];
    if (!(xs > 0.1f && ys > 0.1f && ys < 5.0f)) return false;
    if (!zero(1) || !zero(4) || !zero(12) || !zero(13) || !zero(15)) return false;
    const float measured = ys / xs;
    if (!(std::fabs(measured - aspect) < aspect * 0.01f)) return false;

    bool rowVec = std::fabs(std::fabs(f[11]) - 1.0f) < 1e-4f && zero(3) && zero(7) && zero(6) && zero(2) &&
                  std::fabs(f[8]) < jt && std::fabs(f[9]) < jt && std::isfinite(f[10]) && std::isfinite(f[14]) &&
                  std::fabs(f[14]) > zt;
    bool colVec = std::fabs(std::fabs(f[14]) - 1.0f) < 1e-4f && zero(3) && zero(7) && zero(8) && zero(9) &&
                  std::fabs(f[2]) < jt && std::fabs(f[6]) < jt && std::isfinite(f[10]) && std::isfinite(f[11]) &&
                  std::fabs(f[11]) > zt;
    if (!rowVec && !colVec) return false;
    out.transposed = colVec && !rowVec;
    out.fovY = 2.0f * std::atan(1.0f / ys);
    return out.fovY > 0.3f && out.fovY < 2.4f;
}

void ScanSpanForProjections(const uint8_t* data, size_t size, uintptr_t base, float aspect,
                            std::vector<ProjectionCandidate>& out, size_t maxOut) {
    if (size < 64) return;
    size_t start = (16 - (base & 15)) & 15;
    float f[16];
    for (size_t off = start; off + 64 <= size; off += 16) {
        // Pre-filter on the cheapest structural zeros before copying.
        float f1, f4;
        std::memcpy(&f1, data + off + 4, 4);
        std::memcpy(&f4, data + off + 16, 4);
        if (f1 != 0.0f || f4 != 0.0f) continue;
        std::memcpy(f, data + off, 64);
        ProjectionCandidate c;
        if (EvaluateProjection(f, aspect, c)) {
            c.address = base + off;
            out.push_back(c);
            if (out.size() >= maxOut) return;
        }
    }
}

float MostCommonFov(const std::vector<ProjectionCandidate>& c) {
    std::map<int, std::pair<int, double>> buckets;
    for (const auto& p : c) {
        int key = int(std::lround(p.fovY * 1000.0f));
        auto& b = buckets[key];
        b.first++;
        b.second += p.fovY;
    }
    int bestCount = 0;
    double bestFov = 0;
    for (const auto& kv : buckets) {
        if (kv.second.first > bestCount) {
            bestCount = kv.second.first;
            bestFov = kv.second.second / kv.second.first;
        }
    }
    return float(bestFov);
}

} // namespace sm2m
