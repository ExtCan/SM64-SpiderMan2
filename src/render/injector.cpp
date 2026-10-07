#ifdef _WIN32

#include "injector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/log.h"
#include "d3d12_hook.h"
#include "frame_tracker.h"
#include "gbuffer_format.h"
#include "inject_shaders.h"
#include "inject_shaders_bin.h"

namespace sm2m {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

// b1 of the injected draws (cbuffer Draw in inject_shaders.h; a multiple of
// 256 bytes, root CBV alignment).
constexpr int kAlignRows = 32; // Injector::kAlignEntries
struct DrawConstants {
    float anchorHi[4];
    float anchorLo[4];
    float prevAnchorHi[4];
    float prevAnchorLo[4];
    float material[4];
    uint32_t bits[4];
    float tex[4];
    uint32_t misc[4];
    // Where the camera of the view had Mario (Injector::SetPlacementSource)
    uint32_t align[4];     // x placements, y flags (kAlignMatch, kAlignPrev, kAlignStill)
    float alignNewest[4];  // xyz: the shift when the view matches none (the newest placement's)
    float alignPrev[4];    // xyz: ... the previous frame's
    float alignTol[4];     // x: squared distance (m^2) within which a placement is the view's
    float cam[kAlignRows][4];       // xyz: where the camera was placed, w: the entry of the same camera's
                                    // placement before it (-1: none)
    float shift[kAlignRows][4];     // xyz: Mario's shift for a view from there
    float prevShift[kAlignRows][4]; // xyz: ... the previous frame's Mario's
    uint8_t pad[2048 - (12 + 3 * kAlignRows) * 16];
};
static_assert(sizeof(DrawConstants) == 2048, "draw constants");
constexpr uint32_t kAlignMatch = 1; // find the placement the view came from (G-buffer draws)
constexpr uint32_t kAlignPrev = 2;  // the previous frame was drawn aligned (its shift from V[15])
constexpr uint32_t kAlignStill = 4; // no motion this frame: the previous position is this one

struct Vertex {
    float pos[3], prev[3], nrm[3], col[3], uv[2];
};
static_assert(sizeof(Vertex) == 56, "vertex");

void Split(double v, float& hi, float& lo) {
    hi = float(v);
    lo = float(v - double(hi));
}

const D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
const D3D12_RESOURCE_STATES kTexState =
    D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

constexpr UINT kReadbackSnapOffset = 0;     // live view (from the G-buffer draw)
constexpr UINT kReadbackCapSnapOffset = 1024; // view that rendered the captured depth
constexpr UINT kReadbackDepthOffset = 2048;
constexpr uint32_t kSnapMagic = 0x4F52414Du; // "MARO"

bool Compile(HMODULE comp, const char* entry, const char* target, std::vector<uint8_t>& out, std::string& err) {
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(comp, "D3DCompile")));
    if (!compile) {
        err = "D3DCompile missing";
        return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* msgs = nullptr;
    const HRESULT hr = compile(kInjectShaderSource, std::strlen(kInjectShaderSource), "sm2mario_inject", nullptr,
                               nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &msgs);
    if (FAILED(hr) || !code) {
        err = std::string(entry) + ": " + (msgs ? static_cast<const char*>(msgs->GetBufferPointer()) : "compile failed");
        SafeRelease(msgs);
        SafeRelease(code);
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    out.assign(p, p + code->GetBufferSize());
    SafeRelease(code);
    SafeRelease(msgs);
    return true;
}

ID3D12RootSignature* MakeRootSignature(ID3D12Device* dev, const D3D12_ROOT_SIGNATURE_DESC& desc, std::string& err) {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                         ID3DBlob**);
    auto serialize = d3d12 ? reinterpret_cast<SerializeFn>(
                                 reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")))
                           : nullptr;
    if (!serialize) {
        err = "D3D12SerializeRootSignature missing";
        return nullptr;
    }
    ID3DBlob* blob = nullptr;
    ID3DBlob* msgs = nullptr;
    ID3D12RootSignature* rs = nullptr;
    if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &msgs)) ||
        FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
        err = std::string("root signature: ") + (msgs ? static_cast<const char*>(msgs->GetBufferPointer()) : "failed");
        rs = nullptr;
    }
    SafeRelease(blob);
    SafeRelease(msgs);
    return rs;
}

ID3D12Resource* MakeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

uint32_t PackOps(const d3d12p::StencilOps& o) { return (o.fail & 15) | ((o.depthFail & 15) << 4) | ((o.pass & 15) << 8) | ((o.func & 15) << 12); }

d3d12p::StencilOps UnpackOps(uint32_t v) {
    d3d12p::StencilOps o;
    o.fail = v & 15;
    o.depthFail = (v >> 4) & 15;
    o.pass = (v >> 8) & 15;
    o.func = (v >> 12) & 15;
    return o;
}

D3D12_DEPTH_STENCILOP_DESC ToOpDesc(const d3d12p::StencilOps& o) {
    D3D12_DEPTH_STENCILOP_DESC d{};
    d.StencilFailOp = D3D12_STENCIL_OP(o.fail ? o.fail : 1);
    d.StencilDepthFailOp = D3D12_STENCIL_OP(o.depthFail ? o.depthFail : 1);
    d.StencilPassOp = D3D12_STENCIL_OP(o.pass ? o.pass : 1);
    d.StencilFunc = D3D12_COMPARISON_FUNC(o.func ? o.func : 8);
    return d;
}

} // namespace

bool Injector::PsoKey::operator==(const PsoKey& o) const {
    return kind == o.kind && numRt == o.numRt && std::memcmp(rtFormats, o.rtFormats, sizeof(rtFormats)) == 0 &&
           dsvFormat == o.dsvFormat && samples == o.samples && quality == o.quality && depthFunc == o.depthFunc &&
           stencil == o.stencil && frontOps == o.frontOps && backOps == o.backOps && depthBias == o.depthBias &&
           biasClamp == o.biasClamp && slopeBias == o.slopeBias && depthClip == o.depthClip && rootCbv == o.rootCbv;
}

Injector::~Injector() {
    // Deliberately leaked with the mod (see MarioMod::Get); nothing to do.
}

bool Injector::CompileShaders(std::string& error) {
    // Precompiled with Microsoft's compiler at build time (tools/compile_shaders);
    // compiled here only if the source changed without regenerating them.
    if (InjectShaderSourceHash() == shaderbin::kSourceHash) {
        vsGBuffer_.assign(std::begin(shaderbin::kVSGBuffer), std::end(shaderbin::kVSGBuffer));
        psGBuffer_.assign(std::begin(shaderbin::kPSGBuffer), std::end(shaderbin::kPSGBuffer));
        vsShadow_.assign(std::begin(shaderbin::kVSShadow), std::end(shaderbin::kVSShadow));
        psShadow_.assign(std::begin(shaderbin::kPSShadow), std::end(shaderbin::kPSShadow));
        psShadowCut_.assign(std::begin(shaderbin::kPSShadowCut), std::end(shaderbin::kPSShadowCut));
        csDownsample_.assign(std::begin(shaderbin::kCSReduceDepth), std::end(shaderbin::kCSReduceDepth));
        return true;
    }
    LOGW("in-world rendering: precompiled shaders are stale - compiling them now");
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!comp) {
        error = "d3dcompiler_47.dll not found";
        return false;
    }
    return Compile(comp, "VSGBuffer", "vs_5_1", vsGBuffer_, error) && Compile(comp, "PSGBuffer", "ps_5_1", psGBuffer_, error) &&
           Compile(comp, "VSShadow", "vs_5_1", vsShadow_, error) && Compile(comp, "PSShadow", "ps_5_1", psShadow_, error) &&
           Compile(comp, "PSShadowCut", "ps_5_1", psShadowCut_, error) &&
           Compile(comp, "CSReduceDepth", "cs_5_1", csDownsample_, error);
}

void Injector::SetCaptureSize(int width, int height) {
    captureW_ = std::max(64, std::min(width, kMaxCaptureW));
    captureH_ = std::max(36, std::min(height, kMaxCaptureH));
}

void Injector::SetMarioTexture(const uint8_t* rgba, int w, int h) {
    LockGuard lock(marioMu_);
    texPixels_.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
    pendingTexW_ = w;
    pendingTexH_ = h;
}

void Injector::SetMario(const MarioFrame& f) {
    LockGuard lock(marioMu_);
    // (Tags only go up; a lower one is a restart of the counting: start over.)
    if (f.tag < newestTag_)
        for (MarioFrame& r : ring_) r.tag = 0;
    MarioFrame& r = ring_[f.tag % kRing];
    r.visible = f.visible;
    r.anchor = f.anchor;
    r.vertexCount = f.vertexCount;
    r.cut = f.cut;
    r.pos = f.pos;
    r.nrm = f.nrm;
    r.col = f.col;
    r.uv = f.uv;
    r.metal = f.metal;
    r.vanish = f.vanish;
    r.havePivot = f.havePivot;
    r.pivot = f.pivot;
    r.tag = f.tag;
    newestTag_ = f.tag;
    if (f.cut) cutTag_ = f.tag; // drawn without motion when it (or a later one) is first drawn
}

uint64_t Injector::DrawnTag(uint64_t frame) {
    LockGuard lock(mu_);
    const DrawnTagRec& r = drawnTags_[frame % 32];
    return r.frame == frame ? r.tag : 0;
}

void Injector::Fail(const std::string& why) {
    {
        LockGuard lock(errorMu_);
        if (failed_.load()) return;
        error_ = why;
        failed_.store(true);
    }
    LOGE("in-world rendering disabled: %s", why.c_str());
}

std::string Injector::Error() const {
    LockGuard lock(errorMu_);
    return error_;
}

bool Injector::EnsureDevice(GCL* list) {
    if (failed_.load()) return false;
    if (objectsReady_) return true;
    LockGuard lock(mu_);
    if (objectsReady_) return true;
    if (failed_.load()) return false;
    if (vsGBuffer_.empty()) {
        Fail("shaders not compiled");
        return false;
    }
    if (!device_ && FAILED(list->GetDevice(IID_PPV_ARGS(&device_)))) {
        device_ = nullptr;
        Fail("command list has no device");
        return false;
    }
    if (!CreateObjects()) return false;
    objectsReady_ = true;
    LOGI("in-world rendering: device objects ready");
    return true;
}

bool Injector::CreateObjects() {
    tracker::ScopedInternal internal;
    std::string err;
    // Graphics root signatures: [0] the game's b0 (table or root CBV), [1] our
    // constants, [2] texture, [3] snapshot.
    D3D12_DESCRIPTOR_RANGE viewRange{};
    viewRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    viewRange.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER p[4]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 1;
    p[0].DescriptorTable.pDescriptorRanges = &viewRange;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].Descriptor.ShaderRegister = 1;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[2].Descriptor.ShaderRegister = 0;
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[3].Descriptor.ShaderRegister = 0;
    p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 4;
    rd.pParameters = p;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    rsTable_ = MakeRootSignature(device_, rd, err);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[0].Descriptor.ShaderRegister = 0;
    p[0].Descriptor.RegisterSpace = 0;
    rsCbv_ = MakeRootSignature(device_, rd, err);
    // Compute: [0] SRV table (depth t0, motion t2), [1] view SRV, [2] output UAV, [3] constants.
    D3D12_DESCRIPTOR_RANGE srvRange[2]{};
    srvRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange[0].NumDescriptors = 1;
    srvRange[0].RegisterSpace = 1;
    srvRange[0].OffsetInDescriptorsFromTableStart = 0;
    srvRange[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange[1].NumDescriptors = 1;
    srvRange[1].BaseShaderRegister = 2;
    srvRange[1].RegisterSpace = 1;
    srvRange[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER c[4]{};
    c[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    c[0].DescriptorTable.NumDescriptorRanges = 2;
    c[0].DescriptorTable.pDescriptorRanges = srvRange;
    c[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    c[1].Descriptor.ShaderRegister = 1;
    c[1].Descriptor.RegisterSpace = 1;
    c[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    c[2].Descriptor.ShaderRegister = 0;
    c[2].Descriptor.RegisterSpace = 1;
    c[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    c[3].Constants.ShaderRegister = 0;
    c[3].Constants.RegisterSpace = 1;
    c[3].Constants.Num32BitValues = 8;
    D3D12_ROOT_SIGNATURE_DESC cd{};
    cd.NumParameters = 4;
    cd.pParameters = c;
    rsCompute_ = MakeRootSignature(device_, cd, err);
    if (!rsTable_ || !rsCbv_ || !rsCompute_) {
        Fail(err.empty() ? "root signatures" : err);
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = rsCompute_;
    cp.CS = {csDownsample_.data(), csDownsample_.size()};
    if (FAILED(device_->CreateComputePipelineState(&cp, IID_PPV_ARGS(&csPso_)))) {
        Fail("depth capture pipeline");
        return false;
    }
    upload_ = MakeBuffer(device_, D3D12_HEAP_TYPE_UPLOAD, kSlotBytes * kSlots, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    snapshot_ = MakeBuffer(device_, D3D12_HEAP_TYPE_DEFAULT, kSnapStride * kSlots, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    captureSnap_ = MakeBuffer(device_, D3D12_HEAP_TYPE_DEFAULT, kSnapStride, D3D12_RESOURCE_FLAG_NONE, kReadState);
    // Reduced depth (4 bytes a pixel) followed by the motion vectors (8).
    captureOut_ = MakeBuffer(device_, D3D12_HEAP_TYPE_DEFAULT, UINT64(kMaxCaptureW) * kMaxCaptureH * 12,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int i = 0; i < kReadbackSlots; ++i) {
        readback_[i] = MakeBuffer(device_, D3D12_HEAP_TYPE_READBACK,
                                  kReadbackDepthOffset + UINT64(kMaxCaptureW) * kMaxCaptureH * 12, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        if (!readback_[i]) break;
        D3D12_RANGE all{0, SIZE_T(kReadbackDepthOffset + kMaxCaptureW * kMaxCaptureH * 12)};
        if (FAILED(readback_[i]->Map(0, &all, reinterpret_cast<void**>(&readbackPtr_[i])))) readbackPtr_[i] = nullptr;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_[i]))))
            alloc_[i] = nullptr;
    }
    bool slotsOk = true;
    for (int i = 0; i < kReadbackSlots; ++i) slotsOk = slotsOk && readback_[i] && readbackPtr_[i] && alloc_[i];
    if (!upload_ || !snapshot_ || !captureSnap_ || !captureOut_ || !slotsOk) {
        Fail("buffers");
        return false;
    }
    D3D12_RANGE none{0, 0};
    if (FAILED(upload_->Map(0, &none, reinterpret_cast<void**>(&uploadPtr_)))) {
        Fail("upload buffer map");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kReadbackSlots * 2; // per read-back slot: depth, motion
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)))) {
        Fail("descriptor heap");
        return false;
    }
    srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_PPV_ARGS(&list_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        Fail("command list / fence");
        return false;
    }
    list_->Close();
    // A placeholder texture until the ROM atlas has been uploaded (the pixel
    // shader ignores it while uTex.z is 0, but the root SRV must be valid).
    dummy_ = MakeBuffer(device_, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_NONE, kTexState);
    if (!dummy_) {
        Fail("buffers");
        return false;
    }
    return true;
}

ID3D12PipelineState* Injector::GetPso(const PsoKey& key) {
    for (const PsoEntry& e : psos_)
        if (e.key == key) return e.pso;
    if (psos_.size() > 64) return nullptr;
    tracker::ScopedInternal internal;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = key.rootCbv ? rsCbv_ : rsTable_;
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    d.InputLayout = {layout, 5};
    if (key.kind == 0) {
        d.VS = {vsGBuffer_.data(), vsGBuffer_.size()};
        d.PS = {psGBuffer_.data(), psGBuffer_.size()};
    } else {
        d.VS = {vsShadow_.data(), vsShadow_.size()};
        if (key.kind == 1) d.PS = {psShadow_.data(), psShadow_.size()};
        else d.PS = {psShadowCut_.data(), psShadowCut_.size()};
    }
    d.BlendState.IndependentBlendEnable = TRUE;
    for (uint32_t i = 0; i < 8; ++i)
        d.BlendState.RenderTarget[i].RenderTargetWriteMask = (key.kind == 0 && i < 4) ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthBias = key.depthBias;
    d.RasterizerState.DepthBiasClamp = key.biasClamp;
    d.RasterizerState.SlopeScaledDepthBias = key.slopeBias;
    d.RasterizerState.DepthClipEnable = key.depthClip ? TRUE : FALSE;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(key.depthFunc);
    if (key.stencil & 1) {
        d.DepthStencilState.StencilEnable = TRUE;
        d.DepthStencilState.StencilReadMask = UINT8((key.stencil >> 8) & 0xFF);
        d.DepthStencilState.StencilWriteMask = UINT8((key.stencil >> 16) & 0xFF);
        d.DepthStencilState.FrontFace = ToOpDesc(UnpackOps(key.frontOps));
        d.DepthStencilState.BackFace = ToOpDesc(UnpackOps(key.backOps));
    }
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = key.numRt;
    for (uint32_t i = 0; i < key.numRt && i < 8; ++i) d.RTVFormats[i] = DXGI_FORMAT(key.rtFormats[i]);
    d.DSVFormat = DXGI_FORMAT(key.dsvFormat);
    d.SampleDesc.Count = key.samples ? key.samples : 1;
    d.SampleDesc.Quality = key.quality;
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        LOGE("in-world rendering: pipeline (%s, %u targets, depth %s func %u) failed: 0x%08lx",
             key.kind == 0 ? "G-buffer" : "shadow", key.numRt, d3d12p::FormatName(key.dsvFormat), key.depthFunc,
             static_cast<unsigned long>(hr));
        pso = nullptr;
    } else {
        char stencil[48] = "off";
        if (key.stencil & 1) std::snprintf(stencil, sizeof(stencil), "written under mask 0x%02x", (key.stencil >> 16) & 0xFFu);
        LOGI("in-world rendering: pipeline %zu (%s, %u targets, %s, func %u, stencil %s, bias %d/%.2f)", psos_.size() + 1,
             key.kind == 0 ? "G-buffer" : (key.kind == 1 ? "shadow, depth = z*w" : "shadow"), key.numRt,
             d3d12p::FormatName(key.dsvFormat), key.depthFunc, stencil, key.depthBias, key.slopeBias);
    }
    psos_.push_back({key, pso}); // failures are remembered too, so they aren't retried every frame
    return pso;
}

// Mario's draw set the list's stencil reference (put back after it - also
// after a fault in between, see RestoreAfterFault).
thread_local bool t_stencilRefSet = false;

void Injector::RestoreStencilRef(GCL* list, const frame::ListState& s) {
    t_stencilRefSet = false;
    const ListFns& f = OriginalListFns();
    if (s.stencilRefBack != s.stencilRef && f.OMSetFrontAndBackStencilRef)
        f.OMSetFrontAndBackStencilRef(list, s.stencilRef, s.stencilRefBack);
    else if (f.OMSetStencilRef)
        f.OMSetStencilRef(list, s.stencilRef);
}

void Injector::RestoreAfterFault(GCL* list, const frame::ListState& s) {
    RestoreState(list, s, s.numVp > 0, false, true);
    if (t_stencilRefSet) RestoreStencilRef(list, s);
}

bool Injector::PrepareFrame(uint64_t frameId, FrameDraw& out) {
    LockGuard lock(mu_);
    if (preparedFrame_ == frameId) {
        out = prepared_;
        return preparedSlot_ >= 0;
    }
    preparedFrame_ = frameId;
    preparedSlot_ = -1;
    prepared_ = FrameDraw();
    const int slot = int(frameId % kSlots);
    {
        LockGuard lk(statsMu_);
        ++stats_.prepared;
    }
    if (uploadFence_[slot] && fence_->GetCompletedValue() < uploadFence_[slot]) {
        LockGuard lk(statsMu_);
        ++stats_.gpuBusy; // GPU still on it
        return false;
    }
    MarioFrame m;
    {
        LockGuard lk(marioMu_);
        // The frame made for this one (or `drawLag_` before it); the newest
        // older one if that isn't there.
        const uint64_t lag = uint64_t(drawLag_.load());
        const uint64_t want = frameId > lag ? frameId - lag : 0;
        int pick = -1;
        bool ready = false;
        for (int i = 0; i < kRing; ++i) {
            const uint64_t t = ring_[i].tag;
            if (t == frameId) ready = true;
            if (t == 0 || t > want) continue;
            if (pick < 0 || t > ring_[pick].tag) pick = i;
        }
        if (pick < 0) {
            // (Nothing that old: the newest there is.)
            for (int i = 0; i < kRing; ++i)
                if (ring_[i].tag && (pick < 0 || ring_[i].tag > ring_[pick].tag)) pick = i;
        }
        {
            LockGuard ls(statsMu_);
            if (!ready) ++stats_.notReady;
            if (pick >= 0 && ring_[pick].tag < want) ++stats_.olderDrawn;
        }
        if (pick < 0 || !ring_[pick].visible || ring_[pick].vertexCount == 0) return false;
        m = ring_[pick];
        m.cut = m.tag >= cutTag_ && lastDrawnTag_ < cutTag_ && cutTag_ != 0;
        if (m.tag < lastDrawnTag_) m.cut = true; // (going back in time: no motion either)
        lastDrawnTag_ = m.tag;
        drawnTags_[frameId % 32] = {frameId, m.tag};
    }
    constexpr UINT64 kCbRegion = UINT64(kCbBytes) * kCbVariants;
    const uint32_t n = std::min<uint32_t>(m.vertexCount, uint32_t((kSlotBytes - kCbRegion) / sizeof(Vertex)));
    static_assert(kSlotBytes - kCbRegion >= 3 * 1024 * sizeof(Vertex), "a slot fits Mario's 1024 triangles");
    static_assert(kAlignEntries == kAlignRows, "placements in the draw constants");
    if (m.pos.size() < size_t(n) * 3 || m.nrm.size() < size_t(n) * 3 || m.col.size() < size_t(n) * 3 ||
        m.uv.size() < size_t(n) * 2)
        return false;
    // Motion vectors from the previous drawn frame: only if that was one of
    // the last two (frame generation can put a present between two rendered
    // frames; longer gaps would make them span several), and no vertex
    // jumped (a respawn or an origin shift).
    bool prevOk = havePrev_ && !m.cut && prevPos_.size() == size_t(n) * 3 && Length(m.anchor - prevAnchor_) < 5.0 &&
                  frameId > prevFrame_ && frameId - prevFrame_ <= 2;
    if (prevOk) {
        const DVec3 da = m.anchor - prevAnchor_;
        for (uint32_t i = 0; i < n && prevOk; ++i) {
            double d2 = 0;
            for (int k = 0; k < 3; ++k) {
                const double d = double(m.pos[size_t(i) * 3 + k]) + da[k] - double(prevPos_[size_t(i) * 3 + k]);
                d2 += d * d;
            }
            if (d2 > 1.5 * 1.5) prevOk = false;
        }
        if (!prevOk) {
            LockGuard lk(statsMu_);
            ++stats_.motionCuts;
        }
    }
    uint8_t* base = uploadPtr_ + size_t(slot) * kSlotBytes;
    Vertex* v = reinterpret_cast<Vertex*>(base);
    for (uint32_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            v[i].pos[k] = m.pos[size_t(i) * 3 + k];
            v[i].prev[k] = prevOk ? prevPos_[size_t(i) * 3 + k] : m.pos[size_t(i) * 3 + k];
            v[i].nrm[k] = m.nrm[size_t(i) * 3 + k];
            v[i].col[k] = m.col[size_t(i) * 3 + k];
        }
        v[i].uv[0] = m.uv[size_t(i) * 2];
        v[i].uv[1] = m.uv[size_t(i) * 2 + 1];
    }
    const DVec3 prevAnchor = prevOk ? prevAnchor_ : m.anchor;

    // Where the camera of the view this frame is rendered from had Mario:
    // the placements of the last half second, newest first, each with the
    // shift from this frame's pivot to the one it was placed around (and
    // from the previous frame's, for the motion vectors). The vertex shader
    // picks the one the view was rendered from by its position.
    DrawConstants c{};
    const bool align = align_.load() && m.havePivot;
    DVec3 newestShift;
    double reach = 0; // m: the furthest shift (Mario's bounds for the capture)
    if (align) {
        CameraPlacementRec recs[kAlignEntries];
        int slotOf[kAlignEntries] = {};
        const PlacementSource src = placementSource_.load();
        const int got = src ? std::max(0, std::min(kAlignEntries, src(recs, kAlignEntries))) : 0;
        std::sort(recs, recs + got, [](const CameraPlacementRec& a, const CameraPlacementRec& b) {
            return int32_t(a.order - b.order) > 0; // newest first (order wraps)
        });
        const double now = NowSeconds();
        int used = 0;
        for (int i = 0; i < got; ++i) {
            const CameraPlacementRec& r = recs[i];
            if (!(now - r.time < 0.5) || !(r.blend > 0.0f)) continue;
            const DVec3 d = (r.pivot - m.pivot) * double(std::min(1.0f, r.blend));
            // (Not across a jump - a respawn, an origin shift: it isn't him.)
            if (!(Length(d) < 2.0)) continue;
            const DVec3 e = prevAligned_ ? (r.pivot - prevPivot_) * double(std::min(1.0f, r.blend)) : DVec3();
            c.cam[used][0] = float(r.pos.x);
            c.cam[used][1] = float(r.pos.y);
            c.cam[used][2] = float(r.pos.z);
            c.cam[used][3] = -1.0f;
            slotOf[used] = r.slot;
            for (int k = 0; k < 3; ++k) {
                c.shift[used][k] = float(d[k]);
                c.prevShift[used][k] = Length(e) < 2.0 ? float(e[k]) : float(d[k]);
            }
            if (used == 0) newestShift = d;
            reach = std::max(reach, Length(d));
            ++used;
        }
        // Each entry's predecessor: the same camera's placement before it
        // (the game may draw a view from between two of them).
        for (int i = 0; i < used; ++i)
            for (int j = i + 1; j < used; ++j)
                if (slotOf[j] == slotOf[i]) {
                    c.cam[i][3] = float(j);
                    break;
                }
        c.align[0] = uint32_t(used);
        c.align[1] = kAlignMatch | (prevAligned_ ? kAlignPrev : 0u) | (prevOk ? 0u : kAlignStill);
        for (int k = 0; k < 3; ++k) {
            c.alignNewest[k] = float(newestShift[k]);
            c.alignPrev[k] = float(prevAligned_ ? prevNewestShift_[k] : 0.0);
        }
        c.alignTol[0] = 0.75f * 0.75f;
    } else {
        // Off: no placements, no shift. (The previous frame, if it was moved,
        // most likely by its newest placement's shift: its motion starts there.)
        c.align[1] = prevOk ? (prevAligned_ ? kAlignPrev : 0u) : kAlignStill;
        for (int k = 0; k < 3; ++k) c.alignPrev[k] = float(prevAligned_ ? prevNewestShift_[k] : 0.0);
    }
    {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                const float v0 = m.pos[size_t(i) * 3 + k];
                lo[k] = std::min(lo[k], v0);
                hi[k] = std::max(hi[k], v0);
            }
        const DVec3 grow(reach, reach, reach);
        preparedLo_ = m.anchor + DVec3(lo[0], lo[1], lo[2]) - grow;
        preparedHi_ = m.anchor + DVec3(hi[0], hi[1], hi[2]) + grow;
    }
    prevPos_.assign(m.pos.begin(), m.pos.begin() + size_t(n) * 3);
    prevAnchor_ = m.anchor;
    prevFrame_ = frameId;
    havePrev_ = true;
    prevAligned_ = align;
    prevPivot_ = m.pivot;
    prevNewestShift_ = newestShift;

    Split(m.anchor.x, c.anchorHi[0], c.anchorLo[0]);
    Split(m.anchor.y, c.anchorHi[1], c.anchorLo[1]);
    Split(m.anchor.z, c.anchorHi[2], c.anchorLo[2]);
    Split(prevAnchor.x, c.prevAnchorHi[0], c.prevAnchorLo[0]);
    Split(prevAnchor.y, c.prevAnchorHi[1], c.prevAnchorLo[1]);
    Split(prevAnchor.z, c.prevAnchorHi[2], c.prevAnchorLo[2]);
    c.material[0] = look_.gloss;
    c.material[1] = look_.specular;
    c.material[2] = look_.albedoScale;
    c.material[3] = look_.shadowBias;
    c.bits[0] = (1u << 22) | (1u << 25);     // shading model 1 (standard), flag bit 25 like the default material
    c.bits[1] = 0xFFFFu << 16;               // no material table entry
    c.bits[2] = gbuf::PackSpecularOcclusion(look_.occlusion);
    c.bits[3] = uint32_t(slot);              // snapshot slot
    c.tex[0] = float(texW_ > 0 ? texW_ : 1);
    c.tex[1] = float(texH_ > 0 ? texH_ : 1);
    c.tex[2] = textureReady_.load() ? 1.0f : 0.0f;
    c.tex[3] = float((m.metal ? 1 : 0) | (m.vanish ? 2 : 0)); // both caps can be on at once
    c.misc[0] = uint32_t(frameId);
    c.misc[1] = uint32_t(frameId >> 32);
    // Metal cap: GBuffer1.y with a material table entry, so the game's own
    // reflection passes (screen-space or ray traced) cover him.
    c.misc[3] = look_.metalMaterial >= 0 && look_.metalMaterial < 0xFFFF ? uint32_t(look_.metalMaterial) << 16
                                                                          : 0xFFFFu << 16;
    const size_t cbOff = size_t(kSlotBytes - kCbRegion);
    static_assert(sizeof(DrawConstants) == kCbBytes, "one variant");
    std::memcpy(base + cbOff, &c, sizeof(c));
    c.bits[3] = 0xFFFFFFFFu; // G-buffer draws that can't write a UAV (and shadows) don't write the snapshot
    std::memcpy(base + cbOff + kCbBytes, &c, sizeof(c));
    // Shadows: another view (the light's) - the newest placement's shift.
    c.align[1] &= ~kAlignMatch;
    std::memcpy(base + cbOff + 2 * kCbBytes, &c, sizeof(c));
    c.misc[2] = 1; // light view must be orthographic
    std::memcpy(base + cbOff + 3 * kCbBytes, &c, sizeof(c));

    const D3D12_GPU_VIRTUAL_ADDRESS va = upload_->GetGPUVirtualAddress() + UINT64(slot) * kSlotBytes;
    prepared_.slot = slot;
    prepared_.vb = va;
    prepared_.cbGBuffer = va + cbOff;
    prepared_.cbGBufferNoSnapshot = va + cbOff + kCbBytes;
    prepared_.cbShadow = va + cbOff + 2 * kCbBytes;
    prepared_.cbShadowOrtho = va + cbOff + 3 * kCbBytes;
    prepared_.verts = n;
    preparedSlot_ = slot;
    out = prepared_;
    return true;
}

void Injector::RestoreState(GCL* list, const frame::ListState& s, bool viewportChanged, bool heapsChanged,
                            bool targetsChanged) {
    const ListFns& f = OriginalListFns();
    if (targetsChanged && !s.inRenderPass) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv[8];
        for (uint32_t i = 0; i < 8; ++i) rtv[i].ptr = SIZE_T(s.rtv[i]);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        dsv.ptr = SIZE_T(s.dsv);
        f.OMSetRenderTargets(list, s.numRt, s.numRt ? rtv : nullptr, FALSE, s.dsv ? &dsv : nullptr);
    }
    if (heapsChanged && s.numHeaps) {
        ID3D12DescriptorHeap* heaps[2] = {reinterpret_cast<ID3D12DescriptorHeap*>(uintptr_t(s.heaps[0])),
                                          reinterpret_cast<ID3D12DescriptorHeap*>(uintptr_t(s.heaps[1]))};
        f.SetDescriptorHeaps(list, s.numHeaps, heaps);
    }
    if (viewportChanged) {
        if (s.numVp) {
            D3D12_VIEWPORT vp[16];
            for (uint32_t i = 0; i < s.numVp; ++i)
                vp[i] = {s.vp[i].x, s.vp[i].y, s.vp[i].w, s.vp[i].h, s.vp[i].minZ, s.vp[i].maxZ};
            f.RSSetViewports(list, s.numVp, vp);
        }
        if (s.numScissor) {
            D3D12_RECT rc[16];
            for (uint32_t i = 0; i < s.numScissor; ++i)
                rc[i] = {LONG(s.scissor[i].left), LONG(s.scissor[i].top), LONG(s.scissor[i].right), LONG(s.scissor[i].bottom)};
            f.RSSetScissorRects(list, s.numScissor, rc);
        }
    }
    if (s.rootSig) {
        f.SetGraphicsRootSignature(list, reinterpret_cast<ID3D12RootSignature*>(uintptr_t(s.rootSig)));
        const frame::RootSigLayout* L = s.layout;
        const size_t count = L ? std::min<size_t>(L->info.params.size(), 64) : 64;
        for (size_t i = 0; i < count; ++i) {
            const frame::RootArg& a = s.args[i];
            switch (a.kind) {
            case frame::kArgTable: {
                D3D12_GPU_DESCRIPTOR_HANDLE h;
                h.ptr = a.value;
                f.SetGraphicsRootDescriptorTable(list, UINT(i), h);
                break;
            }
            case frame::kArgConsts:
                if (L && L->info.params[i].num32BitValues && L->constOffset[i] + L->info.params[i].num32BitValues <= 64)
                    f.SetGraphicsRoot32BitConstants(list, UINT(i), L->info.params[i].num32BitValues,
                                                    &s.constPool[L->constOffset[i]], 0);
                break;
            case frame::kArgCbv: f.SetGraphicsRootConstantBufferView(list, UINT(i), a.value); break;
            case frame::kArgSrv: f.SetGraphicsRootShaderResourceView(list, UINT(i), a.value); break;
            case frame::kArgUav: f.SetGraphicsRootUnorderedAccessView(list, UINT(i), a.value); break;
            default: break;
            }
        }
    }
    // The pipeline the game set last: a graphics/compute PSO, or a ray tracing
    // state object (each unbinds the other).
    if (s.stateObjectLast && s.stateObject && f.SetPipelineState1)
        f.SetPipelineState1(list, reinterpret_cast<void*>(uintptr_t(s.stateObject)));
    else if (s.pso)
        f.SetPipelineState(list, reinterpret_cast<ID3D12PipelineState*>(uintptr_t(s.pso)));
    if (s.topology) f.IASetPrimitiveTopology(list, D3D12_PRIMITIVE_TOPOLOGY(s.topology));
    if (s.vbSet & 1u) {
        D3D12_VERTEX_BUFFER_VIEW v{s.vbs[0].gpu, s.vbs[0].size, s.vbs[0].stride};
        f.IASetVertexBuffers(list, 0, 1, &v);
    } else {
        f.IASetVertexBuffers(list, 0, 1, nullptr);
    }
    if (s.predBuffer)
        f.SetPredication(list, reinterpret_cast<ID3D12Resource*>(uintptr_t(s.predBuffer)), s.predOffset,
                         D3D12_PREDICATION_OP(s.predOp));
}

namespace {
// Everything in `s` the injector will have to put back must be known.
const char* CanRestore(const frame::ListState& s) {
    if (s.bundleRan) return "a bundle ran in this list (its bindings aren't known)";
    if (s.stateObjectLast && !OriginalListFns().SetPipelineState1) return "a ray tracing state object is bound";
    if (s.rootSig && !s.layout) return "the list's root signature is unknown";
    if (s.layout) {
        for (size_t i = 0; i < s.layout->info.params.size() && i < 64; ++i)
            if (s.args[i].kind == frame::kArgConsts && s.layout->constTotal > 64) return "root constants don't fit";
    }
    return nullptr;
}
} // namespace

namespace {
// The descriptor heaps the segment's view table lives in must be the ones
// bound now: switching heaps would invalidate the game's compute tables too.
bool SameHeaps(const frame::ListState& s, uint32_t numHeaps, const uint64_t heaps[2]) {
    if (s.numHeaps != numHeaps) return false;
    for (uint32_t i = 0; i < numHeaps && i < 2; ++i)
        if (s.heaps[i] != heaps[i]) return false;
    return true;
}
} // namespace

void Injector::NoteGameTests(uint32_t depthFunc, uint32_t stencilFunc) {
    static const char* const kNames[] = {"?",     "never",   "less",          "equal",
                                         "less-equal", "greater", "not-equal", "greater-equal", "always"};
    auto name = [](uint32_t f) { return f <= 8 ? kNames[f] : "?"; };
    const uint32_t dbit = 1u << (depthFunc & 31), sbit = 1u << (stencilFunc & 31);
    bool logDepth = false, logStencil = false;
    {
        LockGuard lock(mu_);
        if (depthFunc && depthFunc != d3d12p::MarioDepthFunc(depthFunc, reverseZ_.load()) && !(loggedDepthFuncs_ & dbit)) {
            loggedDepthFuncs_ |= dbit;
            logDepth = true;
        }
        if (stencilFunc && stencilFunc != d3d12p::kCmpAlways && !(loggedStencilFuncs_ & sbit)) {
            loggedStencilFuncs_ |= sbit;
            logStencil = true;
        }
    }
    if (logDepth)
        LOGI("in-world rendering: a G-buffer segment's pipeline tests depth %s - Mario's draw tests %s",
             name(depthFunc), name(d3d12p::MarioDepthFunc(depthFunc, reverseZ_.load())));
    if (logStencil)
        LOGI("in-world rendering: a G-buffer segment's pipeline tests the stencil (%s) - Mario's draw writes it like "
             "the pipeline but doesn't test it",
             name(stencilFunc));
}

void Injector::SetStatus(const char* why) {
    LockGuard lock(statsMu_);
    stats_.status = why;
}

void Injector::InjectGBuffer(GCL* list, const frame::ListState& s, const frame::GBufferSeg& g) {
    if (failed_.load() || !EnsureDevice(list)) return;
    // Everything that can stop the injection is checked before any of the
    // list's state is touched.
    if (const char* why = CanRestore(s)) return SetStatus(why);
    bool sameTargets = s.numRt == g.numRt && s.dsv == g.dsv;
    for (uint32_t i = 0; i < g.numRt && sameTargets; ++i) sameTargets = s.rtv[i] == g.rtv[i];
    if (!sameTargets && s.inRenderPass) return SetStatus("targets changed inside the game's render pass");
    if (g.viewTable && !SameHeaps(s, g.numHeaps, g.heaps)) return SetStatus("descriptor heaps changed since the pass");
    FrameDraw fd;
    if (!PrepareFrame(s.frame, fd)) return;
    PsoKey key;
    key.kind = 0;
    const d3d12p::PsoInfo* gi = g.pso ? &g.pso->info : nullptr;
    key.numRt = gi ? gi->numRenderTargets : g.numRt;
    for (uint32_t i = 0; i < key.numRt && i < 8; ++i) key.rtFormats[i] = gi ? gi->rtvFormats[i] : g.rt[i].format;
    key.dsvFormat = gi ? gi->dsvFormat : g.ds.format;
    key.samples = gi ? gi->sampleCount : 1;
    key.quality = gi ? gi->sampleQuality : 0;
    // An ordinary depth test, whatever the segment's pipeline does (0.4 copied
    // it: after a depth pre-pass that was EQUAL, and Mario vanished).
    // A pipeline without a depth test of its own (or without a depth buffer)
    // says nothing about the direction: Mario then tests the game's.
    const bool gameTests = gi && gi->depthEnable;
    const uint32_t gameFunc = gameTests ? gi->depthFunc : d3d12p::kCmpAlways;
    // The game's depth direction: reverse-Z (Spider-Man 2's) unless only
    // ordinary tests have been seen, many of them - one odd pass mustn't turn
    // it around (EQUAL, the main G-buffer pass's, doesn't say).
    if (gameTests && (gameFunc == d3d12p::kCmpLess || gameFunc == d3d12p::kCmpLessEqual)) lessSeen_.fetch_add(1);
    else if (gameTests && d3d12p::IsReverseZFunc(gameFunc)) greaterSeen_.fetch_add(1);
    reverseZ_.store(!(greaterSeen_.load() == 0 && lessSeen_.load() >= 30));
    key.depthFunc = d3d12p::MarioDepthFunc(gameFunc, reverseZ_.load());
    MarioStencil st;
    {
        LockGuard lock(mu_);
        st = stencil_;
    }
    bool setRef = false;
    // A stencil bound read-only (or a render pass without access to it) is
    // never written.
    const bool stencilWritable = !(g.ds.dsvFlags & 2) && !(s.ds.dsvFlags & 2);
    if (!stencilWritable) {
        // (nothing: what is there stays)
    } else if (st.mode == MarioStencil::Copy && look_.stencilMatch && gi && gi->stencilEnable &&
               d3d12p::HasStencil(key.dsvFormat)) {
        // The game's stencil writes, never its stencil test.
        key.stencil = 1u | (uint32_t(gi->stencilReadMask) << 8) | (uint32_t(gi->stencilWriteMask) << 16);
        key.frontOps = PackOps(d3d12p::MarioStencilOps(gi->front));
        key.backOps = PackOps(d3d12p::MarioStencilOps(gi->back));
    } else if (st.mode == MarioStencil::Write && st.mask && d3d12p::HasStencil(key.dsvFormat)) {
        // Spider-Man's mark (or the one set): written wherever Mario is drawn.
        d3d12p::StencilOps w;
        w.fail = 1;      // KEEP
        w.depthFail = 1; // KEEP
        w.pass = 3;      // REPLACE
        w.func = 8;      // ALWAYS
        key.stencil = 1u | (0xFFu << 8) | (uint32_t(st.mask) << 16);
        key.frontOps = key.backOps = PackOps(w);
        setRef = true;
    }
    NoteGameTests(gameTests ? gameFunc : 0, gi && gi->stencilEnable ? gi->front.func : 0);
    key.rootCbv = g.viewTable ? 0 : 1;
    ID3D12PipelineState* pso;
    {
        LockGuard lock(mu_);
        pso = GetPso(key);
    }
    if (!pso) return;
    // Inside a render pass a UAV may only be written if the pass allows it:
    // otherwise this draw doesn't write the view snapshot.
    constexpr uint32_t kAllowUavWrites = 0x1; // D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES
    const bool snapshot = !s.inRenderPass || (s.renderPassFlags & kAllowUavWrites);

    const ListFns& f = OriginalListFns();
    if (s.predBuffer) f.SetPredication(list, nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    bool vpChanged = false, targetsChanged = false;
    if (s.numVp == 0 || !(s.vp[0] == g.vp)) {
        const D3D12_VIEWPORT vp{g.vp.x, g.vp.y, g.vp.w, g.vp.h, g.vp.minZ, g.vp.maxZ};
        const D3D12_RECT rc{LONG(g.scissor.left), LONG(g.scissor.top), LONG(g.scissor.right), LONG(g.scissor.bottom)};
        f.RSSetViewports(list, 1, &vp);
        f.RSSetScissorRects(list, 1, &rc);
        vpChanged = true;
    }
    if (!sameTargets) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv[8];
        for (uint32_t i = 0; i < 8; ++i) rtv[i].ptr = SIZE_T(g.rtv[i]);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        dsv.ptr = SIZE_T(g.dsv);
        f.OMSetRenderTargets(list, g.numRt, rtv, FALSE, g.dsv ? &dsv : nullptr);
        targetsChanged = true;
    }
    f.SetGraphicsRootSignature(list, g.viewTable ? rsTable_ : rsCbv_);
    if (g.viewTable) {
        D3D12_GPU_DESCRIPTOR_HANDLE h;
        h.ptr = g.viewTable;
        f.SetGraphicsRootDescriptorTable(list, 0, h);
    } else {
        f.SetGraphicsRootConstantBufferView(list, 0, g.viewCbv);
    }
    f.SetGraphicsRootConstantBufferView(list, 1, snapshot ? fd.cbGBuffer : fd.cbGBufferNoSnapshot);
    ID3D12Resource* tex = textureReady_.load() && texture_ ? texture_ : dummy_;
    f.SetGraphicsRootShaderResourceView(list, 2, tex->GetGPUVirtualAddress());
    f.SetGraphicsRootUnorderedAccessView(list, 3, snapshot_->GetGPUVirtualAddress());
    f.SetPipelineState(list, pso);
    f.IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW vb{fd.vb, UINT(fd.verts * sizeof(Vertex)), UINT(sizeof(Vertex))};
    f.IASetVertexBuffers(list, 0, 1, &vb);
    if (setRef && f.OMSetStencilRef) {
        f.OMSetStencilRef(list, st.ref);
        t_stencilRefSet = true;
    }
    f.DrawInstanced(list, fd.verts, 1, 0, 0);
    RestoreState(list, s, vpChanged, false, targetsChanged);
    if (t_stencilRefSet) RestoreStencilRef(list, s);
    if (snapshot) snapshotFrame_.store(s.frame);
    LockGuard lock(statsMu_);
    ++stats_.gbufferDraws;
    stats_.lastGBufferFrame = s.frame;
    stats_.status = "drawing in the game's frame";
}

void Injector::InjectShadow(GCL* list, const frame::ListState& s, const frame::ShadowSeg& sh) {
    if (failed_.load() || !EnsureDevice(list)) return;
    if (CanRestore(s)) return;
    if (s.dsv != sh.dsv) return; // the shadow map isn't bound any more
    if (sh.viewTable && !SameHeaps(s, sh.numHeaps, sh.heaps)) return;
    const d3d12p::PsoInfo* ci = sh.pso ? &sh.pso->info : nullptr;
    if (!ci) return;
    FrameDraw fd;
    if (!PrepareFrame(s.frame, fd)) return;
    PsoKey key;
    // Like the game's caster: a pixel shader that writes depth (z * w, as
    // PS_ShadowCaster does), or the rasteriser's depth.
    key.kind = ci->ps.present && ci->ps.writesDepth ? 1 : 2;
    key.numRt = ci->numRenderTargets;
    for (uint32_t i = 0; i < key.numRt && i < 8; ++i) key.rtFormats[i] = ci->rtvFormats[i];
    key.dsvFormat = ci->dsvFormat;
    key.samples = ci->sampleCount;
    key.quality = ci->sampleQuality;
    key.depthFunc = ci->depthFunc >= d3d12p::kCmpNever && ci->depthFunc <= d3d12p::kCmpAlways ? ci->depthFunc
                                                                                               : d3d12p::kCmpLessEqual;
    key.depthBias = ci->depthBias;
    key.biasClamp = ci->depthBiasClamp;
    key.slopeBias = ci->slopeScaledDepthBias;
    key.depthClip = ci->depthClipEnable ? 1 : 0;
    key.rootCbv = sh.viewTable ? 0 : 1;
    ID3D12PipelineState* pso;
    {
        LockGuard lock(mu_);
        pso = GetPso(key);
    }
    if (!pso) return;
    const ListFns& f = OriginalListFns();
    if (s.predBuffer) f.SetPredication(list, nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    bool vpChanged = false;
    if (s.numVp == 0 || !(s.vp[0] == sh.vp)) {
        const D3D12_VIEWPORT vp{sh.vp.x, sh.vp.y, sh.vp.w, sh.vp.h, sh.vp.minZ, sh.vp.maxZ};
        const D3D12_RECT rc{LONG(sh.scissor.left), LONG(sh.scissor.top), LONG(sh.scissor.right), LONG(sh.scissor.bottom)};
        f.RSSetViewports(list, 1, &vp);
        f.RSSetScissorRects(list, 1, &rc);
        vpChanged = true;
    }
    f.SetGraphicsRootSignature(list, sh.viewTable ? rsTable_ : rsCbv_);
    if (sh.viewTable) {
        D3D12_GPU_DESCRIPTOR_HANDLE h;
        h.ptr = sh.viewTable;
        f.SetGraphicsRootDescriptorTable(list, 0, h);
    } else {
        f.SetGraphicsRootConstantBufferView(list, 0, sh.viewCbv);
    }
    f.SetGraphicsRootConstantBufferView(list, 1, sh.viewFromCopy ? fd.cbShadowOrtho : fd.cbShadow);
    // (The texture: the wing cap's wings are cut out of the shadow too.)
    ID3D12Resource* tex = textureReady_.load() && texture_ ? texture_ : dummy_;
    f.SetGraphicsRootShaderResourceView(list, 2, tex->GetGPUVirtualAddress());
    f.SetGraphicsRootUnorderedAccessView(list, 3, snapshot_->GetGPUVirtualAddress());
    f.SetPipelineState(list, pso);
    f.IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW vb{fd.vb, UINT(fd.verts * sizeof(Vertex)), UINT(sizeof(Vertex))};
    f.IASetVertexBuffers(list, 0, 1, &vb);
    f.DrawInstanced(list, fd.verts, 1, 0, 0);
    RestoreState(list, s, vpChanged, false, false);
    LockGuard lock(statsMu_);
    ++stats_.shadowDraws;
}

bool Injector::EnsureDepthCopy(const D3D12_RESOURCE_DESC& rd, UINT width, UINT height) {
    if (depthCopy_ && depthCopyDesc_.Width == width && depthCopyDesc_.Height == height &&
        depthCopyDesc_.Format == rd.Format)
        return true;
    if (depthCopy_) graveyard_.push_back({preparedFrame_ + kSlots + 2, depthCopy_});
    depthCopy_ = nullptr;
    const DXGI_FORMAT f = rd.Format;
    if (f != DXGI_FORMAT_R32_FLOAT && f != DXGI_FORMAT_R32_TYPELESS) {
        Fail("the linear depth target has an unexpected format (" + std::string(d3d12p::FormatName(uint32_t(f))) + ")");
        return false;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    tracker::ScopedInternal internal;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&depthCopy_)))) {
        depthCopy_ = nullptr;
        Fail("depth capture texture");
        return false;
    }
    depthCopyDesc_ = d;
    LOGI("in-world rendering: capturing the %ux%u linear depth target", width, height);
    return true;
}

bool Injector::EnsureMotionCopy(const D3D12_RESOURCE_DESC& rd, UINT width, UINT height) {
    if (motionBroken_) return false;
    if (motionCopy_ && motionCopyDesc_.Width == width && motionCopyDesc_.Height == height &&
        motionCopyDesc_.Format == rd.Format)
        return true;
    if (motionCopy_) graveyard_.push_back({preparedFrame_ + kSlots + 2, motionCopy_});
    motionCopy_ = nullptr;
    const DXGI_FORMAT f = rd.Format;
    if (f != DXGI_FORMAT_R16G16_FLOAT && f != DXGI_FORMAT_R16G16_TYPELESS) {
        motionBroken_ = true;
        LOGW("in-world rendering: the motion-vector target is %s, not RG16F - moving cars can't be told apart",
             d3d12p::FormatName(uint32_t(f)));
        return false;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    tracker::ScopedInternal internal;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&motionCopy_)))) {
        motionCopy_ = nullptr;
        motionBroken_ = true;
        LOGW("in-world rendering: couldn't create the motion-vector copy - moving cars can't be told apart");
        return false;
    }
    motionCopyDesc_ = d;
    LOGI("in-world rendering: capturing the motion vectors too (moving cars and people don't become collision)");
    return true;
}

bool Injector::Capture(GCL* list, const frame::ListState& s, ID3D12Resource* rt0, UINT subresource,
                       D3D12_RESOURCE_STATES state, ID3D12Resource* motion, UINT motionSub,
                       D3D12_RESOURCE_STATES motionState) {
    if (failed_.load() || !rt0 || !EnsureDevice(list)) return false;
    if (snapshotFrame_.load() != s.frame) return false; // Mario wasn't drawn this frame: no view to pair with
    const D3D12_RESOURCE_DESC rd = rt0->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1) return false;
    const UINT mips = rd.MipLevels ? rd.MipLevels : 1;
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) subresource = 0;
    const UINT mip = subresource % mips;
    const UINT w = std::max<UINT>(1, UINT(rd.Width >> mip)), h = std::max<UINT>(1, rd.Height >> mip);
    const int slot = int(s.frame % kSlots);
    // The motion target: same size as the depth, a usable format, and a state
    // it can be copied from with one transition.
    bool withMotion = false;
    if (motion && motion != rt0 && motionState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const D3D12_RESOURCE_DESC md = motion->GetDesc();
        const UINT mm = md.MipLevels ? md.MipLevels : 1;
        if (motionSub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) motionSub = 0;
        const UINT mmip = motionSub % mm;
        withMotion = md.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && md.SampleDesc.Count == 1 &&
                     std::max<UINT>(1, UINT(md.Width >> mmip)) == w && std::max<UINT>(1, md.Height >> mmip) == h;
        if (withMotion) {
            LockGuard lock(mu_);
            withMotion = EnsureMotionCopy(md, w, h);
        }
    }
    {
        LockGuard lock(mu_);
        if (!EnsureDepthCopy(rd, w, h)) return false;
        // Where Mario is in this depth (he was drawn in this frame).
        DrawnBounds& b = captureBounds_[slot];
        b.frame = preparedFrame_ == s.frame && preparedSlot_ >= 0 ? s.frame : 0;
        b.lo = preparedLo_;
        b.hi = preparedHi_;
        b.motionFrame = withMotion ? s.frame : 0;
    }
    const ListFns& f = OriginalListFns();
    if (s.predBuffer) f.SetPredication(list, nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    D3D12_RESOURCE_BARRIER pre[6] = {
        Transition(rt0, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(depthCopy_, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Transition(snapshot_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(captureSnap_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, kReadState, D3D12_RESOURCE_STATE_COPY_DEST),
        {},
        {},
    };
    UINT nPre = 4;
    if (withMotion) {
        pre[nPre++] = Transition(motion, motionSub, motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pre[nPre++] = Transition(motionCopy_, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    f.ResourceBarrier(list, nPre, pre);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = depthCopy_;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = rt0;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresource;
    f.CopyTextureRegion(list, &dst, 0, 0, 0, &src, nullptr);
    if (withMotion) {
        D3D12_TEXTURE_COPY_LOCATION mdst = dst, msrc = src;
        mdst.pResource = motionCopy_;
        msrc.pResource = motion;
        msrc.SubresourceIndex = motionSub;
        f.CopyTextureRegion(list, &mdst, 0, 0, 0, &msrc, nullptr);
    }
    // Freeze the view that rendered this depth next to it, in GPU order.
    f.CopyBufferRegion(list, captureSnap_, 0, snapshot_, UINT64(slot) * kSnapStride, kSnapStride);
    D3D12_RESOURCE_BARRIER post[6];
    for (UINT i = 0; i < nPre; ++i) {
        post[i] = pre[i];
        std::swap(post[i].Transition.StateBefore, post[i].Transition.StateAfter);
    }
    post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (withMotion) post[5].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f.ResourceBarrier(list, nPre, post);
    if (s.predBuffer)
        f.SetPredication(list, reinterpret_cast<ID3D12Resource*>(uintptr_t(s.predBuffer)), s.predOffset,
                         D3D12_PREDICATION_OP(s.predOp));
    captureFrame_.store(s.frame);
    {
        LockGuard lock(mu_);
        captureSrcW_ = w;
        captureSrcH_ = h;
    }
    LockGuard lock(statsMu_);
    ++stats_.captures;
    return true;
}

void Injector::EndFrame(uint64_t frameId) {
    if (failed_.load() || !objectsReady_) return;
    LockGuard lock(mu_);
    // The read-back must run after the game's lists that hold Mario's draws
    // and the depth copy: on their queue (there may be others).
    ID3D12CommandQueue* queue = tracker::InjectionQueue();
    if (queue) {
        ID3D12Device* qdev = nullptr;
        const bool same = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&qdev))) && qdev == device_;
        if (qdev) qdev->Release();
        if (!same) {
            queue->Release();
            queue = nullptr;
        }
    }
    if (!queue) queue = d3d12hook::LastDirectQueue(device_);
    if (!queue) return;
    // The game's lists for this frame are on the queue: once the GPU is past
    // this point, its upload slot can take a later frame's Mario.
    queue->Signal(fence_, ++fenceValue_);
    uploadFence_[frameId % kSlots] = fenceValue_;
    const int k = int(frameId % kReadbackSlots);
    Slot& sl = slots_[k];
    // Collect finished read-backs first.
    CollectResults();
    if (sl.fence && fence_->GetCompletedValue() < sl.fence) {
        // The GPU is more than kReadbackSlots frames behind: wait a little, else skip.
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            fence_->SetEventOnCompletion(sl.fence, ev);
            WaitForSingleObject(ev, 50);
            CloseHandle(ev);
        }
        if (fence_->GetCompletedValue() < sl.fence) {
            queue->Release();
            return;
        }
        CollectResults();
    }
    // Release resources nobody uses any more: retired a few frames ago, and
    // the GPU past the end of the frame before this one (0.6's first cut
    // waited for this one's too - which a busy GPU never is at this point -
    // and kept old capture textures for as long as it stayed busy).
    const UINT64 lastFrameDone = uploadFence_[(frameId + kSlots - 1) % kSlots];
    for (size_t i = 0; i < graveyard_.size();) {
        if (graveyard_[i].first <= frameId && lastFrameDone && fence_->GetCompletedValue() >= lastFrameDone) {
            graveyard_[i].second->Release();
            graveyard_.erase(graveyard_.begin() + long(i));
        } else {
            ++i;
        }
    }

    tracker::ScopedInternal internal;
    const ListFns& f = OriginalListFns();
    bool any = false;
    alloc_[k]->Reset();
    list_->Reset(alloc_[k], nullptr);

    // Mario's texture (once).
    std::vector<uint8_t> pixels;
    int tw = 0, th = 0;
    if (!texture_) {
        LockGuard lk(marioMu_); // (the texture's pixels are under it too)
        if (!texPixels_.empty()) {
            pixels = texPixels_;
            tw = pendingTexW_;
            th = pendingTexH_;
        }
    }
    if (!pixels.empty()) {
        const UINT64 bytes = UINT64(pixels.size());
        texture_ = MakeBuffer(device_, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        textureUpload_ = MakeBuffer(device_, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (texture_ && textureUpload_ && SUCCEEDED(textureUpload_->Map(0, &none, reinterpret_cast<void**>(&p)))) {
            std::memcpy(p, pixels.data(), pixels.size());
            textureUpload_->Unmap(0, nullptr);
            f.CopyBufferRegion(list_, texture_, 0, textureUpload_, 0, bytes);
            const D3D12_RESOURCE_BARRIER b =
                Transition(texture_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, kTexState);
            f.ResourceBarrier(list_, 1, &b);
            texW_ = tw;
            texH_ = th;
            textureCopyPending_ = true;
            any = true;
        } else {
            SafeRelease(texture_);
            SafeRelease(textureUpload_);
        }
    }

    // The newest view written by Mario's G-buffer draw, and the newest depth
    // capture (with the view frozen next to it), if not read back yet.
    const uint64_t snapF = snapshotFrame_.load(), capF = captureFrame_.load();
    const bool snap = snapF > lastSnapProcessed_;
    const bool depth = capF > lastCapProcessed_ && depthCopy_ != nullptr;
    sl.snapFrame = 0;
    sl.capFrame = 0;
    if (snap) {
        lastSnapProcessed_ = snapF;
        sl.snapFrame = snapF;
        D3D12_RESOURCE_BARRIER b = Transition(snapshot_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.ResourceBarrier(list_, 1, &b);
        f.CopyBufferRegion(list_, readback_[k], kReadbackSnapOffset, snapshot_, UINT64(snapF % kSlots) * kSnapStride,
                           kSnapStride);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        f.ResourceBarrier(list_, 1, &b);
        any = true;
    }
    sl.capMario = false;
    if (depth) {
        lastCapProcessed_ = capF;
        sl.capFrame = capF;
        const DrawnBounds& db = captureBounds_[capF % kSlots];
        sl.capMario = db.frame == capF;
        sl.capLo = db.lo;
        sl.capHi = db.hi;
        sl.capMotion = db.motionFrame == capF && motionCopy_ != nullptr;
        const UINT srcW = captureSrcW_, srcH = captureSrcH_;
        UINT outW = UINT(captureW_);
        UINT outH = std::max<UINT>(1, UINT(std::lround(double(outW) * double(srcH) / double(std::max<UINT>(1, srcW)))));
        if (outH > UINT(kMaxCaptureH)) {
            outH = kMaxCaptureH;
            outW = std::max<UINT>(1, UINT(std::lround(double(outH) * double(srcW) / double(std::max<UINT>(1, srcH)))));
        }
        outW = std::min<UINT>(outW, kMaxCaptureW);
        sl.capW = int(outW);
        sl.capH = int(outH);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(k) * 2 * srvStride_;
        device_->CreateShaderResourceView(depthCopy_, &sd, cpu);
        // The motion vectors (a null view when there are none this time).
        sd.Format = DXGI_FORMAT_R16G16_FLOAT;
        cpu.ptr += srvStride_;
        device_->CreateShaderResourceView(sl.capMotion ? motionCopy_ : nullptr, &sd, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(k) * 2 * srvStride_;
        f.SetDescriptorHeaps(list_, 1, &srvHeap_);
        f.SetComputeRootSignature(list_, rsCompute_);
        f.SetPipelineState(list_, csPso_);
        f.SetComputeRootDescriptorTable(list_, 0, gpu);
        f.SetComputeRootShaderResourceView(list_, 1, captureSnap_->GetGPUVirtualAddress());
        f.SetComputeRootUnorderedAccessView(list_, 2, captureOut_->GetGPUVirtualAddress());
        const uint32_t rc[8] = {outW, outH, srcW, srcH, sl.capMotion ? 1u : 0u, 0, 0, 0};
        f.SetComputeRoot32BitConstants(list_, 3, 8, rc, 0);
        f.Dispatch(list_, (outW + 7) / 8, (outH + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER b = Transition(captureOut_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.ResourceBarrier(list_, 1, &b);
        f.CopyBufferRegion(list_, readback_[k], kReadbackDepthOffset, captureOut_, 0,
                           UINT64(outW) * outH * (sl.capMotion ? 12 : 4));
        f.CopyBufferRegion(list_, readback_[k], kReadbackCapSnapOffset, captureSnap_, 0, kSnapStride);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        f.ResourceBarrier(list_, 1, &b);
        any = true;
    }
    list_->Close();
    if (any) {
        ID3D12CommandList* lists[] = {list_};
        d3d12hook::SetInternalSubmit(true);
        queue->ExecuteCommandLists(1, lists);
        d3d12hook::SetInternalSubmit(false);
        if (textureCopyPending_) {
            textureCopyPending_ = false;
            textureReady_ = true;
            LOGI("in-world rendering: Mario's texture uploaded (%dx%d)", texW_, texH_);
        }
    }
    queue->Signal(fence_, ++fenceValue_);
    sl.fence = fenceValue_;
    queue->Release();
}

void Injector::CollectResults() {
    // Called with mu_ held.
    const UINT64 done = fence_->GetCompletedValue();
    int order[kReadbackSlots];
    int n = 0;
    for (int i = 0; i < kReadbackSlots; ++i)
        if (slots_[i].fence && slots_[i].fence <= done && (slots_[i].snapFrame || slots_[i].capFrame)) order[n++] = i;
    for (int i = 1; i < n; ++i) // oldest first (n <= kReadbackSlots)
        for (int j = i; j > 0 && slots_[order[j]].fence < slots_[order[j - 1]].fence; --j) std::swap(order[j], order[j - 1]);
    auto parseSnap = [](const uint8_t* p, CaptureResult& r, uint64_t& frameOut) {
        uint32_t hdr[4];
        std::memcpy(hdr, p + 576, 16);
        if (hdr[1] != kSnapMagic) return false;
        frameOut = uint64_t(hdr[0]) | (uint64_t(hdr[2]) << 32);
        float rows[kViewCbRows][4];
        std::memcpy(rows, p, sizeof(rows));
        // Where the draw put Mario against his camera (the shader's choice).
        uint32_t al[4];
        std::memcpy(al, p + 592, 16);
        r.align = al[3] & 0xFFu;
        r.alignCount = (al[3] >> 8) & 0xFFu;
        float sh[3];
        std::memcpy(sh, al, sizeof(sh));
        r.alignShift = Vec3(sh[0], sh[1], sh[2]);
        if (!IsFinite(r.alignShift)) r.align = CaptureResult::kAlignOff;
        return ParseViewConstants(&rows[0][0], r.view);
    };
    for (int oi = 0; oi < n; ++oi) {
        Slot& sl = slots_[order[oi]];
        const uint8_t* rb = readbackPtr_[order[oi]];
        CaptureResult r;
        bool ok = false;
        // The depth and its view were copied together on the GPU; the view's
        // frame must be the one the capture was recorded in (else Mario's
        // draw for that frame hadn't run yet and the pair is discarded).
        if (sl.capFrame) {
            uint64_t f = 0;
            if (parseSnap(rb + kReadbackCapSnapOffset, r, f) && f == sl.capFrame && f > lastCaptureResult_) {
                lastCaptureResult_ = f;
                r.frame = f;
                r.hasDepth = true;
                r.width = sl.capW;
                r.height = sl.capH;
                r.depth.resize(size_t(sl.capW) * size_t(sl.capH));
                std::memcpy(r.depth.data(), rb + kReadbackDepthOffset, r.depth.size() * 4);
                if (sl.capMotion) {
                    r.motion.resize(r.depth.size() * 2);
                    std::memcpy(r.motion.data(), rb + kReadbackDepthOffset + r.depth.size() * 4, r.motion.size() * 4);
                }
                r.marioDrawn = sl.capMario;
                r.marioLo = sl.capLo;
                r.marioHi = sl.capHi;
                ok = true;
            } else {
                LockGuard lk(statsMu_);
                ++stats_.droppedResults;
            }
        }
        if (!ok && sl.snapFrame) {
            uint64_t f = 0;
            if (parseSnap(rb + kReadbackSnapOffset, r, f) && f == sl.snapFrame) {
                r.frame = f;
                ok = true;
            }
        }
        sl.snapFrame = sl.capFrame = 0;
        if (!ok) continue;
        {
            LockGuard lk(resultMu_);
            // Keep the newest; a result with depth is not replaced by a
            // view-only one until it has been taken (the newer view is kept
            // alongside for the camera).
            if (!haveResult_ || r.hasDepth || !result_.hasDepth) {
                result_ = std::move(r);
                haveResult_ = true;
            } else if (r.frame > pendingViewFrame_) {
                pendingView_ = r.view;
                pendingViewFrame_ = r.frame;
                pendingAlign_ = r.align;
                pendingAlignCount_ = r.alignCount;
                pendingAlignShift_ = r.alignShift;
            }
        }
        LockGuard lk2(statsMu_);
        ++stats_.results;
        stats_.lastResultFrame = std::max<uint64_t>(stats_.lastResultFrame, lastCaptureResult_);
    }
}

bool Injector::PopResult(CaptureResult& out) {
    LockGuard lk(resultMu_);
    if (!haveResult_) return false;
    out = std::move(result_);
    haveResult_ = false;
    if (pendingViewFrame_ > out.frame && !out.hasDepth) {
        out.view = pendingView_;
        out.frame = pendingViewFrame_;
        out.align = pendingAlign_;
        out.alignCount = pendingAlignCount_;
        out.alignShift = pendingAlignShift_;
    }
    pendingViewFrame_ = 0;
    result_ = CaptureResult();
    return true;
}

Injector::Stats Injector::GetStats() {
    LockGuard lock(statsMu_);
    Stats s = stats_;
    s.psoVariants = int(psos_.size());
    if (failed_.load()) s.status = "failed: " + Error();
    return s;
}

} // namespace sm2m

#endif
