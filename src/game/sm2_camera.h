// Finds and follows the game's camera. Sources, in order of preference:
//   1. [Camera] signature in bindings.ini (once someone reverse-engineers it)
//   2. an actor whose name matches Camera.ActorName in the config
//   3. a background scan of process memory (camera_finder.h heuristics)
// The field of view comes from a scanned projection matrix, or the config.
#pragma once

#ifdef _WIN32

#include <atomic>
#include <string>
#include <vector>

#include "../common/ini.h"
#include "../common/platform.h"
#include "camera_finder.h"
#include "sm2.h"

namespace sm2m {

struct CameraConfig {
    std::string source = "auto";    // auto | signature | actor | scan
    std::string actorName;          // substring, case-insensitive
    float fallbackFovDeg = 60.0f;   // vertical
    bool useScannedFov = true;
    float lookAtHeight = 1.0f;
    float minDistance = 0.6f;
    float maxDistance = 14.0f;
    size_t maxRegionMB = 1024;
    int scanThreads = 3;
};

class CameraTracker {
public:
    ~CameraTracker();
    void Configure(const CameraConfig& cfg, const Ini& bindings, Sm2Game* game);

    // Call once per frame. `aspect` = back buffer width/height.
    void Update(const DVec3& heroPos, const Vec3& worldUp, float aspect);

    bool Valid() const { return pose_.valid; }
    const CameraPose& Pose() const { return pose_; }
    float FovY() const;
    const std::string& Source() const { return sourceName_; }
    bool Scanning() const { return scanRunning_.load(); }
    bool HandednessKnown() const { return handKnown_; }
    bool LeftHanded() const { return leftHanded_; }
    int CandidateCount() const { return int(cams_.size()); }
    int CandidateIndex() const { return selected_; }

    void RequestRescan() { rescanRequested_ = true; }
    int ScanCount() const { return scanCount_; }
    uintptr_t SelectedAddress() const {
        return selected_ >= 0 && selected_ < int(cams_.size()) ? cams_[selected_].address : 0;
    }
    void CycleCandidate(int dir);
    void AdjustFovOffset(float deg) { fovOffsetDeg_ += deg; }
    float FovOffsetDeg() const { return fovOffsetDeg_; }

private:
    struct ScanJob {
        DVec3 hero;
        Vec3 up;
        float aspect;
        CameraSearchParams params;
        size_t maxRegionBytes;
        int threads;
        std::vector<CameraCandidate> cams;
        std::vector<ProjectionCandidate> projs;
    };
    static DWORD WINAPI ScanThread(void* self);
    void RunScan(ScanJob& job);
    void StartScan(const DVec3& hero, const Vec3& up, float aspect);
    bool TrySignature(const DVec3& hero, const Vec3& up);
    bool TryActor(const DVec3& hero, const Vec3& up);
    void TrackCandidates(const DVec3& hero, const Vec3& up);
    void TrackFov();

    CameraConfig cfg_;
    Sm2Game* game_ = nullptr;
    CameraSearchParams params_;
    CameraPose pose_;
    std::string sourceName_ = "none";
    bool handKnown_ = false;
    bool leftHanded_ = false;

    // signature source
    uintptr_t sigBase_ = 0;
    std::vector<int64_t> sigDerefs_;
    int64_t sigMatrixOffset_ = 0;
    MatrixLayout sigLayout_ = MatrixLayout::RowsAxes;
    int64_t sigFovOffset_ = -1;
    bool sigFovDegrees = false;
    bool sigAxesKnown_ = false;
    CameraCandidate sigCand_;

    // actor source
    uintptr_t camActor_ = 0;
    CameraCandidate actorCand_;
    double nextActorSearch_ = 0;

    // scan source
    std::vector<CameraCandidate> cams_;
    std::vector<ProjectionCandidate> projs_;
    int selected_ = -1;
    float scannedFov_ = 0;
    int liveProj_ = -1;
    std::atomic<bool> scanRunning_{false};
    std::atomic<bool> scanDone_{false};
    ScanJob* job_ = nullptr;
    HANDLE thread_ = nullptr;
    bool rescanRequested_ = false;
    double lastScanTime_ = -1e9;
    int invalidFrames_ = 0;
    float fovOffsetDeg_ = 0;
    int scanCount_ = 0;
};

} // namespace sm2m

#endif
