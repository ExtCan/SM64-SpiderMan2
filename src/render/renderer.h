// Draws Mario and the HUD into the game's back buffer right before Present.
//
// Mario gets his own depth buffer (for self-occlusion) but is not depth
// tested against the game's scene: reading the game's depth target safely
// needs its resource state, which is unknown without tracking every barrier.
// So he is always visible in front of the world (see README, "Known limits").
#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "../common/vec.h"
#include "../sm64/mario.h"
#include "d3d.h"
#include "overlay.h"

namespace sm2m {

struct MarioDraw {
    bool enabled = false;
    const MarioGeometry* geo = nullptr;
    Mat4 localToClip;           // row-vector convention (v * M)
    float normalToView[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 lightView{0.3f, 0.7f, -0.6f};
    float ambient = 0.5f;
    float brightness = 1.0f;
};

class Renderer {
public:
    ~Renderer();
    // Prepares everything for `sc`; returns false if it isn't a usable D3D12 swap chain.
    bool Ensure(IDXGISwapChain* sc);
    void SetMarioTexture(const uint8_t* rgba, int w, int h);
    void Render(IDXGISwapChain* sc, const MarioDraw& mario, const std::vector<OverlayVertex>& overlay);
    void OnResizeBegin(IDXGISwapChain* sc);
    void OnColorSpace(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE cs);

    bool Ready() const { return ready_; }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    HWND Window() const { return hwnd_; }
    const FontInfo& Font() const { return font_; }
    const std::string& Status() const { return status_; }
    void SetPaperWhite(float nits) { paperWhite_ = nits; }

private:
    bool CreateDeviceObjects();
    bool CreateSizeObjects(IDXGISwapChain3* sc);
    void ReleaseSizeObjects();
    void ReleaseAll();
    void WaitIdle();
    bool UploadTexture(ID3D12Resource* dst, const void* data, UINT rowPitch, UINT w, UINT h, DXGI_FORMAT fmt, UINT bpp);
    bool BuildFontAtlas(std::vector<uint8_t>& pixels);
    bool CompileShaders();

    static constexpr int kFrames = 3;
    static constexpr UINT64 kUploadSize = 1u << 20;

    IDXGISwapChain* sc_ = nullptr; // identity only (never AddRef'd)
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12RootSignature* rootSig_ = nullptr;
    ID3D12PipelineState* marioPso_ = nullptr;
    ID3D12PipelineState* overlayPso_ = nullptr;
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    ID3D12DescriptorHeap* rtvHeap_ = nullptr;
    ID3D12DescriptorHeap* dsvHeap_ = nullptr;
    ID3D12Resource* marioTex_ = nullptr;
    ID3D12Resource* fontTex_ = nullptr;
    ID3D12Resource* depth_ = nullptr;
    ID3D12Resource* upload_[kFrames] = {};
    uint8_t* uploadPtr_[kFrames] = {};
    ID3D12CommandAllocator* alloc_[kFrames] = {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    UINT64 frameFence_[kFrames] = {};
    int frame_ = 0;

    ID3DBlob* vsMario_ = nullptr;
    ID3DBlob* psMario_ = nullptr;
    ID3DBlob* vsOverlay_ = nullptr;
    ID3DBlob* psOverlay_ = nullptr;

    std::vector<uint8_t> pendingTex_;
    int pendingTexW_ = 0, pendingTexH_ = 0;
    bool marioTexReady_ = false;

    UINT width_ = 0, height_ = 0, bufferCount_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    HWND hwnd_ = nullptr;
    HWND lockedHwnd_ = nullptr;
    bool colorSpaceKnown_ = false;
    bool ready_ = false;
    bool deviceObjects_ = false;
    bool failed_ = false;
    float paperWhite_ = 200.0f;
    FontInfo font_;
    std::string status_ = "waiting for the game's swap chain";
    UINT rtvStride_ = 0, srvStride_ = 0;
};

} // namespace sm2m

#endif
