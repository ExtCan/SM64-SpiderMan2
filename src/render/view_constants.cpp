#include "view_constants.h"

#include <cmath>
#include <cstring>

namespace sm2m {
namespace {

bool Finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

bool Invert3(const double m[3][3], double out[3][3]) {
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::fabs(det) > 1e-18)) return false;
    const double k = 1.0 / det;
    out[0][0] = c00 * k;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    out[1][0] = c01 * k;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    out[2][0] = c02 * k;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    return true;
}

} // namespace

bool ParseViewConstants(const float* rows, ViewConstants& v, std::string* why) {
    auto fail = [&](const char* r) {
        if (why) *why = r;
        v.valid = false;
        return false;
    };
    std::memcpy(v.raw, rows, sizeof(v.raw));
    if (!Finite(rows, 4 * 16)) return fail("non-finite camera/projection rows");
    const float* r = rows;
    v.right = Vec3(r[0], r[1], r[2]);
    v.up = Vec3(r[4], r[5], r[6]);
    v.forward = Vec3(r[8], r[9], r[10]);
    v.camPos = DVec3(r[12], r[13], r[14]);
    const Vec3 ax[3] = {v.right, v.up, v.forward};
    for (int i = 0; i < 3; ++i)
        if (std::fabs(Length(ax[i]) - 1.0f) > 2e-3f) return fail("camera axes are not unit length");
    if (std::fabs(Dot(v.right, v.up)) > 2e-3f || std::fabs(Dot(v.right, v.forward)) > 2e-3f ||
        std::fabs(Dot(v.up, v.forward)) > 2e-3f)
        return fail("camera axes are not orthogonal");
    if (std::fabs(v.camPos.x) > 1e6 || std::fabs(v.camPos.y) > 1e6 || std::fabs(v.camPos.z) > 1e6)
        return fail("camera position out of range");

    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            v.vp[i][j] = r[(4 + i) * 4 + j];
            v.prevVp[i][j] = r[(8 + i) * 4 + j];
        }
    v.prevCamPos = DVec3(r[15 * 4 + 0], r[15 * 4 + 1], r[15 * 4 + 2]);
    if (!Finite(&r[15 * 4], 3)) v.prevCamPos = v.camPos;
    v.invWidth = r[34 * 4 + 0];
    v.invHeight = r[34 * 4 + 1];
    v.renderWidth = v.invWidth > 1e-6f && v.invWidth < 1.0f ? int(std::lround(1.0 / v.invWidth)) : 0;
    v.renderHeight = v.invHeight > 1e-6f && v.invHeight < 1.0f ? int(std::lround(1.0 / v.invHeight)) : 0;
    v.skyDepth = std::isfinite(r[35 * 4 + 1]) ? r[35 * 4 + 1] : 0.0f;

    // The w column: forward for a perspective camera, (0,0,0,1) for an
    // orthographic one (shadow cascades).
    const Vec3 wcol(float(v.vp[0][3]), float(v.vp[1][3]), float(v.vp[2][3]));
    if (Length(wcol - v.forward) < 2e-3f && std::fabs(v.vp[3][3]) < 1e-3) {
        v.perspective = true;
    } else if (Length(wcol) < 1e-5f && std::fabs(v.vp[3][3] - 1.0) < 1e-3) {
        v.perspective = false;
    } else {
        return fail("view-projection w column doesn't match the camera");
    }
    // Projection scales along right / up (jitter terms sit on forward).
    const Vec3 c0(float(v.vp[0][0]), float(v.vp[1][0]), float(v.vp[2][0]));
    const Vec3 c1(float(v.vp[0][1]), float(v.vp[1][1]), float(v.vp[2][1]));
    v.xScale = Dot(c0, v.right);
    v.yScale = Dot(c1, v.up);
    if (!(std::fabs(v.xScale) > 1e-6 && std::fabs(v.yScale) > 1e-6)) return fail("degenerate projection");
    // Handedness as it appears on screen. The stored axes alone don't say: an
    // engine may keep a camera x axis that points left (or y down) and flip
    // it in the projection, so screen right is the projection's x direction.
    v.leftHanded = ((Dot(Cross(v.right, v.up), v.forward) > 0.0f) != (v.xScale < 0.0)) != (v.yScale < 0.0);
    v.fovY = v.perspective ? float(2.0 * std::atan(1.0 / std::fabs(v.yScale))) : 0.0f;

    double a[3][3];
    for (int k = 0; k < 3; ++k) {
        a[0][k] = v.vp[k][0];
        a[1][k] = v.vp[k][1];
        a[2][k] = v.vp[k][3];
    }
    if (!v.perspective) {
        // Orthographic: depth comes from the z column instead of w.
        for (int k = 0; k < 3; ++k) a[2][k] = v.vp[k][2];
    }
    v.invOk_ = Invert3(a, v.inv_);
    if (!v.invOk_) return fail("view-projection is not invertible");
    v.valid = true;
    return true;
}

bool ViewConstants::Project(const DVec3& world, double& ndcX, double& ndcY, double& w) const {
    const DVec3 p = world - camPos;
    double c[4];
    for (int j = 0; j < 4; ++j) c[j] = p.x * vp[0][j] + p.y * vp[1][j] + p.z * vp[2][j] + vp[3][j];
    w = c[3];
    if (perspective && w <= 1e-6) return false;
    const double iw = perspective ? 1.0 / w : 1.0;
    ndcX = c[0] * iw;
    ndcY = c[1] * iw;
    if (!perspective) w = c[2];
    return true;
}

bool ViewConstants::Unproject(double ndcX, double ndcY, double w, DVec3& world) const {
    if (!invOk_) return false;
    double b[3];
    if (perspective) {
        b[0] = ndcX * w - vp[3][0];
        b[1] = ndcY * w - vp[3][1];
        b[2] = w - vp[3][3];
    } else {
        b[0] = ndcX - vp[3][0];
        b[1] = ndcY - vp[3][1];
        b[2] = w - vp[3][2];
    }
    DVec3 p;
    p.x = inv_[0][0] * b[0] + inv_[0][1] * b[1] + inv_[0][2] * b[2];
    p.y = inv_[1][0] * b[0] + inv_[1][1] * b[1] + inv_[1][2] * b[2];
    p.z = inv_[2][0] * b[0] + inv_[2][1] * b[1] + inv_[2][2] * b[2];
    world = camPos + p;
    return true;
}

void ViewConstants::PixelToNdc(double px, double py, int width, int height, double& ndcX, double& ndcY) {
    ndcX = (px + 0.5) / double(width) * 2.0 - 1.0;
    ndcY = 1.0 - (py + 0.5) / double(height) * 2.0;
}

void BuildViewConstants(const DVec3& camPos, const Vec3& fwdIn, const Vec3& worldUp, float fovY, int width, int height,
                        float nearZ, const DVec3& prevCamPos, const double prevVp[4][4], float out[kViewCbRows][4]) {
    std::memset(out, 0, sizeof(float) * kViewCbRows * 4);
    const Vec3 F = Normalize(fwdIn);
    Vec3 R = Normalize(Cross(worldUp, F)); // left-handed: right = up x forward
    if (Length(R) < 0.5f) R = Vec3(1, 0, 0);
    const Vec3 U = Cross(F, R);
    const float ys = 1.0f / std::tan(0.5f * fovY);
    const float xs = ys * float(height) / float(width);
    const Vec3 ax[3] = {R, U, F};
    for (int i = 0; i < 3; ++i) {
        out[i][0] = ax[i].x;
        out[i][1] = ax[i].y;
        out[i][2] = ax[i].z;
    }
    out[3][0] = float(camPos.x);
    out[3][1] = float(camPos.y);
    out[3][2] = float(camPos.z);
    out[3][3] = 1.0f;
    // Reverse-Z with an infinite far plane: z_clip = near, w_clip = view depth.
    for (int k = 0; k < 3; ++k) {
        out[4 + k][0] = R[k] * xs;
        out[4 + k][1] = U[k] * ys;
        out[4 + k][2] = 0.0f;
        out[4 + k][3] = F[k];
    }
    out[7][0] = 0.0f;
    out[7][1] = 0.0f;
    out[7][2] = nearZ;
    out[7][3] = 0.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[8 + i][j] = prevVp ? float(prevVp[i][j]) : out[4 + i][j];
    out[15][0] = float(prevCamPos.x);
    out[15][1] = float(prevCamPos.y);
    out[15][2] = float(prevCamPos.z);
    out[33][0] = 1.0f;
    out[33][1] = 1.0f;
    out[34][0] = 1.0f / float(width);
    out[34][1] = 1.0f / float(height);
    out[35][0] = 1e-4f;
    out[35][1] = 100000.0f; // sky depth
}

void BuildOrthoViewConstants(const DVec3& lightPos, const Vec3& fwdIn, const Vec3& worldUp, float halfWidth,
                             float nearDist, float depthRange, int size, float out[kViewCbRows][4]) {
    std::memset(out, 0, sizeof(float) * kViewCbRows * 4);
    const Vec3 F = Normalize(fwdIn);
    Vec3 R = Cross(worldUp, F);
    if (Length(R) < 1e-3f) R = Cross(Vec3(1, 0, 0), F);
    R = Normalize(R);
    const Vec3 U = Cross(F, R);
    const Vec3 ax[3] = {R, U, F};
    for (int i = 0; i < 3; ++i) {
        out[i][0] = ax[i].x;
        out[i][1] = ax[i].y;
        out[i][2] = ax[i].z;
    }
    out[3][0] = float(lightPos.x);
    out[3][1] = float(lightPos.y);
    out[3][2] = float(lightPos.z);
    out[3][3] = 1.0f;
    const float s = 1.0f / halfWidth, zs = 1.0f / depthRange;
    for (int k = 0; k < 3; ++k) {
        out[4 + k][0] = R[k] * s;
        out[4 + k][1] = U[k] * s;
        out[4 + k][2] = F[k] * zs;
        out[4 + k][3] = 0.0f;
    }
    out[7][0] = 0.0f;
    out[7][1] = 0.0f;
    out[7][2] = -nearDist * zs;
    out[7][3] = 1.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[8 + i][j] = out[4 + i][j];
    out[15][0] = out[3][0];
    out[15][1] = out[3][1];
    out[15][2] = out[3][2];
    uint32_t flags = 4096; // orthographic
    std::memcpy(&out[28][3], &flags, 4);
    out[33][0] = out[33][1] = 1.0f;
    out[34][0] = out[34][1] = 1.0f / float(size);
    out[35][0] = 1e-4f;
}

} // namespace sm2m
