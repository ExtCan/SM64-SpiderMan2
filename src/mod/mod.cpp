#ifdef _WIN32

#include "mod.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "../common/log.h"
#include "../common/rom.h"
#include "../game/hero_pin.h"
#include "../game/ped_interact.h"
#include "../game/photo_mode.h"
#include "../render/d3d12_hook.h"
#include "../render/frame_tracker.h"
#include "../render/gbuffer_format.h"
#include "../sm64/sm64_defs.h"
#include "../win/guard.h"
#include "MinHook.h"
#include "modsettings_bridge.h"

#ifndef SM2MARIO_VERSION
#define SM2MARIO_VERSION "0.6.0"
#endif

namespace sm2m {
namespace {

constexpr uint32_t kRed = 0xFF6A6AFFu;
constexpr uint32_t kYellow = 0xFFD84AFFu;
constexpr uint32_t kGreen = 0x7CFF8AFFu;

void PresentCb(IDXGISwapChain* sc) { MarioMod::Get().OnPresent(sc); }
void ClosingCb() { MarioMod::Get().OnGameClosing(); }
void ResizeCb(IDXGISwapChain* sc) { MarioMod::Get().OnResizeBegin(sc); }
void ColorCb(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE cs) { MarioMod::Get().OnColorSpace(sc, cs); }
void Sm64Print(const char* s) { LOGD("libsm64: %s", s); }

int UpIndex(char up) { return up == 'Z' ? 2 : 1; }

// The camera's recent placements (game/hero_pin), for the injector's draws
// (render/injector.h, SetPlacementSource): called on the game's recording
// threads - no allocation, no locks beyond the hooks' seqlocks.
int CameraPlacementsForInjector(CameraPlacementRec* out, int max) {
    hero_pin::CameraPlacement buf[32];
    const int n = hero_pin::RecentCameraPlacements(buf, std::max(0, std::min(32, max)));
    for (int i = 0; i < n; ++i) {
        out[i].pos = buf[i].pos;
        out[i].pivot = buf[i].pivot;
        out[i].blend = buf[i].blend;
        out[i].time = buf[i].time;
        out[i].order = buf[i].order;
        out[i].slot = buf[i].slot;
    }
    return n;
}

// Extra shader digests from bindings.ini [Shaders] (comma-separated hex).
void AddDigests(const Ini& ini, const char* key, std::vector<frame::Digest>& out) {
    for (const auto& t : Ini::SplitList(ini.GetString("Shaders", key, ""))) {
        frame::Digest d{};
        if (d3d12p::ParseDigest(t, d.data())) out.push_back(d);
        else if (!t.empty()) LOGW("bindings [Shaders] %s: '%s' is not a 32-digit hex digest", key, t.c_str());
    }
}

std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    return buf;
}

} // namespace

MarioMod& MarioMod::Get() {
    // Deliberately leaked: a static object's destructor would run at
    // DLL_PROCESS_DETACH under the loader lock, after ExitProcess has killed
    // XAudio2's and the camera scanner's threads - tearing down there hangs.
    static MarioMod* instance = new MarioMod();
    return *instance;
}

void MarioMod::Start(HMODULE self, bool fromScriptEnable) {
    // From script_enable (the game's main thread, every script loaded): if
    // ModSettings is one of them, Mario Mode gets a page in its pause menu.
    // Never from the fallback start thread - ModSettings isn't ours to call
    // from another thread. (Before the init thread starts, which logs the
    // outcome; if this comes after it, Register logs it itself.)
    if (fromScriptEnable) modsettings::Register(initDone_.load(std::memory_order_acquire));
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return;
    self_ = self;
    startThread_ = GetCurrentThreadId();
    HANDLE t = CreateThread(nullptr, 0, &MarioMod::InitThreadProc, this, 0, nullptr);
    if (t) CloseHandle(t);
}

DWORD WINAPI MarioMod::InitThreadProc(void* p) {
    static_cast<MarioMod*>(p)->Init();
    return 0;
}

void MarioMod::Breadcrumb(int bit, const char* text) {
    // One-time progress lines through the first activation: if the game ever
    // dies without a caught fault, the last breadcrumb says how far it got.
    if (breadcrumbs_ & (1u << bit)) return;
    breadcrumbs_ |= 1u << bit;
    LOGI("[first run] %s", text);
}

void MarioMod::StopAfterCrash(const char* where, const std::string& what) {
    failed_ = true;
    failedAt_ = NowSeconds();
    input_shim::SetBlocking(false);
    input_shim::SetMenuCapture(false); // Frame won't run again to release it
    menu_.Hide();
    LOGE("CRASH PREVENTED in %s: %s", where, what.c_str());
    LOGE("Mario Mode has switched itself off for this session so the game keeps running. Please send this log.");
    // Best effort clean-up; each step guarded on its own.
    guard::Fault f;
    auto restoreHero = [&] { hero_.Release(); };
    guard::Call(restoreHero, &f);
    auto stopAudio = [&] { audio_.Stop(); };
    guard::Call(stopAudio, &f);
    // Spider-Man and the camera are the game's again, and no more rays.
    hero_pin::ClearPin();
    hero_pin::SetCameraLead(0.0f);
    hero_pin::KeepHidden(0);
    // (photo mode: the selfie's stand-ins and the pose key are the game's again)
    auto photo = [&] { ReleasePhotoStandIns(true); };
    guard::Call(photo, &f);
    hero_pin::KeepHiddenToo(nullptr, 0);
    input_shim::SetAlsoBlocked({});
    photoPoseBlocked_ = false;
    ReleaseCamera();
    game_physics::SetActive(false, 0);
    game_physics::Clear();
    // No more draws into the game's frame.
    tracker::SetActive(false);
    injector_.Disable("Mario Mode switched itself off");
    active_ = false;
}

void MarioMod::Toast(const std::string& text, float seconds, uint32_t color) {
    LockGuard lock(toastMutex_);
    toasts_.push_back({text, NowSeconds() + seconds, color});
    if (toasts_.size() > 5) toasts_.erase(toasts_.begin());
}

// ------------------------------------------------------------------ init

void MarioMod::Init() {
    if (!ResolveModPaths(self_, paths_)) return;
    guard::Install();
    guard::Fault fault;
    auto body = [&] { InitBody(); };
    bool ok;
    {
        guard::PhaseScope phase("init");
        ok = guard::Call(body, &fault);
    }
    if (!ok) {
        failed_ = true;
        failNoticeOk_ = false;
        LOGE("CRASH PREVENTED during start-up: %s", guard::Describe(fault).c_str());
        LOGE("Mario Mode is disabled for this session. Please send this log.");
    }
    initDone_.store(true, std::memory_order_release);
}

void MarioMod::InitBody() {
    const auto& res = paths_.resourcesDir;
    const auto& data = paths_.dataDir;
    CopyIfMissing(res / L"sm2mario.ini", data / L"sm2mario.ini");
    CopyIfMissing(res / L"PUT_YOUR_ROM_HERE.txt", data / L"PUT_YOUR_ROM_HERE.txt");
    const std::vector<std::string> migrated =
        MigrateUserIni(PathToUtf8(res / L"sm2mario.ini"), PathToUtf8(data / L"sm2mario.ini"), SM2MARIO_VERSION);

    // Shipped defaults first so keys added in updates get sensible values.
    configIni_.LoadFile(PathToUtf8(res / L"sm2mario.ini"));
    Ini user;
    const bool haveUser = user.LoadFile(PathToUtf8(data / L"sm2mario.ini"));
    if (haveUser) configIni_.Merge(user);
    cfg_ = LoadModConfig(configIni_);
    // 0.5's StencilMatch = false (no stencil writes for Mario), from a file
    // that doesn't say MarioStencil yet: still none.
    const bool noStencil = haveUser && user.Has("Render", "StencilMatch") && !user.GetBool("Render", "StencilMatch", true) &&
                           !user.Has("Render", "MarioStencil");
    if (noStencil) cfg_.marioStencil = "keep";
    live_ = LiveFromConfig(cfg_);

    log::Init(PathToUtf8(data / L"sm2mario.log"), cfg_.logLevel == "debug" ? log::Level::Debug : log::Level::Info);
    LOGI("sm2mario %s (libsm64 in Marvel's Spider-Man 2)", SM2MARIO_VERSION);
    LOGI("game: %s", PathToUtf8(paths_.gameExe).c_str());
    LOGI("data folder: %s", PathToUtf8(data).c_str());
    for (const std::string& m : migrated) LOGI("settings: %s", m.c_str());
    if (noStencil) LOGI("settings: StencilMatch = false - MarioStencil = keep (Mario writes no stencil marks)");

    bindingsIni_.LoadFile(PathToUtf8(res / L"bindings.ini"));
    Ini userBindings;
    if (userBindings.LoadFile(PathToUtf8(data / L"bindings.user.ini"))) {
        bindingsIni_.Merge(userBindings);
        LOGI("bindings.user.ini merged");
    }

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) LOGE("MinHook init failed (%d)", int(mh));

    d3d12hook::Callbacks cb;
    cb.present = PresentCb;
    cb.resizeBegin = ResizeCb;
    cb.colorSpace = ColorCb;
    std::string err;
    if (!d3d12hook::Install(cb, err)) LOGE("d3d12 hooks failed: %s", err.c_str());
    {
        tracker::Config tc;
        tc.classify.gbufferFormats = cfg_.gbufferFormats;
        AddDigests(bindingsIni_, "CasterVS", tc.classify.casterVs);
        AddDigests(bindingsIni_, "CasterPS", tc.classify.casterPs);
        AddDigests(bindingsIni_, "CacheCopyPS", tc.classify.cacheCopyPs);
        AddDigests(bindingsIni_, "CacheMovePS", tc.classify.cacheMovePs);
        tc.policy.shadows = cfg_.shadows;
        tc.policy.steadyRegions = cfg_.shadowRegions == "steady";
        tc.policy.capture = true; // switched off below unless the "world" collision is used
        tc.policy.gbufferMarkers = cfg_.gbufferMarkers;
        tracker::Configure(tc, &injector_);
        MarioLook look;
        const gbuf::ShineLook shine = gbuf::LookForShine(live_.shine, cfg_.specular);
        look.gloss = shine.gloss;
        look.specular = shine.f0;
        look.occlusion = shine.occlusion;
        look.metalMaterial = cfg_.metalMaterial;
        look.albedoScale = cfg_.albedoScale;
        look.shadowBias = cfg_.shadowBias;
        look.stencilMatch = cfg_.stencilMatch;
        injector_.SetLook(look);
        ApplyStencilSetting();
        injector_.SetPlacementSource(&CameraPlacementsForInjector);
        injector_.SetAlignment(cfg_.alignToCamera);
        if (!cfg_.alignToCamera) LOGI("settings: AlignToCamera = false - Mario is drawn as 0.6 drew him");
        injector_.SetCaptureSize(cfg_.captureWidth, cfg_.captureWidth);
        std::string serr;
        if (!injector_.CompileShaders(serr)) LOGE("in-world rendering unavailable: %s", serr.c_str());
        if (!tracker::Installed()) LOGW("in-world rendering unavailable: the frame hooks aren't installed");
    }

    input_.Configure(configIni_);
    input_shim::SetPassThrough(input_.PassThroughKeys());
    input_shim::SetMarioControls(cfg_.onlyMarioControls, input_.MarioKeys(), input_.MarioPadButtons(), true);
    input_shim::SetHotkeys(input_.Hotkeys());
    std::vector<int> always;
    // The game never sees the toggle and menu keys.
    const std::pair<const char*, const char*> ourKeys[] = {{"ToggleKey", "M"}, {"MenuKey", "F8"}};
    for (const auto& key : ourKeys)
        for (const auto& k : ParseKeyList(configIni_.GetString("Controls", key.first, key.second)))
            if (!k.ctrl && !k.shift && !k.alt) always.push_back(k.vk);
    input_shim::SetAlwaysBlocked(always);
    for (const auto& k : ParseKeyList(configIni_.GetString("Controls", "PoseKey", "P")))
        if (!k.ctrl && !k.shift && !k.alt) poseVks_.push_back(k.vk);
    input_shim::InstallHooks();
    input_shim::SetClosingCallback(ClosingCb);

    InitSm64();

    game_.Init(bindingsIni_);
    game_.SetUsePositionFunction(cfg_.positionWrites != "direct");
    if (game_.Ready()) {
        // HoldDuringFrame is the switch for these hooks; FollowMarioHeight
        // (also in the menus) only decides whether they hold him each frame.
        hero_pin::Options po;
        po.transformWrites = cfg_.holdDuringFrame;
        po.cameraTarget = cfg_.holdDuringFrame;
        po.keepHidden = cfg_.keepHeroHidden;
        guard::Fault pf;
        auto pinHooks = [&] { hero_pin::Install(game_, bindingsIni_, po); };
        {
            guard::PhaseScope phase("hero pin hooks");
            if (!guard::Call(pinHooks, &pf)) LOGE("hero pin hooks: CRASH PREVENTED (%s)", guard::Describe(pf).c_str());
        }
        auto pedHook = [&] { ped_interact::Install(game_, bindingsIni_); };
        {
            guard::PhaseScope phase("photo request hook");
            if (!guard::Call(pedHook, &pf)) LOGE("photo poses: CRASH PREVENTED (%s)", guard::Describe(pf).c_str());
        }
        auto photoMode = [&] { photo_mode::Install(game_, bindingsIni_); };
        {
            guard::PhaseScope phase("photo mode binding");
            if (!guard::Call(photoMode, &pf)) LOGE("photo mode: CRASH PREVENTED (%s)", guard::Describe(pf).c_str());
        }
        if (cfg_.collisionSource == "physics" || cfg_.pauseWithGame || (cfg_.combat && cfg_.gameDamage)) {
            game_physics::Options gpo;
            gpo.raysPerFrame = cfg_.physicsRaysPerFrame;
            gpo.poolHeadroom = cfg_.physicsPoolHeadroom;
            auto physHooks = [&] { game_physics::Install(game_, bindingsIni_, gpo); };
            guard::PhaseScope phase("game physics hooks");
            if (!guard::Call(physHooks, &pf)) LOGE("game physics: CRASH PREVENTED (%s)", guard::Describe(pf).c_str());
        }
    }
    if (!cfg_.holdDuringFrame) LOGI("hero pin: off ([Hero] HoldDuringFrame = false) - Spider-Man is moved once per frame");
    {
        const std::string ms = modsettings::Status();
        LOGI("ModSettings: %s", ms.empty() ? "not checked" : ms.c_str());
        modsettings::Publish(live_);
    }
    CameraConfig cc;
    cc.source = cfg_.cameraSource;
    cc.actorName = cfg_.cameraActorName;
    cc.fallbackFovDeg = cfg_.fovDegrees;
    cc.useScannedFov = cfg_.useScannedFov;
    cc.lookAtHeight = cfg_.lookAtHeight;
    cc.maxRegionMB = size_t(std::max(64, cfg_.maxRegionMB));
    cc.scanThreads = cfg_.scanThreads;
    camera_.Configure(cc, bindingsIni_, &game_);
    flatRays_ = std::make_unique<FlatGroundRaycaster>(cfg_.upAxis, 0.0);
    {
        WorldModelParams wp;
        wp.cell = std::max(0.1f, cfg_.worldCell);
        wp.range = std::max(8.0f, cfg_.worldRange);
        wp.carveRadius = std::min(12.0f, wp.range);
        world_.SetUpAxis(UpIndex(cfg_.upAxis));
        world_.Configure(wp);
        worldRays_ = std::make_unique<WorldModelRaycaster>(&world_);
    }
    {
        PhysicsWorldParams pp = cfg_.physics;
        pp.cell = cfg_.collision.cellSize;
        pp.radius = cfg_.collision.gridRadiusCells;
        physics_.Configure(pp, UpIndex(cfg_.upAxis));
    }
    if (cfg_.collisionSource == "physics" && game_physics::Available()) {
        source_ = Source::Physics;
    } else if (cfg_.collisionSource == "flat") {
        source_ = Source::Flat;
    } else {
        // Also when the game's physics isn't available (a game update moved it).
        source_ = Source::World;
    }
    if (source_ == Source::World) StartWorldThread();
    tracker::SetCapture(source_ == Source::World);
    hero_.Configure(cfg_.hideHero, bindingsIni_, &game_);
    mario_ = std::make_unique<MarioController>(api_);
    debugOverlay_ = cfg_.debugOverlayOnStart;
    calibrate_ = cfg_.calibrateOnStart;

    if (cfg_.showStartupToast) {
        if (!sm64Ready_) Toast("Mario Mode: " + sm64Error_, 12, kRed);
        else if (!game_.Ready()) Toast("Mario Mode: game hooks not found (game updated?) - see sm2mario.log", 12, kRed);
        else Toast("Mario Mode ready - press M", 6, kGreen);
    }
    LOGI("init complete: sm64=%s game=%s collision=%s, pause with the game: %s", sm64Ready_ ? "ok" : "no",
         game_.Ready() ? "ok" : "no", SourceName(source_),
         !cfg_.pauseWithGame ? "off" : game_physics::PauseDetection() ? "yes" : "unavailable");
    log::Flush();
}

void MarioMod::InitSm64() {
    std::vector<uint8_t> rom;
    RomCheck check;
    std::filesystem::path found;
    bool romOk = FindAndLoadRom(paths_.dataDir, cfg_.romFile, rom, check, found);
#ifdef SM2MARIO_TESTING
    // Test builds only: the smoke test pairs a fake sm64.dll with a fake ROM.
    if (!romOk && GetEnvironmentVariableA("SM2MARIO_FAKE_ROM", nullptr, 0) > 0) {
        std::ifstream f(paths_.dataDir / L"fake.z64", std::ios::binary);
        rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        romOk = !rom.empty();
        found = paths_.dataDir / L"fake.z64";
        check.message = "TEST BUILD: fake ROM accepted";
        LOGW("TEST BUILD: using a fake ROM");
    }
#endif
    if (!romOk) {
        if (found.empty())
            sm64Error_ = "no SM64 ROM found. Put your US ROM (.z64/.n64/.v64) in " + PathToUtf8(paths_.dataDir);
        else
            sm64Error_ = PathToUtf8(found.filename()) + ": " + check.message;
        LOGE("%s", sm64Error_.c_str());
        return;
    }
    LOGI("ROM %s: %s, %s, sha1 %s", PathToUtf8(found.filename()).c_str(), check.message.c_str(),
         RomOrderName(check.order), check.sha1.c_str());
    std::string err;
    if (!LoadSm64Api((paths_.resourcesDir / L"sm64.dll").wstring(), api_, err)) {
        sm64Error_ = err;
        LOGE("%s", err.c_str());
        return;
    }
    if (api_.register_debug_print_function) api_.register_debug_print_function(Sm64Print);
    marioTexture_.assign(size_t(SM64_TEXTURE_WIDTH) * SM64_TEXTURE_HEIGHT * 4, 0);
    guard::Fault fault;
    auto sm64Init = [&] { api_.global_init(rom.data(), marioTexture_.data()); };
    {
        guard::PhaseScope phase("libsm64 init");
        if (!guard::Call(sm64Init, &fault)) {
            sm64Error_ = "libsm64 crashed while reading the ROM (see sm2mario.log)";
            LOGE("CRASH PREVENTED in libsm64 init: %s", guard::Describe(fault).c_str());
            return;
        }
    }
    if (cfg_.audio) {
        // Audio is optional: a failure here (or a crash inside the system's
        // audio stack) only means a silent Mario.
        std::string aerr;
        bool audioOk = false;
        auto audioInit = [&] {
            api_.audio_init(rom.data());
            audioOk = audio_.Init(&api_, aerr);
        };
        guard::PhaseScope phase("audio init");
        if (!guard::Call(audioInit, &fault)) {
            LOGE("audio disabled - CRASH PREVENTED in audio init: %s", guard::Describe(fault).c_str());
            audio_.Disable();
        } else if (audioOk) {
            audio_.SetVolume(live_.volume);
        } else {
            LOGW("audio disabled: %s", aerr.c_str());
            audio_.Disable();
        }
    }
    sm64Ready_ = true;
    injector_.SetMarioTexture(marioTexture_.data(), SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT);
    LOGI("libsm64 initialised");
}

// ------------------------------------------------------------------ hooks

void MarioMod::OnResizeBegin(IDXGISwapChain* sc) {
    if (!initDone_.load(std::memory_order_acquire)) return;
    guard::Fault fault;
    auto fn = [&] { renderer_.OnResizeBegin(sc); };
    guard::PhaseScope phase("resize");
    if (!guard::Call(fn, &fault) && !failed_) {
        failNoticeOk_ = false;
        StopAfterCrash("resize", guard::Describe(fault));
    }
}

void MarioMod::OnColorSpace(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE cs) {
    if (!initDone_.load(std::memory_order_acquire)) return;
    guard::Fault fault;
    auto fn = [&] { renderer_.OnColorSpace(sc, cs); };
    guard::PhaseScope phase("colour space");
    guard::Call(fn, &fault);
}

void MarioMod::OnGameClosing() {
    if (closing_.exchange(true)) return;
    LOGI("the game window is closing - Mario Mode stands down");
    // From the window's thread: only things that are safe to stop from here.
    tracker::SetActive(false);
    input_shim::SetBlocking(false);
    input_shim::SetMenuCapture(false);
    hero_pin::ClearPin();
    guard::Fault f;
    auto stopAudio = [&] { audio_.Stop(); };
    guard::Call(stopAudio, &f);
    log::Flush();
}

void MarioMod::OnPresent(IDXGISwapChain* sc) {
    if (closing_.load(std::memory_order_acquire)) return;
    const double now = NowSeconds();
    double dt = lastFrameTime_ > 0 ? now - lastFrameTime_ : 0;
    lastFrameTime_ = now;
    if (dt > 0.25) dt = 0.25;
    if (!initDone_.load(std::memory_order_acquire)) return;
    guard::Fault fault;
    {
        // Frame boundary for the in-world renderer: read back what this frame's
        // injected draws produced, then start the next frame.
        auto boundary = [&] {
            injector_.EndFrame(frameId_);
            tracker::Census().EndFrame(censusState_ == 1, censusState_ == 0);
            tracker::BeginFrame(++frameId_);
        };
        guard::PhaseScope phase("frame boundary");
        if (!failed_ && !guard::Call(boundary, &fault)) {
            injector_.Disable("crash prevented at the frame boundary");
            tracker::SetActive(false);
            failNoticeOk_ = false;
            StopAfterCrash("frame boundary", guard::Describe(fault));
        }
    }
    if (failed_) {
        tracker::SetActive(false);
        DrawFailureNotice(sc);
        return;
    }
    bool ensured = false;
    auto ensure = [&] { ensured = renderer_.Ensure(sc); };
    {
        guard::PhaseScope phase("renderer setup");
        if (!guard::Call(ensure, &fault)) {
            failNoticeOk_ = false;
            StopAfterCrash("renderer setup", guard::Describe(fault));
            return;
        }
    }
    if (!ensured) return;
    if (!threadLogged_) {
        threadLogged_ = true;
        // Engine calls (GetActor, SetPosition, health writes) run on this thread.
        LOGI("present thread %lu, script_enable thread %lu", GetCurrentThreadId(), startThread_);
    }
    auto frame = [&] { Frame(sc, dt, now); };
    bool ok;
    {
        guard::PhaseScope phase("frame");
        ok = guard::Call(frame, &fault);
    }
    if (!ok) {
        // A fault while recording our draw leaves the renderer unusable.
        const std::string ph = fault.phase;
        if (ph == "render" || ph == "renderer setup") failNoticeOk_ = false;
        StopAfterCrash(fault.phase[0] ? fault.phase : "frame", guard::Describe(fault));
    }
}

void MarioMod::DrawFailureNotice(IDXGISwapChain* sc) {
    // Tell the player once, on screen, then get out of the way entirely.
    if (!failNoticeOk_ || NowSeconds() - failedAt_ > 12.0) return;
    guard::Fault fault;
    auto draw = [&] {
        if (!renderer_.Ensure(sc)) return;
        overlay_.Begin(float(renderer_.Width()), float(renderer_.Height()), renderer_.Font());
        const std::string msg = "Mario Mode hit an error and switched itself off - see sm2mario.log";
        const float ui = overlay_.UiScale();
        const float w = overlay_.TextWidth(msg, 0.9f);
        const float x = (overlay_.Width() - w) * 0.5f, y = overlay_.Height() - 150 * ui;
        overlay_.Rect(x - 14 * ui, y - 8 * ui, w + 28 * ui, overlay_.LineHeight(0.9f) + 16 * ui, 0x000000C0u);
        overlay_.ShadowText(x, y, msg, 0xFF6A6AFFu, 0.9f);
        MarioDraw none;
        renderer_.Render(sc, none, overlay_.Vertices());
    };
    guard::PhaseScope phase("failure notice");
    if (!guard::Call(draw, &fault)) failNoticeOk_ = false;
}

// ------------------------------------------------------------------ in-world rendering

bool MarioMod::GameViewFresh() const { return gameView_.valid && NowSeconds() - gameViewTime_ < 0.5; }

// In the game's frame (world mode) only the game's own view constants decide:
// the memory-scanned camera's axes can't tell screen left from right (the
// 0.2.0 log: the scan said left-handed, the screen is right-handed).
bool MarioMod::HandednessKnown() const {
    if (gameView_.valid) return true;
    return cfg_.renderMode != "world" && camera_.HandednessKnown();
}

bool MarioMod::LeftHanded() const { return gameView_.valid ? gameView_.leftHanded : camera_.LeftHanded(); }

// SM64 is right-handed, and so is Spider-Man 2's screen (0.2.0 assumed
// otherwise: Mario came out mirrored, with left and right swapped). Until the
// game's view says otherwise, Mario's mapping is not mirrored; the first view
// confirms it, and respawns him in the other mapping if not.
bool MarioMod::WantMirror() const {
    if (cfg_.mirror == "on") return true;
    if (cfg_.mirror != "auto") return false;
    return HandednessKnown() ? LeftHanded() : false;
}

void MarioMod::TakeGameViews() {
    if (worldBroken_.load() && source_ == Source::World) {
        source_ = Source::Flat;
        forceRebuild_ = true;
    }
    CaptureResult r;
    while (injector_.PopResult(r)) {
        if (r.frame >= gameViewFrame_) {
            gameView_ = r.view;
            gameViewFrame_ = r.frame;
            gameViewTime_ = NowSeconds();
            if (r.view.valid) {
                ++viewsRead_;
                // The field of view (for the log: the game zooming its camera).
                if (r.view.perspective && r.view.fovY > 0.05f && r.view.fovY < 3.0f) {
                    ++fovFrames_;
                    fovMin_ = std::min(fovMin_, r.view.fovY);
                    fovMax_ = std::max(fovMax_, r.view.fovY);
                    if (fovLast_ > 0.0f && std::fabs(r.view.fovY - fovLast_) > 0.0175f) ++fovJumps_;
                    fovLast_ = r.view.fovY;
                }
            }
            if (active_ && worldRender_ && camOverride_.HasCamera() && camBlend_ > 0.95f && r.view.valid)
                MeasurePairing(r.frame, r.view.camPos);
            // Where the G-buffer draw put Mario against his camera.
            if (active_ && worldRender_ && r.view.valid && r.align != CaptureResult::kAlignOff && r.alignCount > 0) {
                ++alignViews_;
                if (r.align != CaptureResult::kAlignNewest) ++alignMatched_;
                const double s = Length(r.alignShift);
                alignShiftSum_ += s;
                alignShiftMax_ = std::max(alignShiftMax_, s);
            }
        }
        if (r.hasDepth && active_ && source_ == Source::World) SubmitWorldFrame(std::move(r));
    }
}

void MarioMod::StartWorldThread() {
    if (worldThread_) return;
    jobEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worldThread_ = CreateThread(nullptr, 0, &MarioMod::WorldThreadProc, this, 0, nullptr);
}

DWORD WINAPI MarioMod::WorldThreadProc(void* p) {
    static_cast<MarioMod*>(p)->WorldLoop();
    return 0;
}

void MarioMod::WorldLoop() {
    guard::PhaseScope phase("world model");
    DepthFrame frame;
    for (;;) {
        WaitForSingleObject(jobEvent_, INFINITE);
        DVec3 focus, marioLo, marioHi;
        bool marioDrawn = false, heroVisible = false;
        {
            LockGuard lock(jobMu_);
            if (!jobPending_) continue;
            std::swap(frame, job_);
            focus = jobFocus_;
            marioDrawn = jobMarioDrawn_;
            marioLo = jobMarioLo_;
            marioHi = jobMarioHi_;
            heroVisible = jobHeroVisible_;
            jobPending_ = false;
        }
        std::vector<Cylinder> exclude;
        std::vector<ExcludeBox> boxes;
        // Mario is in the captured depth (he was drawn before it was copied),
        // where he was in *that* frame - a few frames ago, metres behind him
        // when he runs. Leave exactly that out (plus a little for depth noise).
        DVec3 feet = focus;
        if (marioDrawn) {
            const double m = 0.12;
            boxes.push_back({marioLo - DVec3(m, m, m), marioHi + DVec3(m, m, m)});
            feet = (marioLo + marioHi) * 0.5;
            feet[UpIndex(cfg_.upAxis)] = marioLo[UpIndex(cfg_.upAxis)];
        }
        if (heroVisible || !marioDrawn) {
            // Spider-Man stands where Mario was drawn (he is pinned to him).
            Cylinder c;
            c.base = feet;
            c.radius = marioDrawn ? 0.6f : 0.85f;
            c.below = 0.2f;
            c.height = 2.2f;
            exclude.push_back(c);
        }
        const double t0 = NowSeconds();
        guard::Fault fault;
        auto integrate = [&] {
            LockGuard lock(worldMu_);
            world_.Integrate(frame, focus, exclude, boxes);
        };
        if (!guard::Call(integrate, &fault)) {
            LOGE("CRASH PREVENTED in the world model: %s - collision falls back to a flat floor",
                 guard::Describe(fault).c_str());
            worldBroken_ = true;
            return;
        }
        worldIntegrateMs_.store((NowSeconds() - t0) * 1000.0);
        worldCells_.store(world_.CellCount());
        {
            LockGuard lock(worldMu_);
            const WorldModelStats& ws = world_.Stats();
            // Something next to Mario was removed (a car drove off, or is
            // driving by): his collision is rebuilt soon, not in a second.
            if (ws.nearChanges > 0) worldChangedNear_.store(true);
            if (ws.motionUsed && !motionLogged_.exchange(true))
                LOGI("collision: the game's motion vectors are in use - moving cars and people don't become solid");
            if (ws.motionFrames >= 60 && ws.motionRejected * 2 > ws.motionFrames && !motionWarned_.exchange(true))
                LOGW("collision: the game's motion vectors don't match its camera in %d of %d frames - not using them",
                     ws.motionRejected, ws.motionFrames);
        }
    }
}

void MarioMod::SubmitWorldFrame(CaptureResult&& r) {
    if (!worldThread_ || !mario_ || !mario_->Alive()) return;
    ++depthFrames_;
    lastDepthFrameTime_ = NowSeconds();
    LockGuard lock(jobMu_);
    job_.width = r.width;
    job_.height = r.height;
    job_.depth = std::move(r.depth);
    job_.motion = std::move(r.motion);
    job_.view = r.view;
    job_.time = NowSeconds();
    jobFocus_ = MarioGamePos(1.0f);
    jobMarioDrawn_ = r.marioDrawn;
    jobMarioLo_ = r.marioLo;
    jobMarioHi_ = r.marioHi;
    jobHeroVisible_ = !hero_.Hidden();
    jobPending_ = true;
    SetEvent(jobEvent_);
    if (depthFrames_ == 1) Breadcrumb(7, "first depth frame from the game for collision");
}

void MarioMod::ResetWorld(const DVec3& feet) {
    LockGuard lock(worldMu_);
    world_.Clear();
    world_.Seed(feet, 0.9f);
    // Until the camera has seen the ground around Spider-Man, assume it is flat.
    world_.SetFallbackGround(feet, feet[UpIndex(cfg_.upAxis)], 3.0);
}

void MarioMod::PublishMario(float alpha, bool visible) {
    MarioFrame f;
    f.visible = visible && mario_ && mario_->Alive();
    if (f.visible) {
        mario_->Geometry(alpha, drawGeo_);
        const uint32_t n = uint32_t(drawGeo_.triangles) * 3;
        const float inv = float(1.0 / map_.Scale());
        f.anchor = map_.Origin();
        f.vertexCount = n;
        f.pos.resize(size_t(n) * 3);
        f.nrm.resize(size_t(n) * 3);
        f.col.assign(drawGeo_.color.begin(), drawGeo_.color.begin() + long(n) * 3);
        f.uv.assign(drawGeo_.uv.begin(), drawGeo_.uv.begin() + long(n) * 2);
        for (uint32_t i = 0; i < n; ++i) {
            const Vec3 lp(drawGeo_.position[i * 3], drawGeo_.position[i * 3 + 1], drawGeo_.position[i * 3 + 2]);
            const Vec3 gp = map_.DirToGame(lp) * inv;
            Vec3 gn = map_.DirToGame(Vec3(drawGeo_.normal[i * 3], drawGeo_.normal[i * 3 + 1], drawGeo_.normal[i * 3 + 2]));
            const float len = Length(gn);
            gn = len > 1e-6f ? gn * (1.0f / len) : Vec3(0, 1, 0);
            f.pos[i * 3] = gp.x;
            f.pos[i * 3 + 1] = gp.y;
            f.pos[i * 3 + 2] = gp.z;
            f.nrm[i * 3] = gn.x;
            f.nrm[i * 3 + 1] = gn.y;
            f.nrm[i * 3 + 2] = gn.z;
        }
        f.cut = cutPending_;
        cutPending_ = false;
        f.metal = (mario_->State().flags & sm64::MARIO_METAL_CAP) != 0;
        f.vanish = (mario_->State().flags & sm64::MARIO_VANISH_CAP) != 0;
        f.havePivot = planPivotValid_ && cfg_.alignToCamera;
        f.pivot = planPivot_;
    }
    f.tag = frameId_; // (the frame this Present started: the game's lists for it carry the same number)
    injector_.SetMario(f);
}

// What Mario writes into the stencil, from [Render] MarioStencil.
void MarioMod::ApplyStencilSetting() {
    MarioStencil st;
    const std::string& m = cfg_.marioStencil;
    double v = 0;
    if (m == "copy") {
        st.mode = MarioStencil::Copy;
    } else if (m != "auto" && m != "keep" && Ini::ParseFloat(m, v) && v >= 0 && v <= 255) {
        st.mode = MarioStencil::Write;
        st.ref = uint8_t(v);
        st.mask = 0xFF;
    } else if (m == "auto") {
        st.mode = MarioStencil::Copy; // (0.5's, until the census decides)
        tracker::Census().SetWindow(StencilWindow());
    }
    injector_.SetStencil(st);
}

// Each frame, while Mario's stencil mark is still being learnt: is
// Spider-Man on screen (for the census), and after each switch between him
// and Mario (M), what Mario's pixels get.
void MarioMod::UpdateStencilMark(double now) {
    constexpr int kMaxSwitches = 12; // (then it's settled: the census stops, and costs nothing)
    const bool learning = cfg_.marioStencil == "auto" && stencilSwitchesSeen_ < kMaxSwitches &&
                          (worldRender_ || cfg_.renderMode == "world");
    tracker::SetCensusWanted(learning);
    int state = 0;
    if (learning && !gamePaused_ && !photoMode_ && !menu_.Open()) {
        if (active_) {
            state = hero_.Hidden() && injector_.GetStats().lastGBufferFrame + 2 >= frameId_ ? 2 : 0;
        } else {
            // Spider-Man on screen: his transform isn't hidden. (His actor is
            // looked up twice a second, not every frame.)
            if (now - censusHeroAt_ > 0.5 || now < censusHeroAt_) {
                censusHero_ = game_.Hero();
                censusHeroAt_ = now;
            }
            uint32_t flags = 0;
            if (censusHero_ && game_.TransformFlags(censusHero_, flags) && !(flags & game_.Layout().transformHiddenBit))
                state = 1;
        }
    }
    censusState_ = state;
    if (!learning) return;
    StencilCensus& census = tracker::Census();
    const int switches = census.Switches();
    if (switches == stencilSwitchesSeen_) return;
    stencilSwitchesSeen_ = switches;
    const StencilCensus::Decision d = census.Decide(StencilWindow());
    if (!d.ready) return;
    if (stencilDecided_ && d.ref == stencilRef_ && d.mask == stencilMask_) return; // (no change)
    const bool again = stencilDecided_;
    stencilDecided_ = true;
    stencilRef_ = d.ref;
    stencilMask_ = d.mask;
    MarioStencil st;
    st.mode = d.mask ? MarioStencil::Write : MarioStencil::Keep;
    st.ref = d.ref;
    st.mask = d.mask;
    injector_.SetStencil(st);
    if (++stencilLogs_ > 6) return;
    const std::string mark = d.mask ? Fmt("0x%02x under mask 0x%02x", d.ref, d.mask) : std::string();
    const std::string counted = Fmt("%d frames with Spider-Man on screen and %d with Mario around %d switch(es)",
                                    census.WindowFramesWithHero(), census.WindowFramesWithout(), switches);
    if (!d.own.empty()) {
        LOGI("stencil: %sSpider-Man's pixels are marked %s - Mario's get the same%s: %s (%s; all marks: %s)",
             again ? "(again) " : "", d.own.c_str(),
             d.shared.empty() ? "" : Fmt(", and what all of the G-buffer gets for the other bits (%s)", d.shared.c_str()).c_str(),
             mark.c_str(), counted.c_str(), census.Describe().c_str());
    } else {
        // Nothing of his own: what everything drawn into the G-buffer writes
        // (his suit too, then), if there is such a mark; else none.
        LOGI("stencil: %snothing is marked only while Spider-Man is on screen (%s) - Mario's pixels get %s (all marks: %s)",
             again ? "(again) " : "", counted.c_str(),
             d.mask ? Fmt("what all of the G-buffer gets: %s", mark.c_str()).c_str() : "none (what is behind them stays)",
             census.Describe().c_str());
    }
}

int MarioMod::StencilWindow() const { return std::max(30, std::min(StencilCensus::kHistory, cfg_.stencilCensusFrames)); }

// For the summaries: the stencil census while it hasn't decided.
std::string MarioMod::StencilStatus() {
    if (cfg_.marioStencil != "auto" || stencilDecided_) return std::string();
    const StencilCensus& census = tracker::Census();
    return Fmt("stencil: not decided yet - it compares the frames just before and just after you press M: %d with "
               "Spider-Man on screen and %d with Mario so far, around %d switch(es) (%d of each needed); until then "
               "Mario's pixels get 0.5's marks",
               census.WindowFramesWithHero(), census.WindowFramesWithout(), census.Switches(), StencilWindow());
}

// A view the game rendered (game frame `frame`, from `view`): which of the
// mod's frames placed the camera there, against which Mario frame was drawn
// in it. Only while the mod places the camera (otherwise nothing tells which
// frame the camera came from), and only where the placements differ (Mario
// moving: standing, every frame puts it in the same spot).
void MarioMod::MeasurePairing(uint64_t frame, const DVec3& view) {
    const uint64_t drawn = injector_.DrawnTag(frame);
    if (!drawn) return;
    const std::vector<hero_pin::CameraPlacement> recs = hero_pin::RecentCameraPlacements();
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    int matches = 0;
    for (const hero_pin::CameraPlacement& c : recs) {
        if (Length(c.pos - view) > 0.003) continue;
        ++matches;
        lo = std::min(lo, c.tag);
        hi = std::max(hi, c.tag);
    }
    if (!matches) {
        ++pairNoMatch_;
        return;
    }
    if (lo != hi) {
        ++pairAmbiguous_;
        return;
    }
    const int64_t d = int64_t(uint32_t(drawn)) - int64_t(lo);
    const int k = int(std::max<int64_t>(-2, std::min<int64_t>(2, d))) + 2;
    ++pairHist_[k];
    ++pairWin_[k];
}

// Every few seconds: draw Mario's frame for this game frame, or the one
// before if the game records its frames before the mod has made his (it
// would get one or the other, by a hair, and Mario would judder) - or if the
// camera turns out to be a frame behind the Mario drawn with it.
void MarioMod::UpdateDrawLag(double now) {
    if (now - pairLastEval_ < 5.0) return;
    pairLastEval_ = now;
    const Injector::Stats st = injector_.GetStats();
    const uint64_t prep = st.prepared - pairPrepared_, late = st.notReady - pairNotReady_;
    pairPrepared_ = st.prepared;
    pairNotReady_ = st.notReady;
    const int old = injector_.DrawLag();
    int lag = old;
    const char* why = "";
    if (prep >= 100 && late * 50 > prep && lagFloor_ < 1) {
        lagFloor_ = 1;
        why = "the game records some of its frames before Mario's frame for them is made";
    }
    uint64_t total = 0;
    for (uint64_t c : pairWin_) total += c;
    if (Aligning()) {
        // Mario is drawn where the camera of each view had him (whichever
        // frame's camera it is): only the frame made for the game frame (or
        // the floor's) - nothing to match against the camera's.
        if (lag > lagFloor_) {
            lag = lagFloor_;
            why = "Mario is drawn where the camera of each frame had him";
        }
        for (uint64_t& c : pairWin_) c = 0;
    } else if (total >= 60) {
        if (pairWin_[3] * 10 >= total * 7 && lag < 2) {
            ++lag;
            why = "Mario was drawn a frame ahead of the camera";
        } else if (pairWin_[1] * 10 >= total * 7 && lag > lagFloor_) {
            --lag;
            why = "Mario was drawn a frame behind the camera";
        }
        for (uint64_t& c : pairWin_) c = 0;
    }
    lag = std::max(lag, lagFloor_);
    if (lag != old) {
        injector_.SetDrawLag(lag);
        LOGI("frames: %s - each game frame now draws Mario's frame %s", why,
             lag == 0 ? "made for it" : (lag == 1 ? "made a frame before it" : "made two frames before it"));
    }
}

bool MarioMod::Aligning() const { return cfg_.alignToCamera && worldRender_ && camBlend_ > 0.0f; }

std::string MarioMod::PairingSummary() {
    const Injector::Stats st = injector_.GetStats();
    const uint64_t prep = st.prepared - sumPrepared_;
    if (prep == 0) return std::string();
    std::string out = Fmt("Mario prepared for %llu game frames: %llu recorded before his frame for them was ready, "
                          "%llu drew an older one than wanted, %llu not drawn (the GPU still busy with his data)",
                          static_cast<unsigned long long>(prep), static_cast<unsigned long long>(st.notReady - sumNotReady_),
                          static_cast<unsigned long long>(st.olderDrawn - sumOlder_),
                          static_cast<unsigned long long>(st.gpuBusy - sumGpuBusy_));
    sumPrepared_ = st.prepared;
    sumNotReady_ = st.notReady;
    sumOlder_ = st.olderDrawn;
    sumGpuBusy_ = st.gpuBusy;
    uint64_t total = 0;
    for (uint64_t c : pairHist_) total += c;
    if (total) {
        auto pc = [&](uint64_t c) { return 100.0 * double(c) / double(total); };
        out += Fmt("; against the camera the mod placed (%llu frames): in step %.0f%%, a frame ahead %.0f%%, behind "
                   "%.0f%%, more %.0f%%",
                   static_cast<unsigned long long>(total), pc(pairHist_[2]), pc(pairHist_[3]), pc(pairHist_[1]),
                   pc(pairHist_[0] + pairHist_[4]));
    }
    if (pairAmbiguous_ || pairNoMatch_)
        out += Fmt(" (%llu views not told apart, %llu from elsewhere)", static_cast<unsigned long long>(pairAmbiguous_),
                   static_cast<unsigned long long>(pairNoMatch_));
    out += Fmt("; drawing %s", injector_.DrawLag() == 0 ? "each frame's own" : "with a lag");
    // Frames presented against those the game rendered (frame generation
    // presents more than it renders).
    const uint64_t presents = frameId_ - presentsAtSummary_;
    presentsAtSummary_ = frameId_;
    out += Fmt("; %llu frames presented, %llu views read back", static_cast<unsigned long long>(presents),
               static_cast<unsigned long long>(viewsRead_));
    viewsRead_ = 0;
    for (uint64_t& c : pairHist_) c = 0;
    pairAmbiguous_ = pairNoMatch_ = 0;
    return out;
}

void MarioMod::CheckWorldRendering() {
    const tracker::Stats ts = tracker::GetStats();
    const double now = NowSeconds();
    if (ts.gbufferInjections != lastInjectionCount_) {
        lastInjectionCount_ = ts.gbufferInjections;
        lastInjectionSeen_ = now;
    }
    // Shadows: if no region qualified (the game refreshes its shadow maps in a
    // way the frame tracker doesn't see), use the regions it redraws every
    // frame instead - once, for the session.
    if (active_ && worldRender_ && cfg_.shadows && cfg_.shadowRegions == "auto" && !tracker::SteadyShadowRegions() &&
        now - activatedAt_ > 4.0 && ts.shadowInjections == shadowInjectionsAtActivate_ &&
        ts.shadowRegions > shadowRegionsAtActivate_ + 120 && ts.gbufferInjections > injectionsAtActivate_) {
        tracker::SetSteadyShadowRegions(true);
        LOGI("shadows: none of the game's shadow regions was seen being refreshed - Mario now also casts into the "
             "regions it redraws every frame ([Render] ShadowRegions = refreshed turns this off)");
        tracker::RequestReport(1, "shadow regions switched to steady");
    }
    if (!active_ || !worldRender_ || worldFallbackDone_) return;
    if (ts.gbufferInjections > injectionsAtActivate_ && !injector_.Failed()) {
        worldFallbackDone_ = true;
        Breadcrumb(8, "Mario is drawn inside the game's frame");
        LOGI("in-world rendering: Mario is in the game's G-buffer (%llu segments so far, shadows %llu, captures %llu)",
             static_cast<unsigned long long>(ts.gbufferInjections), static_cast<unsigned long long>(ts.shadowInjections),
             static_cast<unsigned long long>(ts.captures));
        return;
    }
    if (!injector_.Failed() && now - activatedAt_ < cfg_.fallbackSeconds) return;
    // Give up on the in-world renderer for this activation: draw on top.
    worldFallbackDone_ = true;
    worldRender_ = false;
    tracker::SetActive(false);
    std::string why = injector_.Failed() ? injector_.Error() : ts.lastSkip;
    if (why.empty()) {
        if (ts.gbufferPsos == 0) why = "no G-buffer pipeline was seen (were the hooks installed after the game loaded?)";
        else if (ts.gbufferSegments == 0) why = "the G-buffer pass wasn't found in the frame";
        else why = "no G-buffer segment qualified";
    }
    LOGW("in-world rendering unavailable: %s - drawing Mario on top of the frame instead", why.c_str());
    Toast("Mario is drawn on top of the game (in-world rendering unavailable - see sm2mario.log)", 7, kYellow);
    tracker::RequestReport(2, "in-world rendering unavailable");
}

// ------------------------------------------------------------------ activation

float MarioMod::FaceAngleFromHero(uintptr_t hero) const {
    Vec3 fwd;
    if (!game_.GetForward(hero, fwd)) return 0;
    Vec3 l = map_.DirToLocal(fwd);
    return std::atan2(l.x, l.z);
}

DVec3 MarioMod::MarioGamePos(float alpha) const {
    if (!mario_ || !mario_->Alive()) return lastPinned_;
    return map_.ToGame(mario_->Position(alpha));
}

void MarioMod::RebuildCollision() {
    guard::PhaseScope phase("collision rebuild");
    Vec3 center(0, 0, 0);
    if (mario_ && mario_->Alive()) {
        const SM64MarioState& s = mario_->State();
        center = Vec3(s.position[0], s.position[1], s.position[2]);
    }
    // The floor under a standing Mario before (the old surfaces are still
    // loaded) and after: the log says when one goes away under him.
    const bool standing = mario_ && mario_->Alive() && !sm64::IsAirborne(mario_->State().action) &&
                          !sm64::IsSubmerged(mario_->State().action);
    const float floorBefore = standing && !surfaces_.empty()
                                  ? api_.surface_find_floor_height(center.x, center.y + 50.0f, center.z)
                                  : -1e30f;
    const Source sourceBefore = activeSource_;
    activeSource_ = ActiveSource();
    sourceChosen_ = true;
    surfaceOrigins_.clear();
    switch (activeSource_) {
    case Source::Physics:
        physics_.SetAirborne(mario_ && mario_->Alive() && sm64::IsAirborne(mario_->State().action));
        physics_.Build(map_, center, cfg_.collision, surfaces_, collisionStats_, &surfaceOrigins_);
        break;
    case Source::World: {
        LockGuard lock(worldMu_);
        builder_.Build(*worldRays_, map_, center, cfg_.collision, surfaces_, collisionStats_);
        break;
    }
    default:
        builder_.Build(*flatRays_, map_, center, cfg_.collision, surfaces_, collisionStats_);
        break;
    }
    api_.static_surfaces_load(surfaces_.data(), uint32_t(surfaces_.size()));
    if (standing && activeSource_ == Source::Physics && sourceBefore == Source::Physics &&
        std::fabs(center.y - floorBefore) < 0.3f * float(map_.Scale())) {
        const float floorAfter = api_.surface_find_floor_height(center.x, center.y + 50.0f, center.z);
        if (floorAfter < floorBefore - 0.5f * float(map_.Scale()) && vanishLogs_ < 12) {
            ++vanishLogs_;
            const double now = NowSeconds();
            const std::string next =
                floorAfter < -10000.0f ? std::string("none") : Fmt("%.2f m lower", double(floorBefore - floorAfter) / map_.Scale());
            LOGI("collision: the floor under Mario went away (the next floor down: %s) - under him: %s", next.c_str(),
                 DescribeGround(center, now).c_str());
        }
    }
    if (cfg_.traceCollision)
        LOGI("trace rebuild at local (%.0f %.0f %.0f): %d surfaces, %d floors %d flattened, walls edge %d step %d probe "
             "%d, clipped %d, %d rays",
             center.x, center.y, center.z, collisionStats_.total, collisionStats_.floors, collisionStats_.flattened,
             collisionStats_.edgeWalls, collisionStats_.stepWalls, collisionStats_.probeWalls, collisionStats_.clipped,
             collisionStats_.rays);
    if (cfg_.traceCollision) {
        // The walls libsm64 now has next to Mario.
        int shown = 0;
        for (const SM64Surface& sf : surfaces_) {
            const Vec3 n = Normalize(Sm64SurfaceNormal(sf));
            if (std::fabs(n.y) > 0.01f) continue;
            float cxs = 0, czs = 0;
            for (int k = 0; k < 3; ++k) {
                cxs += float(sf.vertices[k][0]) / 3.0f;
                czs += float(sf.vertices[k][2]) / 3.0f;
            }
            if (std::fabs(cxs - center.x) > 150.0f || std::fabs(czs - center.z) > 150.0f || shown++ > 24) continue;
            LOGI("trace   wall (%d %d %d) (%d %d %d) (%d %d %d) n (%.2f %.2f)", sf.vertices[0][0], sf.vertices[0][1],
                 sf.vertices[0][2], sf.vertices[1][0], sf.vertices[1][1], sf.vertices[1][2], sf.vertices[2][0],
                 sf.vertices[2][1], sf.vertices[2][2], n.x, n.z);
        }
    }
    lastBuildCenter_ = center;
    lastBuildTime_ = NowSeconds();
    forceRebuild_ = false;
}

const char* MarioMod::SourceName(Source s) {
    switch (s) {
    case Source::Physics: return "the game's physics";
    case Source::World: return "rendered world";
    default: return "flat floor";
    }
}

// Until the game has answered for the ground under Mario (the first frames
// after M), and while the rendered world has no depth frames (it only knows
// what they showed: none yet after M, or they stopped - in-world rendering
// unavailable, a menu), Mario gets a flat floor at the height he last stood
// on instead of falling through the unknown.
MarioMod::Source MarioMod::ActiveSource() const {
    switch (source_) {
    case Source::Physics: return physicsLive_ ? Source::Physics : Source::Flat;
    case Source::World: {
        const bool live = depthFrames_ > depthFramesAtActivate_ && NowSeconds() - lastDepthFrameTime_ < 2.0;
        return live ? Source::World : Source::Flat;
    }
    default: return Source::Flat;
    }
}

void MarioMod::UpdateCollisionSource() {
    if (source_ == Source::Physics && !game_physics::Available()) {
        // A ray cast faulted (game_physics turned itself off).
        FallBackToRenderedWorld("the game's physics is unavailable now");
    } else if (source_ == Source::Physics && active_ && !physicsLive_) {
        // Nothing has come back since M: the hooked physics frame never runs
        // (a game update moved it), or it runs but no ray gets an answer.
        const game_physics::Heartbeat hb = game_physics::GetHeartbeat();
        const double waited = NowSeconds() - activatedAt_;
        if (waited > 3.0 && hb.calls == physicsCallsAtActivate_)
            FallBackToRenderedWorld("the game's physics frame hasn't run since M");
        else if (waited > 3.0 && hb.steps - physicsStepsAtActivate_ > 120 && lastPhysicsAnswer_ < activatedAt_)
            FallBackToRenderedWorld("the game's physics hasn't answered a ray since M");
    }
    const Source want = ActiveSource();
    if (sourceChosen_ && want == activeSource_) return;
    const int up = UpIndex(cfg_.upAxis);
    if (want == Source::Flat) {
        const DVec3 at = haveSafe_ ? lastSafe_ : MarioGamePos(1.0f);
        flatRays_->SetHeight(at[up]);
        if (sourceChosen_ && activeSource_ != Source::Flat)
            LOGI("collision: no depth frames from the game for a while - flat floor at %.2f for now", at[up]);
    } else if (want == Source::World) {
        LOGI("collision: the game's rendered world (%llu depth frames)", static_cast<unsigned long long>(depthFrames_));
    } else {
        LOGI("collision: the game's physics (%s)", physics_.Debug(MarioGamePos(1.0f)).c_str());
    }
    forceRebuild_ = true;
}

// What the camera sees, from now on (the game's physics can't be used).
void MarioMod::FallBackToRenderedWorld(const char* why) {
    LOGW("collision: %s - Mario collides with what the camera sees instead", why);
    source_ = Source::World;
    physicsLive_ = false;
    game_physics::SetActive(false, 0);
    game_physics::Clear();
    StartWorldThread();
    tracker::SetCapture(true);
    ResetWorld(MarioGamePos(1.0f));
    depthFramesAtActivate_ = depthFrames_;
}

// ------------------------------------------------------------------ game physics

// What a hit belongs to: people aren't solid for Mario; something that moved
// in the last two seconds (a car, a swinging door) is refreshed quickly.
HitKind MarioMod::ClassifyActor(uintptr_t a, double now) {
    const uint32_t serial = game_.ActorSerial(a);
    auto it = actorKinds_.find(a);
    if (it == actorKinds_.end() || it->second.serial != serial) {
        if (actorKinds_.size() > 4096) {
            for (auto e = actorKinds_.begin(); e != actorKinds_.end();)
                e = now - e->second.seen > 20.0 ? actorKinds_.erase(e) : std::next(e);
        }
        ActorKind k;
        k.serial = serial;
        bool notSolid = false, solid = false;
        for (const std::string& c : cfg_.notSolidComponents)
            if (game_.GetComponentChecked(a, c.c_str())) {
                notSolid = true;
                break;
            }
        if (notSolid)
            for (const std::string& c : cfg_.solidComponents)
                if (game_.GetComponentChecked(a, c.c_str())) {
                    solid = true;
                    break;
                }
        k.character = notSolid && !solid;
        game_.GetPosition(a, k.pos);
        k.checked = now;
        it = actorKinds_.insert_or_assign(a, k).first;
        if (loggedActors_.size() < 24) {
            std::string name = game_.ActorName(a);
            const size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (name.size() > 40) name = name.substr(0, 40);
            if (std::find(loggedActors_.begin(), loggedActors_.end(), name) == loggedActors_.end()) {
                loggedActors_.push_back(name);
                LOGI("collision: '%s' is %s", name.c_str(),
                     k.character ? "not solid for Mario (a person)" : solid ? "solid (a vehicle)" : "solid");
            }
        }
    } else if (now - it->second.checked > 0.1) {
        DVec3 p;
        if (game_.GetPosition(a, p)) {
            if (Length(p - it->second.pos) > 0.02) it->second.movedAt = now;
            it->second.pos = p;
        }
        it->second.checked = now;
    }
    it->second.seen = now;
    if (it->second.character) return HitKind::Character;
    return now - it->second.movedAt < 2.0 ? HitKind::Movable : HitKind::World;
}

// The answers the game gave since the last frame go into the physics world;
// the rays it wants next are queued for the game's thread.
void MarioMod::PumpPhysics(double now) {
    if (source_ != Source::Physics || !mario_ || !mario_->Alive()) return;
    physAnswers_.clear();
    game_physics::Take(physAnswers_);
    for (const game_physics::RawResult& a : physAnswers_) {
        if (a.r.tag & kCameraRayTag) {
            AcceptCameraRay(a, now);
            continue;
        }
        GameRayResult r = a.r;
        int w = 0;
        for (int i = 0; i < r.count && i < kMaxGameHits; ++i) {
            const uintptr_t actor = a.actors[i];
            if (actor && actor == boundHero_) continue; // Spider-Man (if the request couldn't leave him out)
            GameHit h = r.hits[i];
            h.kind = actor ? ClassifyActor(actor, now) : HitKind::World;
            h.actor = actor != 0;
            h.actorId = actor;
            if (h.actor && BurnsMario(h.material) && burningActorLogs_ < 4) {
                ++burningActorLogs_;
                std::string name = game_.ActorName(actor);
                const size_t slash = name.find_last_of("/\\");
                if (slash != std::string::npos) name = name.substr(slash + 1);
                LOGI("collision: '%s' (an actor%s) says its surface is %s - plain ground for Mario, not lava (only the "
                     "world's own lava and acid burn him)",
                     name.c_str(), h.kind == HitKind::Character ? ", a person" : "", PhysicsMaterialName(h.material));
            }
            r.hits[w++] = h;
        }
        r.count = w;
        physics_.Accept(r, now);
    }
    if (!physAnswers_.empty()) lastPhysicsAnswer_ = now;
    const DVec3 feet = MarioGamePos(1.0f);
    if (!physicsLive_ && physics_.Ready(feet)) physicsLive_ = true;
    if (physics_.TakeChanged()) physicsChanged_ = true;
    if (paused_) return;
    const int room = cfg_.physicsRaysPerFrame * 2 - game_physics::Queued();
    if (room <= 0) return;
    // Mario's velocity, game space, m/s (libsm64: units per 30 Hz tick).
    const SM64MarioState& st = mario_->State();
    const Vec3 vg = map_.DirToGame(Vec3(st.velocity[0], st.velocity[1], st.velocity[2])) * float(30.0 / map_.Scale());
    physRays_.clear();
    physics_.Schedule(feet, vg, now, room, physRays_);
    game_physics::Submit(physRays_);
    if (now - lastPhysicsLog_ > 60.0) LogPhysicsStats();
}

void MarioMod::LogPhysicsStats() {
    lastPhysicsLog_ = NowSeconds();
    const game_physics::Stats gs = game_physics::TakeStats();
    const PhysicsWorldStats& ws = physics_.Stats();
    LOGI("collision: the game answered %llu rays (%llu hits, %llu empty) in %llu frames, %.2f ms per frame on average "
         "(max %.2f) on thread %u; %llu waited for the game's queries; most query results in use %d; %llu damage "
         "request(s) | %s",
         static_cast<unsigned long long>(gs.cast), static_cast<unsigned long long>(gs.hits),
         static_cast<unsigned long long>(gs.empty), static_cast<unsigned long long>(gs.frames),
         gs.frames ? gs.totalMs / double(gs.frames) : 0.0, gs.maxFrameMs, gs.thread,
         static_cast<unsigned long long>(gs.deferred), gs.poolUsedMax, static_cast<unsigned long long>(gs.damage),
         physics_.Debug(MarioGamePos(1.0f)).c_str());
    if (ws.lost) LOGI("collision: %llu rays went unanswered and were asked again", static_cast<unsigned long long>(ws.lost));
}

// The game's world stopped (its physics frame doesn't step: the pause menu,
// photo mode, a loading screen): Mario stops too.
void MarioMod::UpdateGamePause(double now) {
    if (!cfg_.pauseWithGame || !game_physics::PauseDetection()) {
        gamePaused_ = false;
        return;
    }
    const game_physics::Heartbeat hb = game_physics::GetHeartbeat();
    if (hb.steps != lastSteps_) {
        lastSteps_ = hb.steps;
        lastStepTime_ = now;
        presentsSinceStep_ = 0;
        stepsSeen_ = true;
        if (gamePaused_ && pausesLogged_ <= 3) LOGI("the game is running again - so is Mario");
        gamePaused_ = false;
        return;
    }
    ++presentsSinceStep_;
    if (stepsSeen_ && !gamePaused_ && presentsSinceStep_ >= 3 && now - lastStepTime_ > 0.12) {
        gamePaused_ = true;
        if (++pausesLogged_ <= 3) LOGI("the game is paused - so is Mario");
    }
}

// Water: where the game's water rays found it under Mario, else [World]
// WaterHeight if set, else none.
void MarioMod::UpdateWater() {
    if (!mario_ || !mario_->Alive()) return;
    const int up = UpIndex(cfg_.upAxis);
    const Vec3 pos = mario_->Position(1.0f);
    double level = 0;
    bool have = false;
    if (source_ == Source::Physics && physicsLive_ && physics_.Water(level)) {
        have = true;
    } else if (cfg_.hasWater) {
        level = cfg_.waterHeight;
        have = true;
    }
    int local = -11000; // SM64's "no water" (FLOOR_LOWER_LIMIT)
    if (have) {
        DVec3 wp = map_.ToGame(pos);
        wp[up] = level;
        local = int(std::lround(std::max(-30000.0f, std::min(30000.0f, map_.ToLocal(wp).y))));
    }
    api_.set_mario_water_level(mario_->Id(), local);
    waterLevelLocal_ = local;
}

bool MarioMod::SpawnAt(const DVec3& feet, float face) {
    mario_->Despawn();
    fallPath_.clear();
    map_.SetOrigin(feet);
    RebuildCollision();
    float floorY = api_.surface_find_floor_height(0.0f, 150.0f, 0.0f);
    if (floorY < -10000.0f) return false;
    if (!mario_->Spawn(Vec3(0, floorY + 1.0f, 0), face)) return false;
    api_.set_mario_health(mario_->Id(), sm64::HEALTH_FULL);
    api_.set_mario_invincibility(mario_->Id(), 60);
    cheats_.Reset(); // a new Mario: the cap goes back on
    pose_.Cancel();
    pendingPose_ = Pose::None;
    apexUp_ = -1e30;
    wasAirborne_ = false;
    airborneTicks_ = 0;
    deathTimer_ = 0;
    prevResidual_ = 0;
    pinDriftFrames_ = 0;
    accumulator_ = 0;
    lastPinned_ = feet;
    havePin_ = true;
    lastSafe_ = feet;
    haveSafe_ = true;
    return true;
}

void MarioMod::BindHero(uintptr_t hero) {
    const uint32_t serial = hero ? game_.ActorSerial(hero) : 0;
    if (hero == boundHero_ && serial == boundHeroSerial_) return;
    if (boundHero_) LOGI("hero actor changed (%p -> %p)", reinterpret_cast<void*>(boundHero_), reinterpret_cast<void*>(hero));
    boundHero_ = hero;
    boundHeroSerial_ = serial;
    game_physics::SetHero(hero); // the rays leave out whoever Spider-Man is now
    heroHealth_ = 0;
    if (hero) {
        heroHealth_ = game_.FindComponentInList(hero, "Health");
        if (!heroHealth_) heroHealth_ = game_.GetComponent(hero, "Health");
    }
    lastHeroHealth_ = -1;
}

// Moves Spider-Man onto Mario and tells the pin hooks where he has to stay
// until the next frame (before the move, so the move itself isn't undone).
void MarioMod::PinHero(uintptr_t hero, const DVec3& pin) {
    uintptr_t t = 0;
    // A pedestrian interaction is playing (the player answered a prompt):
    // the game walks and animates Spider-Man for it - he stays hidden, the
    // camera stays with Mario - and gets him back afterwards.
    // (Over when Mario walks off: Spider-Man comes back to him.)
    if (NowSeconds() < interactUntil_ && Length(pin - interactFrom_) > 2.5) interactUntil_ = -1;
    const bool interacting = NowSeconds() < interactUntil_;
    // CAMERA FOLLOWS JUMPS off: 0.2's behaviour (moved once per frame, the
    // camera stays at street level when Mario jumps).
    if (live_.followHeight && game_.GetTransform(hero, t)) {
        const float p[3] = {float(pin.x), float(pin.y), float(pin.z)};
        hero_pin::SetPin(t, p, UpIndex(cfg_.upAxis), std::max(0.5f, cfg_.holdRadius), !interacting);
    } else {
        hero_pin::ClearPin();
    }
    if (interacting) {
        wasInteracting_ = true;
        return;
    }
    if (wasInteracting_) {
        wasInteracting_ = false;
        havePin_ = false; // (where he ended up isn't a teleport of the game's)
    }
    game_.SetPosition(hero, pin);
    lastPinned_ = pin;
    havePin_ = true;
}

// heroRead: `p` is where the game had Spider-Man at the start of this frame
// (compared with the last pin); else `p` is Mario now, compared with the
// camera of the latest frame the game rendered.
void MarioMod::SampleFollow(const DVec3& p, bool heroRead) {
    if (!mario_ || !mario_->Alive()) return;
    const bool air = sm64::IsAirborne(mario_->State().action);
    const int up = UpIndex(cfg_.upAxis);
    if (heroRead) {
        if (!havePin_) return;
        const DVec3 d = p - lastPinned_;
        double h2 = 0;
        for (int k = 0; k < 3; ++k)
            if (k != up) h2 += d[k] * d[k];
        follow_.SampleResidual(d[up], std::sqrt(h2), air);
        return;
    }
    if (!GameViewFresh() || gameViewFrame_ == followViewFrame_) return;
    followViewFrame_ = gameViewFrame_;
    follow_.Sample(NowSeconds(), p[up], gameView_.camPos[up], air);
    for (const FollowMonitor::Jump& j : follow_.TakeJumps()) {
        if (NowSeconds() - lastJumpLog_ < 20.0) continue;
        lastJumpLog_ = NowSeconds();
        LOGI("camera follow: Mario jumped %.2f m, the game camera rose %.2f m", j.marioRise, j.cameraRise);
    }
}

// The camera is the game's again (Mario Mode off, a crash): nothing placed,
// nothing searched.
void MarioMod::ReleaseCamera() {
    hero_pin::SetCameraSearch(false, DVec3(), Vec3(), 0, 0);
    hero_pin::SetCameraTransform(0);
    CameraOverride::Plan off;
    hero_pin::SetCameraPlan(off);
    camOverride_.Reset();
    camSearching_ = false;
    camSearchFrames_ = 0;
    camMisses_ = camWrong_ = 0;
    camPlacedCount_ = camPlacedNext_ = 0;
    camLogged_ = false;
    camRays_.clear();
    camWasPulled_ = false;
    camAwaySince_ = camRefusedSince_ = -1;
    camSeenSeq_ = 0;
    ClearCameraSlots();
}

// No camera placed (nothing found yet, or let go): nothing known of the
// game's other cameras either.
void MarioMod::ClearCameraSlots() {
    static_assert(kCamSlots == hero_pin::kCameraSlots, "one each");
    camSeen_.clear();
    camActiveSlot_ = 0;
    camActiveSince_ = -1;
    for (int i = 0; i < kCamSlots; ++i) ClearSlotTimes(i);
    camPlacedViewAt_ = -1;
    camElseValid_ = false;
    camElseAt_ = -1;
    if (camWatching_ && !camSearching_) hero_pin::SetCameraSearch(false, DVec3(), Vec3(), 0, 0);
    camWatching_ = false;
}

void MarioMod::ClearSlotTimes(int slot) {
    if (slot < 0 || slot >= kCamSlots) return;
    camSlotViewAt_[slot] = camSlotWriteAt_[slot] = camSlotAddedAt_[slot] = -1;
}

int MarioMod::CamerasPlaced() const {
    int n = 0;
    for (int i = 0; i < kCamSlots; ++i)
        if (hero_pin::CameraTransformAt(i)) ++n;
    return n;
}

DVec3 MarioMod::MarioAtFrame(uint64_t frame, const DVec3& fallback) const {
    const MarioAt* best = nullptr;
    for (const MarioAt& m : camMarioAt_) {
        if (m.frame == 0 || m.frame > frame) continue;
        if (!best || m.frame > best->frame) best = &m;
    }
    return best ? best->pos : fallback;
}

// A view the game rendered from none of the cameras the mod places, while it
// places them: from another of the game's cameras? (In flight 0.6's frames
// came from one it didn't place a fifth of the time - further back: the
// camera snapped close and far.) The transforms written where that view came
// from, in the frames it may have been rendered in, are the candidates; one
// seen twice within 3 s - between views from the cameras placed (the game
// switching between its cameras, not cutting away to a cinematic's) and laid
// out like the first - is placed too.
void MarioMod::WatchOtherCameras(const DVec3& view, const Vec3& forward, uint64_t viewFrame, double gap, double now) {
    // (Watched for from now on, carried along with Mario: hero_pin::SearchArea b.)
    camElseValid_ = true;
    camElseOffset_ = view - MarioAtFrame(viewFrame, camMarioAt_[(camMarioNext_ + 15) % 16].pos);
    camElseForward_ = forward;
    camElseAt_ = now;
    hero_pin::CameraCandidate found[4];
    const uint32_t hi = uint32_t(viewFrame), lo = hi > 4 ? hi - 4 : 1;
    const int n = hero_pin::MatchViewToCandidates(lo, hi, view, 0.1f, found, 4);
    if (n <= 0) {
        ++camViewsUnmatched_;
        return;
    }
    int best = 0;
    for (int i = 1; i < n; ++i)
        if (found[i].meanGap < found[best].meanGap) best = i;
    const hero_pin::CameraCandidate c = found[best];
    if (!cfg_.placeOtherCameras) return; // ([Camera] PlaceOtherCameras = false: only the first)
    if (std::find(camRejected_.begin(), camRejected_.end(), c.transform) != camRejected_.end()) return;
    if (c.row != camRow_ || (c.sign < 0) != (camSign_ < 0)) {
        if (camLayoutLogs_ < 3) {
            ++camLayoutLogs_;
            LOGI("camera: the game rendered from another transform, laid out unlike its camera (row %d against %d) - "
                 "left alone",
                 c.row, camRow_);
        }
        return;
    }
    const bool between = camPlacedViewAt_ >= 0 && now - camPlacedViewAt_ < 1.0;
    camSeen_.erase(std::remove_if(camSeen_.begin(), camSeen_.end(), [&](const CamSeen& x) { return now - x.last > 3.0; }),
                   camSeen_.end());
    auto it = std::find_if(camSeen_.begin(), camSeen_.end(), [&](const CamSeen& x) { return x.transform == c.transform; });
    if (it == camSeen_.end()) {
        if (camSeen_.size() >= 8) camSeen_.erase(camSeen_.begin());
        CamSeen x;
        x.transform = c.transform;
        camSeen_.push_back(x);
        it = camSeen_.end() - 1;
    }
    it->last = now;
    if (between) ++it->views;
    if (it->views < 2) return;
    const uintptr_t t = it->transform;
    camSeen_.erase(it);
    int slot = hero_pin::AddCameraTransform(t);
    if (slot < 0) {
        // All in use: the one the game rendered from least lately makes room
        // (not the first, nor the one the views come from).
        int stalest = -1;
        double oldest = 1e300;
        for (int i = 1; i < kCamSlots; ++i) {
            if (i == camActiveSlot_) continue;
            const double last = std::max(camSlotViewAt_[i], camSlotAddedAt_[i]);
            if (last < oldest) {
                oldest = last;
                stalest = i;
            }
        }
        if (stalest < 0) return;
        hero_pin::RemoveCameraTransform(stalest);
        ClearSlotTimes(stalest);
        ++camLetGo_;
        slot = hero_pin::AddCameraTransform(t);
        if (slot < 0) return;
    }
    camSlotAddedAt_[slot] = camSlotWriteAt_[slot] = now;
    camSlotViewAt_[slot] = -1;
    ++camAdopted_;
    if (++camAdoptLogs_ <= 8 || camAdoptLogs_ % 50 == 0)
        LOGI("camera: the game renders from another of its cameras too (%.1f m from where the mod put the camera) - "
             "the mod places that one around Mario as well (%d camera(s) placed)",
             gap, CamerasPlaced());
}

// Every frame: when the game last wrote each camera; one not written for 10 s
// let go (its memory may be something else's by now); the first one no
// longer written while another is - the game uses that one now - swapped
// for it.
void MarioMod::MaintainCameraSlots(double now) {
    camPlacedMost_ = std::max(camPlacedMost_, CamerasPlaced());
    for (int i = 0; i < kCamSlots; ++i) {
        const hero_pin::CameraSlotStats st = hero_pin::TakeCameraSlotStats(i);
        if (st.transform && st.lastWrite > camSlotWriteAt_[i]) camSlotWriteAt_[i] = st.lastWrite;
    }
    if (paused_ || gamePaused_) {
        // (Held while the game is paused, whatever it writes then.)
        for (int i = 0; i < kCamSlots; ++i)
            if (hero_pin::CameraTransformAt(i)) camSlotWriteAt_[i] = std::max(camSlotWriteAt_[i], now - 0.5);
        return;
    }
    for (int i = 1; i < kCamSlots; ++i) {
        if (!hero_pin::CameraTransformAt(i)) continue;
        if (now - std::max(camSlotWriteAt_[i], camSlotAddedAt_[i]) <= 10.0) continue;
        hero_pin::RemoveCameraTransform(i);
        ClearSlotTimes(i);
        ++camLetGo_;
        if (camActiveSlot_ == i) {
            camActiveSlot_ = 0;
            camActiveSince_ = now;
            camOverride_.SwitchGameSource();
            camSeenSeq_ = 0;
            camPlacedCount_ = camPlacedNext_ = 0;
        }
    }
    if (camSlotWriteAt_[0] >= 0 && now - camSlotWriteAt_[0] > 1.0) {
        int best = -1;
        for (int i = 1; i < kCamSlots; ++i) {
            // (Written, and rendered from lately.)
            if (!hero_pin::CameraTransformAt(i) || !(camSlotWriteAt_[i] >= 0 && now - camSlotWriteAt_[i] < 0.5) ||
                !(camSlotViewAt_[i] >= 0 && now - camSlotViewAt_[i] < 1.0))
                continue;
            if (best < 0 || camSlotViewAt_[i] > camSlotViewAt_[best]) best = i;
        }
        if (best > 0) {
            hero_pin::SwapCameraSlots(0, best);
            std::swap(camSlotViewAt_[0], camSlotViewAt_[best]);
            std::swap(camSlotWriteAt_[0], camSlotWriteAt_[best]);
            std::swap(camSlotAddedAt_[0], camSlotAddedAt_[best]);
            if (camActiveSlot_ == best) camActiveSlot_ = 0;
            else if (camActiveSlot_ == 0) camActiveSlot_ = best;
            ++camPromoted_;
            if (++camPromoteLogs_ <= 8 || camPromoteLogs_ % 50 == 0)
                LOGI("camera: the game stopped writing its first camera and writes another it renders from - that one "
                     "is the game's camera now");
        }
    }
}

void MarioMod::ForgetCamera(const char* why, bool reject) {
    const uintptr_t t = hero_pin::CameraTransform();
    if (t) {
        ++camStatLost_;
        if (camStatWhy_.find(why) == std::string::npos && camStatWhy_.size() < 400)
            camStatWhy_ += std::string(camStatWhy_.empty() ? "" : "; ") + why;
        const std::string gaps = camLastPlacedGap_ >= 0
                                     ? Fmt(" (last view %.2f m from where the mod put it, %.2f m from the game's own)",
                                           camLastPlacedGap_, camLastGameGap_)
                                     : std::string();
        if (++camForgets_ <= 12 || camForgets_ % 50 == 0)
            LOGI("camera: %s - the mod stops placing the camera%s%s%s", why, gaps.c_str(),
                 !reject || camRejected_.size() < 2 ? " (looking again)" : "",
                 camForgets_ > 12 ? Fmt(" [%d times so far]", camForgets_).c_str() : "");
        // (Rejected for good only when its writes are shown not to be the
        // view's; the view moving elsewhere for a while - a new camera after a
        // load - just starts the search again.)
        if (reject && std::find(camRejected_.begin(), camRejected_.end(), t) == camRejected_.end())
            camRejected_.push_back(t);
    }
    hero_pin::SetCameraTransform(0);
    camOverride_.LoseCamera();
    CameraOverride::Plan off;
    hero_pin::SetCameraPlan(off);
    camMisses_ = camWrong_ = 0;
    camPlacedCount_ = camPlacedNext_ = 0;
    camAwaySince_ = camRefusedSince_ = -1;
    ClearCameraSlots();
    if (camRejected_.size() >= 3 && !camGaveUp_) {
        camGaveUp_ = true;
        LOGW("camera: no transform the game renders its view from could be placed - the camera's target still "
             "follows Mario ([Camera] FollowLead)");
    }
}

CameraOverride::Params MarioMod::CameraParams() const {
    CameraOverride::Params p;
    p.verticalTauAir = std::max(0.005f, cfg_.cameraVerticalSmoothing);
    p.verticalTauGround = std::min(p.verticalTauAir, 0.06f);
    p.distance = live_.cameraDistance;
    return p;
}

// A camera ray's answer: how far along it the first wall is (people, the
// hero and things on the move don't stop the camera).
void MarioMod::AcceptCameraRay(const game_physics::RawResult& a, double now) {
    const auto it = std::find_if(camRays_.begin(), camRays_.end(), [&](const CamRay& c) { return c.tag == a.r.tag; });
    if (it == camRays_.end()) return; // from before a reset
    const CamRay ray = *it;
    camRays_.erase(it);
    ++camRayAnswers_;
    float hit = -1.0f;
    for (int i = 0; i < a.r.count && i < kMaxGameHits; ++i) {
        const uintptr_t actor = a.actors[i];
        if (actor && actor == boundHero_) continue;
        if (actor && ClassifyActor(actor, now) != HitKind::World) continue;
        const float d = float(Length(a.r.hits[i].pos - ray.from));
        if (hit < 0.0f || d < hit) hit = d;
    }
    if (hit >= 0.0f) ++camRayHits_;
    camOverride_.AcceptCollision(now, ray.index, hit, ray.length, CameraParams());
}

// The rays that tell whether a wall is between Mario and where his camera
// goes: cast before Mario's own (they matter this frame), the older ones
// still waiting dropped.
void MarioMod::CastCameraRays(const CameraOverride::Plan& plan, const CameraOverride::GameSample& sample) {
    if (!cfg_.cameraCollision || source_ != Source::Physics || !game_physics::Available() || !plan.active ||
        !sample.valid || paused_)
        return;
    const CameraOverride::Params p = CameraParams();
    DVec3 from[CameraOverride::kCollisionRays], to[CameraOverride::kCollisionRays];
    if (!CameraOverride::CollisionRays(plan, sample.rows, p, from, to)) return;
    camRayOut_.clear();
    for (int i = 0; i < CameraOverride::kCollisionRays; ++i) {
        GameRay r;
        r.from = from[i];
        r.to = to[i];
        r.tag = kCameraRayTag | (camRaySeq_++ & 0x7FFFFFFFu);
        r.type = uint8_t(cfg_.cameraQuery);
        r.maxHits = 4;
        camRayOut_.push_back(r);
        CamRay c;
        c.tag = r.tag;
        c.index = i;
        c.from = from[i];
        c.length = float(Length(to[i] - from[i]));
        camRays_.push_back(c);
    }
    // (Unanswered ones from long ago: the queue dropped them.)
    if (camRays_.size() > 24) camRays_.erase(camRays_.begin(), camRays_.end() - 24);
    game_physics::SubmitFirst(camRayOut_, kCameraRayTag);
}

// The game's camera, placed by the mod (camera_override.h): find the
// transform the game writes its view into (a transform written every frame
// where the rendered camera is, aimed like it), make sure the rendered view
// keeps coming from it, and publish where it goes for the hooks.
void MarioMod::UpdateCameraOverride(const DVec3& mario, double now) {
    if (!mario_ || !mario_->Alive()) return;
    const bool want = cfg_.cameraOverride && hero_pin::TransformHooks() && !camGaveUp_;
    if (!want) {
        if (camSearching_) hero_pin::SetCameraSearch(false, DVec3(), Vec3(), 0, 0);
        camSearching_ = false;
        if (hero_pin::CameraTransform()) ForgetCamera("turned off", false);
        return;
    }
    const CameraOverride::Params p = CameraParams();
    const uintptr_t camT = hero_pin::CameraTransform();
    if (camT) MaintainCameraSlots(now);
    // (The camera the views came from lately: its own writes are what is
    // learnt from and judged.)
    if (camActiveSlot_ < 0 || camActiveSlot_ >= kCamSlots || !hero_pin::CameraTransformAt(camActiveSlot_))
        camActiveSlot_ = 0;
    const uintptr_t activeT = hero_pin::CameraTransformAt(camActiveSlot_);

    // What the camera's transform holds now (the mod's placement, if active)
    // and what the game itself put there last, for matching against the views
    // the game renders a frame or two later.
    CameraOverride::GameSample sample;
    hero_pin::GameCameraSample(sample, camActiveSlot_);
    if (GameViewFresh() && Length(gameView_.forward) > 0.5f) {
        sample.haveView = true;
        sample.viewForward = Normalize(gameView_.forward);
    }
    // (Its framing learnt once the views have come from it for a second.)
    sample.learn = camActiveSince_ < 0 || now - camActiveSince_ >= 1.0;
    // Mario in this frame (a view's camera against him then: WatchOtherCameras).
    camMarioAt_[camMarioNext_].frame = frameId_;
    camMarioAt_[camMarioNext_].pos = mario;
    camMarioNext_ = (camMarioNext_ + 1) % 16;
    bool camHeld = false; // the transform holds the position the view was rendered from
    if (activeT) {
        float tp[3];
        if (SafeRead(activeT + game_.Layout().transformPosition, tp, sizeof(tp))) {
            camHeld = GameViewFresh() && Length(DVec3(tp[0], tp[1], tp[2]) - gameView_.camPos) < 0.1;
            CamPos& h = camPlaced_[camPlacedNext_];
            h.placed = DVec3(tp[0], tp[1], tp[2]);
            h.haveGame = sample.valid;
            if (sample.valid) h.game = sample.pos;
            camPlacedNext_ = (camPlacedNext_ + 1) % 12;
            camPlacedCount_ = std::min(12, camPlacedCount_ + 1);
        }
    }

    if (GameViewFresh() && gameViewFrame_ != camCheckFrame_) {
        camCheckFrame_ = gameViewFrame_;
        const DVec3& view = gameView_.camPos;
        if (camT) {
            // Where the view was rendered from, against where the mod put the
            // camera and where the game had it, in the last frames. The mod's
            // when it is nearer that than the game's own: a camera shake or a
            // hit effect on top of it moves the view a little (0.5 wanted it
            // within 5 cm, and dropped the camera in every fight).
            double gapPlaced = 1e9, gapGame = 1e9;
            for (int i = 0; i < camPlacedCount_; ++i) {
                gapPlaced = std::min(gapPlaced, Length(camPlaced_[i].placed - view));
                if (camPlaced_[i].haveGame) gapGame = std::min(gapGame, Length(camPlaced_[i].game - view));
            }
            // ... and where the mod placed any of the game's cameras lately
            // (the hooks' own record): which one the view came from (the one
            // the views came from lately, of two placed in one spot).
            int viewSlot = -1;
            {
                hero_pin::CameraPlacement recs[32];
                const int n = hero_pin::RecentCameraPlacements(recs, 32);
                double best = 1e9, bestActive = 1e9;
                for (int i = 0; i < n; ++i) {
                    if (now - recs[i].time > 1.0) continue;
                    const double g = Length(recs[i].pos - view);
                    if (g < best) {
                        best = g;
                        viewSlot = recs[i].slot;
                    }
                    if (recs[i].slot == camActiveSlot_) bestActive = std::min(bestActive, g);
                }
                if (bestActive <= best + 0.005) viewSlot = camActiveSlot_;
                gapPlaced = std::min(gapPlaced, best);
                if (!(best < 0.05)) viewSlot = -1;
            }
            const bool placedMatch = gapPlaced < 0.05 || (gapPlaced < 1.5 && gapPlaced < 0.5 * gapGame);
            const bool gameMatch = gapGame < 0.05 && gapPlaced > 0.3;
            if (camBlend_ > 0.95f && !paused_ && !gamePaused_) {
                // For the log: how far the views were from the mod's placements.
                if (gapPlaced < 0.05) ++camViewsOurs_;
                else ++camViewsElsewhere_[gapPlaced < 0.3 ? 0 : (gapPlaced < 1.5 ? 1 : (gapPlaced < 5.0 ? 2 : 3))];
            }
            if (viewSlot >= 0 && viewSlot < kCamSlots) {
                const bool activeSeen = camSlotViewAt_[camActiveSlot_] >= 0 && now - camSlotViewAt_[camActiveSlot_] < 0.5;
                camSlotViewAt_[viewSlot] = now;
                camPlacedViewAt_ = now;
                if (viewSlot != camActiveSlot_ && !activeSeen) {
                    // The game renders from another of its cameras now (none
                    // from the one before for half a second): its writes are
                    // the ones to learn from and judge.
                    camActiveSlot_ = viewSlot;
                    camActiveSince_ = now;
                    camOverride_.SwitchGameSource();
                    camSeenSeq_ = 0;
                    camPlacedCount_ = camPlacedNext_ = 0;
                }
            } else if (gapPlaced >= 0.05 && camBlend_ > 0.95f && !paused_ && !gamePaused_) {
                // Not from any camera the mod places: from one of the game's
                // others it doesn't yet? (For the log: or from where the game
                // itself had the one it places - read before the mod's write.)
                if (gapGame < 0.05) ++camViewsGameOwn_;
                if (cfg_.traceCamera && camTraceLogs_ < 400) {
                    ++camTraceLogs_;
                    hero_pin::CameraPlacement recs[32];
                    const int n = hero_pin::RecentCameraPlacements(recs, 32);
                    int ni = -1;
                    for (int i = 0; i < n; ++i)
                        if (ni < 0 || Length(recs[i].pos - view) < Length(recs[ni].pos - view)) ni = i;
                    const CamPos& last = camPlaced_[(camPlacedNext_ + 11) % 12];
                    LOGI("camera trace: view frame %llu (now %llu) at (%.2f %.2f %.2f): %.2f m from the nearest placement "
                         "(camera %d, tag %u, %.0f ms ago), the transform now (%.2f %.2f %.2f), the game's own %.2f m away",
                         static_cast<unsigned long long>(gameViewFrame_), static_cast<unsigned long long>(frameId_), view.x,
                         view.y, view.z, ni >= 0 ? Length(recs[ni].pos - view) : -1.0, ni >= 0 ? recs[ni].slot : -1,
                         ni >= 0 ? recs[ni].tag : 0u, ni >= 0 ? (now - recs[ni].time) * 1000.0 : -1.0, last.placed.x,
                         last.placed.y, last.placed.z, gapGame < 1e8 ? gapGame : -1.0);
                }
                WatchOtherCameras(view, gameView_.forward, gameViewFrame_, gapPlaced, now);
            }
            if (camPlacedCount_ > 0) {
                camLastPlacedGap_ = gapPlaced;
                camLastGameGap_ = gapGame < 1e8 ? gapGame : -1;
            }
            const bool writes = hero_pin::TakeCameraWrites() > 0;
            // Written, but never like the view: its memory is something else's
            // now (a camera freed and its transform reused).
            const uint64_t refused = hero_pin::TakeCameraRefused();
            camRefusedTotal_ += refused;
            if (refused > 0 && !writes && !paused_ && !gamePaused_) {
                if (camRefusedSince_ < 0) camRefusedSince_ = now;
            } else {
                camRefusedSince_ = -1;
            }
            // Judged only while the mod is placing it with the game running:
            // in photo mode, the pause menu or a cinematic the view comes from
            // another camera without this one being wrong (0.5's first cut
            // dropped the camera for good after a minute of photo mode).
            const bool placing = camOverride_.HasCamera() && camBlend_ > 0.95f && writes && !paused_;
            if (!placing) {
                camMisses_ = camWrong_ = 0;
            } else {
                camMisses_ = placedMatch ? 0 : camMisses_ + 1;
                // Placed somewhere else, yet the view stays where the game had
                // it: that transform only follows the camera (an effect on it).
                camWrong_ = (gameMatch && !placedMatch) ? camWrong_ + 1 : 0;
            }
            if (camWrong_ > 30) ForgetCamera("the game's view doesn't come from that transform", true);
            else if (camMisses_ > 120) ForgetCamera("the game renders from somewhere else now", false);
            else if (camRefusedSince_ >= 0 && now - camRefusedSince_ > 1.5)
                ForgetCamera("that transform's writes stopped looking like the game's view", false);
            else if (!camLogged_ && placedMatch && placing) {
                camLogged_ = true;
                const float d = live_.cameraDistance;
                const Vec3 f = camOverride_.Framing() * d;
                LOGI("camera: placed by the mod - %.2f m back, %.2f m up%s from Mario (%s, times %.1f), aimed by the game",
                     f.z, f.y, std::fabs(f.x) > 0.05f ? Fmt(", %.2f m to the side", f.x).c_str() : "",
                     camOverride_.Learnt() ? "the game's framing, learnt while he stood" : "the usual framing until he stands",
                     double(d));
            }
        } else if (!paused_ && !gamePaused_ && now >= camSearchAfter_) {
            // Look for it.
            if (!camSearching_) {
                hero_pin::ClearCameraCandidates();
                camSearchFrames_ = 0;
                camSearching_ = true;
            }
            hero_pin::SetCameraSearch(true, view, gameView_.forward, p.searchRadius, p.searchAim, uint32_t(frameId_));
            if (++camSearchFrames_ >= p.searchFrames) {
                std::vector<hero_pin::CameraCandidate> cands = hero_pin::TakeCameraCandidates();
                // (Only those written in most of the frames: one written in
                // fewer, nearer the view, mustn't push the right one aside.)
                const uint32_t minHits = uint32_t(p.searchFrames * 6 / 10);
                cands.erase(std::remove_if(cands.begin(), cands.end(),
                                           [&](const hero_pin::CameraCandidate& c) {
                                               return c.row < 0 || c.transform == 0 || c.hits < minHits ||
                                                      std::find(camRejected_.begin(), camRejected_.end(), c.transform) !=
                                                          camRejected_.end();
                                           }),
                            cands.end());
                // The most frames first; of those, the one written nearest the
                // view (another transform following the camera - an effect on
                // it - sits a little off it).
                std::sort(cands.begin(), cands.end(), [](const hero_pin::CameraCandidate& a, const hero_pin::CameraCandidate& b) {
                    const uint32_t ha = a.hits / 4, hb = b.hits / 4; // (a frame or two either way is a tie)
                    if (ha != hb) return ha > hb;
                    return a.meanGap < b.meanGap;
                });
                hero_pin::SetCameraSearch(false, DVec3(), Vec3(), 0, 0);
                camSearching_ = false;
                if (!cands.empty()) {
                    ClearCameraSlots();
                    hero_pin::SetCameraTransform(cands[0].transform);
                    camOverride_.SetCamera(cands[0].row, cands[0].sign);
                    camRow_ = cands[0].row;
                    camSign_ = cands[0].sign < 0 ? -1.0f : 1.0f;
                    camSlotWriteAt_[0] = camSlotAddedAt_[0] = now;
                    camActiveSince_ = now;
                    // The game's other cameras written there too (it has
                    // several, and renders from one or another): placed as
                    // well, if laid out like it.
                    std::string others;
                    for (size_t i = 1; i < cands.size(); ++i) {
                        const hero_pin::CameraCandidate& c = cands[i];
                        const bool like = c.row == camRow_ && (c.sign < 0) == (camSign_ < 0) && c.meanGap <= 0.6f;
                        int slot = -1;
                        if (like && cfg_.placeOtherCameras) slot = hero_pin::AddCameraTransform(c.transform);
                        if (slot > 0) camSlotWriteAt_[slot] = camSlotAddedAt_[slot] = now;
                        if (others.size() < 300)
                            others += Fmt("%s%u frames %.2f m%s", others.empty() ? "" : ", ", c.hits, double(c.meanGap),
                                          slot > 0 ? " (placed too)" : (like ? "" : " (laid out otherwise)"));
                    }
                    camMisses_ = camWrong_ = 0;
                    camPlacedCount_ = camPlacedNext_ = 0;
                    camLogged_ = false;
                    camSearchRounds_ = 0;
                    camSeenSeq_ = 0;
                    camSeqTime_ = now;
                    camAwaySince_ = -1;
                    ++camFound_;
                    ++camStatFound_;
                    if (camFound_ <= 12)
                        LOGI("camera: the game's camera is the transform written each frame where it renders from (%u "
                             "of %d frames, %.2f m from the view on average, %zu candidate(s)%s) - the mod places it "
                             "around Mario",
                             cands[0].hits, p.searchFrames, double(cands[0].meanGap), cands.size(),
                             others.empty() ? "" : Fmt("; the others: %s", others.c_str()).c_str());
                } else if (++camSearchRounds_ >= 3) {
                    // (A cinematic shot from a camera the hooks don't see, say:
                    // look again a little later, less and less often.)
                    const int k = std::min(2, camSearchRounds_ - 3);
                    camSearchAfter_ = now + 5.0 * double(1 << k); // 5, 10, then every 20 s
                    if (!camSearchWarned_) {
                        camSearchWarned_ = true;
                        LOGW("camera: no transform is written where the game renders from through the hooked setters - "
                             "the camera's target still follows Mario; the mod looks again now and then");
                    }
                }
            }
        }
    }

    // While the mod places the camera: watching for the game's others -
    // written where it renders from (another transform the game moves with
    // its camera), or where a view lately came from that none of the placed
    // ones gave (the game's other camera, carried along with Mario since).
    // Their writes in each frame are stamped with it (WatchOtherCameras).
    if (hero_pin::CameraTransform()) {
        if (GameViewFresh() && !paused_ && !gamePaused_ && camBlend_ > 0.0f) {
            const DVec3 then = MarioAtFrame(gameViewFrame_, mario);
            hero_pin::SearchArea a, b;
            a.pos = mario + (gameView_.camPos - then);
            a.forward = gameView_.forward;
            a.radius = p.searchRadius;
            if (camElseValid_ && now - camElseAt_ < 3.0) {
                b.pos = mario + camElseOffset_;
                b.forward = camElseForward_;
                b.radius = 3.0f;
            }
            hero_pin::SetCameraSearch(true, a, b, p.searchAim, uint32_t(frameId_));
            camWatching_ = true;
            if (now >= camWatchPruneAt_) {
                // (Room for the next: those not written for two seconds go.)
                hero_pin::PruneCameraCandidates(frameId_ > 120 ? uint32_t(frameId_ - 120) : 0u);
                camWatchPruneAt_ = now + 1.0;
            }
        } else if (camWatching_) {
            hero_pin::SetCameraSearch(false, DVec3(), Vec3(), 0, 0);
            camWatching_ = false;
        }
    }

    // The transform no longer written while the view moves on, or away from
    // Mario for a while - and in either case not what the view is rendered
    // from: the game renders from another camera now (a new one after a load,
    // or the one found was a cinematic's). Look again. (A camera the game
    // simply stops writing while it stands still still holds the view.)
    const bool followAllowed = live_.followHeight && !paused_ && !gamePaused_;
    if (hero_pin::CameraTransform()) {
        const bool running = !paused_ && !gamePaused_ && GameViewFresh();
        if (!running || !sample.valid || sample.seq != camSeenSeq_ || camHeld) {
            camSeenSeq_ = sample.valid ? sample.seq : 0;
            camSeqTime_ = now;
            camSeqView_ = gameView_.camPos;
            camSeqForward_ = gameView_.forward;
        } else if (now - camSeqTime_ > 2.0 &&
                   (Length(gameView_.camPos - camSeqView_) > 1.0 ||
                    Dot(Normalize(gameView_.forward), Normalize(camSeqForward_)) < 0.97f)) {
            ForgetCamera("the game stopped writing that transform while its view moved on", false);
        }
        if (running && followAllowed && sample.valid && !camHeld && hero_pin::CameraTransform() &&
            !camOverride_.Following()) {
            if (camAwaySince_ < 0) camAwaySince_ = now;
            else if (now - camAwaySince_ > 5.0)
                ForgetCamera("that camera has been away from Mario for a while", false);
        } else {
            camAwaySince_ = -1;
        }
    }

    const SM64MarioState& st = mario_->State();
    const bool air = sm64::IsAirborne(st.action) || sm64::IsSubmerged(st.action);
    const float speed = std::sqrt(st.velocity[0] * st.velocity[0] + st.velocity[2] * st.velocity[2]) / float(sm64::TICK_SECONDS) /
                        float(map_.Scale());
    CameraOverride::Plan plan =
        camOverride_.Update(now, mario, air, speed, UpIndex(cfg_.upAxis), sample, followAllowed, p);
    camBlend_ = plan.active ? plan.blend : 0.0f;
    // How much of the time it is placed (while it could be: Mario Mode on, the game running).
    if (camStatLast_ >= 0 && followAllowed && !photoMode_) {
        const double dt = std::min(0.25, now - camStatLast_);
        camStatTime_ += dt;
        if (camBlend_ > 0.95f) camStatPlaced_ += dt;
    }
    camStatLast_ = now;
    // Only writes aimed like the view last rendered, near it: the transform's
    // memory reused by something else is never moved.
    if (GameViewFresh() && Length(gameView_.forward) > 0.5f) {
        plan.checkView = true;
        plan.viewForward = Normalize(gameView_.forward);
        plan.viewPos = gameView_.camPos;
    }
    plan.tag = uint32_t(frameId_);
    hero_pin::SetCameraPlan(plan);
    // (Mario's frame of this Present is drawn where the camera of each view
    // had him, against this pivot: Injector::SetPlacementSource.)
    planPivotValid_ = plan.active;
    planPivot_ = plan.pivot;
    // When the game writes its camera against these frames (for the log).
    {
        const hero_pin::CameraTiming ct = hero_pin::TakeCameraTiming();
        if (camBlend_ > 0.95f && !paused_ && !gamePaused_) {
            ++camWriteHist_[std::min<uint32_t>(3, ct.writes)];
            camAheadUs_ += ct.aheadUs;
            camAheadCount_ += ct.aheadCount;
            camAheadMaxUs_ = std::max(camAheadMaxUs_, ct.aheadMaxUs);
            camCarried_ += ct.carried;
        }
        if (ct.thread) camWriteThread_ = ct.thread;
    }
    CastCameraRays(plan, sample);
    if (plan.active && plan.collide) {
        const bool pulled = plan.reach < 0.9f;
        if (pulled && !camWasPulled_) ++camPulledIn_;
        camWasPulled_ = pulled;
        if (!camCollisionLogged_ && camRayAnswers_ >= 30) {
            camCollisionLogged_ = true;
            LOGI("camera: walls from the game's physics (query %d): the camera comes in front of them", cfg_.cameraQuery);
        }
    }
}

void MarioMod::UpdateCameraLead(const DVec3& mario, double now) {
    if (!mario_ || !mario_->Alive()) return;
    const int up = UpIndex(cfg_.upAxis);
    if (paused_ || !GameViewFresh() || !live_.followHeight || cfg_.followLead <= 0 || camBlend_ > 0.0f) {
        // The camera isn't following Mario right now (the game or Mario is
        // paused, its views stopped coming, or it's told not to follow his
        // height), or the mod places it (camera_override.h): no lead, nothing
        // to learn.
        hero_pin::SetCameraLead(0.0f);
        cameraLead_.Interrupt();
        haveLeadPin_ = false;
        return;
    }
    // The frame the game renders next takes its camera target (and Mario's
    // mesh) from this Present: the camera seen at the next one goes with
    // where Mario is now.
    const double renderedUp = haveLeadPin_ ? leadPinUp_ : mario[up];
    leadPinUp_ = mario[up];
    haveLeadPin_ = true;
    if (gameViewFrame_ == leadViewFrame_) return;
    leadViewFrame_ = gameViewFrame_;
    const bool air = sm64::IsAirborne(mario_->State().action) || sm64::IsSubmerged(mario_->State().action);
    // His vertical speed over his last tick (m/s).
    const double speedUp =
        (map_.ToGame(mario_->Position(1.0f))[up] - map_.ToGame(mario_->Position(0.0f))[up]) / sm64::TICK_SECONDS;
    CameraLead::Params p;
    p.gain = cfg_.followLead;
    p.maxLead = cfg_.followLeadMax;
    const float lead = cameraLead_.Update(now, renderedUp, speedUp, gameView_.camPos[up], air, p);
    hero_pin::SetCameraLead(live_.followHeight ? lead : 0.0f);

    // A camera that stops while Mario keeps going: worth a line in the log.
    camHistory_.push_back({now, gameView_.camPos, mario});
    while (camHistory_.size() > 2 && now - camHistory_.front().t > 1.0) camHistory_.erase(camHistory_.begin());
    const CamSample& old = camHistory_.front();
    if (now - old.t > 0.9 && !paused_ && now > cameraStuckUntil_ && cameraStuckLogs_ < 6 &&
        Length(mario - old.mario) > 2.0 && Length(gameView_.camPos - old.cam) < 0.02) {
        ++cameraStuckLogs_;
        cameraStuckUntil_ = now + 20.0;
        LOGI("camera: the game camera stayed put for a second while Mario moved %.1f m (camera at (%.1f %.1f %.1f), "
             "Mario at (%.1f %.1f %.1f), %s, lead %.2f m)",
             Length(mario - old.mario), gameView_.camPos.x, gameView_.camPos.y, gameView_.camPos.z, mario.x, mario.y,
             mario.z, air ? "in the air" : "on the ground", lead);
    }
}

void MarioMod::LogFollowStats(bool final) {
    lastPinStatsLog_ = NowSeconds();
    const hero_pin::Stats st = hero_pin::TakeStats();
    uint64_t undone = 0;
    std::string per;
    for (int k = 0; k < hero_pin::kKinds; ++k) {
        undone += st.undone[k];
        if (st.undone[k])
            per += Fmt("%s%s %llu", per.empty() ? "" : ", ", hero_pin::KindName(k), static_cast<unsigned long long>(st.undone[k]));
    }
    if (hero_pin::TransformHooks() || hero_pin::CameraHooks())
        LOGI("hero pin: the game moved Spider-Man off Mario %llu time(s)%s%s, %llu far move(s) let through; camera target "
             "reads following Mario: %llu",
             static_cast<unsigned long long>(undone), per.empty() ? "" : " - undone: ", per.c_str(),
             static_cast<unsigned long long>(st.passed), static_cast<unsigned long long>(st.targetReads));
    std::vector<hero_pin::Writer> w = st.writers;
    std::sort(w.begin(), w.end(), [](const hero_pin::Writer& a, const hero_pin::Writer& b) { return a.count > b.count; });
    for (size_t i = 0; i < w.size() && i < 5; ++i)
        LOGI("hero pin:   %s from exe+0x%llx: %u time(s), up to %.2f m", hero_pin::KindName(w[i].kind),
             static_cast<unsigned long long>(w[i].caller - game_.ModuleBase()), w[i].count, w[i].maxOffset);
    if (st.unhideBlocked) {
        std::vector<hero_pin::Writer> u = st.unhiders;
        std::sort(u.begin(), u.end(), [](const hero_pin::Writer& a, const hero_pin::Writer& b) { return a.count > b.count; });
        std::string from;
        for (size_t i = 0; i < u.size() && i < 3; ++i)
            from += Fmt("%sexe+0x%llx (%u)", i ? ", " : "", static_cast<unsigned long long>(u[i].caller - game_.ModuleBase()),
                        u[i].count);
        LOGI("hero: the game tried to show Spider-Man %llu time(s) - kept hidden (from %s)",
             static_cast<unsigned long long>(st.unhideBlocked), from.c_str());
    }
    if (worldRender_) {
        const std::string pairing = PairingSummary();
        if (!pairing.empty()) LOGI("frames: %s", pairing.c_str());
        const std::string stencil = StencilStatus();
        if (!stencil.empty()) LOGI("%s", stencil.c_str());
    }
    const std::string sum = follow_.Summary();
    if (!sum.empty()) LOGI("camera follow%s: %s", final ? "" : " so far", sum.c_str());
    if (camRayAnswers_)
        LOGI("camera: %llu wall ray(s) answered, %llu hit a wall; the camera came in front of a wall %llu time(s)",
             static_cast<unsigned long long>(camRayAnswers_), static_cast<unsigned long long>(camRayHits_),
             static_cast<unsigned long long>(camPulledIn_));
    camRayAnswers_ = camRayHits_ = camPulledIn_ = 0;
    if (cfg_.cameraOverride && camStatTime_ > 1.0) {
        LOGI("camera: placed by the mod %.0f%% of the last %.0f s (found %d time(s), dropped %d%s%s), %.1fx the game's "
             "distance",
             100.0 * camStatPlaced_ / camStatTime_, camStatTime_, camStatFound_, camStatLost_,
             camStatWhy_.empty() ? "" : ": ", camStatWhy_.c_str(), double(live_.cameraDistance));
        const uint64_t frames = camWriteHist_[0] + camWriteHist_[1] + camWriteHist_[2] + camWriteHist_[3];
        if (frames > 0) {
            auto pc = [&](uint64_t c) { return 100.0 * double(c) / double(frames); };
            LOGI("camera: in %llu of the mod's frames the game wrote its camera 0 times %.0f%%, once %.0f%%, twice "
                 "%.0f%%, more %.0f%%; %.1f ms after the mod's frame on average (up to %.1f ms), on thread %u (the "
                 "mod's frames on thread %u%s); %llu write(s) left to the game (not like its camera's)",
                 static_cast<unsigned long long>(frames), pc(camWriteHist_[0]), pc(camWriteHist_[1]), pc(camWriteHist_[2]),
                 pc(camWriteHist_[3]), camAheadCount_ ? double(camAheadUs_) / double(camAheadCount_) / 1000.0 : 0.0,
                 double(camAheadMaxUs_) / 1000.0, camWriteThread_, presentThread_,
                 camCarried_ ? Fmt(": Mario's place as it was a frame before, for %llu write(s)",
                                   static_cast<unsigned long long>(camCarried_))
                                   .c_str()
                             : "",
                 static_cast<unsigned long long>(camRefusedTotal_));
        }
        for (uint64_t& c : camWriteHist_) c = 0;
        camAheadUs_ = camAheadCount_ = camCarried_ = 0;
        camAheadMaxUs_ = 0;
        camRefusedTotal_ = 0;
        if (alignViews_ > 0) {
            auto pc = [&](uint64_t c) { return 100.0 * double(c) / double(alignViews_); };
            LOGI("camera: Mario drawn where the camera of the view had him - %llu views read back: the view's own "
                 "placement found in %.0f%% (the newest one's in %.0f%%), %.1f cm from the Mario frame drawn on "
                 "average, up to %.1f cm",
                 static_cast<unsigned long long>(alignViews_), pc(alignMatched_), pc(alignViews_ - alignMatched_),
                 100.0 * alignShiftSum_ / double(alignViews_), 100.0 * alignShiftMax_);
        }
        alignViews_ = alignMatched_ = 0;
        alignShiftSum_ = alignShiftMax_ = 0;
        // Where the views came from while the mod placed the camera, and the
        // game's other cameras.
        const uint64_t elsewhere = camViewsElsewhere_[0] + camViewsElsewhere_[1] + camViewsElsewhere_[2] + camViewsElsewhere_[3];
        if (camViewsOurs_ + elsewhere > 0) {
            LOGI("camera: views while placed: %llu from the mod's placements, %llu from elsewhere (under 0.3 m off %llu, "
                 "0.3-1.5 m %llu, 1.5-5 m %llu, further %llu; %llu not matched to a transform, %llu where the game itself "
                 "had the camera the mod places); the game's cameras placed: up to %d at once (another one placed too %d "
                 "time(s), the first one replaced %d, let go %d)",
                 static_cast<unsigned long long>(camViewsOurs_), static_cast<unsigned long long>(elsewhere),
                 static_cast<unsigned long long>(camViewsElsewhere_[0]), static_cast<unsigned long long>(camViewsElsewhere_[1]),
                 static_cast<unsigned long long>(camViewsElsewhere_[2]), static_cast<unsigned long long>(camViewsElsewhere_[3]),
                 static_cast<unsigned long long>(camViewsUnmatched_), static_cast<unsigned long long>(camViewsGameOwn_),
                 camPlacedMost_, camAdopted_, camPromoted_, camLetGo_);
        }
        camPlacedMost_ = CamerasPlaced();
        camViewsOurs_ = camViewsUnmatched_ = camViewsGameOwn_ = 0;
        for (uint64_t& c : camViewsElsewhere_) c = 0;
        camAdopted_ = camPromoted_ = camLetGo_ = 0;
        if (fovFrames_ > 0)
            LOGI("camera: field of view %.1f-%.1f degrees (%llu change(s) of more than a degree between views)",
                 double(fovMin_) * 57.29578, double(fovMax_) * 57.29578, static_cast<unsigned long long>(fovJumps_));
        fovFrames_ = fovJumps_ = 0;
        fovMin_ = 1e9f;
        fovMax_ = 0;
        camStatTime_ = camStatPlaced_ = 0;
        camStatFound_ = camStatLost_ = 0;
        camStatWhy_.clear();
    }
    if (cfg_.followLead > 0 && cameraLead_.Learnt())
        LOGI("camera lead: the game camera trails its target by %.2f s (learnt from Mario's jumps); its target is led "
             "that far along them",
             cameraLead_.Lag());
}

void MarioMod::Respawn(const DVec3& feet, const char* why, bool quiet) {
    LOGI("respawn: %s", why);
    cutPending_ = true;
    if (Length(feet - MarioGamePos(1.0f)) > 20.0) ResetWorld(feet);
    float face = mario_->Alive() ? mario_->State().faceAngle : 0.0f;
    const int16_t health = mario_->Alive() ? mario_->State().health : sm64::HEALTH_FULL;
    // The way he faces, in the game's world (the mapping may be mirrored now).
    const Vec3 facing = map_.DirToGame(Vec3(std::sin(face), 0.0f, std::cos(face)));
    const bool mirror = WantMirror();
    map_.Configure(cfg_.upAxis, mirror, cfg_.unitsPerMetre);
    {
        const Vec3 l = map_.DirToLocal(facing);
        if (l.x * l.x + l.z * l.z > 1e-6f) face = std::atan2(l.x, l.z);
    }
    if (activeSource_ == Source::Flat && std::fabs(feet[UpIndex(cfg_.upAxis)] - flatRays_->Height()) > 3.0)
        flatRays_->SetHeight(feet[UpIndex(cfg_.upAxis)]);
    if (source_ == Source::Physics && Length(feet - MarioGamePos(1.0f)) > 20.0) {
        // Far away: what the game said about the old place is no use.
        physics_.Reset();
        physicsLive_ = false;
        game_physics::Clear();
        flatRays_->SetHeight(feet[UpIndex(cfg_.upAxis)]);
    } else if (source_ == Source::Physics && !physics_.Ready(feet)) {
        // The game hasn't been asked about the ground there yet (0.5: a
        // teleport of 12-20 m found no floor and switched Mario Mode off):
        // a flat floor where Spider-Man was put, until it has - as after M.
        physicsLive_ = false;
        flatRays_->SetHeight(feet[UpIndex(cfg_.upAxis)]);
    }
    if (!SpawnAt(feet, face)) {
        // (Nothing at all under him: stand him on a flat floor there.)
        physicsLive_ = false;
        flatRays_->SetHeight(feet[UpIndex(cfg_.upAxis)]);
        if (!SpawnAt(feet, face)) {
            Deactivate("Mario couldn't respawn (no floor)");
            return;
        }
        LOGI("respawn: no floor known there yet - Mario stands on a flat floor at %.2f until the game's physics answers",
             feet[UpIndex(cfg_.upAxis)]);
    }
    enemies_.clear();
    lastHeroHealth_ = -1;
    if (quiet) {
        // Same Mario, new mapping: keep his health and don't announce it.
        if (health >= 0x100) api_.set_mario_health(mario_->Id(), uint16_t(health));
        return;
    }
    Toast(std::string("Mario respawned: ") + why, 3);
}

void MarioMod::Activate() {
    if (!sm64Ready_) {
        Toast(sm64Error_, 8, kRed);
        return;
    }
    if (!game_.Ready()) {
        Toast("Spider-Man 2 hooks not found (game updated?) - see sm2mario.log", 8, kRed);
        return;
    }
    guard::PhaseScope phase("activate");
    uintptr_t hero = game_.Hero();
    DVec3 feet;
    if (!hero || !game_.GetPosition(hero, feet)) {
        Toast("Load into the city first", 4, kYellow);
        return;
    }
    Breadcrumb(0, "activation: hero found");
    const bool mirror = WantMirror();
    mirrorDecided_ = cfg_.mirror != "auto" || HandednessKnown();
    map_.Configure(cfg_.upAxis, mirror, cfg_.unitsPerMetre);
    flatRays_->SetHeight(feet[UpIndex(cfg_.upAxis)]);
    ResetWorld(feet);
    // The rendered world starts from the depth frames that come after M.
    depthFramesAtActivate_ = depthFrames_;
    // The game's physics: asked afresh around Mario (Spider-Man is left out of every ray).
    physics_.Reset();
    physicsLive_ = false;
    physicsChanged_ = false;
    lastPhysicsAnswer_ = -1e9;
    lastPhysicsLog_ = NowSeconds();
    {
        const game_physics::Heartbeat hb = game_physics::GetHeartbeat();
        physicsCallsAtActivate_ = hb.calls;
        physicsStepsAtActivate_ = hb.steps;
    }
    actorKinds_.clear();
    game_physics::Clear();
    game_physics::TakeStats();
    game_physics::SetActive(source_ == Source::Physics, hero);
    sourceChosen_ = false;
    activeSource_ = Source::Flat;
    haveSafe_ = false;
    cutPending_ = true;
    if (!SpawnAt(feet, FaceAngleFromHero(hero))) {
        Toast("Mario couldn't find a floor to spawn on", 5, kRed);
        return;
    }
    Breadcrumb(1, "activation: Mario spawned in libsm64");
    boundHero_ = 0;
    BindHero(hero);
    Breadcrumb(2, heroHealth_ ? "activation: hero Health component found" : "activation: no hero Health component");
    enemies_.clear();
    scanCursor_ = 0;
    hitsLanded_ = hitsTaken_ = 0;
    heroMissingFrames_ = 0;
    pinWarned_ = false;
    follow_.Reset();
    cameraLead_.Reset();
    haveLeadPin_ = false;
    camHistory_.clear();
    ReleaseCamera();
    camGaveUp_ = false;
    camRejected_.clear();
    camSearchRounds_ = 0;
    camSearchAfter_ = 0;
    camAwaySince_ = -1;
    interactUntil_ = -1;
    wasInteracting_ = false;
    lastPinStatsLog_ = NowSeconds();
    hero_pin::TakeStats(); // start counting from here
    active_ = true;
    // Switched on in photo mode: his spawn spin lands before he holds still.
    photoTicks_ = photoMode_ ? 30 : 0;
    photoHoldPose_ = Pose::None;
    audio_.Start();
    worldRender_ = cfg_.renderMode == "world" && tracker::Installed() && !injector_.Failed();
    worldFallbackDone_ = false;
    activatedAt_ = NowSeconds();
    injectionsAtActivate_ = tracker::GetStats().gbufferInjections;
    shadowInjectionsAtActivate_ = tracker::GetStats().shadowInjections;
    shadowRegionsAtActivate_ = tracker::GetStats().shadowRegions;
    tracker::SetActive(worldRender_);
    if (worldRender_ && cfg_.frameReport && !reportedOnce_) {
        reportedOnce_ = true;
        tracker::RequestReport(2, "Mario Mode on");
    }
    const tracker::Stats ts = tracker::GetStats();
    LOGI("Mario activated at (%.2f %.2f %.2f), mirror=%d, collision=%s, rendering=%s (pipelines: %llu, %llu G-buffer, "
         "%llu shadow casters, %llu cache copies)",
         feet.x, feet.y, feet.z, int(mirror), SourceName(source_), worldRender_ ? "in the game's frame" : "overlay",
         static_cast<unsigned long long>(ts.psos), static_cast<unsigned long long>(ts.gbufferPsos),
         static_cast<unsigned long long>(ts.casterPsos), static_cast<unsigned long long>(ts.cacheCopyPsos));
    Toast(source_ == Source::Flat ? "Mario Mode ON  (flat ground only)" : "Mario Mode ON", 4, kGreen);
    if (!worldRender_ && !camera_.Valid()) Toast("Looking for the camera... move it around a little", 6, kYellow);
}

void MarioMod::Deactivate(const char* reason) {
    uintptr_t hero = game_.Hero();
    interactUntil_ = -1;
    ReleasePhotoStandIns(true);
    photoTicks_ = 0;
    photoHoldPose_ = Pose::None;
    if (hero && mario_ && mario_->Alive()) PinHero(hero, MarioGamePos(1.0f));
    hero_pin::ClearPin();
    ReleaseCamera();
    LogFollowStats(true);
    if (source_ == Source::Physics) LogPhysicsStats();
    game_physics::SetActive(false, 0);
    game_physics::Clear();
    hero_.Release();
    tracker::SetActive(false);
    PublishMario(0.0f, false);
    if (mario_) mario_->Despawn();
    audio_.Stop();
    input_shim::SetBlocking(false);
    active_ = false;
    paused_ = false;
    havePin_ = false;
    boundHero_ = 0;
    heroHealth_ = 0;
    LOGI("Mario deactivated: %s", reason);
    Toast(std::string("Mario Mode OFF") + (std::string(reason) == "toggled off" ? "" : std::string(" - ") + reason), 3);
}

// ------------------------------------------------------------------ photo mode

// The game's photo mode (game/photo_mode.h): Mario holds still in it (his
// HUD off the screen), strikes a pose and holds it on the pose key, and takes
// the place of the copy of Spider-Man the game poses for selfies.
void MarioMod::UpdatePhotoMode(double now) {
    (void)now;
    const bool open = photo_mode::Available() && photo_mode::Open();
    if (open != photoMode_) {
        photoMode_ = open;
        ++photoTransitions_;
        if (open) {
            if (photoTransitions_ <= 6 && active_)
                LOGI("photo mode: open - Mario holds still for the shot (the pose key strikes a pose and holds it)");
            photoCaps_ = CapsWanted();
        } else {
            if (photoTransitions_ <= 6 && active_) LOGI("photo mode: closed - Mario carries on");
            photoTicks_ = 0;
            photoHoldPose_ = Pose::None;
        }
        if (!open) ReleasePhotoStandIns(false);
    }
    // The pose key strikes Mario's pose in photo mode: the game (which gets
    // every key in a pause) mustn't see it.
    const bool blockPose = photoMode_ && active_;
    if (blockPose != photoPoseBlocked_) {
        photoPoseBlocked_ = blockPose;
        input_shim::SetAlsoBlocked(blockPose ? poseVks_ : std::vector<int>());
    }
    if (!photoMode_ || !active_ || !mario_ || !mario_->Alive()) {
        if (photoKept_) ReleasePhotoStandIns(true);
        return;
    }
    // A cap switched on or off in photo mode (F8 or the pause menu): let it
    // go on (Mario puts it on, about a second).
    const uint32_t caps = CapsWanted();
    if (caps != photoCaps_) {
        photoCaps_ = caps;
        photoTicks_ = std::max(photoTicks_, 45);
    }
    // The selfie's copy of Spider-Man (and its head, and the phone it holds):
    // hidden while Mario stands in, kept hidden if the game tries to show it.
    if (!game_.HideAvailable()) return;
    const photo_mode::StandIns si = photo_mode::ReadStandIns();
    const uint32_t handles[3] = {si.doppelganger, si.head, si.phone};
    uintptr_t keep[3] = {};
    int n = 0, newlyHidden = 0;
    for (uint32_t h : handles) {
        if (!h) continue;
        const uintptr_t a = game_.ActorFromHandle(h);
        uintptr_t t = 0;
        if (!a || !game_.GetTransform(a, t) || !t) continue;
        uint32_t flags = 0;
        if (game_.TransformFlags(a, flags) && !(flags & game_.Layout().transformHiddenBit)) {
            if (game_.SetHidden(a, true)) {
                ++newlyHidden;
                bool known = false;
                for (uint32_t k : photoHid_)
                    if (k == h) known = true;
                if (!known) photoHid_.push_back(h);
            }
        }
        keep[n++] = t;
    }
    hero_pin::KeepHiddenToo(keep, n);
    photoKept_ = n > 0;
    if (newlyHidden && !photoStandInsLogged_) {
        photoStandInsLogged_ = true;
        LOGI("photo mode: the selfie's copy of Spider-Man is hidden - Mario is the one in the shot (%d actor(s))",
             newlyHidden);
    }
}

// Stops keeping photo mode's stand-ins hidden; `show`: Mario Mode is going
// away with photo mode still open - give the game its selfie Spider-Man back.
void MarioMod::ReleasePhotoStandIns(bool show) {
    hero_pin::KeepHiddenToo(nullptr, 0);
    photoKept_ = false;
    if (show && photoMode_)
        for (uint32_t h : photoHid_)
            if (const uintptr_t a = game_.ActorFromHandle(h)) game_.SetHidden(a, false);
    photoHid_.clear();
}

uint32_t MarioMod::CapsWanted() const {
    return (live_.wingCap ? 1u : 0u) | (live_.metalCap ? 2u : 0u) | (live_.vanishCap ? 4u : 0u);
}

// ------------------------------------------------------------------ frame

// `now`: when this Present began - the moment `dt` was measured to, which
// Mario's interpolation and the camera's easing both go by. (0.6 took the
// camera's time after the frame boundary's work, which takes longer in some
// frames than in others: the camera eased by a few milliseconds in a frame
// in which Mario moved by fifty, and its height jumped against his.)
void MarioMod::Frame(IDXGISwapChain* sc, double dt, double now) {
    presentThread_ = uint32_t(GetCurrentThreadId());
    hero_pin::SetPresentThread(presentThread_);
    planPivotValid_ = false; // (set again when the camera plan is made)
    if (!textureHandedToRenderer_ && sm64Ready_) {
        renderer_.SetMarioTexture(marioTexture_.data(), SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT);
        renderer_.SetPaperWhite(cfg_.paperWhiteNits);
        textureHandedToRenderer_ = true;
    }
    guard::SetPhase("game view");
    TakeGameViews();
    if (active_ && worldRender_) UpdateDrawLag(now);
    UpdateStencilMark(now);
    CheckWorldRendering();
    guard::SetPhase("input");
    input_shim::SubclassWindow(renderer_.Window());
    if (now - lastHookPoll_ > 2.0) {
        input_shim::InstallHooks();
        lastHookPoll_ = now;
    }
    input_.Poll(renderer_.Window());
    overlay_.Begin(float(renderer_.Width()), float(renderer_.Height()), renderer_.Font());
    guard::SetPhase("settings");
    HandleMenu();
    guard::SetPhase("input");

    if (input_.Pressed(Hotkey::Toggle)) {
        if (active_) Deactivate("toggled off");
        else Activate();
    }
    if (input_.Pressed(Hotkey::DebugOverlay)) debugOverlay_ = !debugOverlay_;
    if (input_.Pressed(Hotkey::Dump)) DumpDebug();
    if (input_.Pressed(Hotkey::Rescan)) {
        camera_.RequestRescan();
        Toast("Rescanning memory for the camera...", 3);
    }
    if (input_.Pressed(Hotkey::NextCamera)) camera_.CycleCandidate(1);
    if (input_.Pressed(Hotkey::PrevCamera)) camera_.CycleCandidate(-1);
    if (input_.Pressed(Hotkey::FovUp) || input_.Pressed(Hotkey::FovDown)) {
        camera_.AdjustFovOffset(input_.Pressed(Hotkey::FovUp) ? 1.0f : -1.0f);
        const float deg = camera_.FovY() * 57.29578f;
        Toast(Fmt("Mario FOV %.0f deg  (to keep it: FovDegrees = %.0f under [Camera] in sm2mario.ini)", deg, deg), 4);
        LOGI("FOV adjusted to %.1f deg", deg);
    }
    if (active_ && input_.Pressed(Hotkey::Mirror)) {
        cfg_.mirror = map_.Mirrored() ? "off" : "on";
        mirrorDecided_ = true;
        Respawn(MarioGamePos(1.0f), "mirroring toggled");
    }
    if (active_ && input_.Pressed(Hotkey::Respawn)) Respawn(haveSafe_ ? lastSafe_ : MarioGamePos(1.0f), "respawn key");
    guard::SetPhase("photo mode");
    UpdatePhotoMode(now);
    guard::SetPhase("input");
    if (active_ && !menu_.Open() && input_.Pressed(Hotkey::Pose) && mario_->Alive()) {
        // Facing the camera, so the player sees it. (Started on the next tick
        // Mario is standing or walking.)
        static const Pose kCycle[] = {Pose::Wave, Pose::PeaceSign, Pose::StarDance};
        DVec3 cam;
        if (photoMode_ && !PoseController::CanPose(mario_->State().action) &&
            mario_->State().action != sm64::ACT_END_WAVING_CUTSCENE) {
            if (photoCantPoseLogs_++ < 3) LOGI("photo mode: Mario can only pose standing on the ground");
        } else {
            pendingPose_ = kCycle[poseCycle_++ % 3];
            pendingFace_ = CameraPosition(cam) ? FaceToward(cam) : mario_->State().faceAngle;
            pendingFromKey_ = true;
            pendingUntil_ = now + 1.0;
            // In photo mode the world stands still: Mario's own ticks run just
            // until the pose shows best, and he holds it for the shot.
            if (photoMode_) {
                photoTicks_ = PoseController::HoldTicks(pendingPose_);
                photoHoldPose_ = pendingPose_;
            }
        }
    }
    guard::SetPhase("photo requests");
    TakePhotoRequests();
    if (active_ && !menu_.Open() && input_.InteractPressed()) AnswerInteraction(now);
    guard::SetPhase("input");
    if (input_.Pressed(Hotkey::Calibrate)) {
        calibrate_ = !calibrate_;
        Toast(calibrate_ ? "Calibration: red cube = feet, green = head. They should stick to Spider-Man."
                         : "Calibration marker off",
              5);
    }

    MarioDraw draw;
    bool heroPresent = false;
    if (active_) {
        guard::SetPhase("follow hero");
        uintptr_t hero = game_.Hero();
        DVec3 heroPos;
        if (!hero || !game_.GetPosition(hero, heroPos)) {
            // Brief dropouts happen around loads/cutscenes; only give up if it lasts.
            if (heroMissingFrames_ == 0) {
                game_physics::SetHero(0); // (the old actor may be gone)
                boundHero_ = 0;
                hero_.Suspend();          // (... and his transform with it)
                CameraOverride::Plan off; // the camera is the game's meanwhile
                hero_pin::SetCameraPlan(off);
                camOverride_.ResetBlend(); // (eased in again after)
                camBlend_ = 0;
            }
            if (++heroMissingFrames_ > 45) Deactivate("Spider-Man isn't available (loading or cutscene)");
        } else {
            heroMissingFrames_ = 0;
            heroPresent = true;
            BindHero(hero);
            if (!wasInteracting_) SampleFollow(heroPos, true);
            const double residual = havePin_ && !wasInteracting_ ? Length(heroPos - lastPinned_) : 0.0;
            if (havePin_ && residual > cfg_.teleportDistance && prevResidual_ < 1.0) {
                // A sudden jump right after a good pin: the game moved the hero
                // itself (fast travel, cutscene, checkpoint).
                if (cfg_.onTeleport == "follow")
                    Respawn(heroPos, Fmt("the game moved Spider-Man %.1f m, to (%.1f %.1f %.1f)", residual, heroPos.x,
                                         heroPos.y, heroPos.z)
                                         .c_str());
                else Deactivate("the game moved Spider-Man");
            } else {
                // A steady offset means the pin isn't sticking (the game's
                // character controller puts him back every frame).
                pinDriftFrames_ = residual > 0.5 ? pinDriftFrames_ + 1 : 0;
                if (pinDriftFrames_ == 90 && !pinWarned_) {
                    pinWarned_ = true;
                    LOGW("pin drift: Spider-Man ends up %.2f m from Mario every frame - the game camera won't follow "
                         "Mario. Try [Hero] PositionWrites = %s",
                         residual, cfg_.positionWrites == "direct" ? "function" : "direct");
                    Toast("Spider-Man isn't following Mario (see sm2mario.log)", 6, kYellow);
                }
            }
            prevResidual_ = residual;
        }
    }
    guard::SetPhase("game pause");
    UpdateGamePause(now);
    if (active_ && heroPresent) {
        // Mario waits while the game is paused, and while his menu is open
        // (the game still doesn't get his keys then).
        paused_ = gamePaused_ || photoMode_ || !input_.Focused() ||
                  (cfg_.pauseWhenCursorVisible && input_.CursorVisible()) || menu_.Open();
        input_shim::SetBlocking(cfg_.blockGameInput && (!paused_ || menu_.Open()));
        audio_.SetPaused(paused_);
        guard::SetPhase("game physics");
        PumpPhysics(now);
        const float aspect = renderer_.Height() > 0 ? float(renderer_.Width()) / float(renderer_.Height()) : 16.0f / 9.0f;
        guard::SetPhase("camera");
        if (!GameViewFresh()) camera_.Update(MarioGamePos(1.0f), map_.GameUp(), aspect);
        if (!mirrorDecided_ && cfg_.mirror == "auto" && HandednessKnown()) {
            mirrorDecided_ = true;
            if (LeftHanded() != map_.Mirrored())
                Respawn(MarioGamePos(1.0f), "matched the game's handedness", true);
        }
        if (!paused_) {
            accumulator_ += dt;
            int n = 0;
            while (active_ && accumulator_ >= sm64::TICK_SECONDS && n < 4) {
                SimTick();
                accumulator_ -= sm64::TICK_SECONDS;
                ++n;
            }
            if (n == 4) accumulator_ = 0;
        } else if (photoMode_ && photoTicks_ > 0 && !menu_.Open() && input_.Focused()) {
            // Photo mode: the world stands still, Mario runs on his own just
            // for a pose (or a cap going on) - without the player's input,
            // which moves the game's photo camera.
            accumulator_ += dt;
            int n = 0;
            photoTicking_ = true;
            while (active_ && photoTicks_ > 0 && accumulator_ >= sm64::TICK_SECONDS && n < 4) {
                SimTick();
                accumulator_ -= sm64::TICK_SECONDS;
                --photoTicks_;
                ++n;
            }
            photoTicking_ = false;
            if (photoTicks_ == 0) {
                // Hold the last tick's pose exactly (no interpolation back towards the one before).
                accumulator_ = sm64::TICK_SECONDS * 0.999;
                if (photoHoldPose_ != Pose::None && pose_.Active())
                    LOGI("photo mode: Mario holds the %s for the shot", PoseName(photoHoldPose_));
                photoHoldPose_ = Pose::None;
            }
        }
        if (active_) {
            const float alpha = Clamp(float(accumulator_ / sm64::TICK_SECONDS), 0.0f, 1.0f);
            const DVec3 pin = MarioGamePos(alpha);
            uintptr_t hero = game_.Hero();
            if (hero) {
                guard::SetPhase("move hero with Mario");
                PinHero(hero, pin);
                Breadcrumb(4, "first frame: Spider-Man moved with Mario");
            } else {
                hero_pin::ClearPin();
            }
            SampleFollow(pin, false);
            guard::SetPhase("camera override");
            UpdateCameraOverride(pin, now);
            UpdateCameraLead(pin, now);
            if (now - lastPinStatsLog_ > 60.0) LogFollowStats(false);
            // Only hide Spider-Man while Mario can actually be drawn.
            guard::SetPhase("hide hero");
            hero_.Update(hero, worldRender_ || camera_.Valid());
            if (hero_.Hidden()) Breadcrumb(5, "Spider-Man hidden");
            guard::SetPhase("build Mario draw");
            PublishMario(alpha, worldRender_);
            if (!worldRender_ && camera_.Valid()) BuildDraw(alpha, draw);
        }
    } else if (active_) {
        // Hero missing this frame: hold still (and hold nothing else: his
        // transform may be gone).
        hero_pin::ClearPin();
        input_shim::SetBlocking(false);
        paused_ = true;
        audio_.SetPaused(true);
        PublishMario(0.0f, false);
    } else {
        input_shim::SetBlocking(false);
        hero_.Maintain();
        if (calibrate_) {
            BuildCalibration(draw);
        } else if (!camera_.HandednessKnown() && camera_.ScanCount() < 2) {
            // Pre-warm: find the camera (and the world's handedness) while
            // Spider-Man is around, so Mario is visible right after M.
            uintptr_t hero = game_.Hero();
            DVec3 heroPos;
            if (hero && game_.GetPosition(hero, heroPos)) {
                const float aspect =
                    renderer_.Height() > 0 ? float(renderer_.Width()) / float(renderer_.Height()) : 16.0f / 9.0f;
                camera_.Update(heroPos, cfg_.upAxis == 'Z' ? Vec3(0, 0, 1) : Vec3(0, 1, 0), aspect);
            }
        }
    }
    guard::SetPhase("hud");
    DrawHud();
    DrawMenu();
    guard::SetPhase("render");
    renderer_.Render(sc, draw, overlay_.Vertices());
    if (draw.enabled && draw.geo && draw.geo->triangles) Breadcrumb(6, "first Mario frame drawn");
    guard::SetPhase("frame");
}

void MarioMod::SimTick() {
    const double now = NowSeconds();
    const SM64MarioState prev = mario_->State();
    const int32_t id = mario_->Id();
    const float s = float(map_.Scale());
    Vec3 pos(prev.position[0], prev.position[1], prev.position[2]);

    SM64MarioInputs in{};
    if (!photoTicking_) {
        const MarioButtons& b = input_.Mario();
        in.stickX = b.stickX;
        in.stickY = b.stickY;
        in.buttonA = b.a;
        in.buttonB = b.b;
        in.buttonZ = b.z;
    }
    Vec3 look(0, 0, 1);
    if (GameViewFresh()) {
        // The exact camera the game rendered with.
        const Vec3 camLocal = map_.ToLocal(gameView_.camPos);
        look = pos - camLocal;
        if (look.x * look.x + look.z * look.z < 1.0f) look = map_.DirToLocal(gameView_.forward);
    } else if (camera_.Valid()) {
        Vec3 camLocal = map_.ToLocal(camera_.Pose().position);
        look = pos - camLocal;
        if (look.x * look.x + look.z * look.z < 1.0f) look = map_.DirToLocal(camera_.Pose().forward);
    }
    in.camLookX = look.x;
    in.camLookZ = look.z;

    UpdateCollisionSource();
    if (worldChangedNear_.load() && now - lastBuildTime_ > 0.15) {
        worldChangedNear_.store(false);
        forceRebuild_ = true;
    }
    if (physicsChanged_ && activeSource_ == Source::Physics && now - lastBuildTime_ > 0.1) {
        physicsChanged_ = false;
        forceRebuild_ = true;
    }
    // Leaving the ground (or landing) changes where the safety floor goes.
    if (activeSource_ == Source::Physics && sm64::IsAirborne(prev.action) != physics_.Airborne()) forceRebuild_ = true;
    Vec3 moved = pos - lastBuildCenter_;
    if (forceRebuild_ || std::sqrt(moved.x * moved.x + moved.z * moved.z) > cfg_.rebuildDistance * s ||
        std::fabs(moved.y) > cfg_.rebuildVertical * s || now - lastBuildTime_ > cfg_.rebuildSeconds)
        RebuildCollision();

    UpdateWater();

    // SM64 applies its own fall damage when it lands with vel.y < -55, using a
    // peak height the floating origin shifts invalidate. perform_air_step()
    // applies gravity (-4) after the landing quarter-step and before
    // check_fall_damage() reads vel.y, so cap the approach speed at -51 (-> -55
    // at the check, which is not < -55). The mod applies its own fall damage
    // from game-space heights in HandleFall().
    if (sm64::IsAirborne(prev.action) && prev.velocity[1] < -51.0f) {
        const float floorY = api_.surface_find_floor_height(pos.x, pos.y, pos.z);
        if (pos.y + prev.velocity[1] * 1.5f <= floorY + 10.0f)
            api_.set_mario_velocity(id, prev.velocity[0], -51.0f, prev.velocity[2]);
    }

    guard::SetPhase("poses and cheats");
    UpdatePoses(prev, in);
    cheats_.BeforeTick(api_, id, prev, in, live_);

    guard::SetPhase("libsm64 tick");
    mario_->Tick(in);
    ++tickCount_;
    Breadcrumb(3, "first libsm64 tick");
    const SM64MarioState& cur = mario_->State();
    guard::SetPhase("poses and cheats");
    cheats_.AfterTick(api_, id, cur, live_);
    pos = Vec3(cur.position[0], cur.position[1], cur.position[2]);
    if (cfg_.traceCollision) TraceTick(cur, in);
    HandleFall(prev, cur);
    NoteBumpsAndFalls(prev, cur, in, now);
    if (activeSource_ == Source::Physics && !sm64::IsAirborne(cur.action) && loggedMaterials_.size() < 24 &&
        tickCount_ % 8 == 0) {
        // What the game says he walks on, and the footsteps SM64 plays for it.
        const int m = physics_.FloorMaterialAt(map_.ToGame(pos));
        if (m >= -1 && std::find(loggedMaterials_.begin(), loggedMaterials_.end(), m) == loggedMaterials_.end()) {
            loggedMaterials_.push_back(m);
            LOGI("collision: Mario walks on %s (%s footsteps)", PhysicsMaterialName(m),
                 FootstepSoundName(FootstepSound(SurfaceForMaterial(m))));
        }
    }
    if (cur.action == sm64::ACT_LAVA_BOOST && prev.action != sm64::ACT_LAVA_BOOST && lavaLogs_ < 6) {
        ++lavaLogs_;
        const DVec3 at = map_.ToGame(pos);
        LOGI("collision: Mario landed on lava at (%.1f %.1f %.1f) - the floor there is %s", at.x, at.y, at.z,
             activeSource_ == Source::Physics ? PhysicsMaterialName(physics_.FloorMaterialAt(at)) : "(not the game's physics)");
    }
    RescueFromUnseenGround(cur);
    // (Swimming over deep water the game has no bed for is fine.)
    if (!sm64::IsAirborne(cur.action) && !sm64::IsSubmerged(cur.action) && pos.y >= float(waterLevelLocal_) &&
        collisionStats_.safetyFloorY > -1e29f && pos.y - collisionStats_.safetyFloorY < 2.0f * s &&
        api_.surface_find_floor_height(pos.x, pos.y + 10.0f, pos.z) <= collisionStats_.safetyFloorY + 5.0f) {
        // Standing on the safety floor under the sampled area: he fell out of
        // the world. (Not a safe spot to come back to.) Only standing on it:
        // 0.4 also counted the frame the game's answer for his spot came in
        // with no floor under it (Spider-Man on a wall, a spire), and gave up.
        Respawn(haveSafe_ ? lastSafe_ : MarioGamePos(1.0f), "fell out of the world");
        return;
    }

    if (sm64::IsDying(cur.action) || cur.health < 0x100) {
        deathTimer_ += sm64::TICK_SECONDS;
        if (deathTimer_ > 2.5) {
            deathTimer_ = 0;
            if (cfg_.onDeath == "hero" && heroHealth_) {
                game_.WriteHealthComponent(heroHealth_, 0.0f);
                Deactivate("Mario ran out of health");
                return;
            }
            Respawn(haveSafe_ ? lastSafe_ : MarioGamePos(1.0f), "ran out of health");
            return;
        }
    } else {
        deathTimer_ = 0;
    }

    const bool air = sm64::IsAirborne(cur.action);
    if (!air && !sm64::IsSubmerged(cur.action) && !sm64::IsDying(cur.action)) {
        lastSafe_ = map_.ToGame(pos);
        haveSafe_ = true;
    }
    // Simulated time, not wall-clock: pausing mid-jump must not count. Only
    // an uninterrupted fall counts (flying with the wing cap, moon jumps and
    // BLJs can keep Mario in the air for as long as the player likes).
    const bool falling = air && cur.velocity[1] < 0.0f && cur.action != sm64::ACT_FLYING;
    airborneTicks_ = falling ? airborneTicks_ + 1 : 0;
    if (air && haveSafe_ && airborneTicks_ > int(cfg_.respawnAfterFallSeconds / sm64::TICK_SECONDS)) {
        Respawn(lastSafe_, "fell for too long");
        return;
    }

    guard::SetPhase("combat");
    UpdateCombat(cur, prev.action);
    guard::SetPhase("incoming damage");
    HandleIncomingDamage(cur);
    guard::SetPhase("sim");

    if (cfg_.regenSeconds > 0 && now - lastHurtTime_ > cfg_.regenSeconds && now - lastRegen_ > cfg_.regenSeconds &&
        cur.health < sm64::HEALTH_FULL && cur.health >= 0x100 && !sm64::IsDying(cur.action)) {
        api_.mario_heal(id, 4); // one wedge
        lastRegen_ = now;
    }

    Vec3 shift;
    if (ComputeOriginShift(pos, air, cur.velocity[1], cfg_.origin, shift)) {
        map_.SetOrigin(map_.ToGame(shift));
        mario_->ShiftOrigin(shift);
        fallPath_.clear(); // local coordinates changed
        RebuildCollision();
    }

    if (cfg_.audio) {
        guard::SetPhase("libsm64 audio");
        audio_.Tick();
    }
    guard::SetPhase("sim");
}

// Mario fell through ground libsm64 didn't have yet: the camera hadn't seen
// it (behind a ledge or an obstacle), or the depth frames arrived late (low
// frame rate). Once the ground is known, SM64 can't land him on it if it is
// more than 78 units above him - so if he is falling and a floor he was above
// during this jump now exists above him, put him back on it.
void MarioMod::RescueFromUnseenGround(const SM64MarioState& cur) {
    const Vec3 pos(cur.position[0], cur.position[1], cur.position[2]);
    if (!sm64::IsAirborne(cur.action)) {
        fallPath_.clear();
        return;
    }
    if (fallPath_.size() >= 90) fallPath_.erase(fallPath_.begin()); // 3 s is plenty
    fallPath_.push_back(pos);
    // Floors more than probeUp above him aren't in the collision anyway.
    const float s = float(map_.Scale());
    const float maxClimb = (cfg_.collision.probeUp - 0.5f) * s;
    float floorY = 0;
    if (!GroundAboveFallingMario(api_.surface_find_floor_height, fallPath_.data(), int(fallPath_.size()),
                                 cur.velocity[1], maxClimb, floorY))
        return;
    api_.set_mario_position(mario_->Id(), pos.x, floorY + 1.0f, pos.z);
    api_.set_mario_velocity(mario_->Id(), cur.velocity[0], 0.0f, cur.velocity[2]);
    fallPath_.clear();
    LOGI("Mario had fallen %.2f m through ground the game hadn't shown yet - put him back on it",
         double(floorY - pos.y) / s);
}

// What the game's answers say about the ground at a point (local units).
std::string MarioMod::DescribeGround(const Vec3& local, double now) {
    if (activeSource_ != Source::Physics) return std::string("(collision from ") + SourceName(activeSource_) + ")";
    return physics_.DescribeColumn(map_.ToGame(local), now);
}

namespace {
// Distance (local units, horizontal) from p to the segment a..b.
float SegmentDistanceXZ(const Vec3& p, const Vec3& a, const Vec3& b) {
    const float abx = b.x - a.x, abz = b.z - a.z;
    const float len2 = abx * abx + abz * abz;
    float t = 0.0f;
    if (len2 > 1e-6f) t = std::max(0.0f, std::min(1.0f, ((p.x - a.x) * abx + (p.z - a.z) * abz) / len2));
    const float dx = a.x + abx * t - p.x, dz = a.z + abz * t - p.z;
    return std::sqrt(dx * dx + dz * dz);
}
} // namespace

// The walls in libsm64 next to Mario (local position `p`, facing `face`),
// nearest first, and where each came from.
std::string MarioMod::DescribeObstacles(const Vec3& p, float face, double now) {
    const float s = float(map_.Scale());
    const Vec3 fwd(std::sin(face), 0.0f, std::cos(face));
    if (activeSource_ != Source::Physics || surfaceOrigins_.size() != surfaces_.size())
        return std::string("(collision from ") + SourceName(activeSource_) + ")";
    struct Cand {
        float d;
        size_t i;
    };
    std::vector<Cand> cands;
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        const SurfaceOrigin& o = surfaceOrigins_[i];
        if (o.kind != SurfaceOrigin::RingWall && o.kind != SurfaceOrigin::StepWall) continue;
        const SM64Surface& sf = surfaces_[i];
        float ylo = 1e30f, yhi = -1e30f;
        Vec3 v[3];
        for (int k = 0; k < 3; ++k) {
            v[k] = Vec3(float(sf.vertices[k][0]), float(sf.vertices[k][1]), float(sf.vertices[k][2]));
            ylo = std::min(ylo, v[k].y);
            yhi = std::max(yhi, v[k].y);
        }
        if (yhi < p.y + 0.05f * s || ylo > p.y + 1.5f * s) continue; // not at his body's height
        // A vertical triangle's footprint: its two horizontally farthest corners.
        int a = 0, b = 1;
        float best = -1.0f;
        for (int x = 0; x < 3; ++x)
            for (int y = x + 1; y < 3; ++y) {
                const float d = (v[x].x - v[y].x) * (v[x].x - v[y].x) + (v[x].z - v[y].z) * (v[x].z - v[y].z);
                if (d > best) best = d, a = x, b = y;
            }
        const float d = SegmentDistanceXZ(p, v[a], v[b]);
        if (d > 1.0f * s) continue;
        const Vec3 c = (v[0] + v[1] + v[2]) * (1.0f / 3.0f);
        if ((c.x - p.x) * fwd.x + (c.z - p.z) * fwd.z < -0.6f * s) continue; // well behind him
        cands.push_back({d, i});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
    std::string out;
    std::vector<uint32_t> seenWalls;
    std::vector<Vec3> seenSteps;
    int described = 0;
    for (const Cand& c : cands) {
        if (described >= 3) break;
        const SurfaceOrigin& o = surfaceOrigins_[c.i];
        const SM64Surface& sf = surfaces_[c.i];
        const Vec3 center((float(sf.vertices[0][0]) + float(sf.vertices[1][0]) + float(sf.vertices[2][0])) / 3.0f,
                          (float(sf.vertices[0][1]) + float(sf.vertices[1][1]) + float(sf.vertices[2][1])) / 3.0f,
                          (float(sf.vertices[0][2]) + float(sf.vertices[1][2]) + float(sf.vertices[2][2])) / 3.0f);
        std::string one;
        if (o.kind == SurfaceOrigin::RingWall) {
            if (std::find(seenWalls.begin(), seenWalls.end(), o.wall) != seenWalls.end()) continue;
            seenWalls.push_back(o.wall);
            uintptr_t actor = 0;
            one = physics_.DescribeWall(o.wall, now, &actor);
            if (actor) {
                std::string name = game_.ActorName(actor);
                const size_t slash = name.find_last_of("/\\");
                if (slash != std::string::npos) name = name.substr(slash + 1);
                one += Fmt(", actor '%s'", name.c_str());
            }
        } else {
            bool dup = false;
            for (const Vec3& q : seenSteps) dup = dup || Length(Vec3(q.x - center.x, 0, q.z - center.z)) < 0.3f * s;
            if (dup) continue;
            seenSteps.push_back(center);
            // It faces the low side; the columns either side of it.
            Vec3 n = Normalize(Sm64SurfaceNormal(sf));
            n.y = 0;
            n = Length(n) > 0.5f ? Normalize(n) : Vec3(1, 0, 0);
            const float half = 0.5f * cfg_.physics.cell * s;
            one = "a step wall (the ground's height jumps between two columns) - low side: " +
                  DescribeGround(center + n * half, now) + "; high side: " + DescribeGround(center - n * half, now);
        }
        out += Fmt("%s%.2f m away: %s", described ? " | " : "", double(c.d / s), one.c_str());
        ++described;
    }
    if (!described) out = "no wall within 1 m - the ground ahead: " + DescribeGround(p + fwd * (0.6f * s), now);
    return out;
}

void MarioMod::NoteBumpsAndFalls(const SM64MarioState& prev, const SM64MarioState& cur, const SM64MarioInputs& in,
                                 double now) {
    if (activeSource_ != Source::Physics) {
        fallTracking_ = false;
        return;
    }
    const float s = float(map_.Scale());
    const int up = UpIndex(cfg_.upAxis);
    const Vec3 p0(prev.position[0], prev.position[1], prev.position[2]);
    const Vec3 p1(cur.position[0], cur.position[1], cur.position[2]);
    const bool wasAir = sm64::IsAirborne(prev.action), air = sm64::IsAirborne(cur.action);
    const DVec3 g1 = map_.ToGame(p1);

    // Falls: where he left the ground and where he came down.
    if (!wasAir && air) {
        fallTracking_ = true;
        fallStart_ = map_.ToGame(p0);
        fallStartTime_ = now;
        fallJumped_ = sm64::IsJump(cur.action);
        fallLongLogged_ = false;
        fallStartGround_ = DescribeGround(p0, now);
    }
    if (fallTracking_ && air && !fallLongLogged_ && now - fallStartTime_ > 4.0 && cur.velocity[1] < 0 &&
        cur.action != sm64::ACT_FLYING && fallLogs_ < 30) {
        fallLongLogged_ = true;
        ++fallLogs_;
        LOGI("fall: Mario has been in the air for 4 s since (%.1f %.1f %.1f) (%s), %.1f m below that now; under him "
             "now: %s",
             fallStart_.x, fallStart_.y, fallStart_.z, fallJumped_ ? "a jump" : "he left the ground without jumping",
             fallStart_[up] - g1[up], DescribeGround(p1, now).c_str());
    }
    if (fallTracking_ && !air) {
        fallTracking_ = false;
        const double drop = fallStart_[up] - g1[up];
        DVec3 h = g1 - fallStart_;
        h[up] = 0;
        if (drop > 3.0 && fallLogs_ < 30) {
            ++fallLogs_;
            LOGI("fall: Mario %s and came down %.1f m lower, %.1f m away, %.1f s later (%s at (%.1f %.1f %.1f)); where "
                 "he left the ground (%.1f %.1f %.1f): %s; where he landed: %s",
                 fallJumped_ ? "jumped" : "left the ground without jumping", drop, Length(h), now - fallStartTime_,
                 sm64::IsSubmerged(cur.action) ? "into water" : "landed", g1.x, g1.y, g1.z, fallStart_.x, fallStart_.y,
                 fallStart_.z, fallStartGround_.c_str(), DescribeGround(p1, now).c_str());
        }
    }

    // Bumps: knocked back off something, stopped by a wall, or running on
    // the spot at the edge of what the game has been asked about.
    if (bumpLogs_ >= 40 || now - lastBumpLog_ < 1.5) return;
    const char* what = nullptr;
    if (sm64::IsBonk(cur.action) && cur.action != prev.action && cur.health >= prev.health) {
        what = cur.action == sm64::ACT_GROUND_BONK ? "bonked into something"
               : cur.action == sm64::ACT_AIR_HIT_WALL ? "hit a wall in the air"
                                                      : "bounced off something in the air";
    } else if (!air && cur.action == sm64::ACT_WALKING && prev.action == sm64::ACT_WALKING) {
        const float stick = std::sqrt(in.stickX * in.stickX + in.stickY * in.stickY);
        const float moved = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
        if (prev.forwardVelocity > 12.0f && cur.forwardVelocity <= 6.5f && stick > 0.6f) what = "walked into a wall";
        else if (cur.forwardVelocity > 12.0f && moved < 0.25f * cur.forwardVelocity && stick > 0.6f)
            what = "ran on the spot";
    }
    if (!what) return;
    ++bumpLogs_;
    lastBumpLog_ = now;
    const float speed = std::sqrt(prev.velocity[0] * prev.velocity[0] + prev.velocity[2] * prev.velocity[2]) *
                        float(1.0 / sm64::TICK_SECONDS) / s;
    LOGI("bump: Mario %s at (%.2f %.2f %.2f) going %.1f m/s%s - %s", what, g1.x, g1.y, g1.z, speed,
         wasAir ? " in the air" : "", DescribeObstacles(p0, prev.faceAngle, now).c_str());
}

// [Debug] TraceCollision: what Mario did this tick and what libsm64 and the
// world model know around him.
void MarioMod::TraceTick(const SM64MarioState& cur, const SM64MarioInputs& in) {
    const Vec3 pos(cur.position[0], cur.position[1], cur.position[2]);
    const DVec3 g = map_.ToGame(pos);
    int walls30 = -1, walls60 = -1;
    float px = pos.x, pz = pos.z;
    if (api_.surface_find_wall_collision) {
        float x = pos.x, y = pos.y, z = pos.z;
        walls30 = api_.surface_find_wall_collision(&x, &y, &z, 30.0f, 24.0f);
        x = pos.x, y = pos.y, z = pos.z;
        walls60 = api_.surface_find_wall_collision(&x, &y, &z, 60.0f, 50.0f);
        px = x;
        pz = z;
    }
    const float floorY = api_.surface_find_floor_height(pos.x, pos.y + 100.0f, pos.z);
    LOGI("trace t%llu game (%.3f %.3f %.3f) local (%.0f %.0f %.0f) act %08X fwd %.1f vel (%.1f %.1f %.1f) stick (%.2f "
         "%.2f) | floor %.0f walls %d/%d push (%.1f %.1f) | %d surfaces (edge %d step %d probe %d) built %.2f s ago",
         static_cast<unsigned long long>(tickCount_), g[0], g[1], g[2], pos.x, pos.y, pos.z, cur.action,
         cur.forwardVelocity, cur.velocity[0], cur.velocity[1], cur.velocity[2], in.stickX, in.stickY, floorY, walls30,
         walls60, px - pos.x, pz - pos.z, collisionStats_.total, collisionStats_.edgeWalls, collisionStats_.stepWalls,
         collisionStats_.probeWalls, NowSeconds() - lastBuildTime_);
    if (tickCount_ % 10 == 0 && activeSource_ == Source::Physics) {
        LOGI("trace physics near Mario: %s, floor %s", physics_.Debug(g).c_str(),
             PhysicsMaterialName(physics_.FloorMaterialAt(g)));
    }
    if (tickCount_ % 10 == 0 && activeSource_ == Source::World) {
        std::string cells;
        WorldModelStats ws;
        {
            LockGuard lock(worldMu_);
            cells = world_.DebugString(g, 4);
            ws = world_.Stats();
        }
        LOGI("trace world near Mario (%d frames, last: %d samples %d excluded %d carved):", ws.frames, ws.samples,
             ws.excluded, ws.carved);
        size_t a = 0;
        while (a < cells.size()) {
            size_t e = cells.find('\n', a);
            if (e == std::string::npos) e = cells.size();
            LOGI("%s", cells.substr(a, e - a).c_str());
            a = e + 1;
        }
    }
}

void MarioMod::HandleFall(const SM64MarioState&, const SM64MarioState& cur) {
    const Vec3 pos(cur.position[0], cur.position[1], cur.position[2]);
    const double upNow = map_.ToGame(pos)[UpIndex(cfg_.upAxis)];
    const bool air = sm64::IsAirborne(cur.action);
    if (air) {
        // Flying (wing cap) and gliding down isn't a fall: count from where it ends.
        apexUp_ = wasAirborne_ && cur.action != sm64::ACT_FLYING ? std::max(apexUp_, upNow) : upNow;
    } else if (wasAirborne_ && !sm64::IsSubmerged(cur.action) && cfg_.fallDamage != "off") {
        const double fallUnits = (apexUp_ - upNow) * map_.Scale(); // SM64 thresholds: 1150 / 3000 units
        const bool vanilla = cfg_.fallDamage == "sm64";
        const int wedges = sm64::HealthWedges(cur.health);
        const int32_t id = mario_->Id();
        if (fallUnits > 3000.0) {
            int w = vanilla ? 4 : std::min(2, std::max(0, wedges - 1));
            if (w > 0) {
                const float a = cur.faceAngle;
                api_.mario_take_damage(id, uint32_t(w), 0, pos.x + std::sin(a) * 50.0f, pos.y, pos.z + std::cos(a) * 50.0f);
                lastHurtTime_ = NowSeconds();
            }
        } else if (fallUnits > 1150.0) {
            const int lose = vanilla ? 2 : 1;
            int16_t h = int16_t(std::max<int>(cur.health - lose * 0x100, vanilla ? 0xFF : 0x180));
            if (h < cur.health) {
                api_.set_mario_health(id, uint16_t(h));
                lastHurtTime_ = NowSeconds();
            }
        }
    }
    wasAirborne_ = air;
}

// ------------------------------------------------------------------ combat

void MarioMod::RefreshEnemies() {
    // Drop enemies that left the area, died or whose slot was reused.
    const DVec3 mg = MarioGamePos(1.0f);
    enemies_.erase(std::remove_if(enemies_.begin(), enemies_.end(),
                                  [&](const Enemy& e) {
                                      if (!e.health || game_.ActorSerial(e.actor) != e.serial) return true;
                                      float hp, mx;
                                      if (!game_.ReadHealthComponent(e.health, hp, mx) || hp <= 0) return true;
                                      return Length(e.lastPos - mg) > cfg_.enemyScanRadius * 1.25;
                                  }),
                   enemies_.end());
}

void MarioMod::ScanEnemiesStep() {
    // The pool can hold thousands of slots: examine a bounded batch per tick
    // (one bulk read), so discovery never causes a hitch.
    const uint32_t n = game_.PoolCount();
    const uintptr_t base = game_.PoolBase();
    if (n == 0 || !base || enemies_.size() >= 64) return;
    const uint32_t stride = game_.PoolStride();
    if (scanCursor_ >= n) scanCursor_ = 0;
    const uint32_t batch = std::min<uint32_t>(uint32_t(std::max(16, cfg_.enemySlotsPerTick)), n - scanCursor_);
    poolChunk_.resize(size_t(batch) * stride);
    const uint32_t first = scanCursor_;
    scanCursor_ += batch;
    if (!SafeRead(base + uintptr_t(first) * stride, poolChunk_.data(), poolChunk_.size())) return;
    const DVec3 mg = MarioGamePos(1.0f);
    const uintptr_t hero = game_.Hero();
    const uint32_t transformOff = game_.Layout().actorTransform;
    for (uint32_t k = 0; k < batch; ++k) {
        const uintptr_t a = base + uintptr_t(first + k) * stride;
        if (a == hero) continue;
        uintptr_t transform;
        std::memcpy(&transform, poolChunk_.data() + size_t(k) * stride + transformOff, sizeof(transform));
        if (!LooksLikePointer(transform)) continue;
        DVec3 p;
        if (!game_.GetPosition(a, p) || Length(p - mg) > cfg_.enemyScanRadius) continue;
        const uint32_t serial = game_.ActorSerial(a);
        if (std::any_of(enemies_.begin(), enemies_.end(),
                        [&](const Enemy& e) { return e.actor == a && e.serial == serial; }))
            continue;
        uintptr_t hc = 0;
        for (const std::string& c : cfg_.healthComponents)
            if ((hc = game_.GetComponentChecked(a, c.c_str())) != 0) break;
        float hp, mx;
        if (!hc || !game_.ReadHealthComponent(hc, hp, mx) || hp <= 0) continue;
        const std::string name = Ini::Lower(game_.ActorName(a));
        if (!cfg_.enemyInclude.empty() &&
            std::none_of(cfg_.enemyInclude.begin(), cfg_.enemyInclude.end(),
                         [&](const std::string& s) { return name.find(s) != std::string::npos; }))
            continue;
        if (std::any_of(cfg_.enemyExclude.begin(), cfg_.enemyExclude.end(),
                        [&](const std::string& s) { return name.find(s) != std::string::npos; }))
            continue;
        Enemy e;
        e.actor = a;
        e.serial = serial;
        e.health = hc;
        e.name = name;
        e.person = !cfg_.personComponents.empty() &&
                   std::any_of(cfg_.personComponents.begin(), cfg_.personComponents.end(),
                               [&](const std::string& c) { return game_.GetComponentChecked(a, c.c_str()) != 0; });
        e.lastPos = p;
        enemies_.push_back(e);
        ++candidatesFound_;
        if (cfg_.logHits || candidatesFound_ <= 3)
            LOGI("combat: %s near Mario (slot %u, health %.0f/%.0f%s%s)", e.person ? "a person" : "someone to fight",
                 first + k, hp, mx, name.empty() ? "" : ", '", name.empty() ? "" : (name + "'").c_str());
        if (enemies_.size() >= 64) break;
    }
}

void MarioMod::UpdateCombat(const SM64MarioState& cur, uint32_t prevAction) {
    if (!cfg_.combat) return;
    if (game_.PoolAvailable()) {
        ScanEnemiesStep();
        if (tickCount_ % 15 == 1) RefreshEnemies();
    }
    const float u = float(map_.Scale());
    AttackKind kind = ClassifyAttack(cur.action, cur.flags, cur.velocity[1]);
    if (kind == AttackKind::Shockwave && prevAction == sm64::ACT_GROUND_POUND_LAND) kind = AttackKind::None;
    const Vec3 m(cur.position[0], cur.position[1], cur.position[2]);
    const Vec3 facing(std::sin(cur.faceAngle), 0, std::cos(cur.faceAngle));
    // The game's damage system: its own hit reactions, knockback and deaths.
    const bool gameDamage = cfg_.gameDamage && game_physics::DamageAvailable();
    const int up = UpIndex(cfg_.upAxis);
    bool hitAny = false;
    for (Enemy& e : enemies_) {
        if (e.cooldown > 0) --e.cooldown;
        if (!e.health || game_.ActorSerial(e.actor) != e.serial) {
            e.health = 0;
            continue;
        }
        DVec3 gp;
        if (!game_.GetPosition(e.actor, gp)) continue;
        if (e.kbRemaining > 0 && !gameDamage) {
            const float step = std::min(e.kbRemaining, 0.3f);
            gp = gp + DVec3(e.kbDir) * double(step);
            game_.SetPosition(e.actor, gp);
            e.kbRemaining -= step;
        }
        e.lastPos = gp;
        float hp, mx;
        if (!game_.ReadHealthComponent(e.health, hp, mx) || hp <= 0) continue;
        if (kind == AttackKind::None || e.cooldown > 0) continue;
        // People only take the game's reactions (never health writes).
        if (e.person && (!gameDamage || !cfg_.hitPeople)) continue;
        const Vec3 el = map_.ToLocal(gp);
        if (!AttackReaches(kind, m, cur.velocity[1], el, cfg_.tuning, u)) continue;
        const bool hit = kind == AttackKind::Shockwave ||
                         api_.mario_attack(mario_->Id(), el.x, el.y, el.z, cfg_.tuning.enemyHeight * u);
        if (!hit) continue;
        const float dmg =
            mx * cfg_.tuning.damageFraction[int(kind)] * cfg_.tuning.damageMultiplier * live_.attackStrength;
        e.cooldown = cfg_.tuning.hitCooldownTicks;
        ++hitsLanded_;
        hitAny = true;
        if (gameDamage) {
            // The game's damage: to that actor (by its handle) where the
            // game can, else a sphere around him.
            game_physics::DamageOrder d;
            d.target = game_physics::DamageActorAvailable() ? e.serial : 0;
            d.center = gp;
            d.center[up] += cfg_.tuning.enemyHeight * 0.5;
            d.radius = std::max(0.3f, cfg_.tuning.enemyRadius);
            d.type = 1; // kMelee
            d.knockback = int(cfg_.tuning.reaction[int(kind)]);
            d.knockbackAmount = game_physics::kGameKnockbackAmount * std::max(0.1f, cfg_.tuning.knockback[int(kind)]) *
                                live_.attackStrength;
            d.damager = game_.HeroHandle();
            if (e.person) {
                // A passer-by: flinches and stumbles, unhurt.
                d.amount = 0;
                d.flags = game_physics::kDamageFromLocalPlayer | game_physics::kDamageForceReact |
                          game_physics::kDamageAllowFriendly | game_physics::kDamagePreventKill |
                          game_physics::kDamageNoSpawnRewards;
            } else {
                d.amount = dmg;
                d.flags = game_physics::kDamageFromLocalPlayer | game_physics::kDamageForceReact;
            }
            game_physics::SubmitDamage(d);
            if (cfg_.logHits || hitsLanded_ <= 3)
                LOGI("hit %s%s%s%s with %s: %.0f damage (%.0f of %.0f left), %s%s", e.person ? "a person" : "an enemy",
                     e.name.empty() ? "" : " '", e.name.c_str(), e.name.empty() ? "" : "'", AttackName(kind), d.amount, hp,
                     mx, ReactionName(cfg_.tuning.reaction[int(kind)]), d.target ? "" : " (by sphere)");
            continue;
        }
        const float after = std::max(0.0f, hp - dmg);
        game_.WriteHealthComponent(e.health, after);
        Vec3 d = el - m;
        d.y = 0;
        d = Length(d) > 1.0f ? Normalize(d) : facing;
        e.kbDir = Normalize(map_.DirToGame(d));
        e.kbRemaining = kind == AttackKind::Stomp ? 0.0f : cfg_.tuning.knockback[int(kind)] * live_.attackStrength;
        if (cfg_.logHits || hitsLanded_ <= 3)
            LOGI("hit '%s' with %s: %.0f -> %.0f of %.0f", e.name.c_str(), AttackName(kind), hp, after, mx);
    }
    // Nobody known was in reach (the pool scan finds people over a few
    // ticks): a sphere in front of Mario lets the game hit whoever is there
    // - people react, unhurt.
    if (peopleCooldown_ > 0) --peopleCooldown_;
    const bool swing = kind == AttackKind::Punch || kind == AttackKind::Kick || kind == AttackKind::Trip ||
                       kind == AttackKind::SlideKick || kind == AttackKind::Dive || kind == AttackKind::Shockwave;
    if (gameDamage && cfg_.hitPeople && swing && !hitAny && peopleCooldown_ == 0) {
        peopleCooldown_ = cfg_.tuning.hitCooldownTicks;
        // Spheres that keep clear of Spider-Man, who stands where Mario is (he
        // must not get hit by them himself): one in front of Mario for a punch
        // or a kick, a ring around him for a ground pound's shockwave.
        game_physics::DamageOrder d;
        d.amount = 0;
        d.type = 1;
        d.knockback = int(cfg_.tuning.reaction[int(kind)]);
        d.knockbackAmount =
            game_physics::kGameKnockbackAmount * std::max(0.1f, cfg_.tuning.knockback[int(kind)]) * live_.attackStrength;
        d.flags = game_physics::kDamageFromLocalPlayer | game_physics::kDamageForceReact |
                  game_physics::kDamageAllowFriendly | game_physics::kDamagePreventKill |
                  game_physics::kDamageNoSpawnRewards;
        d.damager = game_.HeroHandle();
        if (kind == AttackKind::Shockwave) {
            const float ring = std::max(1.2f, cfg_.tuning.shockwaveRadius * 0.6f);
            d.radius = std::max(0.6f, cfg_.tuning.shockwaveRadius - ring);
            for (int k = 0; k < 6; ++k) {
                const float a = float(k) * 1.0471976f;
                d.center = map_.ToGame(m + Vec3(std::sin(a) * ring * u, 0.9f * u, std::cos(a) * ring * u));
                game_physics::SubmitDamage(d);
            }
        } else {
            d.center = map_.ToGame(m + facing * (1.0f * u) + Vec3(0, 0.9f * u, 0));
            d.radius = 0.5f;
            game_physics::SubmitDamage(d);
        }
    }
}

void MarioMod::HandleIncomingDamage(const SM64MarioState& cur) {
    if (!heroHealth_) return;
    float hp, mx;
    if (!game_.ReadHealthComponent(heroHealth_, hp, mx)) return;
    if (lastHeroHealth_ >= 0 && hp < lastHeroHealth_ - 0.01f) {
        const bool invulnerable = cur.invincTimer > 0 || (cur.action & sm64::ACT_FLAG_INVULNERABLE) ||
                                  sm64::IsDying(cur.action);
        if (!invulnerable) {
            const int wedges = IncomingDamageWedges(lastHeroHealth_ - hp, mx, cfg_.tuning.incomingScale);
            const Vec3 m(cur.position[0], cur.position[1], cur.position[2]);
            Vec3 src = m + Vec3(std::sin(cur.faceAngle), 0, std::cos(cur.faceAngle)) * 60.0f;
            float best = 1e30f;
            for (const Enemy& e : enemies_) {
                if (!e.health) continue;
                Vec3 el = map_.ToLocal(e.lastPos);
                float d = Length(el - m);
                if (d < best) {
                    best = d;
                    src = el;
                }
            }
            api_.mario_take_damage(mario_->Id(), uint32_t(wedges), 0, src.x, src.y, src.z);
            ++hitsTaken_;
            lastHurtTime_ = NowSeconds();
        }
    }
    if (cfg_.keepHeroAlive && hp < mx) {
        game_.WriteHealthComponent(heroHealth_, mx);
        hp = mx;
    }
    lastHeroHealth_ = hp;
}

// ------------------------------------------------------------------ settings, poses

void MarioMod::HandleMenu() {
    const double now = NowSeconds();
    bool closed = false;
    if (input_.Pressed(Hotkey::Menu)) {
        if (menu_.Open()) {
            menu_.Hide();
            closed = true;
        } else {
            menu_.Show(live_);
        }
    } else if (menu_.Open()) {
        const MenuNav& n = input_.Nav();
        const LiveSettings before = live_;
        if (n.up) menu_.Up();
        if (n.down) menu_.Down();
        if (n.left) menu_.Left(live_);
        if (n.right) menu_.Right(live_);
        if (n.accept) menu_.Accept(live_);
        if (n.back) {
            menu_.Hide();
            closed = true;
        }
        ApplyLive(before, "F8 menu");
    }
    // The menu's keys stay away from the game until a moment after they are
    // let go (Esc closing the menu must not also open the game's pause menu).
    if (menu_.Open() || (menuCapture_ && input_.NavHeld())) menuCaptureUntil_ = now + 0.25;
    menuCapture_ = now < menuCaptureUntil_;
    input_shim::SetMenuCapture(menuCapture_);

    // ModSettings' page in the pause menu.
    {
        const LiveSettings before = live_;
        if (modsettings::TakeChanges(live_)) ApplyLive(before, "pause menu");
    }
    const bool saveAsked = modsettings::TakeSaveRequest();
    if (saveDue_ > 0 && (closed || saveAsked || (now >= saveDue_ && !menu_.Open())))
        SaveLive(closed ? "menu closed" : saveAsked ? "ModSettings saved" : "changed");
}

void MarioMod::ApplyLive(const LiveSettings& before, const char* source) {
    if (live_ == before) return;
    for (const SettingItem& it : SettingItems())
        if (it.kind != SettingItem::Header && GetSetting(before, it.id) != GetSetting(live_, it.id))
            LOGI("settings (%s): %s -> %s", source, it.label, FormatSetting(live_, it.id).c_str());
    if (live_.volume != before.volume && cfg_.audio) audio_.SetVolume(live_.volume);
    if (live_.shine != before.shine) {
        const gbuf::ShineLook l = gbuf::LookForShine(live_.shine, cfg_.specular);
        injector_.SetShine(l.gloss, l.f0, l.occlusion);
    }
    if (!live_.followHeight && before.followHeight) hero_pin::ClearPin();
    if (!live_.photoPoses && !pendingFromKey_) pendingPose_ = Pose::None;
    modsettings::Publish(live_);
    saveDue_ = NowSeconds() + 1.5;
}

void MarioMod::SaveLive(const char* why) {
    saveDue_ = 0;
    std::string err;
    if (SaveLiveSettings(PathToUtf8(paths_.dataDir / L"sm2mario.ini"), live_, &err)) {
        LOGI("settings saved to sm2mario.ini (%s)", why);
    } else {
        LOGW("settings not saved: %s", err.c_str());
        Toast("Couldn't save Mario's settings to sm2mario.ini (see sm2mario.log)", 5, kYellow);
    }
}

bool MarioMod::CameraPosition(DVec3& out) const {
    if (GameViewFresh()) {
        out = gameView_.camPos;
        return true;
    }
    if (camera_.Valid()) {
        out = camera_.Pose().position;
        return true;
    }
    return false;
}

float MarioMod::FaceToward(const DVec3& target) const {
    if (!mario_ || !mario_->Alive()) return 0;
    const Vec3 m = mario_->Position(1.0f);
    const Vec3 t = map_.ToLocal(target);
    const float dx = t.x - m.x, dz = t.z - m.z;
    if (dx * dx + dz * dz < 1.0f) return mario_->State().faceAngle;
    return std::atan2(dx, dz);
}

// Pedestrians who walked up to ask for a picture (the game's interaction
// behaviour started on them). Mario turns to them - and a little to the
// camera, so the player sees it - and poses, once he is standing still.
void MarioMod::TakePhotoRequests() {
    const std::vector<uint32_t> handles = ped_interact::Take();
    if (handles.empty() || !active_ || !mario_ || !mario_->Alive()) return;
    const double now = NowSeconds();
    photoSeen_.erase(std::remove_if(photoSeen_.begin(), photoSeen_.end(),
                                    [&](const PhotoSeen& p) { return now - p.time > 30.0; }),
                     photoSeen_.end());
    const DVec3 mg = MarioGamePos(1.0f);
    for (uint32_t h : handles) {
        ++photoRequests_;
        const uintptr_t ped = game_.ActorFromHandle(h);
        DVec3 pp;
        if (!ped || !game_.GetPosition(ped, pp)) continue;
        const double dist = Length(pp - mg);
        lastAsk_ = {h, pp, now};
        const bool seen = std::any_of(photoSeen_.begin(), photoSeen_.end(), [&](const PhotoSeen& p) { return p.handle == h; });
        if (photoRequests_ <= 3 || cfg_.logHits)
            LOGI("photo request: a pedestrian %.1f m away wants a picture%s", dist,
                 !live_.photoPoses ? " (POSE FOR PHOTOS is off)" : seen ? " (asked already)" : "");
        if (!live_.photoPoses || seen || dist > 12.0 || now - lastPhotoPose_ < 6.0) continue;
        photoSeen_.push_back({h, now});
        if (pendingPose_ != Pose::None && pendingFromKey_) continue;
        static const Pose kCycle[] = {Pose::Wave, Pose::PeaceSign, Pose::StarDance};
        pendingPose_ = kCycle[photoCycle_++ % 3];
        pendingFromKey_ = false;
        pendingUntil_ = now + 5.0;
        // Halfway between the pedestrian and the camera.
        const float toPed = FaceToward(pp);
        DVec3 cam;
        float face = toPed;
        if (CameraPosition(cam)) {
            const float toCam = FaceToward(cam);
            const float sx = std::sin(toPed) + std::sin(toCam), cz = std::cos(toPed) + std::cos(toCam);
            if (sx * sx + cz * cz > 0.05f) face = std::atan2(sx, cz);
        }
        pendingFace_ = face;
        lastPhotoPose_ = now;
    }
}

// The player pressed the game's interact button (Triangle / Y) - the prompt
// over a pedestrian who wants a picture or a high five: Mario answers them
// too, turned their way. (The game gets the button as well.)
void MarioMod::AnswerInteraction(double now) {
    if (!mario_ || !mario_->Alive() || now - lastAsk_.time > 20.0 || lastAsk_.handle == 0) return;
    const uintptr_t ped = game_.ActorFromHandle(lastAsk_.handle);
    DVec3 pp = lastAsk_.pos;
    if (ped) game_.GetPosition(ped, pp);
    if (Length(pp - MarioGamePos(1.0f)) > 6.0) return;
    static const Pose kCycle[] = {Pose::Wave, Pose::PeaceSign, Pose::StarDance};
    pendingPose_ = kCycle[photoCycle_++ % 3];
    pendingFace_ = FaceToward(pp);
    pendingFromKey_ = true; // right away, even if he is walking
    pendingUntil_ = now + 1.0;
    lastPhotoPose_ = now;
    // The game plays its side of it on Spider-Man (who walks up to them and
    // high-fives or poses): let it move him for a while.
    if (cfg_.interactRelease > 0) {
        interactUntil_ = now + cfg_.interactRelease;
        interactFrom_ = MarioGamePos(1.0f);
    }
    LOGI("interaction: Mario answers the pedestrian (%s)%s", PoseName(pendingPose_),
         cfg_.interactRelease > 0 ? " - the game plays it on Spider-Man (hidden)" : "");
}

void MarioMod::UpdatePoses(const SM64MarioState& prev, const SM64MarioInputs& in) {
    const int32_t id = mario_->Id();
    if (pendingPose_ != Pose::None) {
        const bool noInput = std::fabs(in.stickX) < 0.1f && std::fabs(in.stickY) < 0.1f && !in.buttonA && !in.buttonB &&
                             !in.buttonZ;
        if (NowSeconds() > pendingUntil_) {
            if (!pendingFromKey_) LOGI("photo request: Mario didn't stand still in time - no pose");
            pendingPose_ = Pose::None;
        } else if (pendingFromKey_ ? PoseController::CanPose(prev.action) || pose_.Active()
                                   : !pose_.Active() && noInput &&
                                         sm64::ActionGroup(prev.action) == sm64::ACT_GROUP_STATIONARY &&
                                         PoseController::CanPose(prev.action)) {
            if (pose_.Start(api_, id, prev, pendingPose_, pendingFace_)) {
                if (!pendingFromKey_) {
                    ++photoPoses_;
                    LOGI("photo request: Mario poses (%s)", PoseName(pendingPose_));
                } else {
                    LOGI("pose key: Mario poses (%s)", PoseName(pendingPose_));
                }
                pendingPose_ = Pose::None;
            }
        }
    }
    pose_.Tick(api_, id, prev, in);
}

void MarioMod::DrawMenu() {
    if (!menu_.Open()) return;
    const float ui = overlay_.UiScale();
    const auto& items = SettingItems();
    const float itemScale = 0.72f, headScale = 0.62f, smallScale = 0.55f;
    const float rowH = overlay_.LineHeight(itemScale) + 8 * ui;
    const float headH = overlay_.LineHeight(headScale) + 14 * ui;
    const float pad = 26 * ui, w = 720 * ui;
    const SettingItem& sel = items[size_t(std::max(0, std::min(int(items.size()) - 1, menu_.Cursor())))];

    // The selected item's description, wrapped to the panel.
    std::vector<std::string> desc;
    {
        std::string line, word;
        const std::string text = std::string(sel.desc) + " ";
        for (char ch : text) {
            if (ch != ' ') {
                word += ch;
                continue;
            }
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && overlay_.TextWidth(next, smallScale) > w - 2 * pad) {
                desc.push_back(line);
                line = word;
            } else {
                line = next;
            }
            word.clear();
        }
        if (!line.empty()) desc.push_back(line);
    }
    float h = pad + overlay_.LineHeight(1.0f) + 10 * ui;
    for (const SettingItem& it : items) h += it.kind == SettingItem::Header ? headH : rowH;
    h += 12 * ui + float(std::max<size_t>(2, desc.size())) * overlay_.LineHeight(smallScale) + 14 * ui +
         overlay_.LineHeight(smallScale) * (modsettings::Registered() ? 2.0f : 1.0f) + pad;
    const float x = 70 * ui, y0 = std::max(10 * ui, (overlay_.Height() - h) * 0.5f);
    overlay_.Rect(x, y0, w, h, 0x0C0E14E6u);
    overlay_.Rect(x, y0, w, 5 * ui, 0xE8453CFFu);

    float y = y0 + pad;
    overlay_.ShadowText(x + pad, y, "MARIO MODE", 0xFFFFFFFFu, 1.0f);
    const std::string ver = std::string("v") + SM2MARIO_VERSION;
    overlay_.Text(x + w - pad - overlay_.TextWidth(ver, smallScale), y + 8 * ui, ver, 0x8A8F9AFFu, smallScale);
    y += overlay_.LineHeight(1.0f) + 10 * ui;

    for (size_t i = 0; i < items.size(); ++i) {
        const SettingItem& it = items[i];
        if (it.kind == SettingItem::Header) {
            y += 6 * ui;
            overlay_.Text(x + pad, y, it.label, 0xFFD84AFFu, headScale);
            overlay_.Rect(x + pad, y + overlay_.LineHeight(headScale) + 1 * ui, w - 2 * pad, 2 * ui, 0xFFD84A55u);
            y += headH - 6 * ui;
            continue;
        }
        const bool on = int(i) == menu_.Cursor();
        if (on) overlay_.Rect(x + 10 * ui, y - 3 * ui, w - 20 * ui, rowH - 2 * ui, 0xE8453C66u);
        overlay_.Text(x + pad, y, it.label, on ? 0xFFFFFFFFu : 0xC8CCD4FFu, itemScale);
        const std::string value = FormatSetting(live_, it.id);
        const float right = x + w - pad;
        if (it.kind == SettingItem::Slider) {
            const float vw = overlay_.TextWidth("100%", itemScale);
            const float barW = 190 * ui, barH = 8 * ui;
            const float bx = right - vw - 18 * ui - barW, by = y + overlay_.LineHeight(itemScale) * 0.5f - barH * 0.5f;
            const float t = (GetSetting(live_, it.id) - it.min) / std::max(1e-6f, it.max - it.min);
            overlay_.Rect(bx, by, barW, barH, 0xFFFFFF30u);
            overlay_.Rect(bx, by, barW * std::max(0.0f, std::min(1.0f, t)), barH, on ? 0xFFD84AFFu : 0xE8E8E8C0u);
            overlay_.Text(right - overlay_.TextWidth(value, itemScale), y, value, 0xFFFFFFFFu, itemScale);
        } else if (it.kind == SettingItem::Toggle) {
            const bool v = GetSetting(live_, it.id) >= 0.5f;
            overlay_.Text(right - overlay_.TextWidth(value, itemScale), y, value, v ? 0x7CFF8AFFu : 0x8A8F9AFFu,
                          itemScale);
        } else {
            const std::string shown = on ? "< " + value + " >" : value;
            overlay_.Text(right - overlay_.TextWidth(shown, itemScale), y, shown, 0xFFFFFFFFu, itemScale);
        }
        y += rowH;
    }
    y += 12 * ui;
    for (const std::string& l : desc) {
        overlay_.Text(x + pad, y, l, 0xB8BDC8FFu, smallScale);
        y += overlay_.LineHeight(smallScale);
    }
    if (desc.size() < 2) y += overlay_.LineHeight(smallScale) * float(2 - desc.size());
    y += 14 * ui;
    overlay_.Text(x + pad, y, "UP/DOWN choose   LEFT/RIGHT change   ENTER switch   ESC close", 0x8A8F9AFFu,
                  smallScale);
    if (modsettings::Registered())
        overlay_.Text(x + pad, y + overlay_.LineHeight(smallScale), "Also in the game's pause menu (ModSettings)",
                      0x8A8F9AFFu, smallScale);
}

// ------------------------------------------------------------------ drawing

void MarioMod::FillDrawMatrices(const WorldMapping& map, MarioDraw& draw) {
    const CameraPose& cam = camera_.Pose();
    const Vec3 R = cfg_.flipRight ? cam.right * -1.0f : cam.right;
    const Vec3 U = cam.up, F = cam.forward;
    const float inv = float(1.0 / map.Scale());
    const DVec3 rel = map.Origin() - cam.position; // camera-relative: keeps floats precise far from 0,0,0

    Mat4 l2g;
    for (int i = 0; i < 3; ++i) {
        Vec3 e(0, 0, 0);
        e[i] = 1;
        const Vec3 d = map.DirToGame(e);
        l2g.m[i][0] = d.x * inv;
        l2g.m[i][1] = d.y * inv;
        l2g.m[i][2] = d.z * inv;
        l2g.m[i][3] = 0;
        draw.normalToView[i][0] = Dot(d, R);
        draw.normalToView[i][1] = Dot(d, U);
        draw.normalToView[i][2] = Dot(d, F);
    }
    l2g.m[3][0] = float(rel.x);
    l2g.m[3][1] = float(rel.y);
    l2g.m[3][2] = float(rel.z);
    l2g.m[3][3] = 1;

    Mat4 g2v;
    for (int i = 0; i < 3; ++i) {
        g2v.m[i][0] = R[i];
        g2v.m[i][1] = U[i];
        g2v.m[i][2] = F[i];
        g2v.m[i][3] = 0;
    }
    g2v.m[3][0] = g2v.m[3][1] = g2v.m[3][2] = 0;
    g2v.m[3][3] = 1;

    const float aspect = renderer_.Height() > 0 ? float(renderer_.Width()) / float(renderer_.Height()) : 16.0f / 9.0f;
    const float ys = 1.0f / std::tan(0.5f * camera_.FovY());
    const float n = cfg_.nearPlane, f = 20000.0f;
    Mat4 p;
    for (auto& row : p.m)
        for (float& v : row) v = 0;
    p.m[0][0] = ys / aspect;
    p.m[1][1] = ys;
    p.m[2][2] = f / (f - n);
    p.m[2][3] = 1;
    p.m[3][2] = -n * f / (f - n);

    draw.localToClip = l2g * g2v * p;
    draw.enabled = true;
    draw.lightView = Vec3(0.35f, 0.8f, -0.5f);
    draw.ambient = cfg_.ambient;
    draw.brightness = cfg_.brightness;
}

void MarioMod::BuildDraw(float alpha, MarioDraw& draw) {
    mario_->Geometry(alpha, drawGeo_);
    FillDrawMatrices(map_, draw);
    draw.geo = &drawGeo_;
}

namespace {
// Appends an axis-aligned box (12 triangles) to `g`. UV (1,1) is the
// "untextured" convention libsm64 uses, so vertex colours show.
void AddBox(MarioGeometry& g, const Vec3& c, const Vec3& h, const Vec3& color) {
    static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    static const float normals[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    Vec3 corner[8];
    for (int i = 0; i < 8; ++i)
        corner[i] = Vec3(c.x + ((i & 4) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 1) ? h.z : -h.z));
    for (int f = 0; f < 6; ++f) {
        const int tri[2][3] = {{faces[f][0], faces[f][1], faces[f][2]}, {faces[f][0], faces[f][2], faces[f][3]}};
        for (const auto& t : tri) {
            if (g.triangles >= SM64_GEO_MAX_TRIANGLES) return;
            for (int k = 0; k < 3; ++k) {
                const size_t v = size_t(g.triangles) * 3 + size_t(k);
                const Vec3& p = corner[t[k]];
                g.position[v * 3 + 0] = p.x;
                g.position[v * 3 + 1] = p.y;
                g.position[v * 3 + 2] = p.z;
                for (int a = 0; a < 3; ++a) g.normal[v * 3 + size_t(a)] = normals[f][a];
                g.color[v * 3 + 0] = color.x;
                g.color[v * 3 + 1] = color.y;
                g.color[v * 3 + 2] = color.z;
                g.uv[v * 2 + 0] = 1.0f;
                g.uv[v * 2 + 1] = 1.0f;
            }
            ++g.triangles;
        }
    }
}
} // namespace

void MarioMod::BuildCalibration(MarioDraw& draw) {
    uintptr_t hero = game_.Hero();
    DVec3 feet;
    if (!hero || !game_.GetPosition(hero, feet)) return;
    const float aspect = renderer_.Height() > 0 ? float(renderer_.Width()) / float(renderer_.Height()) : 16.0f / 9.0f;
    const Vec3 worldUp = cfg_.upAxis == 'Z' ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    camera_.Update(feet, worldUp, aspect);
    if (!camera_.Valid()) return;
    const bool mirror = WantMirror();
    calibMap_.Configure(cfg_.upAxis, mirror, cfg_.unitsPerMetre);
    calibMap_.SetOrigin(feet);
    if (calibGeo_.position.empty()) calibGeo_.Allocate();
    calibGeo_.triangles = 0;
    const float u = float(cfg_.unitsPerMetre);
    AddBox(calibGeo_, Vec3(0, 0.1f * u, 0), Vec3(0.15f * u, 0.1f * u, 0.15f * u), Vec3(1.0f, 0.15f, 0.1f));   // feet
    AddBox(calibGeo_, Vec3(0, 0.9f * u, 0), Vec3(0.03f * u, 0.7f * u, 0.03f * u), Vec3(0.2f, 0.4f, 1.0f));    // pole
    AddBox(calibGeo_, Vec3(0, 1.8f * u, 0), Vec3(0.12f * u, 0.12f * u, 0.12f * u), Vec3(0.2f, 1.0f, 0.3f));   // head
    AddBox(calibGeo_, Vec3(0.5f * u, 0.05f * u, 0), Vec3(0.5f * u, 0.02f * u, 0.02f * u), Vec3(1.0f, 1.0f, 1.0f)); // +x (local)
    FillDrawMatrices(calibMap_, draw);
    draw.geo = &calibGeo_;
}

void MarioMod::DrawHud() {
    const double now = NowSeconds();
    const float ui = overlay_.UiScale();
    // Photo mode: nothing of Mario Mode's on screen (it would be in the shot)
    // - only the F8 menu or the F10 overlay, when asked for.
    const bool photo = photoMode_;
    if (active_ && cfg_.showHud && mario_ && mario_->Alive() && !photo) {
        const int wedges = sm64::HealthWedges(mario_->State().health);
        const uint32_t fill = wedges >= 6 ? 0x39C75AFFu : wedges >= 3 ? 0xF2C230FFu : 0xE8453CFFu;
        const float cx = 86 * ui, cy = 92 * ui;
        overlay_.Meter(cx, cy, 46 * ui, 8, wedges, fill, 0x2A2A2AC8u, 0x000000A0u);
        const std::string label = paused_ ? "PAUSED" : "MARIO";
        overlay_.ShadowText(cx - overlay_.TextWidth(label, 0.8f) * 0.5f, cy + 58 * ui, label, 0xFFFFFFFFu, 0.8f);
    }
    if (!photo) {
        LockGuard lock(toastMutex_);
        toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const ToastMsg& t) { return t.until < now; }),
                      toasts_.end());
        float y = overlay_.Height() - 150 * ui;
        for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
            // Shrink long messages (e.g. ones with a full path) to fit the screen.
            float scale = 0.9f;
            const float maxW = overlay_.Width() * 0.92f;
            float w = overlay_.TextWidth(it->text, scale);
            if (w > maxW) {
                scale *= maxW / w;
                w = maxW;
            }
            const float x = (overlay_.Width() - w) * 0.5f;
            overlay_.Rect(x - 14 * ui, y - 8 * ui, w + 28 * ui, overlay_.LineHeight(scale) + 16 * ui, 0x000000A0u);
            overlay_.ShadowText(x, y, it->text, it->color, scale);
            y -= overlay_.LineHeight(0.9f) + 22 * ui;
        }
    }
    if (!debugOverlay_) return;
    std::vector<std::string> lines;
    lines.push_back(Fmt("sm2mario %s debug (F10)", SM2MARIO_VERSION));
    lines.push_back(Fmt("game hooks %s | pool %s | components %s", game_.Ready() ? "ok" : "MISSING",
                        game_.PoolAvailable() ? "ok" : "--", game_.ComponentsAvailable() ? "ok" : "--"));
    lines.push_back(Fmt("camera %s %s | cand %d/%d @%p | fov %.1f (%+.0f) | %s", camera_.Source().c_str(),
                        camera_.Valid() ? "valid" : "INVALID", camera_.CandidateIndex() + 1, camera_.CandidateCount(),
                        reinterpret_cast<void*>(camera_.SelectedAddress()), camera_.FovY() * 57.29578f,
                        camera_.FovOffsetDeg(), camera_.Scanning() ? "scanning..." : ""));
    if (camera_.Valid()) {
        const CameraPose& c = camera_.Pose();
        lines.push_back(Fmt("  cam pos (%.2f %.2f %.2f) fwd (%.2f %.2f %.2f) hand %s", c.position.x, c.position.y,
                            c.position.z, c.forward.x, c.forward.y, c.forward.z,
                            camera_.HandednessKnown() ? (camera_.LeftHanded() ? "L" : "R") : "?"));
    }
    if (active_ && mario_->Alive()) {
        const SM64MarioState& s = mario_->State();
        lines.push_back(Fmt("mario act %08X hp %d/8 vel (%.0f %.0f %.0f) local (%.0f %.0f %.0f)", s.action,
                            sm64::HealthWedges(s.health), s.velocity[0], s.velocity[1], s.velocity[2], s.position[0],
                            s.position[1], s.position[2]));
        const DVec3& o = map_.Origin();
        lines.push_back(Fmt("origin (%.1f %.1f %.1f) mirror %d | collision %s: %d tris, %d rays (%d floor %d flat "
                            "| walls %d edge %d step %d probe)",
                            o.x, o.y, o.z, int(map_.Mirrored()), SourceName(activeSource_), collisionStats_.total,
                            collisionStats_.rays, collisionStats_.floors, collisionStats_.flattened,
                            collisionStats_.edgeWalls, collisionStats_.stepWalls, collisionStats_.probeWalls));
        lines.push_back(Fmt("hero: %s | enemies %zu | hits dealt %d taken %d", hero_.Status().c_str(), enemies_.size(),
                            hitsLanded_, hitsTaken_));
    }
    lines.push_back(Fmt("input: %s | blocking %d | focus %d cursor %d", input_shim::Status(),
                        int(input_shim::Blocking()), int(input_.Focused()), int(input_.CursorVisible())));
    lines.push_back(Fmt("renderer: %s %ux%u", renderer_.Status().c_str(), renderer_.Width(), renderer_.Height()));
    {
        const tracker::Stats ts = tracker::GetStats();
        const Injector::Stats is = injector_.GetStats();
        lines.push_back(Fmt("in-world: %s | G-buffer %llu shadows %llu captures %llu | %s",
                            worldRender_ ? "ON" : (cfg_.renderMode == "world" ? "off (overlay)" : "overlay mode"),
                            static_cast<unsigned long long>(ts.gbufferInjections),
                            static_cast<unsigned long long>(ts.shadowInjections),
                            static_cast<unsigned long long>(ts.captures), is.status.c_str()));
        lines.push_back(Fmt("  pipelines %llu (G-buffer %llu, casters %llu, cache copies %llu) | unknown draws/views %llu/%llu | "
                            "markers %s | last skip: %s",
                            static_cast<unsigned long long>(ts.psos), static_cast<unsigned long long>(ts.gbufferPsos),
                            static_cast<unsigned long long>(ts.casterPsos), static_cast<unsigned long long>(ts.cacheCopyPsos),
                            static_cast<unsigned long long>(ts.unknownPsoDraws),
                            static_cast<unsigned long long>(ts.unknownRtvBinds), ts.markers ? "yes" : "no",
                            ts.lastSkip.empty() ? "-" : ts.lastSkip.c_str()));
        if (gameView_.valid)
            lines.push_back(Fmt("  game view frame %llu (%.0f ms old) fov %.1f %s | depth frames %llu | world: %zu cells, "
                                "%.1f ms",
                                static_cast<unsigned long long>(gameViewFrame_), (NowSeconds() - gameViewTime_) * 1000.0,
                                gameView_.fovY * 57.29578f, gameView_.leftHanded ? "LH" : "RH",
                                static_cast<unsigned long long>(depthFrames_), worldCells_.load(),
                                worldIntegrateMs_.load()));
    }
    float y = 16 * ui;
    const float x = overlay_.Width() - 980 * ui;
    overlay_.Rect(x - 10 * ui, y - 6 * ui, 975 * ui, float(lines.size()) * overlay_.LineHeight(0.6f) + 12 * ui, 0x000000B0u);
    for (const auto& l : lines) {
        overlay_.Text(x, y, l, 0xE8E8E8FFu, 0.6f);
        y += overlay_.LineHeight(0.6f);
    }
}

void MarioMod::DumpDebug() {
    DVec3 center = active_ ? MarioGamePos(1.0f) : DVec3();
    uintptr_t hero = game_.Hero();
    if (!active_ && hero) game_.GetPosition(hero, center);
    tracker::RequestReport(2, "dump key");
    game_.DumpHeroComponents();
    game_.DumpComponentRegistry(800);
    game_.DumpActorsNear(center, 30.0f, 400);
    LOGI("camera: source %s valid %d candidates %d selected %d at %p (pointer-scan this address to make a [Camera] "
         "binding)",
         camera_.Source().c_str(), int(camera_.Valid()), camera_.CandidateCount(), camera_.CandidateIndex(),
         reinterpret_cast<void*>(camera_.SelectedAddress()));
    log::Flush();
    Toast("Dumped hero/components/actors to sm2mario.log", 4);
}

} // namespace sm2m

#endif
