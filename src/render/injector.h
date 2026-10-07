// Draws Mario inside the game's own frame.
//
//   G-buffer   InjectGBuffer() records Mario into the game's G-buffer pass, in
//              the game's command list, with the game's view constants (b0
//              of the pass). He is then lit, shadowed, fogged, reflected,
//              anti-aliased and motion-blurred by the game itself.
//   Shadows    InjectShadow() adds him to a shadow map region with that
//              region's light view.
//   Capture    Capture() copies the finished linear-depth target; EndFrame()
//              (from Present, on the game's queue) reduces it to a small depth
//              image and reads it back with the view constants that rendered
//              it, for the collision model.
//
// Every injection restores the command list's state (root signature and
// arguments, pipeline state, input assembler, viewports, predication) to what
// the game had set, so the game's following commands are unaffected.
#pragma once

#ifdef _WIN32

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../common/platform.h"
#include "../common/vec.h"
#include "d3d.h"
#include "frame_policy.h"
#include "list_fns.h"
#include "view_constants.h"

namespace sm2m {

struct MarioLook {
    float gloss = 0.15f;       // 0..1 (G-buffer gloss)
    float specular = 0.03f;    // F0, linear
    float occlusion = 1.0f;    // specular occlusion, 0..1 (how much of the reflections he gets)
    int metalMaterial = 0;     // metal cap: material table index (the game's reflections on him), -1 = none
    float albedoScale = 0.85f; // N64 colours are display colours; real albedo is a bit darker
    float shadowBias = 0.0f;   // metres, pushes Mario's shadow caster away from the light
    bool stencilMatch = true;  // copy the stencil state of the pass Mario joins (StencilMode::Copy only)
};

// What Mario writes into the stencil (stencil_census.h).
struct MarioStencil {
    enum Mode : uint8_t { Keep = 0, Write = 1, Copy = 2 };
    Mode mode = Keep;     // Keep: nothing (what is behind him stays); Write: `ref` under `mask`;
                          // Copy: 0.5's - the stencil writes of the pass he joins
    uint8_t ref = 0, mask = 0;
};

// One frame of Mario as the mod wants him drawn (game space, metres).
struct MarioFrame {
    bool visible = false;
    DVec3 anchor;                 // reference point; positions are relative to it
    uint32_t vertexCount = 0;     // triangle list
    std::vector<float> pos, nrm;  // 3 per vertex
    std::vector<float> col;       // 3 per vertex, 0..1 display colours
    std::vector<float> uv;        // 2 per vertex
    bool cut = false;             // teleported: no motion this frame
    bool metal = false;           // wearing the metal cap: drawn as polished metal
    bool vanish = false;          // the vanish cap: see-through
    uint64_t tag = 0;             // the frame it was made for (tracker frame number when published)
    // The pivot the mod's camera plan of the same frame was made around
    // (mod/camera_override.h), while the mod places the game's camera: a
    // view rendered from a camera placed around another pivot (a write on
    // the game's own thread, a frame earlier or later) gets him moved by the
    // difference - see Injector::SetPlacementSource.
    bool havePivot = false;
    DVec3 pivot;
};

// Where the game's camera was put lately (game/hero_pin, through the mod):
// the position written, the pivot it was placed around and the blend, when
// (NowSeconds), in the order written.
struct CameraPlacementRec {
    DVec3 pos, pivot;
    float blend = 0;
    double time = 0;
    uint32_t order = 0;
    int slot = 0; // which of the game's cameras
};
using PlacementSource = int (*)(CameraPlacementRec* out, int max);

// The game's view (and, when captured, its linear depth) for one frame.
struct CaptureResult {
    uint64_t frame = 0;
    ViewConstants view;
    bool hasDepth = false;
    int width = 0, height = 0;
    std::vector<float> depth; // row 0 at the top; linear metres (sky = view.skyDepth)
    // The G-buffer's motion vectors at the same pixels (2 floats each, the
    // game's encoding: (uv now - uv last frame) * view row 33); empty if the
    // motion target couldn't be captured with this frame.
    std::vector<float> motion;
    // Bounds of Mario's mesh as drawn in that frame (game space): he is in the
    // depth there, and must not become part of the world.
    bool marioDrawn = false;
    DVec3 marioLo, marioHi;
    // Where his camera had him (SetPlacementSource), as the G-buffer draw of
    // that frame found it: `align` kAlignOff (not in use), kAlignNewest (the
    // view matched no placement: the newest one's shift), or the placement's
    // index; and the shift Mario was drawn with (m, game space).
    static constexpr uint32_t kAlignOff = 0xFFu, kAlignNewest = 0x80u;
    uint32_t align = kAlignOff;
    uint32_t alignCount = 0; // placements the shader had to choose from
    Vec3 alignShift;
};

class Injector {
public:
    ~Injector();
    // D3DCompile; call once from the init thread.
    bool CompileShaders(std::string& error);
    void SetLook(const MarioLook& look) {
        LockGuard lock(mu_);
        look_ = look;
    }
    // Live change from the settings menus (Present thread; read when a frame is prepared).
    void SetShine(float gloss, float specular, float occlusion) {
        LockGuard lock(mu_);
        look_.gloss = gloss;
        look_.specular = specular;
        look_.occlusion = occlusion;
    }
    void SetCaptureSize(int width, int height);
    void SetMarioTexture(const uint8_t* rgba, int w, int h);
    void SetMario(const MarioFrame& f);
    // Which of the frames published draws in a game frame: the one made for
    // it (0), or the one before (1, 2) - so each game frame gets the next one,
    // in step with the camera, whenever the game records its frame while the
    // next one is still being made.
    void SetDrawLag(int lag) { drawLag_.store(std::max(0, std::min(2, lag))); }
    // Mario drawn where the camera of the view had him: the vertex shader
    // finds which of the recent placements (`fn`) the view was rendered from
    // - by the view's position - and moves him by its pivot's difference to
    // the pivot of the Mario frame drawn (MarioFrame::pivot). So Mario and
    // his camera come from the same moment, whichever thread the game writes
    // its camera on and whenever it does. (0.6 drew the frame made for the
    // game frame, and its camera came from the frame before or that frame
    // depending on a race: in flight Mario jumped a frame's worth of his
    // motion back and forth against his camera.) Shadows (another view)
    // take the newest placement's shift. Off: as before.
    void SetPlacementSource(PlacementSource fn) { placementSource_.store(fn); }
    void SetAlignment(bool on) { align_.store(on); }
    void SetStencil(const MarioStencil& st) {
        LockGuard lock(mu_);
        stencil_ = st;
    }
    int DrawLag() const { return drawLag_.load(); }
    // The tag of the Mario frame drawn in game frame `frame` (0: unknown).
    uint64_t DrawnTag(uint64_t frame);

    // --- from the hooks (game recording threads)
    void InjectGBuffer(GCL* list, const frame::ListState& s, const frame::GBufferSeg& g);
    void InjectShadow(GCL* list, const frame::ListState& s, const frame::ShadowSeg& sh);
    // True if Mario was drawn in `frame` (so a capture now has a view to go with it).
    bool CanCapture(uint64_t frame) const { return !failed_.load() && snapshotFrame_.load() == frame; }
    // Records the copy of the linear depth target (and of the motion-vector
    // target, if `motion` is given with its current state); false if it didn't.
    bool Capture(GCL* list, const frame::ListState& s, ID3D12Resource* rt0, UINT subresource,
                 D3D12_RESOURCE_STATES state, ID3D12Resource* motion = nullptr, UINT motionSub = 0,
                 D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_COMMON);

    // --- from Present
    void EndFrame(uint64_t frame);
    bool PopResult(CaptureResult& out);

    struct Stats {
        uint64_t gbufferDraws = 0, shadowDraws = 0, captures = 0, results = 0, droppedResults = 0;
        uint64_t lastGBufferFrame = 0, lastResultFrame = 0;
        uint64_t motionCuts = 0; // frames drawn without motion vectors (a skipped frame, a jump)
        uint64_t prepared = 0;   // game frames Mario was prepared for
        uint64_t gpuBusy = 0;    // ... and not drawn in: the GPU still had his data of kSlots frames before
        uint64_t notReady = 0;   // the frame made for it wasn't published yet when the game recorded it
        uint64_t olderDrawn = 0; // an older one than wanted was drawn (the wanted one wasn't there)
        int psoVariants = 0;
        std::string status = "idle";
    };
    Stats GetStats();
    std::string Error() const;
    // Stops all injection for the session (after a prevented crash).
    void Disable(const char* why) { Fail(why); }
    // Puts back the game's state in `list` after a fault during an injection.
    void RestoreAfterFault(GCL* list, const frame::ListState& s);
    bool Failed() const { return failed_.load(); }

private:
    static constexpr int kSlots = 8;            // frames in flight: Mario's vertices (0.5: 4 - he skipped a
                                                // frame whenever the GPU was 2 frames behind with frame generation)
    static constexpr int kReadbackSlots = 4;    // read-backs in flight (the view, the depth image)
    static constexpr int kRing = 4;             // published Mario frames kept
    static constexpr UINT64 kSlotBytes = 512 * 1024;
    static constexpr UINT kCbBytes = 2048;      // one draw-constants variant (DrawConstants)
    static constexpr int kCbVariants = 4;       // G-buffer, G-buffer without the snapshot, shadow, shadow (orthographic)
    static constexpr UINT kSnapStride = 1024;   // bytes per snapshot slot
    static constexpr int kAlignEntries = 32;    // camera placements a frame's draw chooses from
    static constexpr int kMaxCaptureW = 640, kMaxCaptureH = 360;

    struct PsoKey {
        uint32_t kind = 0; // 0 G-buffer, 1 shadow (PS writes depth), 2 shadow (no PS)
        uint32_t numRt = 0;
        uint32_t rtFormats[8] = {};
        uint32_t dsvFormat = 0;
        uint32_t samples = 1, quality = 0;
        uint32_t depthFunc = 0;
        uint32_t stencil = 0; // bit 0 enable, 8-15 read mask, 16-23 write mask
        uint32_t frontOps = 0, backOps = 0;
        int32_t depthBias = 0;
        float biasClamp = 0, slopeBias = 0;
        uint32_t depthClip = 1;
        uint32_t rootCbv = 0;   // 1: b0 comes as a root CBV
        bool operator==(const PsoKey& o) const;
    };
    struct PsoEntry {
        PsoKey key;
        ID3D12PipelineState* pso = nullptr;
    };
    // Logs (once each) the depth / stencil tests of the game's G-buffer
    // pipelines that Mario's draws don't copy.
    void NoteGameTests(uint32_t depthFunc, uint32_t stencilFunc);
    uint32_t loggedDepthFuncs_ = 0, loggedStencilFuncs_ = 0; // bit per D3D12_COMPARISON_FUNC
    std::atomic<bool> reverseZ_{true}; // the depth direction the game's directional tests use
    std::atomic<uint32_t> lessSeen_{0}, greaterSeen_{0};
    struct Slot {
        uint64_t fence = 0;          // signalled when the GPU is done with this slot
        uint64_t snapFrame = 0;      // frame of the view read back here (0: none)
        uint64_t capFrame = 0;       // frame of the depth captured here (0: none)
        int capW = 0, capH = 0;
        bool capMotion = false;      // motion vectors were read back with the depth
        bool capMario = false;       // Mario's drawn bounds in that frame are known
        DVec3 capLo, capHi;
    };
    struct DrawnBounds {
        uint64_t frame = 0;
        DVec3 lo, hi;
        uint64_t motionFrame = 0; // the motion target was copied with this frame's depth
    };

    // What one frame's draws use (read under mu_, so a recording thread
    // never pairs one frame's buffer with the next frame's vertex count).
    struct FrameDraw {
        int slot = -1;
        D3D12_GPU_VIRTUAL_ADDRESS vb = 0, cbGBuffer = 0, cbGBufferNoSnapshot = 0, cbShadow = 0, cbShadowOrtho = 0;
        uint32_t verts = 0;
    };

    bool EnsureDevice(GCL* list);
    bool CreateObjects();
    ID3D12PipelineState* GetPso(const PsoKey& key);  // mu_ held
    // Uploads this frame's Mario (once per frame). False if he can't be drawn.
    bool PrepareFrame(uint64_t frame, FrameDraw& out);
    void RestoreStencilRef(GCL* list, const frame::ListState& s);
    void RestoreState(GCL* list, const frame::ListState& s, bool viewportChanged, bool heapsChanged,
                      bool targetsChanged);
    bool EnsureDepthCopy(const D3D12_RESOURCE_DESC& rd, UINT width, UINT height); // mu_ held
    bool EnsureMotionCopy(const D3D12_RESOURCE_DESC& rd, UINT width, UINT height); // mu_ held
    void CollectResults();                                                          // mu_ held
    void Fail(const std::string& why);
    void SetStatus(const char* why);

    MarioLook look_;
    MarioStencil stencil_;  // under mu_
    Mutex mu_;
    std::atomic<bool> failed_{false};
    mutable Mutex errorMu_;
    std::string error_; // under errorMu_
    ID3D12Device* device_ = nullptr; // AddRef'd
    std::atomic<bool> objectsReady_{false};

    // shaders
    std::vector<uint8_t> vsGBuffer_, psGBuffer_, vsShadow_, psShadow_, psShadowCut_, csDownsample_;
    // pipeline
    ID3D12RootSignature* rsTable_ = nullptr; // b0 via descriptor table
    ID3D12RootSignature* rsCbv_ = nullptr;   // b0 via root CBV
    ID3D12RootSignature* rsCompute_ = nullptr;
    ID3D12PipelineState* csPso_ = nullptr;
    std::vector<PsoEntry> psos_;
    // per-frame data
    ID3D12Resource* upload_ = nullptr;
    uint8_t* uploadPtr_ = nullptr;
    Slot slots_[kReadbackSlots];
    uint64_t uploadFence_[kSlots] = {}; // signalled once the game's frame (frame % kSlots) is done on the GPU
    uint64_t preparedFrame_ = 0;
    int preparedSlot_ = -1;
    FrameDraw prepared_;
    DVec3 preparedLo_, preparedHi_;      // Mario's bounds in the prepared frame
    DrawnBounds captureBounds_[kSlots];  // ... for each captured frame (by frame % kSlots)
    // Mario
    Mutex marioMu_;
    MarioFrame ring_[kRing];        // the newest published frames, at tag % kRing (under marioMu_)
    uint64_t newestTag_ = 0;        // under marioMu_
    uint64_t cutTag_ = 0;           // the newest frame published with a cut (under marioMu_)
    uint64_t lastDrawnTag_ = 0;     // under mu_
    std::atomic<int> drawLag_{0};
    struct DrawnTagRec {
        uint64_t frame = 0, tag = 0;
    };
    DrawnTagRec drawnTags_[32];     // game frame -> tag drawn (under mu_)
    std::vector<float> prevPos_;   // last uploaded positions (relative to prevAnchor_)
    DVec3 prevAnchor_;
    uint64_t prevFrame_ = 0;       // the frame prevPos_ was drawn in
    bool havePrev_ = false;
    // Drawn where his camera had him (SetPlacementSource): the source, on or
    // off, and the previous drawn frame's pivot and its shift when the view
    // matched no placement (for its motion vectors) - under mu_.
    std::atomic<PlacementSource> placementSource_{nullptr};
    std::atomic<bool> align_{true};
    bool prevAligned_ = false;
    DVec3 prevPivot_;
    DVec3 prevNewestShift_;
    std::vector<uint8_t> texPixels_; // under marioMu_
    int pendingTexW_ = 0, pendingTexH_ = 0;
    int texW_ = 0, texH_ = 0;        // under mu_
    ID3D12Resource* texture_ = nullptr;      // raw buffer, default heap
    ID3D12Resource* textureUpload_ = nullptr;
    ID3D12Resource* dummy_ = nullptr;        // stands in for the texture until it is ready
    std::atomic<bool> textureReady_{false};
    bool textureCopyPending_ = false;
    // snapshot / capture
    ID3D12Resource* snapshot_ = nullptr;     // kSlots * kSnapStride, UAV
    ID3D12Resource* captureSnap_ = nullptr;  // the view frozen next to the depth copy
    ID3D12Resource* depthCopy_ = nullptr;    // copy of the linear depth target (NPSR when idle)
    D3D12_RESOURCE_DESC depthCopyDesc_{};
    ID3D12Resource* motionCopy_ = nullptr;   // copy of the motion-vector target (NPSR when idle)
    D3D12_RESOURCE_DESC motionCopyDesc_{};
    bool motionBroken_ = false;              // its format wasn't usable: depth only
    UINT captureSrcW_ = 0, captureSrcH_ = 0;
    ID3D12Resource* captureOut_ = nullptr;   // reduced depth (UAV)
    ID3D12Resource* readback_[kReadbackSlots] = {};
    uint8_t* readbackPtr_[kReadbackSlots] = {};
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    UINT srvStride_ = 0;
    std::vector<std::pair<uint64_t, ID3D12Resource*>> graveyard_; // released after a few frames
    std::atomic<uint64_t> captureFrame_{0}, snapshotFrame_{0};
    uint64_t lastCapProcessed_ = 0, lastSnapProcessed_ = 0, lastCaptureResult_ = 0;
    int captureW_ = 480, captureH_ = 270;
    // our own command list (Present)
    ID3D12CommandAllocator* alloc_[kReadbackSlots] = {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    UINT64 fenceValue_ = 0;
    // results
    Mutex resultMu_;
    CaptureResult result_;
    bool haveResult_ = false;
    ViewConstants pendingView_;
    uint64_t pendingViewFrame_ = 0;
    uint32_t pendingAlign_ = CaptureResult::kAlignOff, pendingAlignCount_ = 0;
    Vec3 pendingAlignShift_;
    Mutex statsMu_;
    Stats stats_;
};

} // namespace sm2m

#endif
