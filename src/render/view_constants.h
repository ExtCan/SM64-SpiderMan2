// The game's per-view constant buffer (b0 in every mesh shader), as found in
// the shaders embedded in Spider-Man2.exe (VS_DefaultMaterialGBuffer,
// VS_ModelShadowCaster, CS_ApplyGBufferLighting_*):
//
//   rows 0-2   camera right / up / forward (world space, unit vectors)
//   row  3     camera position (world)
//   rows 4-7   view-projection, camera-relative: clip = [p - camPos, 1] * VP
//   rows 8-11  previous frame's view-projection (relative to the previous camera)
//   row  15    previous camera position
//   row  33    xy: velocity scale (G-buffer motion vectors)
//   row  34    xy: 1 / render width, 1 / render height
//   row  28    w: flags (bit 12 = orthographic view, used by the shadow casters)
//   rows 31-32 screen -> view ray reconstruction (sky)
//   row  35    x: epsilon for the previous w, y: linear depth written for the sky
//
// Shadow-map views use the same layout with the light as the "camera".
// Mario's injected draws read the game's own buffer on the GPU; the mod reads
// a GPU-side copy of it for the camera and for unprojecting captured depth.
// Platform independent (unit tested).
#pragma once

#include <cstdint>
#include <string>

#include "../common/vec.h"

namespace sm2m {

constexpr int kViewCbRows = 36;
constexpr int kViewCbBytes = kViewCbRows * 16;

struct ViewConstants {
    float raw[kViewCbRows][4] = {};
    bool valid = false;

    DVec3 camPos;
    Vec3 right, up, forward;
    double vp[4][4] = {};     // camera-relative, row-vector convention
    double prevVp[4][4] = {};
    DVec3 prevCamPos;
    float invWidth = 0, invHeight = 0;
    int renderWidth = 0, renderHeight = 0;
    float skyDepth = 0;       // RT0 value of sky pixels (0 if unknown)
    bool perspective = true;
    bool leftHanded = true;   // on screen: dot(cross(screen right, screen up), forward) > 0
    double xScale = 1, yScale = 1; // |projection| scale on right / up: 1/tan(fov/2)
    float fovY = 1.0f;        // radians (perspective only)

    // Projects a world point. Returns false behind the camera (perspective).
    bool Project(const DVec3& world, double& ndcX, double& ndcY, double& w) const;
    // World point at normalised device coords (ndcX, ndcY) with clip w = `w`
    // (for perspective views w is the linear view depth the G-buffer stores).
    bool Unproject(double ndcX, double ndcY, double w, DVec3& world) const;
    // Pixel centre (x+0.5, y+0.5) of a width x height image -> NDC.
    static void PixelToNdc(double px, double py, int width, int height, double& ndcX, double& ndcY);

private:
    friend bool ParseViewConstants(const float*, ViewConstants&, std::string*);
    double inv_[3][3] = {}; // inverse of [c0; c1; c3] for Unproject
    bool invOk_ = false;
};

// Parses 36 float4 rows. Fails (with a reason) if the rows don't look like a
// camera: non-orthonormal axes, non-finite values, or a projection whose w
// column doesn't match the forward axis.
bool ParseViewConstants(const float* rows, ViewConstants& out, std::string* why = nullptr);

// Builds the rows for a perspective camera (reverse-Z, infinite far plane,
// like the game) - used by tests and the mock game.
void BuildViewConstants(const DVec3& camPos, const Vec3& forward, const Vec3& worldUp, float fovYRadians, int width,
                        int height, float nearZ, const DVec3& prevCamPos, const double prevVp[4][4],
                        float out[kViewCbRows][4]);
// Orthographic light view (a shadow cascade): `halfWidth` metres either side,
// depth = (distance along forward - nearDist) / depthRange in [0, 1].
void BuildOrthoViewConstants(const DVec3& lightPos, const Vec3& forward, const Vec3& worldUp, float halfWidth,
                             float nearDist, float depthRange, int size, float out[kViewCbRows][4]);

} // namespace sm2m
