// The mod: owns every subsystem and runs the per-frame logic from the
// Present hook.
#pragma once

#ifdef _WIN32

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../audio/audio.h"
#include "../common/ini.h"
#include "../game/sm2.h"
#include "../game/sm2_camera.h"
#include "../game/sm2_hero.h"
#include "../game/sm2_physics.h"
#include "../input/input.h"
#include "../render/injector.h"
#include "../render/overlay.h"
#include "../render/renderer.h"
#include "../render/view_constants.h"
#include "../sm64/mario.h"
#include "../sm64/sm64_api.h"
#include "../win/paths.h"
#include "../world/collision.h"
#include "../world/coords.h"
#include "../world/physics_world.h"
#include "../world/world_model.h"
#include "camera_lead.h"
#include "camera_override.h"
#include "cheats.h"
#include "config.h"
#include "follow_monitor.h"
#include "settings.h"

namespace sm2m {

class MarioMod {
public:
    static MarioMod& Get();
    // Idempotent. Spawns the init thread. `fromScriptEnable`: called by the
    // scripts loader on the game's main thread (else a fallback start).
    void Start(HMODULE self, bool fromScriptEnable);

    void OnPresent(IDXGISwapChain* sc);
    // The game window is being destroyed (the game is quitting).
    void OnGameClosing();
    void OnResizeBegin(IDXGISwapChain* sc);
    void OnColorSpace(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE cs);

private:
    struct Enemy {
        uintptr_t actor = 0;
        uint32_t serial = 0;   // its handle (changes when the slot is reused)
        uintptr_t health = 0;
        std::string name;
        bool person = false;   // a pedestrian (the game's Pedestrian component): reacts, takes no damage
        int cooldown = 0;
        float kbRemaining = 0; // metres
        Vec3 kbDir;            // game space
        DVec3 lastPos;
    };

    static DWORD WINAPI InitThreadProc(void* self);
    void Init();
    void InitBody();
    void StopAfterCrash(const char* where, const std::string& what);
    void DrawFailureNotice(IDXGISwapChain* sc);
    void Breadcrumb(int bit, const char* text);
    void InitSm64();
    void Toast(const std::string& text, float seconds = 4.0f, uint32_t color = 0xFFFFFFFFu);

    void Frame(IDXGISwapChain* sc, double dt, double now);
    void Activate();
    void Deactivate(const char* reason);
    bool SpawnAt(const DVec3& feet, float faceAngle);
    void Respawn(const DVec3& feet, const char* why, bool quiet = false);
    void SimTick();
    void RebuildCollision();
    void UpdateCombat(const SM64MarioState& s, uint32_t prevAction);
    void RefreshEnemies();
    void HandleIncomingDamage(const SM64MarioState& s);
    void TraceTick(const SM64MarioState& cur, const SM64MarioInputs& in);
    void RescueFromUnseenGround(const SM64MarioState& cur);
    // The log's account of bumps and falls: what Mario ran into (the walls
    // near him and where each came from), where a fall started and ended,
    // and what the game's answers said about the ground there.
    void NoteBumpsAndFalls(const SM64MarioState& prev, const SM64MarioState& cur, const SM64MarioInputs& in, double now);
    std::string DescribeObstacles(const Vec3& local, float faceAngle, double now);
    std::string DescribeGround(const Vec3& local, double now);
    // Where Mario's collision comes from.
    enum class Source { Physics, World, Flat };
    static const char* SourceName(Source s);
    Source ActiveSource() const;
    void UpdateCollisionSource();
    // Game physics: answers in, new rays out (every frame while Mario is on).
    void PumpPhysics(double now);
    HitKind ClassifyActor(uintptr_t actor, double now);
    void LogPhysicsStats();
    // The game's world is paused (its physics frame isn't stepping).
    void UpdateGamePause(double now);
    void UpdateWater();
    void HandleFall(const SM64MarioState& prev, const SM64MarioState& cur);
    void BuildDraw(float alpha, MarioDraw& draw);
    void PublishMario(float alpha, bool visible);
    void TakeGameViews();
    void CheckWorldRendering();
    bool HandednessKnown() const;
    bool LeftHanded() const;
    bool WantMirror() const;
    bool GameViewFresh() const;
    void StartWorldThread();
    static DWORD WINAPI WorldThreadProc(void* self);
    void WorldLoop();
    void SubmitWorldFrame(CaptureResult&& r);
    void ResetWorld(const DVec3& feet);
    void FillDrawMatrices(const WorldMapping& map, MarioDraw& draw);
    void BuildCalibration(MarioDraw& draw);
    void DrawHud();
    void DumpDebug();
    void BindHero(uintptr_t hero);
    void PinHero(uintptr_t hero, const DVec3& pin);
    void SampleFollow(const DVec3& heroBefore, bool heroRead);
    // Pulls the game camera along when it trails Mario's jumps; notes when it seems stuck.
    void UpdateCameraLead(const DVec3& mario, double now);
    void FallBackToRenderedWorld(const char* why);
    void LogFollowStats(bool final);
    void ScanEnemiesStep();
    float FaceAngleFromHero(uintptr_t hero) const;
    DVec3 MarioGamePos(float alpha) const;

    // Live settings (F8 menu, ModSettings' pause menu)
    void HandleMenu();
    void DrawMenu();
    void ApplyLive(const LiveSettings& before, const char* source);
    void SaveLive(const char* why);
    // Poses: the pose key and pedestrians asking for a picture.
    void TakePhotoRequests();
    void AnswerInteraction(double now);
    void UpdatePoses(const SM64MarioState& prev, const SM64MarioInputs& in);
    // Mario's facing (radians, libsm64's convention) toward a game-space point.
    float FaceToward(const DVec3& target) const;
    bool CameraPosition(DVec3& out) const;

    std::atomic<bool> started_{false};
    std::atomic<bool> initDone_{false};
    std::atomic<bool> failed_{false};   // a crash was caught: Mario Mode is off for this session
    std::atomic<bool> closing_{false};  // the game window is gone: do nothing more
    double failedAt_ = 0;
    bool failNoticeOk_ = true;          // false if the renderer itself was the problem
    uint32_t breadcrumbs_ = 0;
    HMODULE self_ = nullptr;
    ModPaths paths_;
    Ini configIni_, bindingsIni_;
    ModConfig cfg_;

    // libsm64
    Sm64Api api_;
    bool sm64Ready_ = false;
    std::string sm64Error_;
    std::vector<uint8_t> marioTexture_;
    bool textureHandedToRenderer_ = false;
    std::unique_ptr<MarioController> mario_;
    MarioGeometry drawGeo_;

    // game
    Sm2Game game_;
    CameraTracker camera_;
    HeroController hero_;
    std::unique_ptr<FlatGroundRaycaster> flatRays_;
    Source source_ = Source::Flat;        // the configured collision source (that is available)
    Source activeSource_ = Source::Flat;  // what the last rebuild used (see ActiveSource)
    bool sourceChosen_ = false;           // activeSource_ is set (a rebuild happened since activation)

    // world
    WorldMapping map_;
    CollisionBuilder builder_;
    std::vector<SM64Surface> surfaces_;
    std::vector<SurfaceOrigin> surfaceOrigins_; // where each of surfaces_ came from (the game's physics only)
    CollisionStats collisionStats_;
    // Bumps and falls for the log (NoteBumpsAndFalls).
    int bumpLogs_ = 0, fallLogs_ = 0, vanishLogs_ = 0;
    double lastBumpLog_ = -1e9;
    bool fallTracking_ = false;  // in the air since fallStart_
    DVec3 fallStart_;            // game space
    double fallStartTime_ = 0;
    bool fallJumped_ = false;    // ... by a jump (else he walked off, or the floor went)
    bool fallLongLogged_ = false;
    std::string fallStartGround_;
    Vec3 lastBuildCenter_;
    double lastBuildTime_ = -1e9;
    bool forceRebuild_ = false;

    // systems
    InputManager input_;
    AudioOut audio_;
    Renderer renderer_;
    Overlay overlay_;

    // in-world rendering
    Injector injector_;
    bool worldRender_ = false;      // Mario is drawn inside the game's frame (else: overlay)
    bool worldFallbackDone_ = false;
    bool reportedOnce_ = false;
    double activatedAt_ = 0;
    uint64_t frameId_ = 1;
    uint64_t injectionsAtActivate_ = 0;
    uint64_t shadowInjectionsAtActivate_ = 0, shadowRegionsAtActivate_ = 0;
    double lastInjectionSeen_ = 0;
    uint64_t lastInjectionCount_ = 0;
    // the game's own view, read back from its view constants
    ViewConstants gameView_;
    double gameViewTime_ = -1e9;
    uint64_t gameViewFrame_ = 0;
    uint64_t depthFrames_ = 0;
    uint64_t depthFramesAtActivate_ = 0;
    double lastDepthFrameTime_ = -1e9;
    bool cutPending_ = true;        // next published Mario has no motion vectors

    // collision from the game's physics
    PhysicsWorld physics_;
    bool physicsLive_ = false;      // the ground under Mario was answered since activation
    bool physicsChanged_ = false;   // something near Mario changed: rebuild soon
    double lastPhysicsAnswer_ = -1e9;
    double lastPhysicsLog_ = 0;
    uint64_t physicsCallsAtActivate_ = 0, physicsStepsAtActivate_ = 0; // the frame hook's heartbeat at M
    int waterLevelLocal_ = -11000;  // Mario's water level (local units; SM64's "none" = -11000)
    std::vector<GameRay> physRays_;
    std::vector<game_physics::RawResult> physAnswers_;
    struct ActorKind {
        uint32_t serial = 0;
        bool character = false;
        DVec3 pos;
        double checked = 0, movedAt = -1e9, seen = 0;
    };
    std::unordered_map<uintptr_t, ActorKind> actorKinds_;
    std::vector<std::string> loggedActors_;
    std::vector<int> loggedMaterials_; // floor materials Mario walked on (logged once each)
    int burningActorLogs_ = 0;         // actors that said their surface is acid / lava (logged)
    int lavaLogs_ = 0;                 // landings on lava (logged)
    // the game's pause, from its physics frame
    bool gamePaused_ = false;
    uint64_t lastSteps_ = 0;
    double lastStepTime_ = 0;
    int presentsSinceStep_ = 0;
    bool stepsSeen_ = false;
    int pausesLogged_ = 0;

    // collision from the game's rendered depth
    WorldModel world_{1};
    std::unique_ptr<WorldModelRaycaster> worldRays_;
    Mutex worldMu_;
    Mutex jobMu_;
    bool jobPending_ = false;
    DepthFrame job_;
    DVec3 jobFocus_;
    bool jobMarioDrawn_ = false;     // Mario's bounds in the job's frame are known
    DVec3 jobMarioLo_, jobMarioHi_;
    bool jobHeroVisible_ = false;    // Spider-Man may be in the depth too (hiding him failed)
    std::vector<Vec3> fallPath_;     // Mario's local positions on each tick of the current fall
    HANDLE jobEvent_ = nullptr;
    HANDLE worldThread_ = nullptr;
    std::atomic<double> worldIntegrateMs_{0};
    std::atomic<bool> worldBroken_{false};
    std::atomic<size_t> worldCells_{0};
    std::atomic<bool> worldChangedNear_{false}; // the world model removed something next to Mario
    std::atomic<bool> motionLogged_{false}, motionWarned_{false};

    // runtime state
    bool active_ = false;
    bool paused_ = false;
    bool mirrorDecided_ = false;
    double accumulator_ = 0;
    double lastFrameTime_ = 0;
    double lastHookPoll_ = 0;
    DVec3 lastPinned_;
    bool havePin_ = false;
    DVec3 lastSafe_;
    bool haveSafe_ = false;
    int airborneTicks_ = 0;
    int heroMissingFrames_ = 0;
    double prevResidual_ = 0;
    int pinDriftFrames_ = 0;
    bool pinWarned_ = false;
    FollowMonitor follow_;          // does the game camera follow Mario up and down?
    CameraLead cameraLead_;
    uint64_t leadViewFrame_ = 0;
    double leadPinUp_ = 0;          // Mario's height at the last Present (the camera seen next goes with it)
    bool haveLeadPin_ = false;
    struct CamSample {
        double t;
        DVec3 cam, mario;
    };
    std::vector<CamSample> camHistory_;
    int cameraStuckLogs_ = 0;
    double cameraStuckUntil_ = 0;
    uint64_t followViewFrame_ = 0;
    double lastJumpLog_ = -1e9;
    double lastPinStatsLog_ = 0;
    bool threadLogged_ = false;
    DWORD startThread_ = 0;
    uintptr_t boundHero_ = 0;
    uint32_t boundHeroSerial_ = 0;
    uint32_t scanCursor_ = 0;
    std::vector<uint8_t> poolChunk_;
    double apexUp_ = -1e30;
    bool wasAirborne_ = false;
    double deathTimer_ = 0;
    double lastHurtTime_ = 0;
    double lastRegen_ = 0;
    uint32_t tickCount_ = 0;
    bool debugOverlay_ = false;
    bool calibrate_ = false;
    WorldMapping calibMap_;
    MarioGeometry calibGeo_;

    // live settings
    LiveSettings live_;
    SettingsMenu menu_;
    bool menuCapture_ = false;      // the menu's keys are kept from the game (until released after closing)
    double menuCaptureUntil_ = 0;
    double saveDue_ = 0;            // > 0: write the user ini at this time (changes settle first)

    // cheats and poses
    Cheats cheats_;
    PoseController pose_;
    Pose pendingPose_ = Pose::None;
    float pendingFace_ = 0;
    bool pendingFromKey_ = false;   // the pose key (else a pedestrian): may interrupt Mario walking
    double pendingUntil_ = 0;
    int poseCycle_ = 0;             // the pose key cycles through the poses
    int photoCycle_ = 0;            // ... and so do photo requests
    double lastPhotoPose_ = -1e9;
    struct PhotoSeen {
        uint32_t handle;
        double time;
    };
    std::vector<PhotoSeen> photoSeen_;
    struct Ask {
        uint32_t handle = 0;
        DVec3 pos;
        double time = -1e9;
    } lastAsk_; // the latest pedestrian who asked for a picture
    int photoRequests_ = 0, photoPoses_ = 0;

    // combat
    std::vector<Enemy> enemies_;
    uintptr_t heroHealth_ = 0;
    float lastHeroHealth_ = -1;
    int hitsLanded_ = 0;
    int hitsTaken_ = 0;
    int peopleCooldown_ = 0;
    int candidatesFound_ = 0; // enemies and people found near Mario (this session)
    // Mario's frames against the game's (Injector::SetDrawLag): which one is
    // drawn in each game frame, so he moves in step with the camera.
    void MeasurePairing(uint64_t frame, const DVec3& view);
    void UpdateDrawLag(double now);
    std::string PairingSummary();
    uint64_t pairHist_[5] = {};  // drawn frame - the camera's plan: -2..+2 (since the last summary)
    uint64_t pairWin_[5] = {};   // ... since the last decision
    uint64_t pairAmbiguous_ = 0, pairNoMatch_ = 0;
    double pairLastEval_ = 0;
    uint64_t pairPrepared_ = 0, pairNotReady_ = 0;     // injector counts at the last decision
    uint64_t sumPrepared_ = 0, sumNotReady_ = 0, sumGpuBusy_ = 0, sumOlder_ = 0; // ... at the last summary
    int lagFloor_ = 0;           // 1 once the game was seen recording frames before Mario's was made
    // Spider-Man's stencil mark (stencil_census.h), for Mario's pixels too.
    void UpdateStencilMark(double now);
    void ApplyStencilSetting();
    std::string StencilStatus();
    int censusState_ = 0;        // this frame for the census: 0 not counted, 1 Spider-Man on screen, 2 Mario instead
    int StencilWindow() const;   // frames compared either side of a switch
    bool stencilDecided_ = false;
    int stencilSwitchesSeen_ = 0; // switches the census had counted at the last decision
    int stencilLogs_ = 0;
    uint8_t stencilRef_ = 0, stencilMask_ = 0; // the last decision's
    uintptr_t censusHero_ = 0;   // Spider-Man's actor, looked up now and then
    double censusHeroAt_ = -1;
    // When the game writes its camera against the mod's frames (for the log).
    uint64_t camWriteHist_[4] = {}; // frames with 0, 1, 2, 3+ placed writes
    uint64_t camAheadUs_ = 0, camAheadCount_ = 0, camCarried_ = 0;
    uint64_t camRefusedTotal_ = 0;  // writes left to the game
    uint32_t camAheadMaxUs_ = 0;
    uint32_t camWriteThread_ = 0, presentThread_ = 0;
    // Mario drawn where the camera of the view had him (Injector::SetPlacementSource):
    // the pivot of this frame's plan for his frame, and what the draws did (for the log).
    bool planPivotValid_ = false;
    DVec3 planPivot_;
    uint64_t alignViews_ = 0, alignMatched_ = 0;
    double alignShiftSum_ = 0, alignShiftMax_ = 0; // m
    bool Aligning() const;
    // camera override (camera_override.h)
    void UpdateCameraOverride(const DVec3& mario, double now);
    void ForgetCamera(const char* why, bool reject);
    void ReleaseCamera();
    CameraOverride camOverride_;
    bool camSearching_ = false;
    int camSearchFrames_ = 0, camSearchRounds_ = 0;
    uint64_t camCheckFrame_ = 0;
    int camMisses_ = 0, camWrong_ = 0;
    bool camGaveUp_ = false;
    double camSearchAfter_ = 0;           // no search before (after failed ones)
    bool camSearchWarned_ = false;
    int camFound_ = 0;                    // cameras found (this session)
    int camForgets_ = 0;                  // ... and given up on
    // the transform's writes: the last sequence seen, since when, and the view then
    uint32_t camSeenSeq_ = 0;
    double camSeqTime_ = 0;
    DVec3 camSeqView_;
    Vec3 camSeqForward_;
    double camAwaySince_ = -1;            // the game's camera not following Mario since
    double camRefusedSince_ = -1;         // its writes not like the view since
    float camBlend_ = 0;                  // the plan's blend last frame (0: not placing)
    std::vector<uintptr_t> camRejected_;  // transforms that turned out not to be the view's
    // The game's other cameras (0.6.1): it has several, and renders from one
    // or another - in flight a fifth of 0.6's frames came from one the mod
    // didn't place (hero_pin's camera slots; slot 0 is the one found first).
    static constexpr int kCamSlots = 4;   // (hero_pin::kCameraSlots)
    struct CamSeen {                      // a camera the game rendered views from, not placed yet
        uintptr_t transform = 0;
        int views = 0;                    // ... between views from the ones placed
        double last = 0;
    };
    std::vector<CamSeen> camSeen_;
    int camRow_ = 2;                      // the cameras' row along the view, and its direction (slot 0's)
    float camSign_ = 1.0f;
    int camActiveSlot_ = 0;               // the camera the views came from lately (its framing is learnt)
    double camActiveSince_ = -1;          // ... since when
    double camSlotViewAt_[kCamSlots] = {-1, -1, -1, -1};  // when a view last came from each
    double camSlotWriteAt_[kCamSlots] = {-1, -1, -1, -1}; // ... and the game last wrote it
    double camSlotAddedAt_[kCamSlots] = {-1, -1, -1, -1}; // ... and the mod began placing it
    double camPlacedViewAt_ = -1;         // when a view last came from one the mod places
    // Where the latest view from none of them came from, against Mario then:
    // watched for, carried along with him (hero_pin::SearchArea b).
    bool camElseValid_ = false;
    DVec3 camElseOffset_;
    Vec3 camElseForward_;
    double camElseAt_ = -1;
    // Mario (the camera's pivot) in the mod's last frames: a view's camera
    // against him then.
    struct MarioAt {
        uint64_t frame = 0;
        DVec3 pos;
    };
    MarioAt camMarioAt_[16];
    int camMarioNext_ = 0;
    DVec3 MarioAtFrame(uint64_t frame, const DVec3& fallback) const;
    bool camWatching_ = false;
    double camWatchPruneAt_ = 0;
    // For the log: views by how far from the mod's placements, and what became of the other cameras.
    uint64_t camViewsOurs_ = 0, camViewsElsewhere_[4] = {}, camViewsUnmatched_ = 0, camViewsGameOwn_ = 0;
    int camAdopted_ = 0, camPromoted_ = 0, camLetGo_ = 0; // since the last summary
    int camPlacedMost_ = 0;                               // ... and the most placed at once
    int camAdoptLogs_ = 0, camLayoutLogs_ = 0, camPromoteLogs_ = 0, camTraceLogs_ = 0;
    void WatchOtherCameras(const DVec3& view, const Vec3& forward, uint64_t viewFrame, double gap, double now);
    void MaintainCameraSlots(double now);
    void ClearCameraSlots();
    void ClearSlotTimes(int slot);
    int CamerasPlaced() const;
    // The game's field of view (log: it changing is the camera zooming), and
    // the frames presented against those the game rendered (frame generation).
    float fovMin_ = 1e9f, fovMax_ = 0, fovLast_ = 0;
    uint64_t fovJumps_ = 0, fovFrames_ = 0;
    uint64_t presentsAtSummary_ = 0, viewsRead_ = 0;
    struct CamPos {
        DVec3 placed, game; // the transform after the write (the mod's), and the game's own
        bool haveGame = false;
    };
    CamPos camPlaced_[12];
    int camPlacedNext_ = 0, camPlacedCount_ = 0;
    uint32_t camLastSeq_ = 0;
    bool camLogged_ = false;
    // How the view matched the last checks (for the log when the camera is dropped).
    double camLastPlacedGap_ = -1, camLastGameGap_ = -1;
    // How much of the time the camera was placed (Mario Mode on, the game running), for the log.
    double camStatTime_ = 0, camStatPlaced_ = 0, camStatLast_ = -1;
    int camStatFound_ = 0, camStatLost_ = 0;
    std::string camStatWhy_;
    // ... walls: the camera's rays to the game's physics (tags with this bit)
    static constexpr uint32_t kCameraRayTag = 0x80000000u;
    CameraOverride::Params CameraParams() const;
    void AcceptCameraRay(const game_physics::RawResult& a, double now);
    void CastCameraRays(const CameraOverride::Plan& plan, const CameraOverride::GameSample& sample);
    struct CamRay {
        uint32_t tag = 0;
        int index = 0;
        DVec3 from;
        float length = 0;
    };
    std::vector<CamRay> camRays_;       // in flight (the newest few)
    std::vector<GameRay> camRayOut_;
    uint32_t camRaySeq_ = 0;
    uint64_t camRayAnswers_ = 0, camRayHits_ = 0, camPulledIn_ = 0;
    bool camWasPulled_ = false, camCollisionLogged_ = false;
    // photo mode (game/photo_mode.h)
    void UpdatePhotoMode(double now);
    void ReleasePhotoStandIns(bool show);
    uint32_t CapsWanted() const;
    bool photoMode_ = false;          // the game's photo mode is open
    int photoTransitions_ = 0;
    int photoTicks_ = 0;              // libsm64 ticks Mario still runs in it (a pose, a cap going on, a spawn)
    bool photoTicking_ = false;       // (SimTick: one of those - no player input)
    Pose photoHoldPose_ = Pose::None; // the pose being held for the shot
    uint32_t photoCaps_ = 0;
    std::vector<uint32_t> photoHid_;  // stand-ins the mod hid (actor handles)
    bool photoKept_ = false, photoStandInsLogged_ = false;
    int photoCantPoseLogs_ = 0;
    bool photoPoseBlocked_ = false;
    std::vector<int> poseVks_;        // the pose key (without modifiers)
    double interactUntil_ = -1;   // a pedestrian interaction may move Spider-Man until then
    DVec3 interactFrom_;          // ... while Mario stays near where he answered it
    bool wasInteracting_ = false;

    // HUD
    struct ToastMsg {
        std::string text;
        double until;
        uint32_t color;
    };
    std::vector<ToastMsg> toasts_;
    Mutex toastMutex_;
};

} // namespace sm2m

#endif
