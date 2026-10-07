#include "frame_policy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace sm2m::frame {
namespace {

bool HasDigest(const std::vector<Digest>& list, const d3d12p::ShaderInfo& s) {
    if (!s.present) return false;
    for (const Digest& d : list)
        if (std::memcmp(d.data(), s.digest, 16) == 0) return true;
    return false;
}

bool ContainsNoCase(const char* hay, const std::string& needle) {
    if (needle.empty()) return false;
    const size_t n = std::strlen(hay);
    if (needle.size() > n) return false;
    for (size_t i = 0; i + needle.size() <= n; ++i) {
        size_t k = 0;
        while (k < needle.size() &&
               std::tolower(static_cast<unsigned char>(hay[i + k])) == std::tolower(static_cast<unsigned char>(needle[k])))
            ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

Digest D(const char* hex) {
    Digest d{};
    d3d12p::ParseDigest(hex, d.data());
    return d;
}

} // namespace

const char* PsoKindName(PsoKind k) {
    switch (k) {
    case PsoKind::GBuffer: return "gbuffer";
    case PsoKind::ShadowCaster: return "shadow-caster";
    case PsoKind::CacheCopy: return "shadow-cache-copy";
    case PsoKind::CacheMove: return "shadow-cache-move";
    default: return "other";
    }
}

const char* BoundaryName(Boundary b) {
    switch (b) {
    case Boundary::RenderTargets: return "OMSetRenderTargets";
    case Boundary::Barrier: return "ResourceBarrier";
    case Boundary::Close: return "Close";
    case Boundary::EndRenderPass: return "EndRenderPass";
    case Boundary::BeginRenderPass: return "BeginRenderPass";
    case Boundary::Viewports: return "RSSetViewports";
    case Boundary::ClearDepth: return "ClearDepthStencilView";
    case Boundary::ClearTarget: return "ClearRenderTargetView";
    default: return "other";
    }
}

PsoKind Classify(const d3d12p::PsoInfo& p, const ClassifyConfig& c) {
    if (!p.graphics) return PsoKind::Other;
    if (HasDigest(c.cacheCopyPs, p.ps)) return PsoKind::CacheCopy;
    if (HasDigest(c.cacheMovePs, p.ps)) return PsoKind::CacheMove;
    const bool depthTarget = d3d12p::IsDepthFormat(p.dsvFormat);
    if (depthTarget && p.depthWrite && (HasDigest(c.casterVs, p.vs) || HasDigest(c.casterPs, p.ps)))
        return PsoKind::ShadowCaster;
    if (!c.gbufferFormats.empty() && depthTarget && p.numRenderTargets >= c.gbufferFormats.size()) {
        bool match = true;
        for (size_t i = 0; i < c.gbufferFormats.size(); ++i)
            if (p.rtvFormats[i] != c.gbufferFormats[i]) match = false;
        if (match) return PsoKind::GBuffer;
    }
    return PsoKind::Other;
}

ClassifyConfig DefaultClassifyConfig() {
    ClassifyConfig c;
    // Container digests (bytes 4..19) of the engine shaders embedded in
    // Spider-Man2.exe, DXIL (SM6.6, what the PC build uses) then DXBC.
    c.casterVs = {D("05a36ebf39ff6d35941e48b710a404d9"), D("6265d4a503cf790106370680eadfc2a2"),  // VS_ModelShadowCaster
                  D("03e989429e07cdc0ae2766446409ac4e"), D("2a0db64335ff3a09fc3e714ed760ab83")}; // ...Alpha
    c.casterPs = {D("b603b5e408ccd4f905d55e890d23bf72"), D("8daf6f4f12c70c78a04f8c9caf38f527"),  // PS_ShadowCaster
                  D("de5a6d58c74224b18f0afd1a91e098da"), D("9d184c35cbdcbd2d3493165a050b3fdf"),  // ...Alpha
                  D("5a5e8f471b59e86c1b33f53ead5d3ad3"), D("cd8768469ac2b98e823cb18aa26d70e8"),  // ...Dissolve
                  D("0c66736fef4337d21941284ed6e6bb2c"), D("df523e663d2115d8da25ca0bc4dce7fd")}; // ...AlphaDissolve
    c.cacheCopyPs = {D("57e678a45ff1fe17d8b60a31cc5c940d"), D("a672343106c448791338b3ea067b275e")}; // PS_ShadowCacheCopyDepth
    c.cacheMovePs = {D("0ec47be661a7c5ce785e401d931c86a2"), D("445fce1a23ad66df1d1b392ad58dbc5f")}; // PS_ShadowCacheMoveDepth
    return c;
}

void BuildLayout(RootSigLayout& l) {
    l.viewVs = l.info.FindTableDescriptor(d3d12p::kRangeCbv, 0, 0, d3d12p::kVisVertex);
    l.viewRootCbv = l.info.FindRootCbv(0, 0);
    uint32_t off = 0;
    bool fits = true;
    for (size_t i = 0; i < l.info.params.size() && i < 64; ++i) {
        const d3d12p::RootParam& p = l.info.params[i];
        l.constOffset[i] = off;
        if (p.type == d3d12p::RootParamType::Constants) {
            off += p.num32BitValues;
            if (off > 64) fits = false;
        }
    }
    l.constTotal = off;
    l.usable = fits && l.info.params.size() <= 64 && (l.viewVs.param >= 0 || l.viewRootCbv >= 0);
}

Rect Viewport::Bounds() const {
    Rect r;
    r.left = int32_t(x);
    r.top = int32_t(y);
    r.right = int32_t(x + w);
    r.bottom = int32_t(y + h);
    return r;
}

bool Covers(const Prepared& p, const Rect& r) {
    if (p.full || p.rc.Contains(r)) return true;
    const int64_t area = r.Area();
    if (area <= 0) return false;
    const int32_t l = std::max(p.rc.left, r.left), t = std::max(p.rc.top, r.top);
    const int32_t rr = std::min(p.rc.right, r.right), b = std::min(p.rc.bottom, r.bottom);
    if (rr <= l || b <= t) return false;
    return int64_t(rr - l) * int64_t(b - t) * 100 >= area * 97;
}

void ListState::Reset(uint64_t frameId) {
    const uint64_t keepList = list;
    const uint32_t keepSerial = serial;
    std::vector<Prepared> keepPrepared;
    keepPrepared.swap(prepared);
    keepPrepared.clear();
    *this = ListState();
    list = keepList;
    serial = keepSerial;
    frame = frameId;
    prepared.swap(keepPrepared); // keeps the allocation
}

bool ListState::InMarker(const std::vector<std::string>& names) const {
    const int depth = std::min(markerDepth, 8);
    for (int i = 0; i < depth; ++i)
        for (const std::string& n : names)
            if (ContainsNoCase(markers[i], n)) return true;
    return false;
}

// ---- Registry --------------------------------------------------------------

void Registry::BeginFrame(uint64_t frame) {
    const uint64_t cur = curMaxArea_.exchange(0);
    const uint64_t dims = curMaxDims_.exchange(0);
    if (cur > 0) {
        prevMaxArea_.store(cur);
        if (dims) prevMaxDims_.store(dims);
    }
    frame_.store(frame, std::memory_order_release);
}

void Registry::NoteGBufferSize(uint32_t w, uint32_t h) {
    const uint64_t a = uint64_t(w) * uint64_t(h);
    uint64_t cur = curMaxArea_.load();
    while (a > cur && !curMaxArea_.compare_exchange_weak(cur, a)) {
    }
    if (a >= curMaxArea_.load()) curMaxDims_.store((uint64_t(w) << 32) | uint64_t(h));
}

bool Registry::IsScreenShaped(uint32_t w, uint32_t h) const {
    const uint64_t dims = prevMaxDims_.load();
    const double mw = double(dims >> 32), mh = double(dims & 0xFFFFFFFFu);
    if (mw < 1 || mh < 1 || w == 0 || h == 0) return false;
    if (w == uint32_t(mw) || h == uint32_t(mh)) return true;
    if (double(w) > mw * 1.05 || double(h) > mh * 1.05) return false;
    const double a = double(w) / double(h), m = mw / mh;
    return std::fabs(a - m) <= m * 0.03;
}

bool Registry::IsMainSize(uint32_t w, uint32_t h) const {
    const uint64_t main = prevMaxArea_.load();
    const uint64_t a = uint64_t(w) * uint64_t(h);
    return main > 0 && a * 10 >= main * 9;
}

bool Registry::MarkersRequired(uint64_t frame) const {
    const uint64_t last = lastMarkerMatch_.load();
    return last != 0 && frame <= last + 30;
}

void Registry::MarkWorking(uint64_t res) {
    LockGuard lock(mu_);
    MapKind& k = maps_[res];
    if (k == MapKind::Unknown) k = MapKind::Working;
}

void Registry::MarkCache(uint64_t res) {
    LockGuard lock(mu_);
    maps_[res] = MapKind::Cache;
}

Registry::MapKind Registry::Kind(uint64_t res) const {
    LockGuard lock(mu_);
    auto it = maps_.find(res);
    return it == maps_.end() ? MapKind::Unknown : it->second;
}

bool Registry::TryClaimShadow(uint64_t frame, uint64_t res, uint32_t slice, const Rect& rc, int maxPerFrame) {
    LockGuard lock(mu_);
    if (shadowClaimFrame_ != frame) {
        shadowClaimFrame_ = frame;
        shadowClaims_.clear();
    }
    if (int(shadowClaims_.size()) >= maxPerFrame) return false;
    shadowClaims_.push_back({frame, res, slice, rc});
    return true;
}

namespace {
int Popcount16(uint32_t m) {
    int n = 0;
    for (m &= 0xFFFFu; m; m &= m - 1) ++n;
    return n;
}
} // namespace

int Registry::NoteCasterRegion(uint64_t frame, uint64_t res, uint32_t slice, const Rect& rc) {
    LockGuard lock(mu_);
    RegionHistory* h = nullptr;
    for (RegionHistory& r : regions_)
        if (r.res == res && r.slice == slice && r.rc == rc) {
            h = &r;
            break;
        }
    if (!h) {
        if (regions_.size() >= 512) {
            // Forget the stalest.
            size_t old = 0;
            for (size_t i = 1; i < regions_.size(); ++i)
                if (regions_[i].lastFrame < regions_[old].lastFrame) old = i;
            regions_.erase(regions_.begin() + long(old));
        }
        regions_.push_back(RegionHistory());
        h = &regions_.back();
        h->res = res;
        h->slice = slice;
        h->rc = rc;
        h->lastFrame = frame;
    }
    if (frame > h->lastFrame) {
        const uint64_t d = frame - h->lastFrame;
        h->mask = d >= 32 ? 0 : h->mask << d;
        h->lastFrame = frame;
    } else if (frame < h->lastFrame) {
        const uint64_t d = h->lastFrame - frame;
        if (d < 32) h->mask |= 1u << d;
        return Popcount16(h->mask);
    }
    h->mask |= 1u;
    return Popcount16(h->mask);
}

void Registry::NoteCaster(const PsoClass* pc) {
    if (!pc) return;
    LockGuard lock(mu_);
    for (const PsoClass*& c : casters_)
        if (c->info.dsvFormat == pc->info.dsvFormat) {
            c = pc;
            return;
        }
    casters_.push_back(pc);
}

const PsoClass* Registry::LastCaster(uint32_t dsvFormat) const {
    LockGuard lock(mu_);
    for (const PsoClass* c : casters_)
        if (c->info.dsvFormat == dsvFormat) return c;
    return nullptr;
}

bool Registry::TryClaimCapture(uint64_t frame) {
    uint64_t prev = captureFrame_.load();
    while (prev < frame) {
        if (captureFrame_.compare_exchange_weak(prev, frame)) return true;
    }
    return false;
}

void Registry::SetMainDepthTarget(uint64_t res, uint32_t subresource) {
    mainRt0Sub_.store(subresource);
    mainRt0_.store(res);
}

bool Registry::IsMainDepthTarget(uint64_t res, uint32_t subresource) const {
    if (!res || res != mainRt0_.load()) return false;
    return subresource == 0xFFFFFFFFu || subresource == mainRt0Sub_.load();
}

void Registry::SetMainMotionTarget(uint64_t res, uint32_t subresource) {
    mainRt1Sub_.store(subresource);
    mainRt1_.store(res);
}

bool Registry::IsMainMotionTarget(uint64_t res, uint32_t subresource) const {
    if (!res || res != mainRt1_.load()) return false;
    return subresource == 0xFFFFFFFFu || subresource == mainRt1Sub_.load();
}

// ---- Policy ----------------------------------------------------------------

uint64_t Policy::ViewAddress(const ListState& s, uint32_t descStride, uint64_t& rootCbv) {
    rootCbv = 0;
    const RootSigLayout* l = s.layout;
    if (!l || !l->usable) return 0;
    if (l->viewVs.param >= 0 && l->viewVs.param < 64) {
        const RootArg& a = s.args[l->viewVs.param];
        if (a.kind == kArgTable && a.value) return a.value + uint64_t(l->viewVs.offset) * descStride;
    }
    if (l->viewRootCbv >= 0 && l->viewRootCbv < 64) {
        const RootArg& a = s.args[l->viewRootCbv];
        if (a.kind == kArgCbv && a.value) rootCbv = a.value;
    }
    return 0;
}

void Policy::OnDraw(ListState& s, uint32_t descStride) {
    ++s.draws;
    const PsoClass* pc = s.psoClass;
    if (!pc) return;
    switch (pc->kind) {
    case PsoKind::GBuffer: {
        ++s.gbufferDraws;
        if (s.numRt < 1 || !s.rt[0].valid) return;
        reg_->NoteGBufferSize(s.rt[0].MipWidth(), s.rt[0].MipHeight());
        GBufferSeg& g = s.gseg;
        if (!g.active) {
            g = GBufferSeg();
            g.active = true;
        }
        ++g.draws;
        g.pso = pc;
        g.psoPtr = s.pso;
        g.rootSig = s.rootSig;
        g.layout = s.layout;
        uint64_t cbv = 0;
        const uint64_t table = ViewAddress(s, descStride, cbv);
        if (table) g.viewTable = table;
        if (cbv) g.viewCbv = cbv;
        if (s.numVp) g.vp = s.vp[0];
        if (s.numScissor) g.scissor = s.scissor[0];
        else g.scissor = g.vp.Bounds();
        g.numRt = s.numRt;
        for (uint32_t i = 0; i < 8; ++i) {
            g.rt[i] = s.rt[i];
            g.rtv[i] = s.rtv[i];
        }
        g.ds = s.ds;
        g.dsv = s.dsv;
        g.heaps[0] = s.heaps[0];
        g.heaps[1] = s.heaps[1];
        g.numHeaps = s.numHeaps;
        g.inRenderPass = s.inRenderPass;
        if (pc->info.depthWrite) g.depthWrite = true;
        if (!cfg_.gbufferMarkers.empty() && s.InMarker(cfg_.gbufferMarkers)) g.markerMatch = true;
        break;
    }
    case PsoKind::ShadowCaster:
    case PsoKind::Other: {
        // Any depth-writing draw into a shadow map region counts as a caster
        // (material-specific casters have their own shaders).
        if (!s.ds.valid || !pc->info.depthWrite || !d3d12p::IsDepthFormat(pc->info.dsvFormat)) return;
        if (pc->kind == PsoKind::Other) {
            bool colorWrites = false;
            for (uint32_t i = 0; i < pc->info.numRenderTargets && i < 8; ++i)
                if (pc->info.rtWriteMask[i]) colorWrites = true;
            if (colorWrites) return;
        }
        const Viewport vp = s.numVp ? s.vp[0] : Viewport();
        if (pc->info.depthFunc == d3d12p::kCmpAlways && pc->kind != PsoKind::ShadowCaster) {
            // Depth written whatever was there: a quad that resets the region
            // (instead of ClearDepthStencilView). The casters that follow draw
            // into a fresh region, so Mario's shadow there won't pile up.
            ++s.refreshDraws;
            Rect rc = vp.Bounds();
            if (s.numScissor) {
                const Rect& sc = s.scissor[0];
                rc.left = std::max(rc.left, sc.left);
                rc.top = std::max(rc.top, sc.top);
                rc.right = std::min(rc.right, sc.right);
                rc.bottom = std::min(rc.bottom, sc.bottom);
            }
            if (s.prepared.size() < 256) {
                Prepared p;
                p.resource = s.ds.resource;
                p.slice = s.ds.firstSlice;
                p.rc = rc;
                p.full = s.numVp == 0;
                p.byDraw = true;
                s.prepared.push_back(p);
            }
            ShadowSeg& sh = s.sseg;
            if (sh.active && sh.ds.resource == s.ds.resource && sh.ds.firstSlice == s.ds.firstSlice && sh.vp == vp &&
                sh.casterDraws == 0)
                sh.prepared = true;
            return;
        }
        ++s.casterDraws;
        ShadowSeg& sh = s.sseg;
        if (sh.active && (sh.ds.resource != s.ds.resource || sh.ds.firstSlice != s.ds.firstSlice || !(sh.vp == vp)))
            sh.active = false; // region changed without a boundary we saw: start over, no injection
        if (!sh.active) {
            sh = ShadowSeg();
            sh.active = true;
            sh.ds = s.ds;
            sh.dsv = s.dsv;
            sh.vp = vp;
            sh.scissor = s.numScissor ? s.scissor[0] : vp.Bounds();
            const Rect vr = vp.Bounds();
            for (const Prepared& p : s.prepared) {
                if (p.resource != s.ds.resource || p.slice != s.ds.firstSlice) continue;
                if (Covers(p, vr) || (s.numScissor && Covers(p, s.scissor[0]))) {
                    sh.prepared = true;
                    if (p.byCopy) sh.byCopy = true;
                }
            }
        }
        {
            uint32_t nrt = 0;
            for (uint32_t i = 0; i < s.numRt && i < 8; ++i)
                if (s.rt[i].valid) ++nrt;
            sh.colorTargets = std::max(sh.colorTargets, nrt);
        }
        // Only a recognised caster or a draw with the view constants
        // reachable gives us the light's view.
        uint64_t cbv = 0;
        const uint64_t table = ViewAddress(s, descStride, cbv);
        if (!table && !cbv) return;
        ++sh.casterDraws;
        sh.pso = pc;
        sh.rootSig = s.rootSig;
        sh.layout = s.layout;
        sh.viewTable = table;
        sh.viewCbv = cbv;
        sh.viewFromCopy = false;
        sh.heaps[0] = s.heaps[0];
        sh.heaps[1] = s.heaps[1];
        sh.numHeaps = s.numHeaps;
        if (pc->kind == PsoKind::ShadowCaster) reg_->NoteCaster(pc);
        break;
    }
    case PsoKind::CacheCopy: {
        ++s.copyDraws;
        if (!s.ds.valid) return;
        reg_->MarkWorking(s.ds.resource);
        if (s.prepared.size() < 256) {
            Prepared p;
            p.resource = s.ds.resource;
            p.slice = s.ds.firstSlice;
            p.rc = s.numVp ? s.vp[0].Bounds() : Rect();
            p.full = s.numVp == 0;
            p.byCopy = true;
            s.prepared.push_back(p);
        }
        // The region is refreshed from the cache: it gets Mario's shadow even
        // if no dynamic caster follows (Spider-Man is hidden), with the light
        // view bound for the copy if there is one (checked on the GPU: it
        // must be an orthographic light view).
        {
            ShadowSeg& sh = s.sseg;
            const Viewport vp = s.numVp ? s.vp[0] : Viewport();
            uint64_t cbv = 0;
            const uint64_t table = ViewAddress(s, descStride, cbv);
            if (!sh.active || sh.ds.resource != s.ds.resource || sh.ds.firstSlice != s.ds.firstSlice || !(sh.vp == vp)) {
                sh = ShadowSeg();
                sh.active = true;
                sh.ds = s.ds;
                sh.dsv = s.dsv;
                sh.vp = vp;
                sh.scissor = s.numScissor ? s.scissor[0] : vp.Bounds();
                sh.prepared = true;
                sh.byCopy = true;
                if (table || cbv) {
                    sh.viewTable = table;
                    sh.viewCbv = cbv;
                    sh.viewFromCopy = true;
                    sh.rootSig = s.rootSig;
                    sh.layout = s.layout;
                    sh.heaps[0] = s.heaps[0];
                    sh.heaps[1] = s.heaps[1];
                    sh.numHeaps = s.numHeaps;
                    sh.pso = reg_->LastCaster(s.ds.format);
                }
            } else {
                sh.prepared = true;
                sh.byCopy = true;
            }
        }
        break;
    }
    case PsoKind::CacheMove:
        if (s.ds.valid) reg_->MarkCache(s.ds.resource);
        break;
    }
}

void Policy::OnClearDepth(ListState& s, const ViewInfo& dsv, const Rect* rects, uint32_t numRects) {
    if (!dsv.valid) return;
    if (numRects == 0 || !rects) {
        if (s.prepared.size() < 256) {
            Prepared p;
            p.resource = dsv.resource;
            p.slice = dsv.firstSlice;
            p.full = true;
            s.prepared.push_back(p);
        }
        return;
    }
    for (uint32_t i = 0; i < numRects && s.prepared.size() < 256; ++i) {
        Prepared p;
        p.resource = dsv.resource;
        p.slice = dsv.firstSlice;
        p.rc = rects[i];
        s.prepared.push_back(p);
    }
}

void Policy::EndGBuffer(ListState& s, std::vector<Decision>& out, std::vector<Skip>* skips) {
    GBufferSeg& g = s.gseg;
    if (!g.active) return;
    g.active = false;
    if (g.draws == 0) return;
    if (g.markerMatch) reg_->NoteMarkerMatch(s.frame);
    const char* why = nullptr;
    if (!cfg_.gbuffer) why = "G-buffer injection disabled";
    else if (!g.depthWrite) why = "no depth writes in this G-buffer segment";
    else if (!g.ds.valid) why = "unknown depth target";
    else if (g.ds.dsvFlags & 1) why = "read-only depth target";
    else if (!g.viewTable && !g.viewCbv) why = "view constants (b0) not reachable";
    else if (!reg_->IsMainSize(g.rt[0].MipWidth(), g.rt[0].MipHeight())) why = "not the main view";
    else if (!cfg_.gbufferMarkers.empty() && reg_->MarkersRequired(s.frame) && !g.markerMatch)
        why = "outside the chosen PIX marker";
    if (why) {
        if (skips) {
            Skip k;
            k.why = why;
            skips->push_back(k);
        }
        return;
    }
    Decision d;
    d.act = Act::GBuffer;
    d.g = g;
    out.push_back(d);
    reg_->SetMainDepthTarget(g.rt[0].resource, g.rt[0].Subresource());
    // RT1 holds the motion vectors (DXGI_FORMAT_R16G16_FLOAT, same size).
    const ViewInfo& m = g.rt[1];
    const bool motion = g.numRt >= 2 && m.valid && m.format == 34 && m.MipWidth() == g.rt[0].MipWidth() &&
                        m.MipHeight() == g.rt[0].MipHeight();
    reg_->SetMainMotionTarget(motion ? m.resource : 0, motion ? m.Subresource() : 0);
}

void Policy::EndShadow(ListState& s, std::vector<Decision>& out, std::vector<Skip>* skips) {
    ShadowSeg& sh = s.sseg;
    if (!sh.active) return;
    sh.active = false;
    if (sh.casterDraws == 0 && !sh.viewFromCopy) return;
    const Registry::MapKind kind = reg_->Kind(sh.ds.resource);
    // Regions the game draws casters into (nearly) every frame.
    const bool steady = sh.casterDraws > 0 &&
                        reg_->NoteCasterRegion(s.frame, sh.ds.resource, sh.ds.firstSlice, sh.vp.Bounds()) >=
                            Registry::kSteadyFrames;
    const char* why = nullptr;
    const uint32_t w = sh.ds.MipWidth(), h = sh.ds.MipHeight();
    // D32_FLOAT_S8X24_UINT / D24_UNORM_S8_UINT: the game's view depth buffers, not its shadow atlases.
    const bool stencil = sh.ds.format == 20 || sh.ds.format == 45;
    if (!cfg_.shadows) why = "shadow injection disabled";
    else if (sh.colorTargets) why = "colour targets bound with the depth (not a shadow map)";
    else if (w < 256 || h < 256) why = "depth target too small for a shadow map";
    else if (reg_->IsScreenShaped(w, h)) why = "screen-sized depth target (not a shadow map)";
    else if (!sh.prepared && !((cfg_.steadyRegions || steady_.load(std::memory_order_relaxed)) && steady))
        why = "shadow region not refreshed in this list (cached?)";
    else if (!sh.prepared && stencil) why = "depth-stencil target redrawn every frame (not a shadow map)";
    else if (kind == Registry::MapKind::Cache) why = "shadow cache texture";
    else if (!sh.byCopy && kind != Registry::MapKind::Working && !steady)
        why = "depth target that is not a known shadow map (not redrawn every frame)";
    else if (!sh.viewTable && !sh.viewCbv) why = "light view constants not reachable";
    else if (!sh.pso) why = "no shadow caster pipeline known for this depth format yet";
    else if (!reg_->TryClaimShadow(s.frame, sh.ds.resource, sh.ds.firstSlice, sh.vp.Bounds(), cfg_.maxShadowsPerFrame))
        why = "too many shadow views this frame";
    if (why) {
        if (skips) {
            Skip k;
            k.why = why;
            k.shadow = true;
            k.ds = sh.ds;
            k.vp = sh.vp;
            k.casters = sh.casterDraws;
            skips->push_back(k);
        }
        return;
    }
    Decision d;
    d.act = Act::Shadow;
    d.s = sh;
    out.push_back(d);
}

void Policy::AtBoundary(ListState& s, Boundary b, std::vector<Decision>& out, std::vector<Skip>* skips) {
    switch (b) {
    case Boundary::Viewports:
        EndShadow(s, out, skips);
        break;
    default:
        EndGBuffer(s, out, skips);
        EndShadow(s, out, skips);
        break;
    }
}

std::string DecodePixEvent(uint32_t metadata, const void* data, uint32_t size) {
    std::string out;
    if (!data || size == 0) return out;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    auto printable = [](uint32_t c) { return c >= 32 && c < 127; };
    if (metadata == 1) { // WINPIX_EVENT_ANSI_VERSION
        for (uint32_t i = 0; i < size && p[i] && out.size() < 47; ++i)
            if (printable(p[i])) out.push_back(char(p[i]));
        return out;
    }
    if (metadata == 0) { // WINPIX_EVENT_UNICODE_VERSION
        for (uint32_t i = 0; i + 1 < size && out.size() < 47; i += 2) {
            const uint32_t c = uint32_t(p[i]) | (uint32_t(p[i + 1]) << 8);
            if (!c) break;
            if (printable(c)) out.push_back(char(c));
        }
        return out;
    }
    // PIX3 blob: three 64-bit headers, then the format string packed into
    // 64-bit words (1 or 2 bytes per character), then arguments.
    uint32_t zeros = 0;
    for (uint32_t i = size > 24 ? 24 : 0; i < size && out.size() < 47; ++i) {
        if (p[i] == 0) {
            if (!out.empty() && ++zeros >= 4) break;
            continue;
        }
        zeros = 0;
        if (!printable(p[i])) {
            if (!out.empty()) break;
            continue;
        }
        out.push_back(char(p[i]));
    }
    return out;
}

} // namespace sm2m::frame
