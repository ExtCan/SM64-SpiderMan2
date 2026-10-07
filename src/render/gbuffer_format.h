// Spider-Man 2's G-buffer, as written by the game's built-in default material
// (PS_DefaultMaterialGBuffer) and read by its lighting (CS_ApplyGBufferLighting_*),
// both embedded in Spider-Man2.exe:
//
//   RT0  R32_FLOAT     linear view depth (SV_Position.w)
//   RT1  R16G16_FLOAT  motion: (uv_now - uv_previous) * view row 33
//   RT2  R32G32_UINT   "GBuffer0"
//          .x  bits 0-11  normal: Lambert azimuthal x  (12 bits)
//              bits 12-23 normal: Lambert azimuthal y
//              bit  24    normal z < 0
//              bits 25-31 gloss (7 bits; 0 for the default material)
//          .y  bits 0-21  specular colour 7:8:7 (0 for the default material)
//              bits 22-24 shading model (1 = standard; 0 = unlit/sky)
//              bits 25-28 flags (the default material sets bit 25)
//   RT3  R32G32_UINT   "GBuffer1"
//          .x  bits 0-22  albedo, sqrt-encoded 8:8:7 (linear = value^2)
//              bits 23-30 specular occlusion (8 bits, valid when bit 31 is set)
//              bit  31    "occlusion present"
//          .y  bits 16-31 material table index (0xFFFF = none)
//   depth D32_FLOAT_S8X24_UINT ("HyperDepthStencil")
//
// This header is the CPU reference used by the unit tests; the HLSL that
// Mario's draw uses (render/inject_shaders.h) follows the same formulas.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sm2m::gbuf {

constexpr uint32_t kShadingModelStd = 1;
constexpr uint32_t kG0yDefault = (kShadingModelStd << 22) | (1u << 25); // 0x02400000, as the default material
constexpr uint32_t kG1xOcclusion = (255u << 23) | 0x80000000u;          // occlusion 1.0
constexpr uint32_t kG1yNoMaterial = 0xFFFFu << 16;

inline float Saturate(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Linear albedo -> packed bits 0-22 (dither in [0,1); 0.5 rounds to nearest).
inline uint32_t PackAlbedo(float r, float g, float b, float dither = 0.5f) {
    const uint32_t R = uint32_t(Saturate(std::sqrt(Saturate(r))) * 255.0f + dither);
    const uint32_t G = uint32_t(Saturate(std::sqrt(Saturate(g))) * 255.0f + dither);
    const uint32_t B = uint32_t(Saturate(std::sqrt(Saturate(b))) * 127.0f + dither);
    return (std::min(R, 255u) | (std::min(G, 255u) << 8) | (std::min(B, 127u) << 16)) & 0x7FFFFFu;
}

// Mirrors the lighting shader's decode.
inline void UnpackAlbedo(uint32_t x, float rgb[3]) {
    const float r = float(x & 255u) / 255.0f, g = float((x >> 8) & 255u) / 255.0f, b = float((x >> 16) & 127u) / 127.0f;
    rgb[0] = r * r;
    rgb[1] = g * g;
    rgb[2] = b * b;
}

// Unit normal -> bits 0-24 of GBuffer0.x (Lambert azimuthal, |z| folded, sign bit).
inline uint32_t PackNormal(float nx, float ny, float nz, float dither = 0.5f) {
    const float k = 1.0f / std::sqrt(std::fabs(nz) * 8.0f + 8.0f) * 5791.1675f; // 4095 * sqrt(2)
    const float bias = dither * 0.5f + 2047.5f;
    const uint32_t ex = uint32_t(std::max(0.0f, nx * k + bias));
    const uint32_t ey = uint32_t(std::max(0.0f, ny * k + bias));
    return (std::min(ex, 4095u) | (std::min(ey, 4095u) << 12)) | (nz < 0.0f ? (1u << 24) : 0u);
}

inline void UnpackNormal(uint32_t x, float n[3]) {
    const float fx = float(x & 4095u) * 6.9067e-4f - 1.41421354f;  // 2*sqrt(2)/4095, -sqrt(2)
    const float fy = float((x >> 12) & 4095u) * 6.9067e-4f - 1.41421354f;
    const float d = fx * fx + fy * fy;
    const float s = std::sqrt(std::max(0.0f, 1.0f - d * 0.25f));
    n[0] = fx * s;
    n[1] = fy * s;
    const float z = 1.0f - d * 0.5f;
    n[2] = (x & (1u << 24)) ? -z : z;
}

struct Packed {
    uint32_t g0x, g0y, g1x, g1y;
};

// Everything the default material writes for one pixel (gloss 0, shading model 1).
inline Packed PackDefault(const float albedoLinear[3], const float normal[3], float dither = 0.5f) {
    Packed p;
    p.g0x = PackNormal(normal[0], normal[1], normal[2], dither);
    p.g0y = kG0yDefault;
    p.g1x = PackAlbedo(albedoLinear[0], albedoLinear[1], albedoLinear[2], dither) | kG1xOcclusion;
    p.g1y = kG1yNoMaterial;
    return p;
}

inline uint32_t ShadingModel(uint32_t g0y) { return (g0y >> 22) & 7u; }

// GBuffer1.x bits 23-31: the material's occlusion (0..1, 8 bits) and
// "present". The lighting (CS_ApplyGBufferLighting_Std) scales the probe
// reflections by it - and the sun too: in full sunlight the sun is multiplied
// by saturate(3 * occlusion - 0.75), which is 0 below 0.25. 0.4 went down to
// 0.12 to dim the reflections and so took the sun off Mario altogether,
// diffuse and highlights alike; it never goes below kMinOcclusion now.
constexpr float kMinOcclusion = 0.6f; // saturate(3 * 0.6 - 0.75) = 1: the sun is untouched
inline uint32_t PackSpecularOcclusion(float o) {
    const uint32_t v = uint32_t(Saturate(o) * 255.0f + 0.5f);
    return (std::min(v, 255u) << 23) | 0x80000000u;
}

// Mario's material for the SHINE setting (0..1) and [Render] Specular (F0 at
// SHINE 50%). SHINE 0 is the game's own default material - no gloss, no
// specular colour (only the Fresnel every surface has) - with the probe
// reflections at 60%; the highlights and reflections grow from there.
struct ShineLook {
    float gloss;     // GBuffer0.x bits 25-31
    float f0;        // GBuffer0.y specular colour (grey)
    float occlusion; // GBuffer1.x bits 23-30
};
inline ShineLook LookForShine(float shine, float specular) {
    const float t = Saturate(shine);
    ShineLook l;
    l.gloss = 0.60f * t;
    l.f0 = std::max(0.0f, specular) * 2.0f * t;
    l.occlusion = kMinOcclusion + (1.0f - kMinOcclusion) * t;
    return l;
}

} // namespace sm2m::gbuf
