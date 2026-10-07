#include "d3d12_parse.h"

#include <cstdio>
#include <cstring>

namespace sm2m::d3d12p {
namespace {

template <typename T>
bool Rd(const uint8_t* base, size_t size, size_t off, T& out) {
    if (off > size || size - off < sizeof(T)) return false;
    std::memcpy(&out, base + off, sizeof(T));
    return true;
}

template <typename T>
T Get(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

// Finds a part in a DXBC container. Returns false if `data` isn't a container.
bool FindPart(const uint8_t* d, size_t size, const char* fourcc, const uint8_t*& part, size_t& partSize) {
    if (size < 32 || std::memcmp(d, "DXBC", 4) != 0) return false;
    uint32_t total = 0, count = 0;
    Rd(d, size, 24, total);
    Rd(d, size, 28, count);
    if (count > 64) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off = 0, sz = 0;
        if (!Rd(d, size, 32 + 4 * i, off) || !Rd(d, size, size_t(off) + 4, sz)) return false;
        if (size_t(off) + 8 + sz > size) return false;
        if (std::memcmp(d + off, fourcc, 4) == 0) {
            part = d + off + 8;
            partSize = sz;
            return true;
        }
    }
    part = nullptr;
    partSize = 0;
    return true;
}

// D3D12_RASTERIZER_DESC / DESC1 / DESC2 share the first 28 bytes except the
// type of DepthBias (INT in DESC, FLOAT in DESC1/DESC2).
void ReadRasterizer(const uint8_t* p, bool floatBias, PsoInfo& out) {
    out.cullMode = Get<uint32_t>(p, 4);
    out.frontCounterClockwise = Get<uint32_t>(p, 8) != 0;
    if (floatBias) out.depthBias = int32_t(Get<float>(p, 12));
    else out.depthBias = Get<int32_t>(p, 12);
    out.depthBiasClamp = Get<float>(p, 16);
    out.slopeScaledDepthBias = Get<float>(p, 20);
    out.depthClipEnable = Get<uint32_t>(p, 24) != 0;
}

void ReadOps(const uint8_t* p, StencilOps& o) {
    o.fail = Get<uint32_t>(p, 0);
    o.depthFail = Get<uint32_t>(p, 4);
    o.pass = Get<uint32_t>(p, 8);
    o.func = Get<uint32_t>(p, 12);
}

// D3D12_DEPTH_STENCIL_DESC (and DESC1, which only appends a BOOL).
void ReadDepthStencil(const uint8_t* p, PsoInfo& out) {
    out.depthEnable = Get<uint32_t>(p, 0) != 0;
    out.depthWrite = Get<uint32_t>(p, 4) != 0;
    out.depthFunc = Get<uint32_t>(p, 8);
    out.stencilEnable = Get<uint32_t>(p, 12) != 0;
    out.stencilReadMask = p[16];
    out.stencilWriteMask = p[17];
    ReadOps(p + 20, out.front);
    ReadOps(p + 36, out.back);
}

// D3D12_DEPTH_STENCIL_DESC2: per-face masks.
void ReadDepthStencil2(const uint8_t* p, PsoInfo& out) {
    out.depthEnable = Get<uint32_t>(p, 0) != 0;
    out.depthWrite = Get<uint32_t>(p, 4) != 0;
    out.depthFunc = Get<uint32_t>(p, 8);
    out.stencilEnable = Get<uint32_t>(p, 12) != 0;
    ReadOps(p + 16, out.front);
    out.stencilReadMask = p[32];
    out.stencilWriteMask = p[33];
    ReadOps(p + 36, out.back);
}

void ReadBlend(const uint8_t* p, PsoInfo& out) {
    // D3D12_BLEND_DESC: two BOOLs, then 8 x D3D12_RENDER_TARGET_BLEND_DESC
    // (40 bytes each; RenderTargetWriteMask is the UINT8 at +36).
    const bool independent = Get<uint32_t>(p, 4) != 0;
    for (int i = 0; i < 8; ++i) out.rtWriteMask[i] = p[8 + 40 * (independent ? i : 0) + 36];
}

void ReadShader(const uint8_t* p, ShaderInfo& s) {
    const uint64_t ptr = Get<uint64_t>(p, 0), len = Get<uint64_t>(p, 8);
    if (ptr && len) ParseShader(reinterpret_cast<const void*>(uintptr_t(ptr)), size_t(len), s);
}

} // namespace

int RootSigInfo::FindRootCbv(uint32_t reg, uint32_t space) const {
    for (size_t i = 0; i < params.size(); ++i) {
        const RootParam& p = params[i];
        if (p.type == RootParamType::Cbv && p.shaderRegister == reg && p.registerSpace == space &&
            (p.visibility == kVisAll || p.visibility == kVisVertex))
            return int(i);
    }
    return -1;
}

TableSlot RootSigInfo::FindTableDescriptor(uint32_t rangeType, uint32_t reg, uint32_t space, uint32_t stage) const {
    TableSlot slot;
    for (size_t i = 0; i < params.size(); ++i) {
        const RootParam& p = params[i];
        if (p.type != RootParamType::Table || (p.visibility != kVisAll && p.visibility != stage)) continue;
        for (const DescRange& r : p.ranges) {
            if (r.type != rangeType || r.space != space || reg < r.baseReg) continue;
            if (r.num != 0xFFFFFFFFu && reg - r.baseReg >= r.num) continue;
            slot.param = int(i);
            slot.offset = r.offset + (reg - r.baseReg);
            return slot;
        }
    }
    return slot;
}

bool ParseRootSignature(const void* blob, size_t size, RootSigInfo& out, std::string* why) {
    auto fail = [&](const char* r) {
        if (why) *why = r;
        return false;
    };
    const uint8_t* d = static_cast<const uint8_t*>(blob);
    const uint8_t* rs = d;
    size_t rsSize = size;
    const uint8_t* part = nullptr;
    size_t partSize = 0;
    if (FindPart(d, size, "RTS0", part, partSize)) {
        if (!part) return fail("container without RTS0");
        rs = part;
        rsSize = partSize;
    }
    uint32_t version = 0, numParams = 0, paramsOff = 0, numSamplers = 0, samplersOff = 0, flags = 0;
    if (!Rd(rs, rsSize, 0, version) || !Rd(rs, rsSize, 4, numParams) || !Rd(rs, rsSize, 8, paramsOff) ||
        !Rd(rs, rsSize, 12, numSamplers) || !Rd(rs, rsSize, 16, samplersOff) || !Rd(rs, rsSize, 20, flags))
        return fail("truncated header");
    if (version < 1 || version > 3) return fail("unknown root signature version");
    if (numParams > 64) return fail("too many parameters");
    out.version = version;
    out.flags = flags;
    out.params.clear();
    const size_t rangeSize = version == 1 ? 20 : 24;
    for (uint32_t i = 0; i < numParams; ++i) {
        uint32_t type = 0, vis = 0, payload = 0;
        const size_t h = size_t(paramsOff) + 12 * i;
        if (!Rd(rs, rsSize, h, type) || !Rd(rs, rsSize, h + 4, vis) || !Rd(rs, rsSize, h + 8, payload))
            return fail("truncated parameter");
        if (type > 4) return fail("unknown parameter type");
        RootParam p;
        p.type = RootParamType(type);
        p.visibility = vis;
        if (p.type == RootParamType::Table) {
            uint32_t numRanges = 0, rangesOff = 0;
            if (!Rd(rs, rsSize, payload, numRanges) || !Rd(rs, rsSize, size_t(payload) + 4, rangesOff))
                return fail("truncated table");
            if (numRanges > 64) return fail("too many ranges");
            uint32_t next = 0;
            for (uint32_t k = 0; k < numRanges; ++k) {
                const size_t r = size_t(rangesOff) + rangeSize * k;
                DescRange dr;
                uint32_t off = 0;
                if (!Rd(rs, rsSize, r, dr.type) || !Rd(rs, rsSize, r + 4, dr.num) || !Rd(rs, rsSize, r + 8, dr.baseReg) ||
                    !Rd(rs, rsSize, r + 12, dr.space) || !Rd(rs, rsSize, r + rangeSize - 4, off))
                    return fail("truncated range");
                if (dr.type > 3) return fail("unknown range type");
                dr.offset = off == 0xFFFFFFFFu ? next : off; // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                next = dr.num == 0xFFFFFFFFu ? dr.offset : dr.offset + dr.num;
                p.ranges.push_back(dr);
            }
        } else if (p.type == RootParamType::Constants) {
            if (!Rd(rs, rsSize, payload, p.shaderRegister) || !Rd(rs, rsSize, size_t(payload) + 4, p.registerSpace) ||
                !Rd(rs, rsSize, size_t(payload) + 8, p.num32BitValues))
                return fail("truncated constants");
        } else {
            if (!Rd(rs, rsSize, payload, p.shaderRegister) || !Rd(rs, rsSize, size_t(payload) + 4, p.registerSpace))
                return fail("truncated descriptor");
        }
        out.params.push_back(std::move(p));
    }
    return true;
}

bool ParseShader(const void* bytecode, size_t size, ShaderInfo& out) {
    out = ShaderInfo{};
    if (!bytecode || size < 32) return false;
    const uint8_t* d = static_cast<const uint8_t*>(bytecode);
    if (std::memcmp(d, "DXBC", 4) != 0) return false;
    out.present = true;
    std::memcpy(out.digest, d + 4, 16);
    const char* names[3] = {"OSG1", "OSG5", "OSGN"};
    const size_t strides[3] = {32, 28, 24};
    const size_t lead[3] = {4, 4, 0}; // OSG1/OSG5 start with a stream index
    for (int k = 0; k < 3; ++k) {
        const uint8_t* part = nullptr;
        size_t ps = 0;
        if (!FindPart(d, size, names[k], part, ps) || !part) continue;
        uint32_t count = 0, off = 0;
        if (!Rd(part, ps, 0, count) || !Rd(part, ps, 4, off) || count > 64) return true;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t e = size_t(off) + strides[k] * i + lead[k];
            uint32_t nameOff = 0, semIdx = 0, sysVal = 0;
            if (!Rd(part, ps, e, nameOff) || !Rd(part, ps, e + 4, semIdx) || !Rd(part, ps, e + 8, sysVal)) break;
            std::string name;
            for (size_t c = nameOff; c < ps && part[c] && name.size() < 32; ++c) name.push_back(char(part[c]));
            // D3D_NAME_DEPTH 65, DEPTH_GREATER_EQUAL 67, DEPTH_LESS_EQUAL 68; TARGET 64
            if (sysVal == 65 || sysVal == 67 || sysVal == 68 || name == "SV_Depth" || name == "SV_DepthGreaterEqual" ||
                name == "SV_DepthLessEqual")
                out.writesDepth = true;
            else if (sysVal == 64 || name == "SV_Target")
                ++out.numTargets;
        }
        return true;
    }
    return true;
}

bool ParseDigest(const std::string& hexIn, uint8_t out[16]) {
    std::string hex;
    for (char c : hexIn)
        if (c != ' ' && c != '-' && c != '\t') hex.push_back(c);
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = hex[size_t(i * 2 + k)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        out[i] = uint8_t(v);
    }
    return true;
}

std::string DigestHex(const uint8_t d[16]) {
    char buf[33];
    for (int i = 0; i < 16; ++i) std::snprintf(buf + i * 2, 3, "%02x", d[i]);
    return std::string(buf, 32);
}

bool IsDepthFormat(uint32_t f) {
    return f == kFmtD32FloatS8X24Uint || f == kFmtD32Float || f == kFmtD24UnormS8Uint || f == kFmtD16Unorm ||
           f == kFmtR32G8X24Typeless || f == kFmtR24G8Typeless;
}

bool HasStencil(uint32_t f) {
    return f == kFmtD32FloatS8X24Uint || f == kFmtD24UnormS8Uint || f == kFmtR32G8X24Typeless || f == kFmtR24G8Typeless;
}

const char* FormatName(uint32_t f) {
    switch (f) {
    case kFmtUnknown: return "UNKNOWN";
    case kFmtR32G32B32A32Float: return "R32G32B32A32_FLOAT";
    case kFmtR16G16B16A16Float: return "R16G16B16A16_FLOAT";
    case 11: return "R16G16B16A16_UNORM";
    case 13: return "R16G16B16A16_SNORM";
    case 16: return "R32G32_FLOAT";
    case kFmtR32G32Uint: return "R32G32_UINT";
    case kFmtR32G8X24Typeless: return "R32G8X24_TYPELESS";
    case kFmtD32FloatS8X24Uint: return "D32_FLOAT_S8X24_UINT";
    case kFmtR10G10B10A2Unorm: return "R10G10B10A2_UNORM";
    case 25: return "R10G10B10A2_UINT";
    case kFmtR11G11B10Float: return "R11G11B10_FLOAT";
    case 27: return "R8G8B8A8_TYPELESS";
    case kFmtR8G8B8A8Unorm: return "R8G8B8A8_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 30: return "R8G8B8A8_UINT";
    case 35: return "R16G16_UNORM";
    case 36: return "R16G16_UINT";
    case 37: return "R16G16_SNORM";
    case kFmtR16G16Float: return "R16G16_FLOAT";
    case kFmtR32Typeless: return "R32_TYPELESS";
    case kFmtD32Float: return "D32_FLOAT";
    case kFmtR32Float: return "R32_FLOAT";
    case kFmtR32Uint: return "R32_UINT";
    case kFmtR24G8Typeless: return "R24G8_TYPELESS";
    case kFmtD24UnormS8Uint: return "D24_UNORM_S8_UINT";
    case 49: return "R8G8_UNORM";
    case 50: return "R8G8_UINT";
    case kFmtR16Typeless: return "R16_TYPELESS";
    case kFmtR16Float: return "R16_FLOAT";
    case kFmtD16Unorm: return "D16_UNORM";
    case kFmtR16Unorm: return "R16_UNORM";
    case 57: return "R16_UINT";
    case 61: return "R8_UNORM";
    case 62: return "R8_UINT";
    case kFmtB8G8R8A8Unorm: return "B8G8R8A8_UNORM";
    default: return "?";
    }
}

bool ParsePipelineStream(const void* stream, size_t size, PsoInfo& out, std::string* why) {
    auto fail = [&](const char* r) {
        if (why) *why = r;
        return false;
    };
    out = PsoInfo{};
    // D3D12 defaults for subobjects that are absent.
    out.depthEnable = true;
    out.depthWrite = true;
    out.depthFunc = kCmpLess;
    out.cullMode = 3; // BACK
    const uint8_t* d = static_cast<const uint8_t*>(stream);
    size_t off = 0;
    while (off + 4 <= size) {
        uint32_t type = 0;
        Rd(d, size, off, type);
        size_t innerSize = 0, innerAlign = 4;
        switch (type) {
        case 0: innerSize = 8; innerAlign = 8; break;               // root signature
        case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25:
            innerSize = 16; innerAlign = 8; break;                   // shader bytecode
        case 7: innerSize = 32; innerAlign = 8; break;               // stream output
        case 8: innerSize = 328; break;                              // blend
        case 9: case 13: case 14: case 16: case 18: case 20: innerSize = 4; break;
        case 10: case 27: innerSize = 44; break;                     // rasterizer / rasterizer1
        case 28: innerSize = 40; break;                              // rasterizer2
        case 11: innerSize = 52; break;                              // depth stencil
        case 21: innerSize = 56; break;                              // depth stencil1
        case 26: innerSize = 60; break;                              // depth stencil2
        case 12: innerSize = 16; innerAlign = 8; break;              // input layout
        case 15: innerSize = 36; break;                              // RT formats
        case 17: innerSize = 8; break;                               // sample desc
        case 19: innerSize = 16; innerAlign = 8; break;              // cached PSO
        case 22: innerSize = 24; innerAlign = 8; break;              // view instancing
        default: return fail("unknown pipeline subobject");
        }
        const size_t in = (off + 4 + innerAlign - 1) & ~(innerAlign - 1);
        if (in + innerSize > size) return fail("truncated pipeline stream");
        const uint8_t* p = d + in;
        switch (type) {
        case 0: std::memcpy(&out.rootSignature, p, 8); break;
        case 1: ReadShader(p, out.vs); break;
        case 2: ReadShader(p, out.ps); break;
        case 5: ReadShader(p, out.gs); break;
        case 6: out.graphics = false; break;
        case 8: ReadBlend(p, out); break;
        case 10: ReadRasterizer(p, false, out); break;
        case 27: case 28: ReadRasterizer(p, true, out); break;
        case 11: case 21: ReadDepthStencil(p, out); break;
        case 26: ReadDepthStencil2(p, out); break;
        case 14: std::memcpy(&out.topologyType, p, 4); break;
        case 15: {
            std::memcpy(out.rtvFormats, p, 32);
            std::memcpy(&out.numRenderTargets, p + 32, 4);
            if (out.numRenderTargets > 8) return fail("bad render target count");
            break;
        }
        case 16: std::memcpy(&out.dsvFormat, p, 4); break;
        case 17:
            std::memcpy(&out.sampleCount, p, 4);
            std::memcpy(&out.sampleQuality, p + 4, 4);
            break;
        case 22: std::memcpy(&out.viewInstanceCount, p, 4); break;
        default: break;
        }
        off = (in + innerSize + 7) & ~size_t(7);
    }
    if (out.dsvFormat == kFmtUnknown) out.depthEnable = out.depthWrite = false;
    return true;
}

bool ParseGraphicsPsoDesc(const void* descIn, PsoInfo& out) {
    out = PsoInfo{};
    if (!descIn) return false;
    const uint8_t* p = static_cast<const uint8_t*>(descIn);
    out.graphics = true;
    std::memcpy(&out.rootSignature, p + 0, 8);
    ReadShader(p + 8, out.vs);
    ReadShader(p + 24, out.ps);
    ReadShader(p + 72, out.gs);
    ReadBlend(p + 120, out);
    ReadRasterizer(p + 452, false, out);
    ReadDepthStencil(p + 496, out);
    out.topologyType = Get<uint32_t>(p, 572);
    out.numRenderTargets = Get<uint32_t>(p, 576);
    if (out.numRenderTargets > 8) return false;
    std::memcpy(out.rtvFormats, p + 580, 32);
    out.dsvFormat = Get<uint32_t>(p, 612);
    out.sampleCount = Get<uint32_t>(p, 616);
    out.sampleQuality = Get<uint32_t>(p, 620);
    if (out.dsvFormat == kFmtUnknown) out.depthEnable = out.depthWrite = false;
    return true;
}

} // namespace sm2m::d3d12p
