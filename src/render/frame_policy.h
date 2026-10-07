// Where in the game's frame Mario goes.
//
// The mod's D3D12 hooks (render/frame_tracker.cpp) mirror every command list
// the game records into a ListState and ask this module, at each "boundary"
// (render targets change, a barrier, the list closes...), whether something
// should be injected right there:
//
//   G-buffer   The game draws its opaque world into 4 render targets with
//              fixed formats (linear depth R32F, motion R16G16F, GBuffer0/1
//              R32G32 uint) and a depth buffer. At the end of every such
//              stretch of draws ("segment") for the main view, Mario is drawn
//              into the same targets with the same view constants, so the
//              game's own lighting, shadows, AO, reflections, fog, TAA and
//              motion blur treat him like any other object. Drawing him more
//              than once per frame is harmless (same depth, same values), and
//              the last draw wins over decals rendered between segments.
//
//   Shadows    The sun/local shadow maps are cached: static casters are drawn
//              into a cache, which is copied into the working map every frame
//              (PS_ShadowCacheCopyDepth) before dynamic casters are drawn.
//              Mario's shadow is drawn only into regions that were copied or
//              cleared in the same command list, so it can never be baked into
//              a cache (no ghost shadows).
//
//   Capture    When the main linear-depth target leaves the RENDER_TARGET state
//              (the G-buffer is complete), its contents are copied for the
//              collision model (world/world_model.h).
//
// Platform independent: handles are plain integers, so the decisions are unit
// tested on Linux with synthetic command streams.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/platform.h"
#include "d3d12_parse.h"

namespace sm2m::frame {

using Digest = std::array<uint8_t, 16>;

enum class PsoKind : uint8_t { Other = 0, GBuffer, ShadowCaster, CacheCopy, CacheMove };
const char* PsoKindName(PsoKind k);

struct PsoClass {
    PsoKind kind = PsoKind::Other;
    d3d12p::PsoInfo info;
    uint32_t id = 0; // creation order, for logs
};

struct ClassifyConfig {
    // RT0..RTn formats of the game's full G-buffer (linear depth, motion,
    // GBuffer0, GBuffer1). A PSO matches when its first formats are these.
    std::vector<uint32_t> gbufferFormats{d3d12p::kFmtR32Float, d3d12p::kFmtR16G16Float, d3d12p::kFmtR32G32Uint,
                                         d3d12p::kFmtR32G32Uint};
    std::vector<Digest> casterVs, casterPs, cacheCopyPs, cacheMovePs;
};
PsoKind Classify(const d3d12p::PsoInfo& pso, const ClassifyConfig& cfg);
// Built-in digests of the game's engine shaders (DXIL and DXBC containers in
// Spider-Man2.exe): VS_ModelShadowCaster(Alpha), PS_ShadowCaster*,
// PS_ShadowCacheCopyDepth, PS_ShadowCacheMoveDepth.
ClassifyConfig DefaultClassifyConfig();

// A root signature plus what the injector needs from it.
struct RootSigLayout {
    d3d12p::RootSigInfo info;
    d3d12p::TableSlot viewVs;   // b0 space0 as the vertex shader sees it (descriptor table)
    int viewRootCbv = -1;       // ... or as a root CBV
    uint32_t constOffset[64] = {};
    uint32_t constTotal = 0;
    bool usable = false;        // b0 reachable and constants fit
};
void BuildLayout(RootSigLayout& l);

struct Rect {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const Rect& o) const { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
    bool Contains(const Rect& o) const { return left <= o.left && top <= o.top && right >= o.right && bottom >= o.bottom; }
    int64_t Area() const { return int64_t(right - left) * int64_t(bottom - top); }
};

struct Viewport {
    float x = 0, y = 0, w = 0, h = 0, minZ = 0, maxZ = 1;
    Rect Bounds() const;
    bool operator==(const Viewport& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h && minZ == o.minZ && maxZ == o.maxZ;
    }
};

// A resolved RTV / DSV descriptor.
struct ViewInfo {
    uint64_t resource = 0;
    uint32_t format = 0;         // view format
    uint32_t resFormat = 0;      // resource format
    uint32_t width = 0, height = 0; // resource size (mip 0)
    uint16_t mipLevels = 1, arraySize = 1;
    uint32_t mip = 0, firstSlice = 0, numSlices = 1;
    uint32_t dsvFlags = 0;       // D3D12_DSV_FLAGS: 1 read-only depth, 2 read-only stencil
    uint32_t dimension = 0;      // D3D12_RTV/DSV_DIMENSION
    bool valid = false;
    uint32_t Subresource() const { return mip + firstSlice * mipLevels; }
    uint32_t MipWidth() const { return width >> mip ? width >> mip : 1; }
    uint32_t MipHeight() const { return height >> mip ? height >> mip : 1; }
};

enum RootArgKind : uint8_t { kArgUnset = 0, kArgTable, kArgConsts, kArgCbv, kArgSrv, kArgUav };
struct RootArg {
    uint8_t kind = kArgUnset;
    uint64_t value = 0; // GPU descriptor handle or GPU virtual address
};

struct VbView {
    uint64_t gpu = 0;
    uint32_t size = 0, stride = 0;
};

struct GBufferSeg {
    bool active = false;
    int draws = 0;
    const PsoClass* pso = nullptr;
    uint64_t psoPtr = 0;
    uint64_t rootSig = 0;
    const RootSigLayout* layout = nullptr;
    uint64_t viewTable = 0; // GPU handle of the b0 descriptor (table start + offset)
    uint64_t viewCbv = 0;   // or root CBV address
    Viewport vp;
    Rect scissor;
    uint32_t numRt = 0;
    ViewInfo rt[8], ds;
    uint64_t rtv[8] = {}, dsv = 0;
    uint64_t heaps[2] = {};
    uint32_t numHeaps = 0;
    bool inRenderPass = false;
    bool markerMatch = false;
    bool depthWrite = false; // some draw in the segment wrote depth (the DSV is writable)
};

struct ShadowSeg {
    bool active = false;
    int casterDraws = 0;
    const PsoClass* pso = nullptr;
    uint64_t rootSig = 0;
    const RootSigLayout* layout = nullptr;
    uint64_t viewTable = 0, viewCbv = 0;
    Viewport vp;
    Rect scissor;
    ViewInfo ds;
    uint64_t dsv = 0;
    bool prepared = false, byCopy = false;
    bool viewFromCopy = false;   // the light view comes from the cache-copy draw (no casters seen)
    uint32_t colorTargets = 0;   // colour targets bound with the depth (shadow maps have none)
    uint64_t heaps[2] = {};
    uint32_t numHeaps = 0;
};

// Shadow-map areas made ready for casters in this list (copied from a cache,
// or cleared).
struct Prepared {
    uint64_t resource = 0;
    uint32_t slice = 0;
    Rect rc;
    bool full = false;
    bool byCopy = false;
    bool byDraw = false; // a depth-only draw with depth test ALWAYS (the region's clear quad)
};

// Covers (nearly all of) region `r`: clear rects are sometimes inset by a
// texel or two from the viewport the casters use.
bool Covers(const Prepared& p, const Rect& r);

struct ListState {
    uint64_t list = 0;
    uint64_t frame = 0;
    uint32_t serial = 0;        // per-list number, for logs
    // pipeline
    uint64_t pso = 0;
    const PsoClass* psoClass = nullptr;
    uint64_t stateObject = 0;      // SetPipelineState1 (ray tracing); it and pso unbind each other
    bool stateObjectLast = false;  // the last pipeline call was SetPipelineState1
    bool bundleRan = false;        // ExecuteBundle may have changed bindings we didn't see
    uint64_t rootSig = 0;
    const RootSigLayout* layout = nullptr;
    RootArg args[64];
    uint32_t constPool[64] = {};
    uint64_t heaps[2] = {};
    uint32_t numHeaps = 0;
    uint32_t topology = 0;
    VbView vbs[16];
    uint32_t vbSet = 0;         // slots the game set
    Viewport vp[16];
    uint32_t numVp = 0;
    Rect scissor[16];
    uint32_t numScissor = 0;
    uint32_t stencilRef = 0;
    uint32_t stencilRefBack = 0; // OMSetFrontAndBackStencilRef (CommandList8) can set them apart
    // predication (restored around injected work)
    uint64_t predBuffer = 0, predOffset = 0;
    uint32_t predOp = 0;
    // output merger
    uint32_t numRt = 0;
    uint64_t rtv[8] = {};
    uint64_t dsv = 0;
    ViewInfo rt[8], ds;
    bool inRenderPass = false;
    uint32_t renderPassFlags = 0; // D3D12_RENDER_PASS_FLAGS of the current render pass
    // PIX markers (innermost last)
    int markerDepth = 0;
    char markers[8][48] = {};
    // segments
    GBufferSeg gseg;
    ShadowSeg sseg;
    std::vector<Prepared> prepared;
    // per-list counters (frame report)
    uint32_t draws = 0, gbufferDraws = 0, casterDraws = 0, copyDraws = 0, injections = 0;
    uint32_t refreshDraws = 0;    // depth-only ALWAYS draws into a depth target (region clears)
    uint32_t unknownPsoDraws = 0; // draws with a pipeline the tracker never saw created
    uint32_t captures = 0;        // depth captures recorded into this list
    // The main motion-vector target's state after the last barrier on it in
    // this list (the depth capture copies it too).
    uint64_t motionRes = 0;
    uint32_t motionState = 0;
    bool reported = false;
    std::string trace;      // frame report: render target bindings and what was drawn into them
    uint32_t traceDraws = 0, traceKinds = 0;
    // Stencil writes into the main view, for the stencil census (handed over
    // once per list, at Close: the census has a lock).
    uint32_t numStencilKeys = 0;
    uint32_t stencilKeys[16] = {};
    uint32_t stencilDraws[16] = {};

    void Reset(uint64_t frameId);
    bool InMarker(const std::vector<std::string>& names) const;
};

struct PolicyConfig {
    bool gbuffer = true;
    bool shadows = true;
    bool capture = true;
    // Optional: only inject into G-buffer segments inside one of these PIX
    // markers, if the game emits them (falls back to every segment when none
    // has matched for a while).
    std::vector<std::string> gbufferMarkers{"GBuffer Dynamic", "GBuffer Animated"};
    int maxShadowsPerFrame = 24;
    // Shadow regions whose refresh isn't seen (cleared, cache-copied or reset
    // with a quad in the same list): also use the ones the game draws casters
    // into nearly every frame. The game must be refreshing those somehow, or
    // its own moving objects would smear; shadow caches, built once in a
    // while, never qualify.
    bool steadyRegions = false;
};

// Knowledge shared by all lists and frames. Thread-safe.
class Registry {
public:
    enum class MapKind : uint8_t { Unknown = 0, Working, Cache };

    void BeginFrame(uint64_t frame);
    uint64_t Frame() const { return frame_.load(std::memory_order_acquire); }

    // Main view: the largest G-buffer seen in the previous frame.
    void NoteGBufferSize(uint32_t w, uint32_t h);
    bool IsMainSize(uint32_t w, uint32_t h) const;
    // A screen-space target: the main view's size or aspect (half, quarter...
    // resolution depth, velocity and occlusion buffers), never a shadow map.
    bool IsScreenShaped(uint32_t w, uint32_t h) const;
    uint64_t MainArea() const { return prevMaxArea_.load(); }

    void NoteMarkerMatch(uint64_t frame) { lastMarkerMatch_.store(frame); }
    bool MarkersRequired(uint64_t frame) const;

    // Shadow maps.
    void MarkWorking(uint64_t res);
    void MarkCache(uint64_t res);
    MapKind Kind(uint64_t res) const;
    bool TryClaimShadow(uint64_t frame, uint64_t res, uint32_t slice, const Rect& rc, int maxPerFrame);
    // A shadow region got casters in `frame`. Returns how many of the last 16
    // frames (this one included) it got casters in.
    int NoteCasterRegion(uint64_t frame, uint64_t res, uint32_t slice, const Rect& rc);
    static constexpr int kSteadyFrames = 12; // of 16

    // The last shadow-caster pipeline seen per depth format: Mario's shadow
    // pipeline copies its depth test / bias when a region had no casters.
    void NoteCaster(const PsoClass* pc);
    const PsoClass* LastCaster(uint32_t dsvFormat) const;

    // Depth capture: once per frame.
    bool TryClaimCapture(uint64_t frame);
    void SetMainDepthTarget(uint64_t res, uint32_t subresource);
    bool IsMainDepthTarget(uint64_t res, uint32_t subresource) const;
    bool HasMainDepthTarget() const { return mainRt0_.load(std::memory_order_relaxed) != 0; }
    uint32_t MainDepthSubresource() const { return mainRt0Sub_.load(); }
    // The G-buffer's motion vectors (RT1, RG16F) next to the main depth: they
    // tell moving objects (cars, people) from the static world. 0 = unknown.
    void SetMainMotionTarget(uint64_t res, uint32_t subresource);
    bool IsMainMotionTarget(uint64_t res, uint32_t subresource) const;
    uint64_t MainMotionTarget() const { return mainRt1_.load(std::memory_order_relaxed); }
    uint32_t MainMotionSubresource() const { return mainRt1Sub_.load(); }

private:
    std::atomic<uint64_t> frame_{1};
    std::atomic<uint64_t> curMaxArea_{0}, prevMaxArea_{0};
    std::atomic<uint64_t> curMaxDims_{0}, prevMaxDims_{0}; // width << 32 | height of that G-buffer
    std::atomic<uint64_t> lastMarkerMatch_{0};
    std::atomic<uint64_t> captureFrame_{0};
    std::atomic<uint64_t> mainRt0_{0};
    std::atomic<uint32_t> mainRt0Sub_{0};
    std::atomic<uint64_t> mainRt1_{0};
    std::atomic<uint32_t> mainRt1Sub_{0};
    mutable Mutex mu_;
    std::unordered_map<uint64_t, MapKind> maps_;
    struct Claim {
        uint64_t frame;
        uint64_t res;
        uint32_t slice;
        Rect rc;
    };
    std::vector<Claim> shadowClaims_;
    struct RegionHistory {
        uint64_t res = 0;
        uint32_t slice = 0;
        Rect rc;
        uint64_t lastFrame = 0;
        uint32_t mask = 0; // bit n: casters n frames before lastFrame
    };
    std::vector<RegionHistory> regions_;
    uint64_t shadowClaimFrame_ = 0;
    std::vector<const PsoClass*> casters_;
};

enum class Boundary : uint8_t {
    RenderTargets,   // OMSetRenderTargets
    Barrier,         // ResourceBarrier / Barrier
    Close,
    EndRenderPass,
    BeginRenderPass,
    Viewports,       // RSSetViewports (shadow regions only)
    ClearDepth,      // ClearDepthStencilView
    ClearTarget,     // ClearRenderTargetView
    Other
};
const char* BoundaryName(Boundary b);

enum class Act : uint8_t { GBuffer, Shadow };

struct Decision {
    Act act = Act::GBuffer;
    GBufferSeg g;
    ShadowSeg s;
};

// Why a segment ended without an injection (frame report).
struct Skip {
    const char* why = nullptr;
    // shadow segments: the region (frame report)
    bool shadow = false;
    ViewInfo ds;
    Viewport vp;
    int casters = 0;
};

class Policy {
public:
    Policy(Registry* reg, const PolicyConfig& cfg) : reg_(reg), cfg_(cfg) {}
    void SetConfig(const PolicyConfig& cfg) { cfg_ = cfg; }
    const PolicyConfig& Config() const { return cfg_; }
    // Switches PolicyConfig::steadyRegions on at run time (any thread).
    void SetSteadyRegions(bool on) { steady_.store(on, std::memory_order_relaxed); }
    bool SteadyRegions() const { return cfg_.steadyRegions || steady_.load(std::memory_order_relaxed); }
    // Depth captures for the collision ("world" source) on or off at run time.
    void SetCapture(bool on) { capture_.store(on, std::memory_order_relaxed); }
    bool CaptureOn() const { return cfg_.capture && capture_.load(std::memory_order_relaxed); }

    // After a draw / ExecuteIndirect was recorded. `descStride` is the
    // CBV_SRV_UAV descriptor increment.
    void OnDraw(ListState& s, uint32_t descStride);
    // After ClearDepthStencilView was recorded.
    void OnClearDepth(ListState& s, const ViewInfo& dsv, const Rect* rects, uint32_t numRects);

    // Before a boundary is recorded: ends segments and appends what to inject
    // now. `skips` (optional) receives reasons for segments that ended without
    // an injection.
    void AtBoundary(ListState& s, Boundary b, std::vector<Decision>& out, std::vector<Skip>* skips = nullptr);

private:
    void EndGBuffer(ListState& s, std::vector<Decision>& out, std::vector<Skip>* skips);
    void EndShadow(ListState& s, std::vector<Decision>& out, std::vector<Skip>* skips);
    static uint64_t ViewAddress(const ListState& s, uint32_t descStride, uint64_t& rootCbv);

    Registry* reg_;
    PolicyConfig cfg_;
    std::atomic<bool> steady_{false};
    std::atomic<bool> capture_{true};
};

// Decodes a PIX event payload (ID3D12GraphicsCommandList::BeginEvent): ANSI,
// UTF-16 or PIX3 blobs. Best effort: printable characters of the format string.
std::string DecodePixEvent(uint32_t metadata, const void* data, uint32_t size);

} // namespace sm2m::frame
