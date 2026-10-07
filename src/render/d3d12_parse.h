// Byte-level parsers for the D3D12 data the mod's hooks see: serialized root
// signatures (RTS0), pipeline state streams (ID3D12Device2::CreatePipelineState),
// classic graphics pipeline descriptions and shader containers (DXBC/DXIL
// output signatures). Platform independent so they can be unit tested on
// Linux; layouts follow the public D3D12 headers (checked against
// DirectX-Headers with offsetof).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sm2m::d3d12p {

// ---- root signatures -----------------------------------------------------

enum class RootParamType : uint32_t { Table = 0, Constants = 1, Cbv = 2, Srv = 3, Uav = 4 };
// D3D12_DESCRIPTOR_RANGE_TYPE
enum : uint32_t { kRangeSrv = 0, kRangeUav = 1, kRangeCbv = 2, kRangeSampler = 3 };
// D3D12_SHADER_VISIBILITY
enum : uint32_t { kVisAll = 0, kVisVertex = 1, kVisPixel = 5 };

struct DescRange {
    uint32_t type = 0;
    uint32_t num = 0;       // 0xFFFFFFFF = unbounded
    uint32_t baseReg = 0;
    uint32_t space = 0;
    uint32_t offset = 0;    // absolute offset in the table (appends resolved)
};

struct RootParam {
    RootParamType type = RootParamType::Table;
    uint32_t visibility = 0; // D3D12_SHADER_VISIBILITY (0 = all, 1 = VS, 5 = PS)
    uint32_t shaderRegister = 0, registerSpace = 0, num32BitValues = 0;
    std::vector<DescRange> ranges; // descriptor tables only
};

struct TableSlot {
    int param = -1;        // root parameter index, -1 if not found
    uint32_t offset = 0;   // descriptor index inside that table
};

struct RootSigInfo {
    uint32_t version = 0; // 1 = 1.0, 2 = 1.1, 3 = 1.2
    uint32_t flags = 0;
    std::vector<RootParam> params;
    // Index of the root CBV bound to b<reg> space<space> visible to the vertex
    // shader, or -1.
    int FindRootCbv(uint32_t reg, uint32_t space) const;
    // The descriptor table entry that a shader stage (kVisVertex / kVisPixel)
    // sees as register `reg` of `rangeType` in `space`.
    TableSlot FindTableDescriptor(uint32_t rangeType, uint32_t reg, uint32_t space, uint32_t stage) const;
};

// `blob` is what ID3D12Device::CreateRootSignature receives: a DXBC container
// with an RTS0 part, or a bare RTS0 part.
bool ParseRootSignature(const void* blob, size_t size, RootSigInfo& out, std::string* why = nullptr);

// ---- shader containers ---------------------------------------------------

struct ShaderInfo {
    bool present = false;
    uint8_t digest[16] = {}; // the container's checksum: identifies a shader exactly
    bool writesDepth = false; // SV_Depth / SV_DepthGreaterEqual / SV_DepthLessEqual output
    int numTargets = 0;       // SV_Target outputs
};

// Parses the DXBC container header and its output signature (OSGN/OSG1/OSG5).
bool ParseShader(const void* bytecode, size_t size, ShaderInfo& out);

// "0123..." (32 hex chars) -> digest; false if malformed.
bool ParseDigest(const std::string& hex, uint8_t out[16]);
std::string DigestHex(const uint8_t d[16]);

// ---- pipeline states -----------------------------------------------------

struct StencilOps {
    uint32_t fail = 1, depthFail = 1, pass = 1; // D3D12_STENCIL_OP (1 = KEEP)
    uint32_t func = 8;                          // D3D12_COMPARISON_FUNC (8 = ALWAYS)
};

struct PsoInfo {
    bool graphics = true;
    void* rootSignature = nullptr;
    uint32_t numRenderTargets = 0;
    uint32_t rtvFormats[8] = {};
    uint32_t dsvFormat = 0;
    bool depthEnable = false;
    bool depthWrite = false;
    uint32_t depthFunc = 0; // D3D12_COMPARISON_FUNC (1 never .. 8 always)
    bool stencilEnable = false;
    uint8_t stencilReadMask = 0xFF, stencilWriteMask = 0xFF;
    StencilOps front, back;
    int32_t depthBias = 0;
    float depthBiasClamp = 0, slopeScaledDepthBias = 0;
    bool depthClipEnable = true;
    uint32_t cullMode = 1;      // D3D12_CULL_MODE (1 none, 2 front, 3 back)
    bool frontCounterClockwise = false;
    uint32_t sampleCount = 1, sampleQuality = 0;
    uint32_t topologyType = 3;  // TRIANGLE
    uint8_t rtWriteMask[8] = {15, 15, 15, 15, 15, 15, 15, 15};
    ShaderInfo vs, ps, gs;
    uint32_t viewInstanceCount = 0;
};

// Parses a D3D12_PIPELINE_STATE_STREAM_DESC payload.
bool ParsePipelineStream(const void* stream, size_t size, PsoInfo& out, std::string* why = nullptr);
// Parses a D3D12_GRAPHICS_PIPELINE_STATE_DESC (x64 layout, 656 bytes).
bool ParseGraphicsPsoDesc(const void* desc, PsoInfo& out);
constexpr size_t kGraphicsPsoDescSize = 656;

// Comparison funcs (D3D12_COMPARISON_FUNC).
enum : uint32_t { kCmpNever = 1, kCmpLess, kCmpEqual, kCmpLessEqual, kCmpGreater, kCmpNotEqual, kCmpGreaterEqual, kCmpAlways };
// True for GREATER / GREATER_EQUAL (reverse-Z).
inline bool IsReverseZFunc(uint32_t f) { return f == kCmpGreater || f == kCmpGreaterEqual; }

// The depth test of Mario's G-buffer draw, whatever the pipeline of the
// segment he is drawn after uses: an ordinary nearer-or-equal test in the
// game's depth direction. EQUAL (draws after a depth pre-pass) would hide him
// - nothing of his is in the pre-pass - and ALWAYS would show him through
// walls. (Equal still passes: he is drawn once per segment.) EQUAL, ALWAYS
// and the like don't tell the direction: `reverseZ` is the one the game's
// other pipelines use (Spider-Man 2: reverse-Z).
inline uint32_t MarioDepthFunc(uint32_t gameFunc, bool reverseZ = true) {
    if (gameFunc == kCmpLess || gameFunc == kCmpLessEqual) return kCmpLessEqual;
    if (gameFunc == kCmpGreater || gameFunc == kCmpGreaterEqual) return kCmpGreaterEqual;
    return reverseZ ? kCmpGreaterEqual : kCmpLessEqual;
}

// Mario writes the stencil the way the segment's pipeline does (its pass op,
// with the list's reference value) but is never tested against it: a copied
// test - say EQUAL to a mask the pixels behind him don't carry - culls him.
inline StencilOps MarioStencilOps(const StencilOps& game) {
    StencilOps o;
    o.fail = 1;      // KEEP
    o.depthFail = 1; // KEEP
    o.pass = game.pass ? game.pass : 1;
    o.func = kCmpAlways;
    return o;
}

// DXGI formats the mod cares about.
enum : uint32_t {
    kFmtUnknown = 0,
    kFmtR32G32B32A32Float = 2,
    kFmtR16G16B16A16Float = 10,
    kFmtR32G32Uint = 17,
    kFmtR32G8X24Typeless = 19,
    kFmtD32FloatS8X24Uint = 20,
    kFmtR32FloatX8X24Typeless = 21,
    kFmtR10G10B10A2Unorm = 24,
    kFmtR11G11B10Float = 26,
    kFmtR8G8B8A8Unorm = 28,
    kFmtR16G16Float = 34,
    kFmtR32Typeless = 39,
    kFmtD32Float = 40,
    kFmtR32Float = 41,
    kFmtR32Uint = 42,
    kFmtR24G8Typeless = 44,
    kFmtD24UnormS8Uint = 45,
    kFmtR16Typeless = 53,
    kFmtR16Float = 54,
    kFmtD16Unorm = 55,
    kFmtR16Unorm = 56,
    kFmtB8G8R8A8Unorm = 87,
};
bool IsDepthFormat(uint32_t f);
bool HasStencil(uint32_t f);
const char* FormatName(uint32_t f);

} // namespace sm2m::d3d12p
