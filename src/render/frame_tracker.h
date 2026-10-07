// D3D12 device / command list hooks: mirrors what the game records into each
// command list (frame::ListState), classifies pipeline states as they are
// created, and asks frame::Policy where Mario goes. The Injector records the
// actual commands. Also produces the "frame report" in sm2mario.log that
// describes how the game builds its frame (passes, formats, shadow maps,
// injection decisions).
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>

#include "d3d.h"
#include "frame_policy.h"
#include "stencil_census.h"

namespace sm2m {

class Injector;

namespace tracker {

struct Config {
    frame::ClassifyConfig classify = frame::DefaultClassifyConfig();
    frame::PolicyConfig policy;
};

// Hooks the device / list / pipeline-library entry points, taking their
// addresses from the given throwaway objects.
bool Install(ID3D12Device* dummyDevice, ID3D12GraphicsCommandList* dummyList, std::string& error);
bool Installed();
// Marks the calling thread as recording the mod's own commands: the hooks
// pass straight through.
class ScopedInternal {
public:
    ScopedInternal();
    ~ScopedInternal();
    ScopedInternal(const ScopedInternal&) = delete;
    ScopedInternal& operator=(const ScopedInternal&) = delete;
};
void Configure(const Config& cfg, Injector* injector);
// Injections are recorded only while active (Mario Mode on, renderer healthy).
void SetActive(bool on);
bool Active();
// Called at the start of every Present.
void BeginFrame(uint64_t frame);
uint64_t Frame();
// The stencil marks drawn into the main view (stencil_census.h): the mod
// closes each frame's count with whether Spider-Man was on screen.
StencilCensus& Census();
// Whether the draws' stencil writes are noted for the census (off: nothing to learn).
void SetCensusWanted(bool on);
// Notified by the ExecuteCommandLists hook.
void OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
// Describes the next `frames` frames in the log.
void RequestReport(int frames, const char* why);
// frame::PolicyConfig::steadyRegions, switched at run time.
void SetSteadyShadowRegions(bool on);
bool SteadyShadowRegions();
// Depth captures for Mario's collision (only the "world" source needs them).
void SetCapture(bool on);

// The queue that executed the last command lists Mario was recorded into
// (AddRef'd; null until there was one).
ID3D12CommandQueue* InjectionQueue();

struct Stats {
    uint64_t psos = 0, gbufferPsos = 0, casterPsos = 0, cacheCopyPsos = 0, cacheMovePsos = 0;
    uint64_t rootSigs = 0, rtvs = 0, dsvs = 0, lists = 0;
    uint64_t unknownPsoDraws = 0, unknownRtvBinds = 0;
    uint64_t gbufferSegments = 0, shadowRegions = 0;
    uint64_t gbufferInjections = 0, shadowInjections = 0, captures = 0;
    uint64_t lastGBufferFrame = 0, lastShadowFrame = 0, lastCaptureFrame = 0;
    bool markers = false;
    std::string lastSkip;
};
Stats GetStats();

} // namespace tracker
} // namespace sm2m

#endif
