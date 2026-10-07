#ifdef _WIN32

#include "renderer.h"

#include <algorithm>
#include <cstring>

#include "../common/log.h"
#include "d3d12_hook.h"
#include "frame_tracker.h"

namespace sm2m {
namespace {

const char* kShaderSource = R"HLSL(
cbuffer Root : register(b0) {
    row_major float4x4 uLocalToClip;
    float4 uN0;
    float4 uN1;
    float4 uN2;
    float4 uLight;   // xyz: direction towards the light (view space), w: ambient
    float4 uParams;  // x: output mode (0 SDR, 1 scRGB, 2 HDR10), y: paper white nits, z: brightness
    float4 uScreen;  // xy: 2/width, 2/height
};
Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);

float3 SrgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 LinearToPQ(float3 nits) {
    float3 y = saturate(nits / 10000.0);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(y, m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float3 Encode(float3 c) {
    if (uParams.x < 0.5) return c;
    float3 lin = SrgbToLinear(saturate(c));
    if (uParams.x < 1.5) return lin * (uParams.y / 80.0);
    const float3x3 to2020 = { 0.6274, 0.3293, 0.0433,
                              0.0691, 0.9195, 0.0114,
                              0.0164, 0.0880, 0.8956 };
    return LinearToPQ(mul(to2020, lin) * uParams.y);
}

struct MarioVSIn { float3 pos : POSITION; float3 nrm : NORMAL; float3 col : COLOR; float2 uv : TEXCOORD; };
struct MarioPSIn { float4 pos : SV_Position; float3 nrm : NORMAL; float3 col : COLOR; float2 uv : TEXCOORD; };

MarioPSIn VSMario(MarioVSIn i) {
    MarioPSIn o;
    o.pos = mul(float4(i.pos, 1.0), uLocalToClip);
    o.nrm = i.nrm.x * uN0.xyz + i.nrm.y * uN1.xyz + i.nrm.z * uN2.xyz;
    o.col = i.col;
    o.uv = i.uv;
    return o;
}
float4 PSMario(MarioPSIn i) : SV_Target {
    float4 t = gTex.Sample(gSamp, i.uv);
    float3 base = lerp(i.col, t.rgb, t.a);
    float ndl = saturate(dot(normalize(i.nrm), uLight.xyz));
    float light = uLight.w + (1.0 - uLight.w) * ndl;
    return float4(Encode(base * light * uParams.z), 1.0);
}

struct OvVSIn { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
struct OvPSIn { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 col : COLOR; };
OvPSIn VSOverlay(OvVSIn i) {
    OvPSIn o;
    o.pos = float4(i.pos.x * uScreen.x - 1.0, 1.0 - i.pos.y * uScreen.y, 0.0, 1.0);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}
float4 PSOverlay(OvPSIn i) : SV_Target {
    float cov = gTex.Sample(gSamp, i.uv).r;
    return float4(Encode(i.col.rgb), i.col.a * cov);
}
)HLSL";

struct RootConstants {
    float localToClip[16];
    float n0[4], n1[4], n2[4];
    float light[4];
    float params[4];
    float screen[4];
};
static_assert(sizeof(RootConstants) == 40 * 4, "root constants must be 40 dwords");

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool IsRenderableFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM ||
           f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

} // namespace

Renderer::~Renderer() { ReleaseAll(); }

void Renderer::SetMarioTexture(const uint8_t* rgba, int w, int h) {
    pendingTex_.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
    pendingTexW_ = w;
    pendingTexH_ = h;
    marioTexReady_ = false;
}

bool Renderer::CompileShaders() {
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!comp) {
        status_ = "d3dcompiler_47.dll not found";
        return false;
    }
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(comp, "D3DCompile")));
    if (!compile) {
        status_ = "D3DCompile missing";
        return false;
    }
    struct Job {
        const char* entry;
        const char* target;
        ID3DBlob** out;
    } jobs[] = {{"VSMario", "vs_5_0", &vsMario_},
                {"PSMario", "ps_5_0", &psMario_},
                {"VSOverlay", "vs_5_0", &vsOverlay_},
                {"PSOverlay", "ps_5_0", &psOverlay_}};
    for (auto& j : jobs) {
        ID3DBlob* err = nullptr;
        HRESULT hr = compile(kShaderSource, std::strlen(kShaderSource), "sm2mario", nullptr, nullptr, j.entry, j.target,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, j.out, &err);
        if (FAILED(hr)) {
            LOGE("shader %s failed: %s", j.entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            SafeRelease(err);
            status_ = "shader compile failed";
            return false;
        }
        SafeRelease(err);
    }
    return true;
}

bool Renderer::BuildFontAtlas(std::vector<uint8_t>& px) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = CreateFontW(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SIZE sz{};
    GetTextExtentPoint32W(dc, L"W", 1, &sz);
    font_.cellW = int(sz.cx) + 2;
    font_.cellH = int(tm.tmHeight) + 2;
    font_.columns = 16;
    font_.firstChar = 32;
    font_.lastChar = 126;
    const int count = font_.lastChar - font_.firstChar + 1;
    const int rows = (count + font_.columns - 1) / font_.columns;
    int w = 256, h = 64;
    while (w < font_.columns * font_.cellW) w *= 2;
    while (h < rows * font_.cellH + 8) h *= 2;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT rc{0, 0, w, h};
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < count; ++i) {
        wchar_t ch = wchar_t(font_.firstChar + i);
        TextOutW(dc, (i % font_.columns) * font_.cellW + 1, (i / font_.columns) * font_.cellH + 1, &ch, 1);
    }
    GdiFlush();
    px.assign(size_t(w) * h, 0);
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = src + (size_t(y) * w + x) * 4;
            px[size_t(y) * w + x] = std::max(p[0], std::max(p[1], p[2]));
        }
    for (int y = h - 4; y < h; ++y)
        for (int x = w - 4; x < w; ++x) px[size_t(y) * w + x] = 255;
    font_.atlasW = w;
    font_.atlasH = h;
    font_.whiteU = (float(w) - 2.0f) / float(w);
    font_.whiteV = (float(h) - 2.0f) / float(h);
    font_.basePixelHeight = float(font_.cellH);

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return true;
}

bool Renderer::UploadTexture(ID3D12Resource* dst, const void* data, UINT rowPitch, UINT w, UINT h, DXGI_FORMAT,
                             UINT bpp) {
    tracker::ScopedInternal internal; // our own commands: the frame hooks pass through
    // Allocator 0 may still be executing a frame; this is a one-off, so just drain the queue.
    WaitIdle();
    const UINT alignedPitch = (w * bpp + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 size = UINT64(alignedPitch) * h;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* staging = nullptr;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&staging))))
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    staging->Map(0, &none, reinterpret_cast<void**>(&mapped));
    for (UINT y = 0; y < h; ++y)
        std::memcpy(mapped + size_t(y) * alignedPitch, static_cast<const uint8_t*>(data) + size_t(y) * rowPitch, w * bpp);
    staging->Unmap(0, nullptr);

    alloc_[0]->Reset();
    list_->Reset(alloc_[0], nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    D3D12_RESOURCE_DESC td = dst->GetDesc();
    from.PlacedFootprint.Offset = 0;
    from.PlacedFootprint.Footprint.Format = td.Format;
    from.PlacedFootprint.Footprint.Width = w;
    from.PlacedFootprint.Footprint.Height = h;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = alignedPitch;
    list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER b =
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &b);
    list_->Close();
    ID3D12CommandList* lists[] = {list_};
    d3d12hook::SetInternalSubmit(true);
    queue_->ExecuteCommandLists(1, lists);
    d3d12hook::SetInternalSubmit(false);
    WaitIdle();
    staging->Release();
    return true;
}

void Renderer::WaitIdle() {
    if (!queue_ || !fence_) return;
    const UINT64 v = ++fenceValue_;
    queue_->Signal(fence_, v);
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 2000);
    }
}

bool Renderer::CreateDeviceObjects() {
    tracker::ScopedInternal internal;
    if (!CompileShaders()) return false;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto serialize = reinterpret_cast<SerializeFn>(
        reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")));
    if (!serialize) {
        status_ = "D3D12SerializeRootSignature missing";
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(RootConstants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)))) {
        SafeRelease(blob);
        SafeRelease(err);
        status_ = "root signature failed";
        return false;
    }
    SafeRelease(blob);
    SafeRelease(err);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap_)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) return false;
    srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (int i = 0; i < kFrames; ++i) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_[i]))))
            return false;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kUploadSize;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&upload_[i]))))
            return false;
        D3D12_RANGE none{0, 0};
        upload_[i]->Map(0, &none, reinterpret_cast<void**>(&uploadPtr_[i]));
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_PPV_ARGS(&list_))))
        return false;
    list_->Close();
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Font atlas (R8) and its SRV in slot 1.
    std::vector<uint8_t> fontPx;
    if (!BuildFontAtlas(fontPx)) {
        status_ = "font atlas failed";
        return false;
    }
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = UINT64(font_.atlasW);
    td.Height = UINT(font_.atlasH);
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&fontTex_))))
        return false;
    if (!UploadTexture(fontTex_, fontPx.data(), UINT(font_.atlasW), UINT(font_.atlasW), UINT(font_.atlasH),
                       DXGI_FORMAT_R8_UNORM, 1))
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += srvStride_;
    device_->CreateShaderResourceView(fontTex_, &sd, h);

    // Slot 0 starts as a 1x1 transparent texture so untextured draws (the
    // calibration marker) work before the ROM's Mario atlas is uploaded.
    td.Width = 1;
    td.Height = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&marioTex_))))
        return false;
    const uint32_t transparent = 0;
    if (!UploadTexture(marioTex_, &transparent, 4, 1, 1, td.Format, 4)) return false;
    sd.Format = td.Format;
    device_->CreateShaderResourceView(marioTex_, &sd, srvHeap_->GetCPUDescriptorHandleForHeapStart());
    deviceObjects_ = true;
    return true;
}

bool Renderer::CreateSizeObjects(IDXGISwapChain3* sc) {
    tracker::ScopedInternal internal;
    DXGI_SWAP_CHAIN_DESC1 d{};
    if (FAILED(sc->GetDesc1(&d))) return false;
    HWND hwnd = nullptr;
    sc->GetHwnd(&hwnd);
    const bool formatChanged = d.Format != format_;
    width_ = d.Width;
    height_ = d.Height;
    bufferCount_ = d.BufferCount;
    format_ = d.Format;
    hwnd_ = hwnd;
    if (!IsRenderableFormat(format_)) {
        status_ = "unsupported back buffer format " + std::to_string(int(format_));
        return false;
    }
    // If the game chose its colour space before our hooks went in, infer it:
    // a 10-bit back buffer on an HDR output is HDR10 (PQ) in practice.
    if (!colorSpaceKnown_) {
        colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        IDXGIOutput* out = nullptr;
        if (format_ == DXGI_FORMAT_R10G10B10A2_UNORM && SUCCEEDED(sc->GetContainingOutput(&out)) && out) {
            IDXGIOutput6* out6 = nullptr;
            if (SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&out6))) && out6) {
                DXGI_OUTPUT_DESC1 od{};
                if (SUCCEEDED(out6->GetDesc1(&od)) && od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                    colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                out6->Release();
            }
            out->Release();
        }
    }

    // Depth for Mario's self-occlusion.
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = width_;
    dd.Height = height_;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    if (FAILED(device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                IID_PPV_ARGS(&depth_))))
        return false;
    device_->CreateDepthStencilView(depth_, nullptr, dsvHeap_->GetCPUDescriptorHandleForHeapStart());

    if (formatChanged || !marioPso_) {
        SafeRelease(marioPso_);
        SafeRelease(overlayPso_);
        D3D12_INPUT_ELEMENT_DESC marioLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = rootSig_;
        p.VS = {vsMario_->GetBufferPointer(), vsMario_->GetBufferSize()};
        p.PS = {psMario_->GetBufferPointer(), psMario_->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthEnable = TRUE;
        p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        p.InputLayout = {marioLayout, 4};
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = format_;
        p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        p.SampleDesc.Count = 1;
        if (FAILED(device_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&marioPso_)))) {
            status_ = "Mario pipeline failed";
            return false;
        }
        D3D12_INPUT_ELEMENT_DESC ovLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC o = p;
        o.VS = {vsOverlay_->GetBufferPointer(), vsOverlay_->GetBufferSize()};
        o.PS = {psOverlay_->GetBufferPointer(), psOverlay_->GetBufferSize()};
        o.InputLayout = {ovLayout, 3};
        o.DepthStencilState.DepthEnable = FALSE;
        o.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        o.DSVFormat = DXGI_FORMAT_UNKNOWN;
        auto& rt = o.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (FAILED(device_->CreateGraphicsPipelineState(&o, IID_PPV_ARGS(&overlayPso_)))) {
            status_ = "overlay pipeline failed";
            return false;
        }
    }
    return true;
}

void Renderer::ReleaseSizeObjects() { SafeRelease(depth_); }

void Renderer::ReleaseAll() {
    WaitIdle();
    ReleaseSizeObjects();
    SafeRelease(marioPso_);
    SafeRelease(overlayPso_);
    SafeRelease(rootSig_);
    SafeRelease(srvHeap_);
    SafeRelease(rtvHeap_);
    SafeRelease(dsvHeap_);
    SafeRelease(marioTex_);
    SafeRelease(fontTex_);
    for (int i = 0; i < kFrames; ++i) {
        if (upload_[i]) upload_[i]->Unmap(0, nullptr);
        SafeRelease(upload_[i]);
        uploadPtr_[i] = nullptr;
        SafeRelease(alloc_[i]);
        frameFence_[i] = 0;
    }
    SafeRelease(list_);
    SafeRelease(fence_);
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    SafeRelease(vsMario_);
    SafeRelease(psMario_);
    SafeRelease(vsOverlay_);
    SafeRelease(psOverlay_);
    SafeRelease(queue_);
    SafeRelease(device_);
    deviceObjects_ = false;
    marioTexReady_ = false;
    ready_ = false;
    format_ = DXGI_FORMAT_UNKNOWN;
}

bool Renderer::Ensure(IDXGISwapChain* scIn) {
    if (failed_) return false;
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(scIn->QueryInterface(IID_PPV_ARGS(&sc)))) return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    HWND hwnd = nullptr;
    if (FAILED(sc->GetDesc1(&desc)) || FAILED(sc->GetHwnd(&hwnd)) || !hwnd) {
        sc->Release();
        return false;
    }
    // Only ever draw on one window (the game's). Another swap chain in the
    // process (launcher, overlay, tool) is ignored instead of thrashing.
    if (lockedHwnd_ && !IsWindow(lockedHwnd_)) lockedHwnd_ = nullptr;
    if (lockedHwnd_ && hwnd != lockedHwnd_) {
        sc->Release();
        return false;
    }
    if (!lockedHwnd_ && (desc.Width < 320 || desc.Height < 180 || !IsWindowVisible(hwnd))) {
        sc->Release();
        return false;
    }
    // The swap chain pointer isn't AddRef'd, so a recreated chain can land at
    // the same address: compare what matters, not just the pointer.
    if (ready_ && scIn == sc_ && desc.Width == width_ && desc.Height == height_ && desc.Format == format_ &&
        desc.BufferCount == bufferCount_ && hwnd == hwnd_) {
        sc->Release();
        return true;
    }
    ready_ = false;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&queue))) && queue) {
        queue->GetDevice(IID_PPV_ARGS(&device));
    } else if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device))) && device) {
        queue = d3d12hook::LastDirectQueue(device);
    }
    if (!device || !queue) {
        SafeRelease(queue);
        SafeRelease(device);
        sc->Release();
        status_ = "swap chain is not D3D12 or its queue is unknown yet";
        return false;
    }

    if (device != device_) {
        ReleaseAll();
        device_ = device;
        queue_ = queue;
        if (!CreateDeviceObjects()) {
            LOGE("renderer: %s", status_.c_str());
            failed_ = true;
            sc->Release();
            return false;
        }
    } else {
        SafeRelease(device);
        if (queue != queue_) {
            SafeRelease(queue_);
            queue_ = queue;
        } else {
            SafeRelease(queue);
        }
        WaitIdle();
        ReleaseSizeObjects();
    }
    sc_ = scIn;
    if (!CreateSizeObjects(sc)) {
        LOGE("renderer: %s", status_.c_str());
        sc->Release();
        return false;
    }
    sc->Release();
    ready_ = true;
    lockedHwnd_ = hwnd_;
    status_ = "ok";
    LOGI("renderer: %ux%u format %d, %u buffers, window %p", width_, height_, int(format_), bufferCount_,
         static_cast<void*>(hwnd_));
    return true;
}

void Renderer::OnResizeBegin(IDXGISwapChain* sc) {
    if (sc != sc_) return;
    WaitIdle();
    ReleaseSizeObjects();
    ready_ = false;
    sc_ = nullptr;
}

void Renderer::OnColorSpace(IDXGISwapChain* scIn, DXGI_COLOR_SPACE_TYPE cs) {
    // Match by window, not pointer: a recreated swap chain sets its colour
    // space before its first Present.
    IDXGISwapChain1* sc = nullptr;
    HWND hwnd = nullptr;
    if (SUCCEEDED(scIn->QueryInterface(IID_PPV_ARGS(&sc))) && sc) {
        sc->GetHwnd(&hwnd);
        sc->Release();
    }
    if (lockedHwnd_ && hwnd != lockedHwnd_) return;
    colorSpace_ = cs;
    colorSpaceKnown_ = true;
}

void Renderer::Render(IDXGISwapChain* scIn, const MarioDraw& mario, const std::vector<OverlayVertex>& overlay) {
    if (!ready_ || scIn != sc_) return;
    tracker::ScopedInternal internal; // our own commands: the frame hooks pass through
    if (!mario.enabled && overlay.empty()) return;
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(scIn->QueryInterface(IID_PPV_ARGS(&sc)))) return;

    // Lazy Mario texture upload (sm64_global_init runs on another thread).
    if (!marioTexReady_ && !pendingTex_.empty()) {
        D3D12_HEAP_PROPERTIES dh{};
        dh.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = UINT64(pendingTexW_);
        td.Height = UINT(pendingTexH_);
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        WaitIdle(); // the placeholder texture may still be in use by in-flight frames
        SafeRelease(marioTex_);
        if (SUCCEEDED(device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&marioTex_))) &&
            UploadTexture(marioTex_, pendingTex_.data(), UINT(pendingTexW_) * 4, UINT(pendingTexW_), UINT(pendingTexH_),
                          td.Format, 4)) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = td.Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(marioTex_, &sd, srvHeap_->GetCPUDescriptorHandleForHeapStart());
            marioTexReady_ = true;
        }
    }

    frame_ = (frame_ + 1) % kFrames;
    if (frameFence_[frame_] && fence_->GetCompletedValue() < frameFence_[frame_]) {
        fence_->SetEventOnCompletion(frameFence_[frame_], fenceEvent_);
        WaitForSingleObject(fenceEvent_, 1000);
    }
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) {
        sc->Release();
        return;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = format_;
    rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(bb, &rtvd, rtv);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();

    alloc_[frame_]->Reset();
    list_->Reset(alloc_[frame_], nullptr);
    D3D12_RESOURCE_BARRIER toRt = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &toRt);

    D3D12_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
    D3D12_RECT sr{0, 0, LONG(width_), LONG(height_)};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &sr);
    list_->SetGraphicsRootSignature(rootSig_);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_};
    list_->SetDescriptorHeaps(1, heaps);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    RootConstants rc{};
    const float mode = (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT)                                          ? 1.0f
                       : (colorSpace_ == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && format_ == DXGI_FORMAT_R10G10B10A2_UNORM) ? 2.0f
                                                                                                              : 0.0f;
    rc.params[0] = mode;
    rc.params[1] = paperWhite_;
    rc.params[2] = mario.brightness;
    rc.screen[0] = 2.0f / float(width_);
    rc.screen[1] = 2.0f / float(height_);

    uint8_t* up = uploadPtr_[frame_];
    const D3D12_GPU_VIRTUAL_ADDRESS base = upload_[frame_]->GetGPUVirtualAddress();
    UINT64 used = 0;
    auto push = [&](const void* data, UINT64 bytes) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (used + bytes > kUploadSize) return 0;
        std::memcpy(up + used, data, size_t(bytes));
        D3D12_GPU_VIRTUAL_ADDRESS a = base + used;
        used += (bytes + 255) & ~UINT64(255);
        return a;
    };

    if (mario.enabled && mario.geo && mario.geo->triangles > 0) {
        const UINT verts = UINT(mario.geo->triangles) * 3;
        D3D12_VERTEX_BUFFER_VIEW vbs[4]{};
        const void* streams[4] = {mario.geo->position.data(), mario.geo->normal.data(), mario.geo->color.data(),
                                  mario.geo->uv.data()};
        const UINT strides[4] = {12, 12, 12, 8};
        bool ok = true;
        for (int s = 0; s < 4; ++s) {
            D3D12_GPU_VIRTUAL_ADDRESS a = push(streams[s], UINT64(verts) * strides[s]);
            if (!a) ok = false;
            vbs[s].BufferLocation = a;
            vbs[s].SizeInBytes = verts * strides[s];
            vbs[s].StrideInBytes = strides[s];
        }
        if (ok) {
            list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            std::memcpy(rc.localToClip, &mario.localToClip.m[0][0], sizeof(rc.localToClip));
            for (int k = 0; k < 3; ++k) {
                rc.n0[k] = mario.normalToView[0][k];
                rc.n1[k] = mario.normalToView[1][k];
                rc.n2[k] = mario.normalToView[2][k];
            }
            Vec3 l = Normalize(mario.lightView);
            rc.light[0] = l.x;
            rc.light[1] = l.y;
            rc.light[2] = l.z;
            rc.light[3] = mario.ambient;
            list_->SetPipelineState(marioPso_);
            list_->SetGraphicsRoot32BitConstants(0, sizeof(rc) / 4, &rc, 0);
            list_->SetGraphicsRootDescriptorTable(1, srvHeap_->GetGPUDescriptorHandleForHeapStart());
            list_->IASetVertexBuffers(0, 4, vbs);
            list_->DrawInstanced(verts, 1, 0, 0);
        }
    }

    if (!overlay.empty()) {
        const UINT64 bytes = UINT64(overlay.size()) * sizeof(OverlayVertex);
        D3D12_GPU_VIRTUAL_ADDRESS a = push(overlay.data(), bytes);
        if (a) {
            D3D12_VERTEX_BUFFER_VIEW vb{a, UINT(bytes), UINT(sizeof(OverlayVertex))};
            list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list_->SetPipelineState(overlayPso_);
            list_->SetGraphicsRoot32BitConstants(0, sizeof(rc) / 4, &rc, 0);
            D3D12_GPU_DESCRIPTOR_HANDLE fontSrv = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            fontSrv.ptr += srvStride_;
            list_->SetGraphicsRootDescriptorTable(1, fontSrv);
            list_->IASetVertexBuffers(0, 1, &vb);
            list_->DrawInstanced(UINT(overlay.size()), 1, 0, 0);
        }
    }

    D3D12_RESOURCE_BARRIER toPresent = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &toPresent);
    list_->Close();
    ID3D12CommandList* lists[] = {list_};
    d3d12hook::SetInternalSubmit(true);
    queue_->ExecuteCommandLists(1, lists);
    d3d12hook::SetInternalSubmit(false);
    frameFence_[frame_] = ++fenceValue_;
    queue_->Signal(fence_, fenceValue_);
    bb->Release();
    sc->Release();
}

} // namespace sm2m

#endif
