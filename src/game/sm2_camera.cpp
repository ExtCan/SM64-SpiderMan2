#ifdef _WIN32

#include "sm2_camera.h"

#include <algorithm>
#include <cmath>

#include "../common/log.h"

namespace sm2m {

namespace {
constexpr float kPi = 3.14159265358979f;

struct ScanShared {
    std::vector<std::pair<uintptr_t, size_t>> regions;
    std::atomic<size_t> next{0};
    DVec3 hero;
    Vec3 up;
    float aspect = 1.0f;
    CameraSearchParams params;
    Mutex mutex;
    std::vector<CameraCandidate>* cams = nullptr;
    std::vector<ProjectionCandidate>* projs = nullptr;
    std::atomic<uint64_t> bytes{0};
};

DWORD WINAPI ScanWorker(void* arg) {
    auto* s = static_cast<ScanShared*>(arg);
    const size_t chunk = 4u << 20;
    std::vector<uint8_t> buf(chunk + 64);
    std::vector<CameraCandidate> cams;
    std::vector<ProjectionCandidate> projs;
    for (;;) {
        size_t idx = s->next.fetch_add(1);
        if (idx >= s->regions.size()) break;
        const uintptr_t base = s->regions[idx].first;
        const size_t size = s->regions[idx].second;
        for (size_t off = 0; off < size; off += chunk) {
            size_t want = std::min(chunk + 64, size - off);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base + off), buf.data(), want, &got) ||
                got < 64)
                continue;
            s->bytes += got;
            if (cams.size() < 512)
                ScanSpanForCameras(buf.data(), got, base + off, s->hero, s->up, s->params, cams, 512);
            if (projs.size() < 256) ScanSpanForProjections(buf.data(), got, base + off, s->aspect, projs, 256);
        }
    }
    LockGuard lock(s->mutex);
    s->cams->insert(s->cams->end(), cams.begin(), cams.end());
    s->projs->insert(s->projs->end(), projs.begin(), projs.end());
    return 0;
}

bool ReadPointerChain(uintptr_t base, const std::vector<int64_t>& chain, uintptr_t& out) {
    uintptr_t a = base;
    for (int64_t off : chain) {
        uintptr_t next = 0;
        if (!SafeReadT(a + uintptr_t(off), next) || !LooksLikePointer(next)) return false;
        a = next;
    }
    out = a;
    return true;
}
} // namespace

CameraTracker::~CameraTracker() {
    if (thread_) {
        WaitForSingleObject(thread_, 10000);
        CloseHandle(thread_);
    }
    delete job_;
}

void CameraTracker::Configure(const CameraConfig& cfg, const Ini& b, Sm2Game* game) {
    cfg_ = cfg;
    game_ = game;
    params_.lookAtHeight = cfg.lookAtHeight;
    params_.minDistance = cfg.minDistance;
    params_.maxDistance = cfg.maxDistance;

    if (game && !b.GetString("Camera", "pattern").empty()) {
        std::string detail;
        sigBase_ = game->ResolveSection(b, "Camera", detail);
        LOGI("camera signature: %s", sigBase_ ? detail.c_str() : ("unresolved: " + detail).c_str());
        for (const auto& item : Ini::SplitList(b.GetString("Camera", "pointer_chain"))) {
            int64_t v;
            if (Ini::ParseInt(item, v)) sigDerefs_.push_back(v);
        }
        sigMatrixOffset_ = b.GetInt("Camera", "matrix_offset", 0);
        sigLayout_ = Ini::Lower(b.GetString("Camera", "layout", "rows")) == "columns" ? MatrixLayout::ColumnsAxes
                                                                                      : MatrixLayout::RowsAxes;
        sigFovOffset_ = b.GetInt("Camera", "fov_offset", -1);
        sigFovDegrees = Ini::Lower(b.GetString("Camera", "fov_units", "radians")) == "degrees";
    }
}

float CameraTracker::FovY() const {
    float fov = cfg_.fallbackFovDeg * kPi / 180.0f;
    if (cfg_.useScannedFov && scannedFov_ > 0.2f) fov = scannedFov_;
    if (sourceName_ == "signature" && sigFovOffset_ >= 0 && pose_.fovY > 0.2f) fov = pose_.fovY;
    fov += fovOffsetDeg_ * kPi / 180.0f;
    return Clamp(fov, 0.2f, 2.8f);
}

bool CameraTracker::TrySignature(const DVec3& hero, const Vec3& up) {
    if (!sigBase_) return false;
    uintptr_t obj;
    if (!ReadPointerChain(sigBase_, sigDerefs_, obj)) return false;
    float f[16];
    if (!SafeRead(obj + uintptr_t(sigMatrixOffset_), f, sizeof(f))) return false;
    CameraPose pose;
    if (!sigAxesKnown_) {
        CameraSearchParams loose = params_;
        loose.maxDistance *= 2;
        loose.minLookDot = 0.6f;
        if (!EvaluateCameraMatrix(f, sigLayout_, hero, up, loose, sigCand_, pose)) return false;
        sigAxesKnown_ = true;
        leftHanded_ = CameraIsLeftHanded(sigCand_, f);
        handKnown_ = true;
    }
    if (!PoseFromCandidate(f, sigCand_, params_, pose)) return false;
    if (sigFovOffset_ >= 0) {
        float fov = 0;
        if (SafeReadT(obj + uintptr_t(sigFovOffset_), fov) && std::isfinite(fov))
            pose.fovY = sigFovDegrees ? fov * kPi / 180.0f : fov;
    }
    pose_ = pose;
    sourceName_ = "signature";
    return true;
}

bool CameraTracker::TryActor(const DVec3& hero, const Vec3& up) {
    if (cfg_.actorName.empty() || !game_ || !game_->PoolAvailable()) return false;
    const double now = NowSeconds();
    if (!camActor_ && now >= nextActorSearch_) {
        nextActorSearch_ = now + 2.0;
        const std::string needle = Ini::Lower(cfg_.actorName);
        const uint32_t n = game_->PoolCount();
        for (uint32_t i = 0; i < n; ++i) {
            uintptr_t a = game_->PoolSlot(i);
            if (!a) continue;
            std::string name = Ini::Lower(game_->ActorName(a));
            if (name.find(needle) == std::string::npos) continue;
            float f[16];
            CameraPose pose;
            CameraCandidate c;
            if (game_->GetMatrix(a, f) && EvaluateCameraMatrix(f, MatrixLayout::RowsAxes, hero, up, params_, c, pose)) {
                camActor_ = a;
                actorCand_ = c;
                leftHanded_ = CameraIsLeftHanded(c, f);
                handKnown_ = true;
                LOGI("camera: using actor #%u '%s'", i, game_->ActorName(a).c_str());
                break;
            }
        }
    }
    if (!camActor_) return false;
    float f[16];
    CameraPose pose;
    if (!game_->GetMatrix(camActor_, f) || !PoseFromCandidate(f, actorCand_, params_, pose)) {
        camActor_ = 0;
        return false;
    }
    pose_ = pose;
    sourceName_ = "actor";
    return true;
}

void CameraTracker::StartScan(const DVec3& hero, const Vec3& up, float aspect) {
    if (scanRunning_) return;
    delete job_;
    job_ = new ScanJob();
    job_->hero = hero;
    job_->up = up;
    job_->aspect = aspect;
    job_->params = params_;
    job_->maxRegionBytes = cfg_.maxRegionMB << 20;
    job_->threads = std::max(1, std::min(cfg_.scanThreads, 8));
    scanRunning_ = true;
    scanDone_ = false;
    ++scanCount_;
    lastScanTime_ = NowSeconds();
    if (thread_) {
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    thread_ = CreateThread(nullptr, 0, &CameraTracker::ScanThread, this, 0, nullptr);
    if (!thread_) {
        scanRunning_ = false;
        LOGE("camera: could not start scan thread");
    }
}

DWORD WINAPI CameraTracker::ScanThread(void* self) {
    auto* t = static_cast<CameraTracker*>(self);
    t->RunScan(*t->job_);
    t->scanDone_ = true;
    t->scanRunning_ = false;
    return 0;
}

void CameraTracker::RunScan(ScanJob& job) {
    const double t0 = NowSeconds();
    ScanShared shared;
    shared.hero = job.hero;
    shared.up = job.up;
    shared.aspect = job.aspect;
    shared.params = job.params;
    shared.cams = &job.cams;
    shared.projs = &job.projs;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    size_t total = 0;
    while (addr < 0x7FFFFFFF0000ull &&
           VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        const bool rw = (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
        const bool bad = (prot & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE | PAGE_NOACCESS)) != 0;
        if (mbi.State == MEM_COMMIT && rw && !bad && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
            size <= job.maxRegionBytes) {
            shared.regions.push_back({base, size});
            total += size;
        }
        if (base + size <= addr) break;
        addr = base + size;
    }
    // Biggest regions first so threads finish together.
    std::sort(shared.regions.begin(), shared.regions.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<HANDLE> threads;
    for (int i = 0; i < job.threads; ++i) {
        HANDLE h = CreateThread(nullptr, 0, ScanWorker, &shared, 0, nullptr);
        if (h) threads.push_back(h);
    }
    if (threads.empty()) ScanWorker(&shared);
    for (HANDLE h : threads) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    std::sort(job.cams.begin(), job.cams.end(),
              [](const CameraCandidate& a, const CameraCandidate& b) { return a.score > b.score; });
    LOGI("camera scan: %zu regions, %.0f MB read in %.2fs -> %zu camera, %zu projection candidates",
         shared.regions.size(), double(shared.bytes.load()) / 1048576.0, NowSeconds() - t0, job.cams.size(),
         job.projs.size());
    (void)total;
}

void CameraTracker::CycleCandidate(int dir) {
    if (cams_.empty()) return;
    const int n = int(cams_.size());
    selected_ = ((selected_ < 0 ? 0 : selected_) + dir % n + n) % n;
    LOGI("camera: candidate %d/%d at %p", selected_ + 1, n, reinterpret_cast<void*>(cams_[selected_].address));
}

void CameraTracker::TrackCandidates(const DVec3& hero, const Vec3& up) {
    const DVec3 target = hero + DVec3(up) * double(params_.lookAtHeight);
    int best = -1;
    float bestScore = -1e9f;
    for (size_t i = 0; i < cams_.size(); ++i) {
        CameraCandidate& c = cams_[i];
        float f[16];
        CameraPose p;
        bool ok = SafeRead(c.address, f, sizeof(f)) && PoseFromCandidate(f, c, params_, p);
        if (ok) {
            DVec3 d = target - p.position;
            double dist = Length(d);
            Vec3 dir = (d * (1.0 / std::max(dist, 1e-6))).ToFloat();
            ok = dist > params_.minDistance * 0.5 && dist < params_.maxDistance * 1.5 && Dot(p.forward, dir) > 0.5f;
        }
        if (ok) {
            if (Length(p.position - c.lastPos) > 1e-3) c.movedFrames = std::min(c.movedFrames + 1, 100000);
            c.lastPos = p.position;
            c.validFrames = c.validFrames < 0 ? 1 : std::min(c.validFrames + 1, 100000);
            float s = c.score + 0.002f * float(std::min(c.movedFrames, 200));
            if (s > bestScore) {
                bestScore = s;
                best = int(i);
            }
        } else {
            c.validFrames = std::min(c.validFrames, 0) - 1; // negative = consecutive invalid frames
        }
    }
    // Drop candidates that stayed invalid for ~4 s at 30 fps+.
    for (size_t i = 0; i < cams_.size();) {
        if (cams_[i].validFrames < -240 && int(i) != selected_) {
            if (selected_ > int(i)) --selected_;
            cams_.erase(cams_.begin() + long(i));
            if (best > int(i)) --best;
            else if (best == int(i)) best = -1;
        } else {
            ++i;
        }
    }
    const bool selectedOk = selected_ >= 0 && selected_ < int(cams_.size()) && cams_[selected_].validFrames > 0;
    if (!selectedOk) selected_ = best;
    if (selected_ >= 0) {
        float f[16];
        CameraPose p;
        const CameraCandidate& c = cams_[selected_];
        if (SafeRead(c.address, f, sizeof(f)) && PoseFromCandidate(f, c, params_, p) && c.validFrames > 0) {
            pose_ = p;
            sourceName_ = "scan";
            if (!handKnown_) {
                leftHanded_ = CameraIsLeftHanded(c, f);
                handKnown_ = true;
                LOGI("camera: world looks %s-handed", leftHanded_ ? "left" : "right");
            }
            invalidFrames_ = 0;
            return;
        }
    }
    pose_.valid = false;
    ++invalidFrames_;
}

void CameraTracker::TrackFov() {
    if (projs_.empty()) return;
    for (int attempt = 0; attempt < int(projs_.size()); ++attempt) {
        if (liveProj_ < 0 || liveProj_ >= int(projs_.size())) liveProj_ = 0;
        float f[16];
        ProjectionCandidate c;
        // Aspect is re-validated against whatever the matrix says; the finder
        // only accepted matrices that matched at scan time.
        if (SafeRead(projs_[liveProj_].address, f, sizeof(f)) && f[0] > 0.1f &&
            EvaluateProjection(f, f[5] / f[0], c) && std::fabs(c.fovY - scannedFov_) < 0.6f) {
            scannedFov_ = c.fovY;
            return;
        }
        liveProj_ = (liveProj_ + 1) % int(projs_.size());
    }
}

void CameraTracker::Update(const DVec3& hero, const Vec3& up, float aspect) {
    const std::string& src = cfg_.source;
    if ((src == "auto" || src == "signature") && TrySignature(hero, up)) {
        if (sigFovOffset_ < 0) TrackFov();
        return;
    }
    if ((src == "auto" || src == "actor") && TryActor(hero, up)) {
        TrackFov();
        return;
    }
    if (src == "signature" || src == "actor") {
        pose_.valid = false;
        return;
    }

    if (scanDone_.exchange(false) && job_) {
        cams_ = std::move(job_->cams);
        projs_ = std::move(job_->projs);
        selected_ = -1;
        if (!projs_.empty()) {
            scannedFov_ = MostCommonFov(projs_);
            // Keep only projections that agree with the dominant FOV.
            std::vector<ProjectionCandidate> keep;
            for (const auto& p : projs_)
                if (std::fabs(p.fovY - scannedFov_) < 0.01f) keep.push_back(p);
            projs_.swap(keep);
            liveProj_ = 0;
            LOGI("camera: projection FOV %.2f deg (%zu matrices)", scannedFov_ * 180.0f / kPi, projs_.size());
        }
    }
    const double now = NowSeconds();
    const bool needScan = rescanRequested_ || (cams_.empty() && now - lastScanTime_ > 8.0) ||
                          (invalidFrames_ > 90 && now - lastScanTime_ > 8.0);
    if (needScan && !scanRunning_) {
        rescanRequested_ = false;
        invalidFrames_ = 0;
        StartScan(hero, up, aspect);
    }
    TrackCandidates(hero, up);
    TrackFov();
}

} // namespace sm2m

#endif
