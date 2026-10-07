// HLSL for Mario's draws inside the game's frame. The G-buffer encoding is the
// game's own (render/gbuffer_format.h has the CPU reference and the source of
// each formula); the vertex transforms are those of VS_DefaultMaterialGBuffer
// and VS_ModelShadowCaster, reading the game's view constants (b0).
#pragma once

#include <cstdint>

namespace sm2m {

inline const char* kInjectShaderSource = R"HLSL(
// The game's per-view constants (36 rows): 0-2 camera axes, 3 camera
// position, 4-7 camera-relative view-projection, 8-11 previous one, 15
// previous camera position, 28.w flags (bit 12: orthographic), 33 motion
// scale, 34 1/render size, 35 (w epsilon, sky depth).
cbuffer GameView : register(b0) { float4 V[36]; };

cbuffer Draw : register(b1) {
    float4 uAnchorHi;     // Mario's reference point, split hi + lo for precision
    float4 uAnchorLo;
    float4 uPrevAnchorHi;
    float4 uPrevAnchorLo;
    float4 uMaterial;     // x gloss, y specular F0, z albedo scale, w shadow push (m)
    uint4  uBits;         // x GBuffer0.y base bits, y GBuffer1.y, z GBuffer1.x occlusion bits, w snapshot slot
    float4 uTex;          // x width, y height, z texture ready, w caps (bit 0 metal, bit 1 vanish)
    uint4  uMisc;         // x frame (low), y frame (high), z flags (1: light view must be orthographic),
                          // w GBuffer1.y with the metal cap (a game material: its reflections)
    // Where the camera of the view had Mario: the recent camera placements
    // and, for a view rendered from each, Mario's shift (this frame's and the
    // previous frame's Mario's).
    uint4  uAlign;        // x placements, y flags (1: find the view's, 2: the previous frame was aligned,
                          // 4: no motion this frame)
    float4 uAlignNewest;  // xyz: the shift when the view matches none (the newest placement's; shadows)
    float4 uAlignPrev;    // xyz: ... the previous frame's
    float4 uAlignTol;     // x: squared distance (m^2) within which a placement is the view's
    float4 uCam[32];      // xyz: where the camera was placed, w: the entry of the same camera's placement
                          // before it (-1: none)
    float4 uShift[32];    // xyz: Mario's shift for a view from there
    float4 uPrevShift[32];
};

ByteAddressBuffer gTex : register(t0);     // Mario's ROM texture atlas, RGBA8
RWByteAddressBuffer gSnap : register(u0);  // copy of V for the mod (camera, depth unprojection)

struct VIn {
    float3 pos  : POSITION;    // game space, relative to the anchor
    float3 prev : TEXCOORD1;   // last frame's, relative to the previous anchor
    float3 nrm  : NORMAL;
    float3 col  : COLOR;
    float2 uv   : TEXCOORD0;
    uint   vid  : SV_VertexID;
};

float4 ToClip(float3 rel, int r) { return rel.x * V[r] + rel.y * V[r + 1] + rel.z * V[r + 2] + V[r + 3]; }

// Mario's shift for a view rendered from `camPos`: that of the placement it
// came from (the nearest within the tolerance) - or, for a view the game
// drew from between two placements of one camera, as far between their
// shifts - else the newest's. Returns the placement's index in `which`
// (0x80: none matched, 0xFF: not in use).
float3 AlignShift(float3 camPos, bool prev, out uint which) {
    float3 s = prev ? uAlignPrev.xyz : uAlignNewest.xyz;
    which = 0x80u;
    if ((uAlign.y & 1u) == 0u) {
        which = 0xFFu;
        return s;
    }
    float best = uAlignTol.x;
    uint n = min(uAlign.x, 32u);
    for (uint k = 0u; k < n; ++k) {
        float3 sk = prev ? uPrevShift[k].xyz : uShift[k].xyz;
        float3 d = uCam[k].xyz - camPos;
        float dd = dot(d, d);
        if (dd < best) {
            best = dd;
            s = sk;
            which = k;
        }
        int j = int(uCam[k].w);
        if (j > int(k) && j < int(n)) {
            float3 a = uCam[j].xyz;
            float3 ab = uCam[k].xyz - a;
            float l2 = dot(ab, ab);
            if (l2 > 1e-8) {
                float t = saturate(dot(camPos - a, ab) / l2);
                float3 e = a + ab * t - camPos;
                float ee = dot(e, e);
                if (ee < best) {
                    best = ee;
                    float3 sj = prev ? uPrevShift[j].xyz : uShift[j].xyz;
                    s = lerp(sj, sk, t);
                    which = k;
                }
            }
        }
    }
    return s;
}

// This frame's shift (for the view, V[3]) and the previous frame's (for the
// view before, V[15]: the position Mario's motion vectors start from).
void Shifts(out float3 now, out float3 before, out uint which) {
    now = AlignShift(V[3].xyz, false, which);
    uint w2;
    if ((uAlign.y & 4u) != 0u) before = now;
    else if ((uAlign.y & 2u) != 0u) before = AlignShift(V[15].xyz, true, w2);
    else before = float3(0.0, 0.0, 0.0);
}

// ---------------------------------------------------------------- G-buffer

struct GVOut {
    float4 pos      : SV_Position;
    float4 prevClip : TEXCOORD1;
    float3 nrm      : NORMAL;
    float3 col      : COLOR;
    float2 uv       : TEXCOORD0;
};

GVOut VSGBuffer(VIn i) {
    GVOut o;
    float3 shift, prevShift;
    uint which;
    Shifts(shift, prevShift, which);
    float3 rel = (uAnchorHi.xyz - V[3].xyz) + uAnchorLo.xyz + i.pos + shift;
    o.pos = ToClip(rel, 4);
    float3 prel = (uPrevAnchorHi.xyz - V[15].xyz) + uPrevAnchorLo.xyz + i.prev + prevShift;
    o.prevClip = ToClip(prel, 8);
    // The camera jumped against Mario since the frame before (the game
    // rendered from another of its cameras, or back; a wall brought it in at
    // once): his motion vectors are as if it hadn't - as if it had followed
    // him, as it does - just his animation, seen from this view. (Otherwise
    // the game's motion blur smears him along the jump: 0.6's blocky trail.)
    float3 relNow = (V[3].xyz - uAnchorHi.xyz) - uAnchorLo.xyz - shift;
    float3 relPrev = (V[15].xyz - uPrevAnchorHi.xyz) - uPrevAnchorLo.xyz - prevShift;
    float3 jump = relNow - relPrev;
    if (dot(jump, jump) > 1.0) {
        float3 crel = (uAnchorHi.xyz - V[3].xyz) + uAnchorLo.xyz + i.prev + shift;
        o.prevClip = ToClip(crel, 4);
    }
    o.nrm = i.nrm;
    o.col = i.col;
    o.uv = i.uv;
    if (i.vid == 0 && uBits.w != 0xFFFFFFFFu) {
        uint base = uBits.w * 1024u;
        [unroll] for (int r = 0; r < 36; ++r) gSnap.Store4(base + uint(r) * 16u, asuint(V[r]));
        gSnap.Store4(base + 576u, uint4(uMisc.x, 0x4F52414Du, uMisc.y, 1u));
        gSnap.Store4(base + 592u, uint4(asuint(shift), which | (min(uAlign.x, 255u) << 8)));
    }
    return o;
}

float4 Texel(int2 p) {
    int2 size = int2(uTex.xy);
    p = clamp(p, int2(0, 0), size - 1);
    uint v = gTex.Load(uint(p.y * size.x + p.x) * 4u);
    return float4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24) / 255.0;
}

float4 SampleTex(float2 uv) {
    float2 t = uv * uTex.xy - 0.5;
    float2 fl = floor(t);
    float2 f = t - fl;
    int2 p = int2(fl);
    float4 a = lerp(Texel(p), Texel(p + int2(1, 0)), f.x);
    float4 b = lerp(Texel(p + int2(0, 1)), Texel(p + int2(1, 1)), f.x);
    return lerp(a, b, f.y);
}

// The wing cap's wings (atlas cells 9 and 10) are cut out by the texture's
// alpha, like SM64's alpha-tested wing quads; every other texture is a decal
// blended over the vertex colour. ((1, 1) means untextured.)
bool WingTexel(float2 uv) { return uv.x >= 9.0 / 11.0 && !(uv.x > 0.9999 && uv.y > 0.9999); }

float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

// GBuffer0.x bits 0-24: Lambert azimuthal normal (12 + 12 bits, sign of z).
uint PackNormal(float3 n) {
    float k = 5791.1675 * rsqrt(abs(n.z) * 8.0 + 8.0);
    uint ex = min((uint)max(n.x * k + 2047.75, 0.0), 4095u);
    uint ey = min((uint)max(n.y * k + 2047.75, 0.0), 4095u);
    return ex | (ey << 12) | (n.z < 0.0 ? (1u << 24) : 0u);
}

// GBuffer1.x bits 0-22: sqrt-encoded albedo 8:8:7.
uint PackAlbedo(float3 a) {
    float3 s = saturate(sqrt(saturate(a)));
    return min((uint)(s.r * 255.0 + 0.5), 255u) | (min((uint)(s.g * 255.0 + 0.5), 255u) << 8) |
           (min((uint)(s.b * 127.0 + 0.5), 127u) << 16);
}

// GBuffer0.y bits 0-21: sqrt-encoded specular colour 7:8:7.
uint PackSpecularColour(float3 f0) {
    float3 s = saturate(sqrt(saturate(f0)));
    return min((uint)(s.r * 127.0 + 0.5), 127u) | (min((uint)(s.g * 255.0 + 0.5), 255u) << 7) |
           (min((uint)(s.b * 127.0 + 0.5), 127u) << 15);
}
uint PackSpecular(float f0) { return PackSpecularColour(float3(f0, f0, f0)); }

struct GOut {
    float depth  : SV_Target0;  // linear view depth
    float2 motion : SV_Target1;
    uint2 g0     : SV_Target2;
    uint2 g1     : SV_Target3;
};

GOut PSGBuffer(GVOut i) {
    GOut o;
    float3 base = i.col;
    uint caps = (uint)(uTex.w + 0.5);
    if ((caps & 2u) != 0u) {
        // Vanish cap: see-through. Half the pixels, the other half next
        // frame - the game's temporal anti-aliasing blends them.
        uint2 px = uint2(i.pos.xy);
        if (((px.x + px.y + uMisc.x) & 1u) == 0u) discard;
    }
    if (uTex.z > 0.5) {
        float4 t = SampleTex(i.uv);
        if (WingTexel(i.uv) && t.a < 0.5) discard;
        base = lerp(i.col, t.rgb, t.a);
    }
    float3 lin = SrgbToLinear(saturate(base));
    float3 albedo = lin * uMaterial.z;
    float glossIn = uMaterial.x;
    uint spec = PackSpecular(uMaterial.y);
    uint occlusion = uBits.z;
    uint material = uBits.y;
    if ((caps & 1u) != 0u) {
        // Metal cap: polished metal - the colour goes into the reflection,
        // almost none is diffuse (a little stays, so he isn't black where
        // there is little to reflect). With a game material the game's own
        // reflection passes (screen-space or ray traced) cover him.
        spec = PackSpecularColour(lerp(0.55, 0.95, lin));
        albedo = lin * 0.10;
        glossIn = 0.92;
        occlusion = (255u << 23) | 0x80000000u;
        material = uMisc.w;
    }
    float3 n = normalize(i.nrm);
    o.depth = i.pos.w;
    float2 uvCur = i.pos.xy * V[34].xy;
    float2 uvPrev = i.prevClip.xy / max(i.prevClip.w, V[35].x) * float2(0.5, -0.5) + 0.5;
    o.motion = (uvCur - uvPrev) * V[33].xy;
    uint gloss = min((uint)(saturate(glossIn) * 127.0 + 0.5), 127u);
    o.g0 = uint2(PackNormal(n) | (gloss << 25), uBits.x | spec);
    o.g1 = uint2(PackAlbedo(albedo) | occlusion, material);
    return o;
}

// ---------------------------------------------------------------- shadows

struct SVOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

SVOut VSShadow(VIn i) {
    SVOut o;
    // (Another view - the light's: the newest placement's shift.)
    float3 rel = (uAnchorHi.xyz - V[3].xyz) + uAnchorLo.xyz + i.pos + uAlignNewest.xyz;
    // Like VS_ModelShadowCaster: push along the light direction (orthographic
    // views) or away from the light (perspective views).
    bool ortho = (asuint(V[28].w) & 4096u) != 0u;
    float3 dir = ortho ? V[2].xyz : normalize(rel);
    rel += dir * uMaterial.w;
    o.pos = ToClip(rel, 4);
    // A view taken from the cache-copy draw must be an orthographic light view.
    if ((uMisc.z & 1u) != 0u && !ortho) o.pos = float4(2.0, 2.0, 2.0, 1.0);
    o.uv = i.uv;
    return o;
}

// PS_ShadowCaster: depth = z * w. (The wings' cut-out parts cast no shadow.)
float PSShadow(SVOut i) : SV_Depth {
    if (uTex.z > 0.5 && WingTexel(i.uv) && SampleTex(i.uv).a < 0.5) discard;
    return i.pos.z * i.pos.w;
}

// For casters drawn with the rasteriser's depth (no pixel shader of their
// own): only the cut-out.
void PSShadowCut(SVOut i) {
    if (uTex.z > 0.5 && WingTexel(i.uv) && SampleTex(i.uv).a < 0.5) discard;
}

// ---------------------------------------------------------------- capture

Texture2D<float> gDepth : register(t0, space1);
ByteAddressBuffer gView : register(t1, space1);
Texture2D<float2> gMotion : register(t2, space1);  // the G-buffer's motion vectors (RT1)
RWByteAddressBuffer gOut : register(u0, space1);
cbuffer Reduce : register(b0, space1) {
    uint4 uReduce;   // out w, out h, src w, src h
    uint4 uReduce2;  // x: 1 = motion vectors too (after the depth, 2 floats per pixel)
};

[numthreads(8, 8, 1)]
void CSReduceDepth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uReduce.x || id.y >= uReduce.y) return;
    float2 inv = asfloat(gView.Load2(34u * 16u));
    float2 src = float2(uReduce.zw);
    float2 area = (inv.x > 0.0 && inv.y > 0.0) ? min(1.0 / inv, src) : src;
    int2 p = int2((float2(id.xy) + 0.5) * area / float2(uReduce.xy));
    uint i = id.y * uReduce.x + id.x;
    float d = gDepth.Load(int3(p, 0));
    gOut.Store(i * 4u, asuint(d));
    if (uReduce2.x != 0u) {
        float2 m = gMotion.Load(int3(p, 0));
        gOut.Store2(uReduce.x * uReduce.y * 4u + i * 8u, asuint(m));
    }
}
)HLSL";

// FNV-1a of kInjectShaderSource (must match shaderbin::kSourceHash in
// inject_shaders_bin.h, else that file is out of date).
inline uint64_t InjectShaderSourceHash() {
    uint64_t h = 1469598103934665603ull;
    for (const char* s = kInjectShaderSource; *s; ++s) {
        h ^= uint8_t(*s);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace sm2m
