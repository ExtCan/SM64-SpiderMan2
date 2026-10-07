// Heuristic camera discovery. Nobody has published Spider-Man 2's camera
// structure, so the mod looks for it: scan readable memory for a rotation +
// translation matrix whose translation sits a few metres from the hero and
// one of whose axes points at the hero (a third-person camera looking at its
// target), plus a perspective projection matrix whose aspect ratio matches
// the swap chain (which yields the live field of view).
//
// This file is platform independent (operates on byte spans) so it can be
// unit tested; the Windows region walk lives in game/sm2_camera.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../common/vec.h"

namespace sm2m {

enum class MatrixLayout : uint8_t {
    RowsAxes,    // f[0..2], f[4..6], f[8..10] are axes; translation f[12..14]
    ColumnsAxes, // axes are columns (f[0],f[4],f[8]) ...; translation f[3],f[7],f[11]
};

struct CameraPose {
    DVec3 position;
    Vec3 right, up, forward; // orthonormal, game space; forward = view direction
    float fovY = 1.0f;       // radians
    bool valid = false;
};

struct CameraCandidate {
    uintptr_t address = 0;
    MatrixLayout layout = MatrixLayout::RowsAxes;
    int forwardAxis = 2, forwardSign = 1;
    int upAxis = 1, upSign = 1;
    int rightAxis = 0;
    float score = 0;
    double distance = 0;
    // Runtime bookkeeping for the live validator.
    int validFrames = 0;
    int movedFrames = 0;
    DVec3 lastPos;
};

struct CameraSearchParams {
    float minDistance = 0.6f;       // game units from the hero's look-at point
    float maxDistance = 14.0f;
    float lookAtHeight = 1.0f;      // camera targets roughly the hero's chest
    float minLookDot = 0.80f;       // cos of max angle between an axis and the hero
    float orthoTolerance = 2.5e-3f; // |len-1| and |dot| tolerance
    float minUpDot = 0.25f;
};

struct ProjectionCandidate {
    uintptr_t address = 0;
    bool transposed = false;
    float fovY = 0; // radians
};

// Evaluates 16 floats as a camera world matrix. Fills `pose` (without fovY)
// and axis assignment in `cand` when plausible.
bool EvaluateCameraMatrix(const float* f, MatrixLayout layout, const DVec3& heroPos, const Vec3& worldUp,
                          const CameraSearchParams& params, CameraCandidate& cand, CameraPose& pose);

// Re-reads a known candidate (axes already assigned) into a pose. Returns
// false if the memory no longer looks like a camera.
bool PoseFromCandidate(const float* f, const CameraCandidate& cand, const CameraSearchParams& params, CameraPose& pose);

// Scans a span (16-byte aligned positions). `baseAddress` is the address the
// span represents in the target process (for reporting).
void ScanSpanForCameras(const uint8_t* data, size_t size, uintptr_t baseAddress, const DVec3& heroPos,
                        const Vec3& worldUp, const CameraSearchParams& params, std::vector<CameraCandidate>& out,
                        size_t maxOut);

// Perspective projection with |aspect - aspectExpected| small. Handles
// standard and reverse-Z, row- or column-vector conventions, TAA jitter.
bool EvaluateProjection(const float* f, float aspectExpected, ProjectionCandidate& out);
void ScanSpanForProjections(const uint8_t* data, size_t size, uintptr_t baseAddress, float aspect,
                            std::vector<ProjectionCandidate>& out, size_t maxOut);

// Picks the most common FOV among candidates (they are usually many copies of
// the same matrix). Returns 0 if none.
float MostCommonFov(const std::vector<ProjectionCandidate>& c);

// Tells which world handedness the camera basis implies, assuming the engine
// stores +right as the remaining axis: true = left-handed (+X right, +Y up,
// +Z forward, like Direct3D), false = right-handed.
bool CameraIsLeftHanded(const CameraCandidate& cand, const float* f);

} // namespace sm2m
