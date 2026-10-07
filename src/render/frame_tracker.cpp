#ifdef _WIN32

#include "frame_tracker.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "../common/log.h"
#include "../common/platform.h"
#include "../win/guard.h"
#include "MinHook.h"
#include "injector.h"
#include "list_fns.h"

namespace sm2m {
namespace {

ListFns g_fns;

} // namespace

const ListFns& OriginalListFns() { return g_fns; }

namespace tracker {
namespace {

// ---------------------------------------------------------------------------
// Lock-free pointer-keyed tables (insert / overwrite, never erase). Lookups
// happen on every SetPipelineState, from many threads.

uint64_t HashKey(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return k;
}

template <typename V>
class PtrTable {
public:
    explicit PtrTable(size_t capacityPow2) : mask_(capacityPow2 - 1), slots_(new Slot[capacityPow2]) {}
    V* Find(uint64_t key) const {
        if (!key) return nullptr;
        size_t i = size_t(HashKey(key)) & mask_;
        for (size_t n = 0; n <= mask_; ++n, i = (i + 1) & mask_) {
            const uint64_t k = slots_[i].key.load(std::memory_order_acquire);
            if (k == key) return slots_[i].val.load(std::memory_order_acquire);
            if (k == 0) return nullptr;
        }
        return nullptr;
    }
    bool Put(uint64_t key, V* v) {
        if (!key) return false;
        size_t i = size_t(HashKey(key)) & mask_;
        for (size_t n = 0; n <= mask_; ++n, i = (i + 1) & mask_) {
            uint64_t k = slots_[i].key.load(std::memory_order_acquire);
            if (k == 0) {
                uint64_t expected = 0;
                if (slots_[i].key.compare_exchange_strong(expected, key)) {
                    slots_[i].val.store(v, std::memory_order_release);
                    return true;
                }
                k = expected;
            }
            if (k == key) {
                slots_[i].val.store(v, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

private:
    struct Slot {
        std::atomic<uint64_t> key{0};
        std::atomic<V*> val{nullptr};
    };
    size_t mask_;
    std::unique_ptr<Slot[]> slots_;
};

// RTV / DSV descriptor -> what it points at. Values live in the slot behind a
// sequence lock (descriptors are rewritten while other threads read them).
class DescTable {
public:
    explicit DescTable(size_t capacityPow2) : mask_(capacityPow2 - 1), slots_(new Slot[capacityPow2]) {}
    bool Find(uint64_t key, frame::ViewInfo& out) const {
        if (!key) return false;
        size_t i = size_t(HashKey(key)) & mask_;
        for (size_t n = 0; n <= mask_; ++n, i = (i + 1) & mask_) {
            const uint64_t k = slots_[i].key.load(std::memory_order_acquire);
            if (k == 0) return false;
            if (k != key) continue;
            for (int tries = 0; tries < 64; ++tries) {
                const uint32_t s1 = slots_[i].seq.load(std::memory_order_acquire);
                if (s1 & 1) continue;
                std::memcpy(&out, &slots_[i].v, sizeof(out));
                std::atomic_thread_fence(std::memory_order_acquire);
                if (slots_[i].seq.load(std::memory_order_relaxed) == s1) return out.valid;
            }
            return false;
        }
        return false;
    }
    bool Put(uint64_t key, const frame::ViewInfo& v) {
        if (!key) return false;
        size_t i = size_t(HashKey(key)) & mask_;
        for (size_t n = 0; n <= mask_; ++n, i = (i + 1) & mask_) {
            uint64_t k = slots_[i].key.load(std::memory_order_acquire);
            if (k == 0) {
                uint64_t expected = 0;
                if (slots_[i].key.compare_exchange_strong(expected, key)) k = key;
                else k = expected;
            }
            if (k != key) continue;
            // Writers of the same descriptor are serialised by the spin below.
            uint32_t s = slots_[i].seq.load(std::memory_order_relaxed);
            for (;;) {
                if (!(s & 1) && slots_[i].seq.compare_exchange_weak(s, s + 1, std::memory_order_acquire)) break;
                s = slots_[i].seq.load(std::memory_order_relaxed);
            }
            std::memcpy(&slots_[i].v, &v, sizeof(v));
            slots_[i].seq.store(s + 2, std::memory_order_release);
            return true;
        }
        return false;
    }

private:
    struct Slot {
        std::atomic<uint64_t> key{0};
        std::atomic<uint32_t> seq{0};
        frame::ViewInfo v;
    };
    size_t mask_;
    std::unique_ptr<Slot[]> slots_;
};

// ---------------------------------------------------------------------------
// Globals

PtrTable<frame::PsoClass>* g_psos = nullptr;
PtrTable<frame::RootSigLayout>* g_rootSigs = nullptr;
PtrTable<frame::ListState>* g_lists = nullptr;
DescTable* g_descs = nullptr;

frame::Registry g_reg;
frame::Policy* g_policy = nullptr;
frame::ClassifyConfig g_classify = frame::DefaultClassifyConfig();
Mutex g_cfgMutex;
Injector* g_injector = nullptr;
std::atomic<bool> g_active{false};
std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_descStride{0}, g_rtvStride{0}, g_dsvStride{0};
std::atomic<uint32_t> g_psoSerial{0}, g_listSerial{0};
std::atomic<int> g_tableFull{0};

std::atomic<bool> g_broken{false}; // a fault in our hook code: everything passes straight through

thread_local int t_internal = 0;
inline bool Track() { return !t_internal && !g_broken.load(std::memory_order_relaxed); }
thread_local GCL* t_list = nullptr;
thread_local frame::ListState* t_state = nullptr;
thread_local std::vector<frame::Decision>* t_decisions = nullptr;
thread_local std::vector<frame::Skip>* t_skips = nullptr;

struct AtomicStats {
    std::atomic<uint64_t> psos{0}, gbufferPsos{0}, casterPsos{0}, cacheCopyPsos{0}, cacheMovePsos{0};
    std::atomic<uint64_t> rootSigs{0}, rtvs{0}, dsvs{0}, lists{0};
    std::atomic<uint64_t> unknownPsoDraws{0}, unknownRtvBinds{0};
    std::atomic<uint64_t> gbufferSegments{0}, shadowRegions{0};
    std::atomic<uint64_t> gbufferInjections{0}, shadowInjections{0}, captures{0};
    std::atomic<uint64_t> lastGBufferFrame{0}, lastShadowFrame{0}, lastCaptureFrame{0};
    std::atomic<bool> markers{false};
} g_stats;
Mutex g_skipMutex;
std::string g_lastSkip;

// Runs our part of a hook under the crash guard. A fault turns the in-world
// renderer off for the session instead of taking the game down.
template <typename F>
void Guarded(const char* what, F&& body) {
    if (g_broken.load(std::memory_order_relaxed)) return;
    guard::Fault fault;
    auto fn = [&] { body(); };
    bool ok;
    {
        guard::PhaseScope phase(what);
        ok = guard::Call(fn, &fault);
    }
    if (!ok && !g_broken.exchange(true)) {
        g_active.store(false);
        if (g_injector) g_injector->Disable("crash prevented in the frame hooks");
        LOGE("CRASH PREVENTED in the frame hooks (%s): %s - Mario is drawn as an overlay from now on",
             what, guard::Describe(fault).c_str());
    }
}

// ---------------------------------------------------------------------------
// Frame report

struct Report {
    Mutex mu;
    std::atomic<int> framesLeft{0};
    std::vector<std::string> lines;
    size_t dropped = 0;
    std::string why;
    // Depth-only pipelines that drew but aren't known shadow casters: their
    // shader hashes, for bindings.user.ini [Shaders] after a game update.
    std::vector<std::pair<std::string, int>> depthOnly;
} g_report;

bool Reporting() { return g_report.framesLeft.load(std::memory_order_relaxed) > 0; }

#if defined(__MINGW32__)
__attribute__((format(gnu_printf, 1, 2)))
#endif
void ReportLine(const char* fmt, ...);
void ReportLine(const char* fmt, ...) {
    if (!Reporting()) return;
    char buf[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    LockGuard lock(g_report.mu);
    if (g_report.lines.size() >= 900) {
        ++g_report.dropped;
        return;
    }
    g_report.lines.emplace_back(buf);
}

const char* ShortFormat(uint32_t f) {
    switch (f) {
    case d3d12p::kFmtR32Float: return "R32F";
    case d3d12p::kFmtR16G16Float: return "RG16F";
    case d3d12p::kFmtR32G32Uint: return "RG32U";
    case d3d12p::kFmtD32FloatS8X24Uint: return "D32S8";
    case d3d12p::kFmtD32Float: return "D32";
    case d3d12p::kFmtD16Unorm: return "D16";
    case d3d12p::kFmtD24UnormS8Uint: return "D24S8";
    case d3d12p::kFmtR16G16B16A16Float: return "RGBA16F";
    case d3d12p::kFmtR11G11B10Float: return "R11G11B10F";
    case d3d12p::kFmtR8G8B8A8Unorm: return "RGBA8";
    case d3d12p::kFmtR10G10B10A2Unorm: return "RGB10A2";
    case d3d12p::kFmtR32Uint: return "R32U";
    default: return d3d12p::FormatName(f);
    }
}

uint32_t ShortId(uint64_t p) { return uint32_t((p >> 4) ^ (p >> 24)) & 0xFFFFFu; }

void TraceFlushBinding(frame::ListState& s) {
    if (!Reporting() || s.trace.empty()) return;
    if (s.traceDraws || s.traceKinds) {
        char k[96];
        std::snprintf(k, sizeof(k), " draws=%u%s%s%s%s]", s.traceDraws, (s.traceKinds & 1) ? " gbuffer" : "",
                      (s.traceKinds & 2) ? " caster" : "", (s.traceKinds & 4) ? " cache-copy" : "",
                      (s.traceKinds & 8) ? " cache-move" : "");
        s.trace += k;
    } else {
        // Nothing drawn into the previous binding: drop it.
        const size_t open = s.trace.rfind('[');
        if (open != std::string::npos) s.trace.resize(open);
    }
    s.traceDraws = 0;
    s.traceKinds = 0;
}

void TraceBinding(frame::ListState& s) {
    if (!Reporting()) return;
    TraceFlushBinding(s);
    if (s.trace.size() > 1400) return;
    std::string b = "[";
    for (uint32_t i = 0; i < s.numRt && i < 8; ++i) {
        if (i) b += ",";
        b += s.rt[i].valid ? ShortFormat(s.rt[i].format) : "?";
    }
    b += "|";
    if (s.ds.valid) b += ShortFormat(s.ds.format);
    else if (s.dsv) b += "?";
    char size[48] = "";
    const frame::ViewInfo& v = s.numRt && s.rt[0].valid ? s.rt[0] : s.ds;
    if (v.valid) std::snprintf(size, sizeof(size), " %ux%u#%05x", v.MipWidth(), v.MipHeight(), ShortId(v.resource));
    b += size;
    s.trace += b;
}

void NoteSkip(const frame::ListState& s, frame::Boundary b, const char* what, const frame::Skip& k) {
    {
        LockGuard lock(g_skipMutex);
        g_lastSkip = std::string(what) + ": " + k.why;
    }
    if (k.shadow)
        ReportLine("  L%u shadow region at %s: %s %ux%u#%05x vp(%.0f,%.0f %.0fx%.0f) %d casters: skipped (%s)", s.serial,
                   frame::BoundaryName(b), ShortFormat(k.ds.format), k.ds.MipWidth(), k.ds.MipHeight(),
                   ShortId(k.ds.resource), k.vp.x, k.vp.y, k.vp.w, k.vp.h, k.casters, k.why);
    else
        ReportLine("  L%u %s at %s: skipped (%s)", s.serial, what, frame::BoundaryName(b), k.why);
}

// ---------------------------------------------------------------------------
// State lookup

frame::ListState* State(GCL* l, bool create = false) {
    if (t_list == l && t_state) return t_state;
    frame::ListState* s = g_lists->Find(uint64_t(uintptr_t(l)));
    if (!s && create) {
        s = new frame::ListState();
        s->list = uint64_t(uintptr_t(l));
        s->serial = ++g_listSerial;
        if (!g_lists->Put(s->list, s)) {
            delete s;
            return nullptr;
        }
        ++g_stats.lists;
    }
    if (s) {
        t_list = l;
        t_state = s;
    }
    return s;
}

std::vector<frame::Decision>& Decisions() {
    if (!t_decisions) t_decisions = new std::vector<frame::Decision>();
    t_decisions->clear();
    return *t_decisions;
}
std::vector<frame::Skip>& Skips() {
    if (!t_skips) t_skips = new std::vector<frame::Skip>();
    t_skips->clear();
    return *t_skips;
}

void EnsureStrides(ID3D12Device* dev) {
    if (g_descStride.load(std::memory_order_relaxed) || !dev) return;
    g_rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dsvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    g_descStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void EnsureStridesFromList(GCL* l) {
    if (g_descStride.load(std::memory_order_relaxed)) return;
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(l->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
        EnsureStrides(dev);
        dev->Release();
    }
}

bool ResolveView(uint64_t handle, frame::ViewInfo& v) {
    if (!handle) return false;
    if (g_descs->Find(handle, v)) return true;
    ++g_stats.unknownRtvBinds;
    return false;
}

// Runs the policy at a boundary and records what it decides.
void AtBoundaryImpl(GCL* l, frame::ListState* s, frame::Boundary b) {
    if (!s || !g_policy) return;
    if (!s->gseg.active && !s->sseg.active) return;
    const bool gActive = s->gseg.active, sActive = s->sseg.active;
    const int gDraws = s->gseg.draws, sDraws = s->sseg.casterDraws;
    auto& out = Decisions();
    auto& skips = Skips();
    g_policy->AtBoundary(*s, b, out, &skips);
    if (gActive && gDraws > 0) ++g_stats.gbufferSegments;
    if (sActive && sDraws > 0) ++g_stats.shadowRegions;
    for (const frame::Skip& k : skips) NoteSkip(*s, b, "segment", k);
    if (out.empty()) return;
    const bool active = g_active.load(std::memory_order_acquire) && g_injector;
    for (const frame::Decision& d : out) {
        if (d.act == frame::Act::GBuffer) {
            if (Reporting())
                ReportLine("  L%u %s: G-buffer segment of %d draws into %ux%u#%05x (b0 %s%s) -> %s", s->serial,
                           frame::BoundaryName(b), d.g.draws, d.g.rt[0].MipWidth(), d.g.rt[0].MipHeight(),
                           ShortId(d.g.rt[0].resource), d.g.viewTable ? "table" : "root CBV",
                           d.g.markerMatch ? ", in marker" : "", active ? "MARIO" : "(Mario Mode off)");
            if (!active) continue;
            ++t_internal;
            g_injector->InjectGBuffer(l, *s, d.g);
            --t_internal;
            ++s->injections;
            ++g_stats.gbufferInjections;
            g_stats.lastGBufferFrame = s->frame;
        } else {
            if (Reporting())
                ReportLine("  L%u %s: shadow region %s %ux%u#%05x vp(%.0f,%.0f %.0fx%.0f) %d casters, %s -> %s", s->serial,
                           frame::BoundaryName(b), ShortFormat(d.s.ds.format), d.s.ds.MipWidth(), d.s.ds.MipHeight(),
                           ShortId(d.s.ds.resource), d.s.vp.x, d.s.vp.y, d.s.vp.w, d.s.vp.h, d.s.casterDraws,
                           d.s.byCopy ? "cache copied" : "cleared", active ? "MARIO SHADOW" : "(Mario Mode off)");
            if (!active) continue;
            ++t_internal;
            g_injector->InjectShadow(l, *s, d.s);
            --t_internal;
            ++s->injections;
            ++g_stats.shadowInjections;
            g_stats.lastShadowFrame = s->frame;
        }
    }
}

void AtBoundary(GCL* l, frame::ListState* s, frame::Boundary b) {
    if (!s || (!s->gseg.active && !s->sseg.active)) return;
    const bool wasBroken = g_broken.load(std::memory_order_relaxed);
    Guarded("frame boundary", [&] { AtBoundaryImpl(l, s, b); });
    if (!wasBroken && g_broken.load() && g_injector) {
        // A fault in the middle of an injection may have left Mario's root
        // signature or pipeline bound: put the game's state back.
        guard::Fault f;
        auto restore = [&] { g_injector->RestoreAfterFault(l, *s); };
        guard::Call(restore, &f);
    }
}

// ---------------------------------------------------------------------------
// Device hooks

using CreateGpsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID,
                                                 void**);
using CreatePsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, REFIID, void**);
using CreateRsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using CreateRtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateDsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescsFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                             UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                             D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescsSimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using LoadGpsoFn = HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID,
                                               void**);
using LoadPsoFn = HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR, const void*, REFIID, void**);

using CreateListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                                 ID3D12PipelineState*, REFIID, void**);
CreateListFn o_CreateCommandList = nullptr;
CreateGpsoFn o_CreateGraphicsPipelineState = nullptr;
CreatePsoFn o_CreatePipelineState = nullptr;
CreateRsFn o_CreateRootSignature = nullptr;
CreateRtvFn o_CreateRenderTargetView = nullptr;
CreateDsvFn o_CreateDepthStencilView = nullptr;
CopyDescsFn o_CopyDescriptors = nullptr;
CopyDescsSimpleFn o_CopyDescriptorsSimple = nullptr;
LoadGpsoFn o_LoadGraphicsPipeline = nullptr;
LoadPsoFn o_LoadPipeline = nullptr;

void RegisterPso(void* pso, const d3d12p::PsoInfo& info) {
    if (!pso) return;
    auto* pc = new frame::PsoClass();
    pc->info = info;
    {
        LockGuard lock(g_cfgMutex);
        pc->kind = frame::Classify(info, g_classify);
    }
    pc->id = ++g_psoSerial;
    if (!g_psos->Put(uint64_t(uintptr_t(pso)), pc)) {
        delete pc;
        if (g_tableFull.fetch_add(1) == 0) LOGW("frame: pipeline table full - later pipelines are not classified");
        return;
    }
    ++g_stats.psos;
    switch (pc->kind) {
    case frame::PsoKind::GBuffer:
        if (g_stats.gbufferPsos++ == 0)
            LOGI("frame: first G-buffer pipeline (#%u): %u targets %s %s %s %s | %s, depth %s func %u, stencil %d "
                 "(r%02x w%02x pass %u)",
                 pc->id, info.numRenderTargets, ShortFormat(info.rtvFormats[0]), ShortFormat(info.rtvFormats[1]),
                 ShortFormat(info.rtvFormats[2]), ShortFormat(info.rtvFormats[3]), ShortFormat(info.dsvFormat),
                 info.depthWrite ? "write" : "read", info.depthFunc, int(info.stencilEnable), info.stencilReadMask,
                 info.stencilWriteMask, info.front.pass);
        break;
    case frame::PsoKind::ShadowCaster:
        if (g_stats.casterPsos++ == 0)
            LOGI("frame: first shadow caster pipeline (#%u): %s func %u bias %d slope %.2f cull %u clip %d", pc->id,
                 ShortFormat(info.dsvFormat), info.depthFunc, info.depthBias, info.slopeScaledDepthBias, info.cullMode,
                 int(info.depthClipEnable));
        break;
    case frame::PsoKind::CacheCopy:
        if (g_stats.cacheCopyPsos++ == 0) LOGI("frame: shadow cache copy pipeline found (#%u)", pc->id);
        break;
    case frame::PsoKind::CacheMove:
        if (g_stats.cacheMovePsos++ == 0) LOGI("frame: shadow cache move pipeline found (#%u)", pc->id);
        break;
    default: break;
    }
}

// A new list starts out recording (no Reset). It may sit at the address of a
// list that was released, whose state must not carry over.
HRESULT STDMETHODCALLTYPE H_CreateCommandList(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                              ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid,
                                              void** pp) {
    const HRESULT hr = o_CreateCommandList(dev, mask, type, alloc, pso, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && Track()) {
        Guarded("list tracking", [&] {
            if (frame::ListState* s = State(static_cast<GCL*>(*pp), true)) {
                s->Reset(g_reg.Frame());
                s->pso = uint64_t(uintptr_t(pso));
                s->psoClass = pso ? g_psos->Find(s->pso) : nullptr;
            }
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_CreateGraphicsPipelineState(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,
                                                        REFIID riid, void** pp) {
    const HRESULT hr = o_CreateGraphicsPipelineState(dev, d, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && d && Track()) {
        Guarded("pipeline tracking", [&] {
            EnsureStrides(dev);
            d3d12p::PsoInfo info;
            if (d3d12p::ParseGraphicsPsoDesc(d, info)) RegisterPso(*pp, info);
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_CreatePipelineState(ID3D12Device* dev, const void* desc, REFIID riid, void** pp) {
    const HRESULT hr = o_CreatePipelineState(dev, desc, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && desc && Track()) {
        Guarded("pipeline tracking", [&] {
            EnsureStrides(dev);
            // D3D12_PIPELINE_STATE_STREAM_DESC { SIZE_T SizeInBytes; void* pPipelineStateSubobjectStream; }
            size_t size = 0;
            const void* stream = nullptr;
            std::memcpy(&size, desc, 8);
            std::memcpy(&stream, static_cast<const uint8_t*>(desc) + 8, 8);
            d3d12p::PsoInfo info;
            if (stream && size && d3d12p::ParsePipelineStream(stream, size, info) && info.graphics) RegisterPso(*pp, info);
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_LoadGraphicsPipeline(void* lib, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,
                                                 REFIID riid, void** pp) {
    const HRESULT hr = o_LoadGraphicsPipeline(lib, name, d, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && d && Track()) {
        Guarded("pipeline tracking", [&] {
            d3d12p::PsoInfo info;
            if (d3d12p::ParseGraphicsPsoDesc(d, info)) RegisterPso(*pp, info);
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_LoadPipeline(void* lib, LPCWSTR name, const void* desc, REFIID riid, void** pp) {
    const HRESULT hr = o_LoadPipeline(lib, name, desc, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && desc && Track()) {
        Guarded("pipeline tracking", [&] {
            size_t size = 0;
            const void* stream = nullptr;
            std::memcpy(&size, desc, 8);
            std::memcpy(&stream, static_cast<const uint8_t*>(desc) + 8, 8);
            d3d12p::PsoInfo info;
            if (stream && size && d3d12p::ParsePipelineStream(stream, size, info) && info.graphics)
                RegisterPso(*pp, info);
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE H_CreateRootSignature(ID3D12Device* dev, UINT mask, const void* blob, SIZE_T len,
                                                REFIID riid, void** pp) {
    const HRESULT hr = o_CreateRootSignature(dev, mask, blob, len, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && blob && Track()) Guarded("root signature tracking", [&] {
        auto* l = new frame::RootSigLayout();
        std::string why;
        if (d3d12p::ParseRootSignature(blob, size_t(len), l->info, &why)) {
            frame::BuildLayout(*l);
            if (g_rootSigs->Put(uint64_t(uintptr_t(*pp)), l)) {
                if (g_stats.rootSigs++ < 8 || Reporting())
                    LOGI("frame: root signature %05x: %zu params, flags %#x, view b0 %s param %d +%u", ShortId(uintptr_t(*pp)),
                         l->info.params.size(), l->info.flags,
                         l->viewVs.param >= 0 ? "table" : (l->viewRootCbv >= 0 ? "root CBV" : "not found"),
                         l->viewVs.param >= 0 ? l->viewVs.param : l->viewRootCbv, l->viewVs.offset);
                return;
            }
        } else {
            LOGW("frame: couldn't parse a root signature (%s)", why.c_str());
        }
        delete l;
    });
    return hr;
}

frame::ViewInfo DescribeResource(ID3D12Resource* res) {
    frame::ViewInfo v;
    if (!res) return v;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return v;
    v.resource = uint64_t(uintptr_t(res));
    v.resFormat = uint32_t(rd.Format);
    v.format = v.resFormat;
    v.width = uint32_t(rd.Width);
    v.height = rd.Height;
    v.mipLevels = rd.MipLevels ? rd.MipLevels : 1;
    v.arraySize = rd.DepthOrArraySize ? rd.DepthOrArraySize : 1;
    v.numSlices = v.arraySize;
    v.valid = rd.SampleDesc.Count <= 1;
    return v;
}

void STDMETHODCALLTYPE H_CreateRenderTargetView(ID3D12Device* dev, ID3D12Resource* res,
                                                const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateRenderTargetView(dev, res, d, h);
    if (!Track()) return;
    Guarded("view tracking", [&] {
    EnsureStrides(dev);
    frame::ViewInfo v = DescribeResource(res);
    if (d && v.valid) {
        if (d->Format != DXGI_FORMAT_UNKNOWN) v.format = uint32_t(d->Format);
        v.dimension = uint32_t(d->ViewDimension);
        if (d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
            v.mip = d->Texture2D.MipSlice;
            v.firstSlice = 0;
            v.numSlices = 1;
        } else if (d->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
            v.mip = d->Texture2DArray.MipSlice;
            v.firstSlice = d->Texture2DArray.FirstArraySlice;
            v.numSlices = d->Texture2DArray.ArraySize;
        } else {
            v.valid = false;
        }
    }
    if (g_descs->Put(uint64_t(h.ptr), v)) ++g_stats.rtvs;
    });
}

void STDMETHODCALLTYPE H_CreateDepthStencilView(ID3D12Device* dev, ID3D12Resource* res,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateDepthStencilView(dev, res, d, h);
    if (!Track()) return;
    Guarded("view tracking", [&] {
    EnsureStrides(dev);
    frame::ViewInfo v = DescribeResource(res);
    if (d && v.valid) {
        if (d->Format != DXGI_FORMAT_UNKNOWN) v.format = uint32_t(d->Format);
        v.dimension = uint32_t(d->ViewDimension);
        v.dsvFlags = uint32_t(d->Flags);
        if (d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) {
            v.mip = d->Texture2D.MipSlice;
            v.firstSlice = 0;
            v.numSlices = 1;
        } else if (d->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) {
            v.mip = d->Texture2DArray.MipSlice;
            v.firstSlice = d->Texture2DArray.FirstArraySlice;
            v.numSlices = d->Texture2DArray.ArraySize;
        } else {
            v.valid = false;
        }
    }
    if (g_descs->Put(uint64_t(h.ptr), v)) ++g_stats.dsvs;
    });
}

void CopyDescInfo(uint64_t dst, uint64_t src) {
    frame::ViewInfo v;
    if (!g_descs->Find(src, v)) v = frame::ViewInfo();
    g_descs->Put(dst, v);
}

void STDMETHODCALLTYPE H_CopyDescriptorsSimple(ID3D12Device* dev, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                               D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    o_CopyDescriptorsSimple(dev, n, dst, src, type);
    if (!Track() || (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) return;
    const uint32_t stride = dev->GetDescriptorHandleIncrementSize(type);
    for (UINT i = 0; i < n; ++i) CopyDescInfo(dst.ptr + uint64_t(i) * stride, src.ptr + uint64_t(i) * stride);
}

void STDMETHODCALLTYPE H_CopyDescriptors(ID3D12Device* dev, UINT numDst, const D3D12_CPU_DESCRIPTOR_HANDLE* dstStarts,
                                         const UINT* dstSizes, UINT numSrc, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts,
                                         const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    o_CopyDescriptors(dev, numDst, dstStarts, dstSizes, numSrc, srcStarts, srcSizes, type);
    if (!Track() || (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) return;
    if (!dstStarts || !srcStarts) return;
    const uint32_t stride = dev->GetDescriptorHandleIncrementSize(type);
    UINT di = 0, si = 0, dOff = 0, sOff = 0;
    while (di < numDst && si < numSrc) {
        const UINT dSize = dstSizes ? dstSizes[di] : 1, sSize = srcSizes ? srcSizes[si] : 1;
        if (dOff >= dSize) {
            ++di;
            dOff = 0;
            continue;
        }
        if (sOff >= sSize) {
            ++si;
            sOff = 0;
            continue;
        }
        CopyDescInfo(dstStarts[di].ptr + uint64_t(dOff) * stride, srcStarts[si].ptr + uint64_t(sOff) * stride);
        ++dOff;
        ++sOff;
    }
}

// ---------------------------------------------------------------------------
// Command list hooks

void FlushStencil(frame::ListState& s); // (the stencil census, below)

HRESULT STDMETHODCALLTYPE H_Reset(GCL* l, ID3D12CommandAllocator* a, ID3D12PipelineState* pso) {
    const HRESULT hr = g_fns.Reset(l, a, pso);
    if (Track() && SUCCEEDED(hr)) {
        if (frame::ListState* s = State(l, true)) {
            s->Reset(g_reg.Frame());
            s->pso = uint64_t(uintptr_t(pso));
            s->psoClass = pso ? g_psos->Find(s->pso) : nullptr;
        }
    }
    return hr;
}

void ReportListClose(frame::ListState& s) {
    if (!Reporting()) return;
    TraceFlushBinding(s);
    if (s.draws == 0 && s.injections == 0 && s.trace.empty()) return;
    ReportLine("L%u frame %llu: %u draws (G-buffer %u, casters %u, cache copies %u, region clears %u), %u injections%s%s",
               s.serial, static_cast<unsigned long long>(s.frame), s.draws, s.gbufferDraws, s.casterDraws, s.copyDraws,
               s.refreshDraws, s.injections, s.trace.empty() ? "" : " ", s.trace.c_str());
}

HRESULT STDMETHODCALLTYPE H_Close(GCL* l) {
    if (Track()) {
        if (frame::ListState* s = State(l)) {
            AtBoundary(l, s, frame::Boundary::Close);
            if (Reporting()) Guarded("frame report", [&] { ReportListClose(*s); });
            if (s->numStencilKeys) Guarded("stencil census", [&] { FlushStencil(*s); });
            s->trace.clear();
            if (s->unknownPsoDraws) {
                g_stats.unknownPsoDraws.fetch_add(s->unknownPsoDraws, std::memory_order_relaxed);
                s->unknownPsoDraws = 0;
            }
        }
    }
    return g_fns.Close(l);
}

void STDMETHODCALLTYPE H_ClearState(GCL* l, ID3D12PipelineState* pso) {
    if (Track()) {
        if (frame::ListState* s = State(l)) AtBoundary(l, s, frame::Boundary::Other);
    }
    g_fns.ClearState(l, pso);
    if (Track()) {
        if (frame::ListState* s = State(l)) {
            const uint64_t f = s->frame;
            if (s->unknownPsoDraws) g_stats.unknownPsoDraws.fetch_add(s->unknownPsoDraws, std::memory_order_relaxed);
            if (s->numStencilKeys) Guarded("stencil census", [&] { FlushStencil(*s); });
            std::vector<frame::Prepared> keep = s->prepared;
            std::string trace = s->trace;
            s->Reset(f);
            s->prepared = keep;
            s->trace = trace;
            s->pso = uint64_t(uintptr_t(pso));
            s->psoClass = pso ? g_psos->Find(s->pso) : nullptr;
        }
    }
}

StencilCensus g_census;
std::atomic<bool> g_censusWanted{false};
std::atomic<uint64_t> g_mainDsv{0}; // the depth buffer of the main view's G-buffer

// A draw into the main view that writes the stencil: noted in its list for
// the census (only while the mod is still learning Spider-Man's mark).
void NoteStencil(frame::ListState& s) {
    if (!g_censusWanted.load(std::memory_order_relaxed)) return;
    const d3d12p::PsoInfo& pi = s.psoClass->info;
    if (!pi.graphics || !s.ds.valid || !s.ds.resource) return;
    const bool gbuffer = s.psoClass->kind == frame::PsoKind::GBuffer;
    if (gbuffer && s.numRt && s.rt[0].valid && g_reg.IsMainDepthTarget(s.rt[0].resource, s.rt[0].Subresource()))
        g_mainDsv.store(s.ds.resource, std::memory_order_relaxed);
    if (!pi.stencilEnable || !pi.stencilWriteMask || (s.ds.dsvFlags & 2)) return; // (2: read-only stencil)
    if (s.ds.resource != g_mainDsv.load(std::memory_order_relaxed)) return;
    if (pi.front.pass == 1 /* KEEP */) return;
    // (in one of the passes Mario is drawn in: what is drawn with him)
    const bool marioPass = gbuffer && s.gseg.active && s.gseg.markerMatch;
    const uint32_t key =
        StencilCensus::Key(uint8_t(s.stencilRef), pi.stencilWriteMask, uint8_t(pi.front.pass), gbuffer, marioPass);
    for (uint32_t i = 0; i < s.numStencilKeys; ++i)
        if (s.stencilKeys[i] == key) {
            ++s.stencilDraws[i];
            return;
        }
    if (s.numStencilKeys < 16) {
        s.stencilKeys[s.numStencilKeys] = key;
        s.stencilDraws[s.numStencilKeys] = 1;
        ++s.numStencilKeys;
    }
}

// The list's stencil writes, to the census (one lock per list, not per draw).
void FlushStencil(frame::ListState& s) {
    if (!s.numStencilKeys) return;
    const int n = int(s.numStencilKeys);
    s.numStencilKeys = 0;
    if (g_censusWanted.load(std::memory_order_relaxed)) g_census.NoteList(s.stencilKeys, s.stencilDraws, n);
}

void AfterDraw(GCL* l) {
    frame::ListState* s = State(l);
    if (!s) return;
    // A draw needs a graphics pipeline: one we don't know was created through
    // a path that isn't hooked (counted per list, summed at Close).
    if (!s->psoClass) ++s->unknownPsoDraws;
    if (!s->psoClass && !Reporting()) {
        ++s->draws;
        return;
    }
    Guarded("draw tracking", [&] {
    EnsureStridesFromList(l);
    g_policy->OnDraw(*s, g_descStride.load(std::memory_order_relaxed));
    if (s->psoClass) NoteStencil(*s);
    if (Reporting()) {
        ++s->traceDraws;
        if (s->psoClass && s->psoClass->kind == frame::PsoKind::Other) {
            const d3d12p::PsoInfo& pi = s->psoClass->info;
            if (pi.graphics && pi.numRenderTargets == 0 && pi.dsvFormat && pi.depthWrite) {
                char extra[160];
                std::snprintf(extra, sizeof(extra), " (depth func %u, cull %u, bias %d) into %s %ux%u#%05x",
                              pi.depthFunc, pi.cullMode, pi.depthBias, ShortFormat(s->ds.format), s->ds.MipWidth(),
                              s->ds.MipHeight(), ShortId(s->ds.resource));
                const std::string key = "VS " + d3d12p::DigestHex(pi.vs.digest) + " PS " +
                                        (pi.ps.present ? d3d12p::DigestHex(pi.ps.digest) : std::string("none")) + extra;
                LockGuard lock(g_report.mu);
                bool found = false;
                for (auto& e : g_report.depthOnly)
                    if (e.first == key) {
                        ++e.second;
                        found = true;
                        break;
                    }
                if (!found && g_report.depthOnly.size() < 16) g_report.depthOnly.push_back({key, 1});
            }
        }
        if (s->psoClass) {
            switch (s->psoClass->kind) {
            case frame::PsoKind::GBuffer: s->traceKinds |= 1; break;
            case frame::PsoKind::ShadowCaster: s->traceKinds |= 2; break;
            case frame::PsoKind::CacheCopy: s->traceKinds |= 4; break;
            case frame::PsoKind::CacheMove: s->traceKinds |= 8; break;
            default: break;
            }
        }
    }
    });
}

void STDMETHODCALLTYPE H_DrawInstanced(GCL* l, UINT a, UINT b, UINT c, UINT d) {
    g_fns.DrawInstanced(l, a, b, c, d);
    if (Track()) AfterDraw(l);
}

void STDMETHODCALLTYPE H_DrawIndexedInstanced(GCL* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    g_fns.DrawIndexedInstanced(l, a, b, c, d, e);
    if (Track()) AfterDraw(l);
}

void STDMETHODCALLTYPE H_ExecuteIndirect(GCL* l, ID3D12CommandSignature* sig, UINT n, ID3D12Resource* args,
                                         UINT64 argOff, ID3D12Resource* count, UINT64 countOff) {
    g_fns.ExecuteIndirect(l, sig, n, args, argOff, count, countOff);
    // Indirect dispatches go through here too; only count it as a draw when
    // a graphics pipeline with render targets / depth is bound.
    if (Track()) {
        frame::ListState* s = State(l);
        if (s && s->psoClass && s->psoClass->info.graphics) AfterDraw(l);
    }
}

void STDMETHODCALLTYPE H_IASetPrimitiveTopology(GCL* l, D3D12_PRIMITIVE_TOPOLOGY t) {
    g_fns.IASetPrimitiveTopology(l, t);
    if (Track())
        if (frame::ListState* s = State(l)) s->topology = uint32_t(t);
}

void STDMETHODCALLTYPE H_RSSetViewports(GCL* l, UINT n, const D3D12_VIEWPORT* v) {
    frame::ListState* s = Track() ? State(l) : nullptr;
    if (s) AtBoundary(l, s, frame::Boundary::Viewports);
    g_fns.RSSetViewports(l, n, v);
    if (s && v) {
        s->numVp = std::min<UINT>(n, 16);
        for (UINT i = 0; i < s->numVp; ++i) {
            s->vp[i].x = v[i].TopLeftX;
            s->vp[i].y = v[i].TopLeftY;
            s->vp[i].w = v[i].Width;
            s->vp[i].h = v[i].Height;
            s->vp[i].minZ = v[i].MinDepth;
            s->vp[i].maxZ = v[i].MaxDepth;
        }
    }
}

void STDMETHODCALLTYPE H_RSSetScissorRects(GCL* l, UINT n, const D3D12_RECT* r) {
    g_fns.RSSetScissorRects(l, n, r);
    if (!Track() || !r) return;
    if (frame::ListState* s = State(l)) {
        s->numScissor = std::min<UINT>(n, 16);
        for (UINT i = 0; i < s->numScissor; ++i) {
            s->scissor[i].left = int32_t(r[i].left);
            s->scissor[i].top = int32_t(r[i].top);
            s->scissor[i].right = int32_t(r[i].right);
            s->scissor[i].bottom = int32_t(r[i].bottom);
        }
    }
}

void STDMETHODCALLTYPE H_OMSetStencilRef(GCL* l, UINT ref) {
    g_fns.OMSetStencilRef(l, ref);
    if (Track())
        if (frame::ListState* s = State(l)) s->stencilRef = s->stencilRefBack = ref;
}

void STDMETHODCALLTYPE H_OMSetFrontAndBackStencilRef(GCL* l, UINT front, UINT back) {
    g_fns.OMSetFrontAndBackStencilRef(l, front, back);
    if (Track())
        if (frame::ListState* s = State(l)) {
            s->stencilRef = front;
            s->stencilRefBack = back;
        }
}

void STDMETHODCALLTYPE H_SetPipelineState(GCL* l, ID3D12PipelineState* pso) {
    g_fns.SetPipelineState(l, pso);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) {
        s->pso = uint64_t(uintptr_t(pso));
        s->psoClass = pso ? g_psos->Find(s->pso) : nullptr;
        s->stateObjectLast = false;
    }
}

void STDMETHODCALLTYPE H_SetPipelineState1(GCL* l, void* stateObject) {
    g_fns.SetPipelineState1(l, stateObject);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) {
        s->stateObject = uint64_t(uintptr_t(stateObject));
        s->stateObjectLast = true;
    }
}

void STDMETHODCALLTYPE H_ExecuteBundle(GCL* l, ID3D12GraphicsCommandList* bundle) {
    g_fns.ExecuteBundle(l, bundle);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) s->bundleRan = true; // its state changes are invisible to us
}

// The main motion-vector target before this barrier batch: its own transition
// in the batch, else its state after an earlier barrier in this list.
bool MotionTargetNow(const frame::ListState& s, UINT n, const D3D12_RESOURCE_BARRIER* b, ID3D12Resource*& res,
                     UINT& sub, D3D12_RESOURCE_STATES& state) {
    const uint64_t m = g_reg.MainMotionTarget();
    if (!m) return false;
    for (UINT i = 0; i < n; ++i) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const D3D12_RESOURCE_TRANSITION_BARRIER& t = b[i].Transition;
        if (uint64_t(uintptr_t(t.pResource)) != m || !g_reg.IsMainMotionTarget(m, t.Subresource)) continue;
        if (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) return false; // mid-transition
        res = t.pResource;
        sub = t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? g_reg.MainMotionSubresource() : t.Subresource;
        state = t.StateBefore;
        return true;
    }
    if (s.motionRes != m) return false;
    res = reinterpret_cast<ID3D12Resource*>(uintptr_t(m));
    sub = g_reg.MainMotionSubresource();
    state = D3D12_RESOURCE_STATES(s.motionState);
    return true;
}

void STDMETHODCALLTYPE H_ResourceBarrier(GCL* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (Track() && b) {
        if (frame::ListState* s = State(l)) {
            AtBoundary(l, s, frame::Boundary::Barrier);
            if (g_reg.HasMainDepthTarget()) Guarded("depth capture", [&] {
            for (UINT i = 0; i < n; ++i) {
                if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
                const D3D12_RESOURCE_TRANSITION_BARRIER& t = b[i].Transition;
                const uint64_t res = uint64_t(uintptr_t(t.pResource));
                if (!g_reg.IsMainDepthTarget(res, t.Subresource)) continue;
                const bool leavesRt = (t.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) &&
                                      !(t.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET);
                ReportLine("  L%u barrier on the main linear depth #%05x: %#x -> %#x%s", s->serial, ShortId(res),
                           unsigned(t.StateBefore), unsigned(t.StateAfter), leavesRt ? " (G-buffer complete)" : "");
                if (!leavesRt || !g_active.load(std::memory_order_acquire) || !g_injector) break;
                if (!g_policy->CaptureOn()) break;
                // The end of a split barrier: the target is mid-transition, so
                // it can't be copied here (the BEGIN_ONLY half was the moment).
                if (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) break;
                // Only a frame Mario was drawn in has a view to pair the depth
                // with; don't use up the frame's capture on an earlier exit.
                if (!g_injector->CanCapture(s->frame)) break;
                if (!g_reg.TryClaimCapture(s->frame)) break;
                ++t_internal;
                const UINT sub = t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? g_reg.MainDepthSubresource()
                                                                                         : t.Subresource;
                ID3D12Resource* motion = nullptr;
                UINT msub = 0;
                D3D12_RESOURCE_STATES mstate = D3D12_RESOURCE_STATE_COMMON;
                if (!MotionTargetNow(*s, n, b, motion, msub, mstate)) motion = nullptr;
                const bool captured = g_injector->Capture(l, *s, t.pResource, sub, t.StateBefore, motion, msub, mstate);
                --t_internal;
                if (captured) {
                    ++s->captures;
                    ++g_stats.captures;
                    g_stats.lastCaptureFrame = s->frame;
                }
                break;
            }
            });
            // Track the motion target's state through this list.
            if (const uint64_t m = g_reg.MainMotionTarget()) {
                for (UINT i = 0; i < n; ++i) {
                    if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
                    const D3D12_RESOURCE_TRANSITION_BARRIER& t = b[i].Transition;
                    if (uint64_t(uintptr_t(t.pResource)) != m) continue;
                    // A split barrier's first half leaves it unusable until the second.
                    s->motionRes = (b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) ? 0 : m;
                    s->motionState = uint32_t(t.StateAfter);
                }
            }
        }
    }
    g_fns.ResourceBarrier(l, n, b);
}

void STDMETHODCALLTYPE H_SetDescriptorHeaps(GCL* l, UINT n, ID3D12DescriptorHeap* const* h) {
    g_fns.SetDescriptorHeaps(l, n, h);
    if (!Track() || !h) return;
    if (frame::ListState* s = State(l)) {
        s->numHeaps = std::min<UINT>(n, 2);
        for (UINT i = 0; i < s->numHeaps; ++i) s->heaps[i] = uint64_t(uintptr_t(h[i]));
    }
}

void STDMETHODCALLTYPE H_SetGraphicsRootSignature(GCL* l, ID3D12RootSignature* rs) {
    g_fns.SetGraphicsRootSignature(l, rs);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) {
        // Setting the root signature that is already set keeps the bindings
        // (the game may rely on that and not bind everything again).
        if (rs && s->rootSig == uint64_t(uintptr_t(rs))) return;
        s->rootSig = uint64_t(uintptr_t(rs));
        s->layout = rs ? g_rootSigs->Find(s->rootSig) : nullptr;
        for (auto& a : s->args) a = frame::RootArg();
        std::memset(s->constPool, 0, sizeof(s->constPool));
    }
}

void STDMETHODCALLTYPE H_SetGraphicsRootDescriptorTable(GCL* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    g_fns.SetGraphicsRootDescriptorTable(l, i, h);
    if (!Track() || i >= 64) return;
    if (frame::ListState* s = State(l)) {
        s->args[i].kind = frame::kArgTable;
        s->args[i].value = h.ptr;
    }
}

void StoreConstants(frame::ListState* s, UINT i, UINT n, const void* data, UINT off) {
    if (!s || i >= 64) return;
    s->args[i].kind = frame::kArgConsts;
    if (!s->layout || !data) return;
    const uint32_t base = s->layout->constOffset[i];
    if (base + off + n > 64) return;
    std::memcpy(&s->constPool[base + off], data, size_t(n) * 4);
}

void STDMETHODCALLTYPE H_SetGraphicsRoot32BitConstant(GCL* l, UINT i, UINT v, UINT off) {
    g_fns.SetGraphicsRoot32BitConstant(l, i, v, off);
    if (Track()) StoreConstants(State(l), i, 1, &v, off);
}

void STDMETHODCALLTYPE H_SetGraphicsRoot32BitConstants(GCL* l, UINT i, UINT n, const void* d, UINT off) {
    g_fns.SetGraphicsRoot32BitConstants(l, i, n, d, off);
    if (Track()) StoreConstants(State(l), i, n, d, off);
}

void StoreRootView(GCL* l, UINT i, uint8_t kind, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (!Track() || i >= 64) return;
    if (frame::ListState* s = State(l)) {
        s->args[i].kind = kind;
        s->args[i].value = va;
    }
}

void STDMETHODCALLTYPE H_SetGraphicsRootConstantBufferView(GCL* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    g_fns.SetGraphicsRootConstantBufferView(l, i, va);
    StoreRootView(l, i, frame::kArgCbv, va);
}

void STDMETHODCALLTYPE H_SetGraphicsRootShaderResourceView(GCL* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    g_fns.SetGraphicsRootShaderResourceView(l, i, va);
    StoreRootView(l, i, frame::kArgSrv, va);
}

void STDMETHODCALLTYPE H_SetGraphicsRootUnorderedAccessView(GCL* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    g_fns.SetGraphicsRootUnorderedAccessView(l, i, va);
    StoreRootView(l, i, frame::kArgUav, va);
}

void STDMETHODCALLTYPE H_IASetVertexBuffers(GCL* l, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    g_fns.IASetVertexBuffers(l, start, n, v);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) {
        for (UINT k = 0; k < n && start + k < 16; ++k) {
            frame::VbView& d = s->vbs[start + k];
            if (v) {
                d.gpu = v[k].BufferLocation;
                d.size = v[k].SizeInBytes;
                d.stride = v[k].StrideInBytes;
            } else {
                d = frame::VbView();
            }
            s->vbSet |= 1u << (start + k);
        }
    }
}

void BindTargets(frame::ListState* s, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL range,
                 const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    s->numRt = std::min<UINT>(n, 8);
    const uint32_t stride = g_rtvStride.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < 8; ++i) {
        s->rtv[i] = 0;
        s->rt[i] = frame::ViewInfo();
    }
    for (uint32_t i = 0; i < s->numRt && rtvs; ++i) {
        const uint64_t h = range ? rtvs[0].ptr + uint64_t(i) * stride : rtvs[i].ptr;
        s->rtv[i] = h;
        ResolveView(h, s->rt[i]);
    }
    s->dsv = dsv ? dsv->ptr : 0;
    s->ds = frame::ViewInfo();
    if (dsv) ResolveView(dsv->ptr, s->ds);
}

void STDMETHODCALLTYPE H_OMSetRenderTargets(GCL* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL range,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    frame::ListState* s = Track() ? State(l) : nullptr;
    if (s) AtBoundary(l, s, frame::Boundary::RenderTargets);
    g_fns.OMSetRenderTargets(l, n, rtvs, range, dsv);
    if (s) Guarded("render target tracking", [&] {
        EnsureStridesFromList(l);
        BindTargets(s, n, rtvs, range, dsv);
        TraceBinding(*s);
    });
}

void STDMETHODCALLTYPE H_ClearDepthStencilView(GCL* l, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS flags, FLOAT d,
                                               UINT8 st, UINT n, const D3D12_RECT* r) {
    frame::ListState* s = Track() ? State(l) : nullptr;
    if (s) AtBoundary(l, s, frame::Boundary::ClearDepth);
    g_fns.ClearDepthStencilView(l, h, flags, d, st, n, r);
    if (s && (flags & D3D12_CLEAR_FLAG_DEPTH)) Guarded("clear tracking", [&] {
        frame::ViewInfo v;
        const bool known = ResolveView(h.ptr, v);
        if (Reporting()) {
            char rects[64] = "";
            if (n && r)
                std::snprintf(rects, sizeof(rects), ", first (%ld,%ld)-(%ld,%ld)", long(r[0].left), long(r[0].top),
                              long(r[0].right), long(r[0].bottom));
            if (known)
                ReportLine("  L%u ClearDepthStencilView %s %ux%u#%05x slice %u to %.3f, %u rect(s)%s", s->serial,
                           ShortFormat(v.format), v.MipWidth(), v.MipHeight(), ShortId(v.resource), v.firstSlice, d, n,
                           rects);
            else
                ReportLine("  L%u ClearDepthStencilView on an unknown view", s->serial);
        }
        if (known) {
            std::vector<frame::Rect> rects;
            for (UINT i = 0; i < n && r; ++i)
                rects.push_back({int32_t(r[i].left), int32_t(r[i].top), int32_t(r[i].right), int32_t(r[i].bottom)});
            g_policy->OnClearDepth(*s, v, rects.empty() ? nullptr : rects.data(), uint32_t(rects.size()));
        }
    });
}

void STDMETHODCALLTYPE H_ClearRenderTargetView(GCL* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* c, UINT n,
                                               const D3D12_RECT* r) {
    frame::ListState* s = Track() ? State(l) : nullptr;
    if (s) AtBoundary(l, s, frame::Boundary::ClearTarget);
    g_fns.ClearRenderTargetView(l, h, c, n, r);
}

void STDMETHODCALLTYPE H_SetPredication(GCL* l, ID3D12Resource* buf, UINT64 off, D3D12_PREDICATION_OP op) {
    g_fns.SetPredication(l, buf, off, op);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) {
        s->predBuffer = uint64_t(uintptr_t(buf));
        s->predOffset = off;
        s->predOp = uint32_t(op);
    }
}

void STDMETHODCALLTYPE H_BeginEvent(GCL* l, UINT meta, const void* data, UINT size) {
    g_fns.BeginEvent(l, meta, data, size);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) {
        if (s->markerDepth < 8) Guarded("marker tracking", [&] {
            const std::string name = frame::DecodePixEvent(meta, data, size);
            std::snprintf(s->markers[s->markerDepth], sizeof(s->markers[0]), "%s", name.c_str());
            if (!g_stats.markers.exchange(true)) LOGI("frame: the game emits PIX markers (first: \"%s\")", name.c_str());
            if (Reporting() && s->trace.size() < 1400) s->trace += " <" + name + ">";
        });
        ++s->markerDepth;
    }
}

void STDMETHODCALLTYPE H_EndEvent(GCL* l) {
    g_fns.EndEvent(l);
    if (!Track()) return;
    if (frame::ListState* s = State(l)) {
        if (s->markerDepth > 0) --s->markerDepth;
        if (s->markerDepth < 8) s->markers[s->markerDepth][0] = 0;
    }
}

void STDMETHODCALLTYPE H_BeginRenderPass(GCL* l, UINT n, const void* rts, const void* ds, UINT flags) {
    frame::ListState* s = Track() ? State(l) : nullptr;
    if (s) AtBoundary(l, s, frame::Boundary::BeginRenderPass);
    g_fns.BeginRenderPass(l, n, rts, ds, flags);
    if (!s) return;
    Guarded("render pass tracking", [&] {
    EnsureStridesFromList(l);
    D3D12_CPU_DESCRIPTOR_HANDLE handles[8] = {};
    const UINT count = std::min<UINT>(n, 8);
    for (UINT i = 0; i < count && rts; ++i)
        std::memcpy(&handles[i], static_cast<const uint8_t*>(rts) + i * kRenderPassRtDescSize, 8);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (ds) std::memcpy(&dsv, ds, 8);
    BindTargets(s, count, handles, FALSE, ds ? &dsv : nullptr);
    s->inRenderPass = true;
    s->renderPassFlags = flags;
    if (ds && s->ds.valid) {
        uint32_t depthAccess = 0, stencilAccess = 0;
        std::memcpy(&depthAccess, static_cast<const uint8_t*>(ds) + kRenderPassDsDepthAccessOffset, 4);
        std::memcpy(&stencilAccess, static_cast<const uint8_t*>(ds) + kRenderPassDsStencilAccessOffset, 4);
        if (depthAccess == 3) s->ds.dsvFlags |= 1;                    // NO_ACCESS: not writable
        if (stencilAccess == 3) s->ds.dsvFlags |= 2;                  // ... the stencil neither
        if (depthAccess == 2) g_policy->OnClearDepth(*s, s->ds, nullptr, 0); // CLEAR
    }
    TraceBinding(*s);
    });
}

void STDMETHODCALLTYPE H_EndRenderPass(GCL* l) {
    frame::ListState* s = Track() ? State(l) : nullptr;
    if (s) AtBoundary(l, s, frame::Boundary::EndRenderPass);
    g_fns.EndRenderPass(l);
    if (s) {
        s->inRenderPass = false;
        s->renderPassFlags = 0;
        BindTargets(s, 0, nullptr, FALSE, nullptr);
    }
}

void STDMETHODCALLTYPE H_Barrier(GCL* l, UINT32 n, const void* groups) {
    if (Track()) {
        if (frame::ListState* s = State(l)) AtBoundary(l, s, frame::Boundary::Barrier);
    }
    g_fns.Barrier(l, n, groups);
}

// ---------------------------------------------------------------------------
// Installation

void* Vt(void* obj, int index) { return (*reinterpret_cast<void***>(obj))[index]; }

struct HookSpec {
    void* target;
    void* detour;
    void** original;
    const char* name;
    bool required;
};

bool InstallHooks(std::vector<HookSpec>& specs, std::string& error) {
    // Two entry points sharing one function (identical code folded by the
    // linker) can't be told apart: hook only the first, leave the other alone.
    std::vector<void*> seen;
    for (HookSpec& h : specs) {
        if (!h.target) {
            if (h.required) {
                error = std::string("no address for ") + h.name;
                return false;
            }
            continue;
        }
        if (std::find(seen.begin(), seen.end(), h.target) != seen.end()) {
            LOGW("frame: %s shares its code with another hooked method - not hooked", h.name);
            if (h.required) {
                error = std::string("shared entry point: ") + h.name;
                return false;
            }
            *h.original = nullptr;
            continue;
        }
        seen.push_back(h.target);
        const MH_STATUS st = MH_CreateHook(h.target, h.detour, h.original);
        if (st != MH_OK) {
            LOGW("frame: MH_CreateHook(%s) = %d", h.name, int(st));
            if (h.required) {
                error = std::string("MH_CreateHook failed for ") + h.name;
                return false;
            }
            *h.original = nullptr;
            continue;
        }
    }
    for (HookSpec& h : specs) {
        if (!h.target || !*h.original) continue;
        if (MH_EnableHook(h.target) != MH_OK) {
            error = std::string("MH_EnableHook failed for ") + h.name;
            return false;
        }
    }
    return true;
}

const GUID kIID_Device1 = {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}};
const GUID kIID_Device2 = {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};
const GUID kIID_List4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
const GUID kIID_List7 = {0xdd171223, 0x8b61, 0x4769, {0x90, 0xe3, 0x16, 0x0c, 0xcd, 0xe4, 0xe2, 0xc1}};
const GUID kIID_List8 = {0xee936ef9, 0x599d, 0x4d28, {0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51}};
const GUID kIID_PipelineLibrary = {0xc64226a8, 0x9201, 0x46af, {0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f}};
const GUID kIID_PipelineLibrary1 = {0x80eabf42, 0x2568, 0x4e5e, {0xbd, 0x82, 0xc3, 0x7f, 0x86, 0x96, 0x1d, 0xc3}};

bool Supports(IUnknown* o, const GUID& iid) {
    void* p = nullptr;
    if (FAILED(o->QueryInterface(iid, &p)) || !p) return false;
    static_cast<IUnknown*>(p)->Release();
    return true;
}

template <typename T>
void** Orig(T& fnPtr) {
    return reinterpret_cast<void**>(&fnPtr);
}

} // namespace

bool Install(ID3D12Device* dev, ID3D12GraphicsCommandList* list, std::string& error) {
    if (g_installed.load()) return true;
    g_psos = new PtrTable<frame::PsoClass>(size_t(1) << 19);
    g_rootSigs = new PtrTable<frame::RootSigLayout>(size_t(1) << 12);
    g_lists = new PtrTable<frame::ListState>(size_t(1) << 13);
    g_descs = new DescTable(size_t(1) << 16);
    if (!g_policy) g_policy = new frame::Policy(&g_reg, frame::PolicyConfig());

    // Entry points we only call (not hooked).
    g_fns.Dispatch = reinterpret_cast<decltype(g_fns.Dispatch)>(Vt(list, 14));
    g_fns.CopyBufferRegion = reinterpret_cast<decltype(g_fns.CopyBufferRegion)>(Vt(list, 15));
    g_fns.CopyTextureRegion = reinterpret_cast<decltype(g_fns.CopyTextureRegion)>(Vt(list, 16));
    g_fns.SetComputeRootSignature = reinterpret_cast<decltype(g_fns.SetComputeRootSignature)>(Vt(list, 29));
    g_fns.SetComputeRootDescriptorTable = reinterpret_cast<decltype(g_fns.SetComputeRootDescriptorTable)>(Vt(list, 31));
    g_fns.SetComputeRoot32BitConstants = reinterpret_cast<decltype(g_fns.SetComputeRoot32BitConstants)>(Vt(list, 35));
    g_fns.SetComputeRootShaderResourceView =
        reinterpret_cast<decltype(g_fns.SetComputeRootShaderResourceView)>(Vt(list, 39));
    g_fns.SetComputeRootUnorderedAccessView =
        reinterpret_cast<decltype(g_fns.SetComputeRootUnorderedAccessView)>(Vt(list, 41));

    std::vector<HookSpec> specs = {
        {Vt(dev, 10), reinterpret_cast<void*>(&H_CreateGraphicsPipelineState), Orig(o_CreateGraphicsPipelineState),
         "CreateGraphicsPipelineState", true},
        {Vt(dev, 16), reinterpret_cast<void*>(&H_CreateRootSignature), Orig(o_CreateRootSignature), "CreateRootSignature",
         true},
        {Vt(dev, 20), reinterpret_cast<void*>(&H_CreateRenderTargetView), Orig(o_CreateRenderTargetView),
         "CreateRenderTargetView", true},
        {Vt(dev, 21), reinterpret_cast<void*>(&H_CreateDepthStencilView), Orig(o_CreateDepthStencilView),
         "CreateDepthStencilView", true},
        {Vt(dev, 23), reinterpret_cast<void*>(&H_CopyDescriptors), Orig(o_CopyDescriptors), "CopyDescriptors", false},
        {Vt(dev, 24), reinterpret_cast<void*>(&H_CopyDescriptorsSimple), Orig(o_CopyDescriptorsSimple),
         "CopyDescriptorsSimple", false},
        {Vt(list, 9), reinterpret_cast<void*>(&H_Close), Orig(g_fns.Close), "Close", true},
        {Vt(list, 10), reinterpret_cast<void*>(&H_Reset), Orig(g_fns.Reset), "Reset", true},
        {Vt(list, 11), reinterpret_cast<void*>(&H_ClearState), Orig(g_fns.ClearState), "ClearState", false},
        {Vt(list, 12), reinterpret_cast<void*>(&H_DrawInstanced), Orig(g_fns.DrawInstanced), "DrawInstanced", true},
        {Vt(list, 13), reinterpret_cast<void*>(&H_DrawIndexedInstanced), Orig(g_fns.DrawIndexedInstanced),
         "DrawIndexedInstanced", true},
        {Vt(list, 20), reinterpret_cast<void*>(&H_IASetPrimitiveTopology), Orig(g_fns.IASetPrimitiveTopology),
         "IASetPrimitiveTopology", true},
        {Vt(list, 21), reinterpret_cast<void*>(&H_RSSetViewports), Orig(g_fns.RSSetViewports), "RSSetViewports", true},
        {Vt(list, 22), reinterpret_cast<void*>(&H_RSSetScissorRects), Orig(g_fns.RSSetScissorRects), "RSSetScissorRects",
         true},
        {Vt(list, 24), reinterpret_cast<void*>(&H_OMSetStencilRef), Orig(g_fns.OMSetStencilRef), "OMSetStencilRef", true},
        {Vt(list, 25), reinterpret_cast<void*>(&H_SetPipelineState), Orig(g_fns.SetPipelineState), "SetPipelineState",
         true},
        {Vt(list, 26), reinterpret_cast<void*>(&H_ResourceBarrier), Orig(g_fns.ResourceBarrier), "ResourceBarrier", true},
        {Vt(list, 28), reinterpret_cast<void*>(&H_SetDescriptorHeaps), Orig(g_fns.SetDescriptorHeaps),
         "SetDescriptorHeaps", true},
        {Vt(list, 30), reinterpret_cast<void*>(&H_SetGraphicsRootSignature), Orig(g_fns.SetGraphicsRootSignature),
         "SetGraphicsRootSignature", true},
        {Vt(list, 32), reinterpret_cast<void*>(&H_SetGraphicsRootDescriptorTable),
         Orig(g_fns.SetGraphicsRootDescriptorTable), "SetGraphicsRootDescriptorTable", true},
        {Vt(list, 34), reinterpret_cast<void*>(&H_SetGraphicsRoot32BitConstant), Orig(g_fns.SetGraphicsRoot32BitConstant),
         "SetGraphicsRoot32BitConstant", true},
        {Vt(list, 36), reinterpret_cast<void*>(&H_SetGraphicsRoot32BitConstants),
         Orig(g_fns.SetGraphicsRoot32BitConstants), "SetGraphicsRoot32BitConstants", true},
        {Vt(list, 38), reinterpret_cast<void*>(&H_SetGraphicsRootConstantBufferView),
         Orig(g_fns.SetGraphicsRootConstantBufferView), "SetGraphicsRootConstantBufferView", true},
        {Vt(list, 40), reinterpret_cast<void*>(&H_SetGraphicsRootShaderResourceView),
         Orig(g_fns.SetGraphicsRootShaderResourceView), "SetGraphicsRootShaderResourceView", true},
        {Vt(list, 42), reinterpret_cast<void*>(&H_SetGraphicsRootUnorderedAccessView),
         Orig(g_fns.SetGraphicsRootUnorderedAccessView), "SetGraphicsRootUnorderedAccessView", true},
        {Vt(list, 44), reinterpret_cast<void*>(&H_IASetVertexBuffers), Orig(g_fns.IASetVertexBuffers),
         "IASetVertexBuffers", true},
        {Vt(list, 46), reinterpret_cast<void*>(&H_OMSetRenderTargets), Orig(g_fns.OMSetRenderTargets),
         "OMSetRenderTargets", true},
        {Vt(list, 47), reinterpret_cast<void*>(&H_ClearDepthStencilView), Orig(g_fns.ClearDepthStencilView),
         "ClearDepthStencilView", true},
        {Vt(list, 48), reinterpret_cast<void*>(&H_ClearRenderTargetView), Orig(g_fns.ClearRenderTargetView),
         "ClearRenderTargetView", false},
        {Vt(list, 55), reinterpret_cast<void*>(&H_SetPredication), Orig(g_fns.SetPredication), "SetPredication", true},
        {Vt(list, 57), reinterpret_cast<void*>(&H_BeginEvent), Orig(g_fns.BeginEvent), "BeginEvent", false},
        {Vt(list, 58), reinterpret_cast<void*>(&H_EndEvent), Orig(g_fns.EndEvent), "EndEvent", false},
        {Vt(list, 59), reinterpret_cast<void*>(&H_ExecuteIndirect), Orig(g_fns.ExecuteIndirect), "ExecuteIndirect", true},
    };
    if (Supports(dev, kIID_Device2))
        specs.push_back({Vt(dev, 47), reinterpret_cast<void*>(&H_CreatePipelineState), Orig(o_CreatePipelineState),
                         "CreatePipelineState", false});
    specs.push_back({Vt(list, 27), reinterpret_cast<void*>(&H_ExecuteBundle), Orig(g_fns.ExecuteBundle),
                     "ExecuteBundle", false});
    if (Supports(list, kIID_List4)) {
        specs.push_back({Vt(list, 68), reinterpret_cast<void*>(&H_BeginRenderPass), Orig(g_fns.BeginRenderPass),
                         "BeginRenderPass", false});
        specs.push_back({Vt(list, 69), reinterpret_cast<void*>(&H_EndRenderPass), Orig(g_fns.EndRenderPass),
                         "EndRenderPass", false});
        specs.push_back({Vt(list, 75), reinterpret_cast<void*>(&H_SetPipelineState1), Orig(g_fns.SetPipelineState1),
                         "SetPipelineState1", false});
    }
    specs.push_back({Vt(dev, 12), reinterpret_cast<void*>(&H_CreateCommandList), Orig(o_CreateCommandList),
                     "CreateCommandList", false});
    if (Supports(list, kIID_List7))
        specs.push_back({Vt(list, 80), reinterpret_cast<void*>(&H_Barrier), Orig(g_fns.Barrier), "Barrier", false});
    // (Mario's draw sets the stencil reference and puts the game's back: both of them.)
    if (Supports(list, kIID_List8))
        specs.push_back({Vt(list, 81), reinterpret_cast<void*>(&H_OMSetFrontAndBackStencilRef),
                         Orig(g_fns.OMSetFrontAndBackStencilRef), "OMSetFrontAndBackStencilRef", false});
    // Pipeline libraries hand out pipelines without CreatePipelineState.
    void* device1 = nullptr;
    void* lib = nullptr;
    if (SUCCEEDED(dev->QueryInterface(kIID_Device1, &device1)) && device1) {
        using CreateLibFn = HRESULT(STDMETHODCALLTYPE*)(void*, const void*, SIZE_T, REFIID, void**);
        auto createLib = reinterpret_cast<CreateLibFn>(Vt(device1, 44));
        if (FAILED(createLib(device1, nullptr, 0, kIID_PipelineLibrary, &lib))) lib = nullptr;
        static_cast<IUnknown*>(device1)->Release();
    }
    if (lib) {
        specs.push_back({Vt(lib, 9), reinterpret_cast<void*>(&H_LoadGraphicsPipeline), Orig(o_LoadGraphicsPipeline),
                         "LoadGraphicsPipeline", false});
        if (Supports(static_cast<IUnknown*>(lib), kIID_PipelineLibrary1))
            specs.push_back({Vt(lib, 13), reinterpret_cast<void*>(&H_LoadPipeline), Orig(o_LoadPipeline), "LoadPipeline",
                             false});
    }
    const bool ok = InstallHooks(specs, error);
    if (lib) static_cast<IUnknown*>(lib)->Release();
    if (!ok) return false;
    // Optional hooks that didn't install: keep the originals callable.
    if (!g_fns.ClearRenderTargetView)
        g_fns.ClearRenderTargetView = reinterpret_cast<decltype(g_fns.ClearRenderTargetView)>(Vt(list, 48));
    if (!g_fns.ClearState) g_fns.ClearState = reinterpret_cast<decltype(g_fns.ClearState)>(Vt(list, 11));
    if (!g_fns.BeginEvent) g_fns.BeginEvent = reinterpret_cast<decltype(g_fns.BeginEvent)>(Vt(list, 57));
    if (!g_fns.EndEvent) g_fns.EndEvent = reinterpret_cast<decltype(g_fns.EndEvent)>(Vt(list, 58));
    g_installed = true;
    LOGI("frame: %zu device/list hooks installed (render passes %s, enhanced barriers %s, pipeline library %s)",
         specs.size(), g_fns.BeginRenderPass ? "yes" : "no", g_fns.Barrier ? "yes" : "no",
         o_LoadGraphicsPipeline ? "yes" : "no");
    return true;
}

bool Installed() { return g_installed.load(); }
StencilCensus& Census() { return g_census; }
void SetCensusWanted(bool on) { g_censusWanted.store(on, std::memory_order_relaxed); }

ScopedInternal::ScopedInternal() { ++t_internal; }
ScopedInternal::~ScopedInternal() { --t_internal; }

void Configure(const Config& cfg, Injector* injector) {
    {
        LockGuard lock(g_cfgMutex);
        g_classify = cfg.classify;
    }
    if (!g_policy) g_policy = new frame::Policy(&g_reg, cfg.policy);
    else g_policy->SetConfig(cfg.policy);
    g_injector = injector;
}

void SetActive(bool on) { g_active.store(on, std::memory_order_release); }
void SetSteadyShadowRegions(bool on) {
    if (g_policy) g_policy->SetSteadyRegions(on);
}
bool SteadyShadowRegions() { return g_policy && g_policy->SteadyRegions(); }

void SetCapture(bool on) {
    if (g_policy) g_policy->SetCapture(on);
}
bool Active() { return g_active.load(std::memory_order_acquire); }

void BeginFrame(uint64_t frameId) {
    g_reg.BeginFrame(frameId);
    if (!Reporting() || g_broken.load()) return;
    std::vector<std::string> lines;
    size_t dropped = 0;
    {
        LockGuard lock(g_report.mu);
        lines.swap(g_report.lines);
        dropped = g_report.dropped;
        g_report.dropped = 0;
    }
    if (!lines.empty()) {
        LOGI("---- frame report (frame %llu, %s) ----", static_cast<unsigned long long>(frameId - 1),
             g_report.why.c_str());
        for (const std::string& l : lines) LOGI("%s", l.c_str());
        if (dropped) LOGI("  (%zu more lines not shown)", dropped);
    }
    if (g_report.framesLeft.fetch_sub(1) == 1) {
        std::vector<std::pair<std::string, int>> depthOnly;
        {
            LockGuard lock(g_report.mu);
            depthOnly.swap(g_report.depthOnly);
        }
        for (const auto& e : depthOnly)
            LOGI("  depth-only pipeline that isn't a known shadow caster (%d draws): %s", e.second, e.first.c_str());
        const Stats st = GetStats();
        LOGI("---- end of frame report: %llu pipelines (%llu G-buffer, %llu shadow casters, %llu cache copies, %llu "
             "cache moves), %llu root signatures, %llu RTVs, %llu DSVs, %llu lists; draws with unknown pipelines %llu, unknown "
             "render target views %llu; PIX markers %s ----",
             (unsigned long long)st.psos, (unsigned long long)st.gbufferPsos, (unsigned long long)st.casterPsos,
             (unsigned long long)st.cacheCopyPsos, (unsigned long long)st.cacheMovePsos, (unsigned long long)st.rootSigs,
             (unsigned long long)st.rtvs, (unsigned long long)st.dsvs, (unsigned long long)st.lists,
             (unsigned long long)st.unknownPsoDraws, (unsigned long long)st.unknownRtvBinds, st.markers ? "yes" : "no");
        log::Flush();
    }
}

uint64_t Frame() { return g_reg.Frame(); }

// The queue that runs the lists Mario was recorded into: the injector's
// read-back must go on that queue, after them.
Mutex g_injectQueueMu;
ID3D12CommandQueue* g_injectQueue = nullptr;

void OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (!lists || !g_lists || !queue) return;
    bool ours = false;
    for (UINT i = 0; i < count && !ours; ++i)
        if (frame::ListState* s = g_lists->Find(uint64_t(uintptr_t(lists[i])))) ours = s->injections || s->captures;
    if (ours) {
        LockGuard lock(g_injectQueueMu);
        if (g_injectQueue != queue) {
            queue->AddRef();
            if (g_injectQueue) g_injectQueue->Release();
            g_injectQueue = queue;
            LOGI("frame: Mario's draws run on queue %05x", ShortId(uintptr_t(queue)));
        }
    }
    if (!Reporting()) return;
    std::string ids;
    for (UINT i = 0; i < count && ids.size() < 400; ++i) {
        frame::ListState* s = g_lists->Find(uint64_t(uintptr_t(lists[i])));
        char b[16];
        std::snprintf(b, sizeof(b), " L%u", s ? s->serial : 0u);
        ids += b;
    }
    ReportLine("submit queue %05x:%s", ShortId(uintptr_t(queue)), ids.c_str());
}

ID3D12CommandQueue* InjectionQueue() {
    LockGuard lock(g_injectQueueMu);
    if (g_injectQueue) g_injectQueue->AddRef();
    return g_injectQueue;
}

void RequestReport(int frames, const char* why) {
    {
        LockGuard lock(g_report.mu);
        g_report.why = why ? why : "";
        g_report.lines.clear();
        g_report.dropped = 0;
    }
    g_report.framesLeft.store(frames + 1);
}

Stats GetStats() {
    Stats s;
    s.psos = g_stats.psos;
    s.gbufferPsos = g_stats.gbufferPsos;
    s.casterPsos = g_stats.casterPsos;
    s.cacheCopyPsos = g_stats.cacheCopyPsos;
    s.cacheMovePsos = g_stats.cacheMovePsos;
    s.rootSigs = g_stats.rootSigs;
    s.rtvs = g_stats.rtvs;
    s.dsvs = g_stats.dsvs;
    s.lists = g_stats.lists;
    s.unknownPsoDraws = g_stats.unknownPsoDraws;
    s.unknownRtvBinds = g_stats.unknownRtvBinds;
    s.gbufferSegments = g_stats.gbufferSegments;
    s.shadowRegions = g_stats.shadowRegions;
    s.gbufferInjections = g_stats.gbufferInjections;
    s.shadowInjections = g_stats.shadowInjections;
    s.captures = g_stats.captures;
    s.lastGBufferFrame = g_stats.lastGBufferFrame;
    s.lastShadowFrame = g_stats.lastShadowFrame;
    s.lastCaptureFrame = g_stats.lastCaptureFrame;
    s.markers = g_stats.markers;
    {
        LockGuard lock(g_skipMutex);
        s.lastSkip = g_lastSkip;
    }
    return s;
}

} // namespace tracker
} // namespace sm2m

#endif
