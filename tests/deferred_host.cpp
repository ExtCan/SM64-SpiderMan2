// End-to-end test host: a small deferred renderer shaped like Spider-Man 2's
// frame, plus the mock engine the mod binds to (tests/mock_game.S).
//
//   * G-buffer with the game's formats and encoding: RT0 R32F linear depth,
//     RT1 RG16F motion, RT2/RT3 RG32U GBuffer0/1, D32S8 depth (reverse-Z)
//   * the game's root signature layout: 24 root constants at b15, a global
//     SRV table, then per stage SRV / CBV / UAV / sampler tables; the view
//     constants (36 rows, same layout) are b0 of the VS CBV table
//   * several command lists with PIX markers ("GBuffer Static", "GBuffer
//     Dynamic", "Create CSM"), resource barriers, two descriptor heaps
//   * a cached sun shadow: static casters go into a cache texture every 60
//     frames; each frame region A of the atlas gets the cache copied in
//     (PS_ShadowCacheCopyDepth style) and dynamic casters drawn on top, region
//     B is drawn without a copy (must never receive Mario)
//   * compute lighting that decodes the G-buffer like CS_ApplyGBufferLighting
//   * the hero is drawn unless his transform's hidden bit (0x5C & 0x20) is set
//
// It writes its own shader digests to sm2mario/bindings.user.ini so the mod
// recognises its shadow shaders, then loads scripts/sm2mario.dll, plays a
// scripted session and prints checks (prefixed "host:") to stdout.
//
//   deferred_host.exe [seconds] [--script]
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------- mock engine
extern "C" {
struct FakeHeroSystem {
    void* vftable;
    uint8_t pad[8];
    int32_t unk;
    uint32_t heroHandle;
};
FakeHeroSystem g_heroSystem;
uint8_t* g_actorArray = nullptr;
uint32_t g_actorCount = 0;
uint64_t g_scratch = 0;
uint32_t g_renderFrame = 0;
void fake_hero_ctor();
uintptr_t fake_get_actor(const uint32_t*);
void fake_set_position(void*, const float*);
void fake_transform_set_position(void*, const float*);
void fake_transform_hide(void*);
void fake_transform_unhide(void*);
void fake_transform_set_matrix(void*, const float*, float*);
// Transform::SetMatrixEx(t, matrix, rotation, scale) and SetMatrixEx2(t,
// matrix, rotation, scale, flags, extra): the game's other two setters - the
// second has six arguments, the sixth a pointer it reads.
void fake_transform_set_matrix_ex(void*, const float*, const void*, const void*);
void fake_transform_set_matrix_ex2(void*, const float*, const void*, const void*, uintptr_t, const void*);
void fake_transform_mark_dirty(void*, int);
float* fake_ct_get_position(void*, float*);
float* fake_ct_get_matrix(void*, float*);
float* fake_ct_get_track(void*, float*);
void fake_ct_nop();
uint32_t g_pedEnterCount = 0;
void fake_ped_enter(void*, const void*);

// The game's physics and damage (mock_game.S; bindings.ini [PhysicsRaycast] ... [DamageSphere]).
// The physics system: its query system is at +0x90, whose result counter is at +0x1E880.
alignas(16) uint8_t g_physicsMem[0x90 + 0x1E880 + 0x100];
void* g_physics = g_physicsMem;
uint8_t g_physPaused = 0;
float g_physTimestep = 1.0f / 60.0f;
float g_physTimescale = 1.0f;
uint64_t g_collTypes[64];
int32_t g_collLive = 0; // requests made and not released yet
uint64_t g_collPool = 0;
alignas(16) uint8_t g_damageSystem[64];
void fake_phys_caller();
void* fake_phys_ray(void*, void*, const float*, const float*, const char*);
void fake_phys_frame(void*);
void* fake_req_init(void*, uint32_t);
void fake_req_release(void*);
void fake_req_ignore(void*, void*);
void* fake_result_actor(void*, int);
void fake_query_alloc();
uint8_t* fake_damage_sphere(void*, const float*, float, const void*);
uint8_t* fake_damage_actor(void*, const uint32_t*);
void fake_damage_caller();
// The game's photo mode ([PhotoMode]): PhotomodeSystem, one global object -
// +0xCE9 is 1 while it is open; in selfie mode +0x884 holds the actor handle
// of the copy of Spider-Man it poses (the "doppelganger").
alignas(16) uint8_t g_photoMode[0xD00];
void fake_photo_switch();
// The game's component registry ([ComponentRegistry] / [GetComponentInfo]):
// a type's ComponentInfo by the CRC of its name, as the real game looks
// components up (E2E_NO_REGISTRY=1: it answers nothing - lookups by name).
alignas(16) uint8_t g_componentRegistry[64];
void fake_component_lookup();
uintptr_t mock_get_component_info(uintptr_t registry, uint32_t crc);
void* mock_ray_cast(uint8_t* qs, const uint8_t* req, const float* from, const float* to, const char* tag);
void mock_physics_step(void* physics);
void* mock_transform_lookup(void* record, uint32_t index);
uint8_t* mock_damage_alloc(void* system);
uint8_t* mock_damage_fill(uint8_t* entry, const float* centre, float radius, const uint8_t* request);
}

namespace {

constexpr int kW = 960, kH = 540;
constexpr float kFeetY = 5.0f;
const float kP0[3] = {100.0f, kFeetY, -40.0f};

alignas(16) float g_heroTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, kP0[0], kP0[1], kP0[2], 1};
alignas(16) float g_pedTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, kP0[0] - 3, kFeetY, kP0[2] + 1, 1};
// A thug in front of Mario's spawn (Mario punches him), and a car that drives by.
alignas(16) float g_enemyTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, kP0[0], kFeetY, kP0[2] + 0.75f, 1};
alignas(16) float g_carTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, kP0[0], -1000.0f, kP0[2] + 14.4f, 1};
// Photo mode's selfie Spider-Man (slot 7), spawned where the hero is while it is open.
alignas(16) float g_doppelTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1000.0f, 0, 1};
bool g_doppelSpawned = false;
bool g_photoOpen = false;
constexpr uint32_t kDoppelHandle = (13u << 20) | 7u;
// Transforms by their id (+0x64), as the physics' hit records name them.
float* const g_xforms[5] = {nullptr, g_heroTransform, g_pedTransform, g_enemyTransform, g_carTransform};

// The game's ComponentInfo: its name at +0x60, the types it derives from at
// +0x80 (count at +0xE6), and +0xE4 bit 6: a lookup of it also matches types
// derived from it (the game's GetComponent).
struct MockComponentInfo {
    uint8_t pad[0x60];
    const char* name;
    uint8_t pad2[0x18];
    const void* parents[12];
    uint8_t pad3[4];
    uint8_t flags;
    uint8_t pad4;
    uint8_t parentCount;
    uint8_t pad5[9];
};
static_assert(offsetof(MockComponentInfo, parents) == 0x80 && offsetof(MockComponentInfo, flags) == 0xE4 &&
                  offsetof(MockComponentInfo, parentCount) == 0xE6,
              "ComponentInfo layout");
struct MockEntry {
    void* info;
    void* comp;
};
alignas(16) MockComponentInfo g_healthInfo{{}, "Health", {}, {}, {}, 0x40, 0, 0, {}};
alignas(16) uint8_t g_heroHealth[0x100];
MockEntry g_heroComps[1] = {{&g_healthInfo, g_heroHealth}};
alignas(16) MockComponentInfo g_pedInfo{{}, "Pedestrian", {}, {}, {}, 0, 0, 0, {}};
alignas(16) MockComponentInfo g_vehicleInfo{{}, "Vehicle", {}, {}, {}, 0, 0, 0, {}};
// The thug's health: a type derived from Health (as the game's BotHealth,
// BossHealth ... are), by a name none of the mod's lists has - only a lookup
// that matches derived types finds it.
alignas(16) MockComponentInfo g_guardHealthInfo{{}, "MockGuardHealth", {}, {&g_healthInfo}, {}, 0, 0, 1, {}};
alignas(16) uint8_t g_enemyHealth[0x100], g_pedComp[0x100], g_carComp[0x100];
MockEntry g_enemyComps[1] = {{&g_guardHealthInfo, g_enemyHealth}};
MockEntry g_pedComps[1] = {{&g_pedInfo, g_pedComp}};
MockEntry g_carComps[1] = {{&g_vehicleInfo, g_carComp}};

uint32_t& HeroFlags() { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g_heroTransform) + 0x5C); }
uint32_t& DoppelFlags() { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g_doppelTransform) + 0x5C); }
bool HeroHidden() { return (HeroFlags() & 0x20) != 0; }

// The follow camera's target, a Camera2::CameraTarget with MSVC-style RTTI
// (type descriptor, complete object locator, vtable) like the game's, so the
// mod finds its virtuals by class name. Filled in by SetupMock (the locator
// holds image-relative offsets).
struct MsvcTypeDescriptor {
    const void* vftable;
    void* spare;
    char name[48];
};
struct MsvcLocator {
    uint32_t signature, offset, cdOffset, typeDescriptor, classDescriptor, self;
};
alignas(16) MsvcTypeDescriptor g_ctType = {&g_scratch, nullptr, ".?AVCameraTarget@Camera2@@"};
alignas(16) MsvcLocator g_ctLocator;
alignas(16) const void* g_ctVtable[1 + 22];
struct MockCameraTarget {
    const void* const* vtable;
    void* pad;
    void* actor;
};
MockCameraTarget g_heroTarget;

// A pedestrian asking for a picture: the game runs BehaviorPedestrianInteract
// on them (found by RTTI, like CameraTarget); its activation is vtable slot 1
// and the pedestrian's actor handle is at +0x48.
alignas(16) MsvcTypeDescriptor g_piType = {&g_scratch, nullptr, ".?AVBehaviorPedestrianInteract@@"};
alignas(16) MsvcLocator g_piLocator;
alignas(16) const void* g_piVtable[1 + 12];
struct MockPedBehavior {
    const void* const* vtable;
    uint8_t pad[0x40];
    uint32_t owner; // +0x48
    uint8_t rest[0xC0];
};
MockPedBehavior g_pedBehavior;
constexpr uint32_t kPedHandle = (5u << 20) | 4u;
constexpr uint32_t kHeroHandle = (7u << 20) | 3u;
constexpr uint32_t kEnemyHandle = (9u << 20) | 5u;
constexpr uint32_t kCarHandle = (11u << 20) | 6u;

void AskForPicture() {
    // What the game's AI does when the pedestrian starts asking.
    using EnterFn = void (*)(MockPedBehavior*, const void*);
    const uint32_t params[4] = {2, 0, 0, 0};
    g_pedBehavior.owner = kPedHandle;
    reinterpret_cast<EnterFn>(const_cast<void*>(g_pedBehavior.vtable[1]))(&g_pedBehavior, params);
    std::printf("host: a pedestrian asks for a picture (behaviour entered %u time(s))\n", g_pedEnterCount);
    std::fflush(stdout);
}

// The ModSettings mod (tests/mock_modsettings.cpp), if the run put it next to the game.
struct ModSettingsMock {
    int (*open)(char*, int) = nullptr;
    int (*set)(const char*, float) = nullptr;
    int (*save)() = nullptr;
    int (*calls)() = nullptr;
} g_ms;

using TargetFn = float* (*)(MockCameraTarget*, float*);
TargetFn TargetSlot(int i) { return reinterpret_cast<TargetFn>(const_cast<void*>(g_heroTarget.vtable[i])); }

void SetupMock() {
    g_actorArray = static_cast<uint8_t*>(_aligned_malloc(0xC0 * 8, 16));
    std::memset(g_actorArray, 0, 0xC0 * 8);
    uint8_t* slot = g_actorArray + 3 * 0xC0;
    *reinterpret_cast<float**>(slot) = g_heroTransform;
    *reinterpret_cast<uint16_t*>(slot + 0x08) = 7; // serial (the handle's bits 20-30)
    *reinterpret_cast<uint32_t*>(slot + 0x0C) = 3; // its own slot index
    *reinterpret_cast<MockEntry**>(slot + 0x68) = g_heroComps;
    *reinterpret_cast<uint16_t*>(slot + 0x70) = 1;
    alignas(8) static const char kHeroName[] = "hero_mock_spiderman";
    *reinterpret_cast<const char**>(slot + 0xB0) = kHeroName;
    *reinterpret_cast<float*>(g_heroHealth + 0xA0) = 1000.0f;
    *reinterpret_cast<float*>(g_heroHealth + 0xD0) = 1000.0f;
    g_actorCount = 8;
    g_heroSystem.heroHandle = (7u << 20) | 3u;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_ctLocator.signature = 1;
    g_ctLocator.typeDescriptor = uint32_t(reinterpret_cast<uintptr_t>(&g_ctType) - base);
    g_ctLocator.self = uint32_t(reinterpret_cast<uintptr_t>(&g_ctLocator) - base);
    g_ctVtable[0] = &g_ctLocator;
    for (int i = 1; i < 23; ++i) g_ctVtable[i] = reinterpret_cast<const void*>(&fake_ct_nop);
    g_ctVtable[1 + 10] = reinterpret_cast<const void*>(&fake_ct_get_position);
    g_ctVtable[1 + 11] = reinterpret_cast<const void*>(&fake_ct_get_matrix);
    g_ctVtable[1 + 18] = reinterpret_cast<const void*>(&fake_ct_get_track);
    g_heroTarget.vtable = g_ctVtable + 1;
    g_heroTarget.actor = slot;
    // the pedestrian (slot 4)
    uint8_t* ped = g_actorArray + 4 * 0xC0;
    *reinterpret_cast<float**>(ped) = g_pedTransform;
    *reinterpret_cast<uint16_t*>(ped + 0x08) = 5;
    *reinterpret_cast<uint32_t*>(ped + 0x0C) = 4;
    alignas(8) static const char kPedName[] = "ped_mock_tourist";
    *reinterpret_cast<const char**>(ped + 0xB0) = kPedName;
    g_piLocator.signature = 1;
    g_piLocator.typeDescriptor = uint32_t(reinterpret_cast<uintptr_t>(&g_piType) - base);
    g_piLocator.self = uint32_t(reinterpret_cast<uintptr_t>(&g_piLocator) - base);
    g_piVtable[0] = &g_piLocator;
    for (int i = 1; i < 13; ++i) g_piVtable[i] = reinterpret_cast<const void*>(&fake_ct_nop);
    g_piVtable[1 + 1] = reinterpret_cast<const void*>(&fake_ped_enter);
    g_pedBehavior.vtable = g_piVtable + 1;
    *reinterpret_cast<MockEntry**>(ped + 0x68) = g_pedComps;
    *reinterpret_cast<uint16_t*>(ped + 0x70) = 1;
    // the thug (slot 5) and the car (slot 6)
    uint8_t* thug = g_actorArray + 5 * 0xC0;
    *reinterpret_cast<float**>(thug) = g_enemyTransform;
    *reinterpret_cast<uint16_t*>(thug + 0x08) = 9;
    *reinterpret_cast<uint32_t*>(thug + 0x0C) = 5;
    *reinterpret_cast<MockEntry**>(thug + 0x68) = g_enemyComps;
    *reinterpret_cast<uint16_t*>(thug + 0x70) = 1;
    alignas(8) static const char kThugName[] = "enemy_mock_thug";
    alignas(8) static const char kCarName[] = "car_mock_taxi";
    *reinterpret_cast<const char**>(thug + 0xB0) = kThugName;
    *reinterpret_cast<float*>(g_enemyHealth + 0xA0) = 500.0f;
    *reinterpret_cast<float*>(g_enemyHealth + 0xD0) = 500.0f;
    uint8_t* car = g_actorArray + 6 * 0xC0;
    *reinterpret_cast<float**>(car) = g_carTransform;
    *reinterpret_cast<uint16_t*>(car + 0x08) = 11;
    *reinterpret_cast<uint32_t*>(car + 0x0C) = 6;
    *reinterpret_cast<MockEntry**>(car + 0x68) = g_carComps;
    *reinterpret_cast<uint16_t*>(car + 0x70) = 1;
    *reinterpret_cast<const char**>(car + 0xB0) = kCarName;
    // photo mode's selfie Spider-Man (slot 7): no serial (no actor) until photo mode spawns it
    uint8_t* doppel = g_actorArray + 7 * 0xC0;
    *reinterpret_cast<float**>(doppel) = g_doppelTransform;
    *reinterpret_cast<uint32_t*>(doppel + 0x0C) = 7;
    alignas(8) static const char kDoppelName[] = "hero_mock_spiderman_photomode_doppelganger";
    *reinterpret_cast<const char**>(doppel + 0xB0) = kDoppelName;
    // Each transform: "belongs to an actor" (+0x5C & 0x1000), its id (+0x64), the actor's handle (+0x68).
    const uint32_t handles[5] = {0, kHeroHandle, kPedHandle, kEnemyHandle, kCarHandle};
    for (uint32_t i = 1; i < 5; ++i) {
        uint8_t* x = reinterpret_cast<uint8_t*>(g_xforms[i]);
        *reinterpret_cast<uint32_t*>(x + 0x5C) |= 0x1000;
        *reinterpret_cast<uint32_t*>(x + 0x64) = i;
        *reinterpret_cast<uint32_t*>(x + 0x68) = handles[i];
    }
    volatile uintptr_t keep = reinterpret_cast<uintptr_t>(&fake_hero_ctor) ^ reinterpret_cast<uintptr_t>(&fake_get_actor) ^
                              reinterpret_cast<uintptr_t>(&fake_set_position) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_set_position) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_hide) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_unhide) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_set_matrix) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_set_matrix_ex) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_set_matrix_ex2) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_mark_dirty) ^
                              reinterpret_cast<uintptr_t>(&fake_phys_caller) ^ reinterpret_cast<uintptr_t>(&fake_phys_ray) ^
                              reinterpret_cast<uintptr_t>(&fake_req_init) ^ reinterpret_cast<uintptr_t>(&fake_req_release) ^
                              reinterpret_cast<uintptr_t>(&fake_req_ignore) ^ reinterpret_cast<uintptr_t>(&fake_result_actor) ^
                              reinterpret_cast<uintptr_t>(&fake_query_alloc) ^ reinterpret_cast<uintptr_t>(&fake_damage_sphere) ^
                              reinterpret_cast<uintptr_t>(&fake_damage_actor) ^ reinterpret_cast<uintptr_t>(&fake_damage_caller) ^
                              reinterpret_cast<uintptr_t>(&fake_photo_switch) ^
                              reinterpret_cast<uintptr_t>(&fake_component_lookup);
    (void)keep;
}

// ---------------------------------------------------------------- math
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 Mk(float x, float y, float z) {
    V3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}
V3 Sub(V3 a, V3 b) { return Mk(a.x - b.x, a.y - b.y, a.z - b.z); }
V3 Add(V3 a, V3 b) { return Mk(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 Mul(V3 a, float s) { return Mk(a.x * s, a.y * s, a.z * s); }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return Mk(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
V3 Norm(V3 a) { return Mul(a, 1.0f / std::sqrt(Dot(a, a))); }

// The game's view constants (render/view_constants.h): rows 0-2 axes, 3
// position, 4-7 camera-relative VP, 8-11 previous VP, 15 previous position,
// 28.w flags, 33 motion scale, 34 1/size, 35 (w eps, sky depth).
struct ViewCB {
    float r[36][4];
};

// Like Spider-Man 2, the screen is right-handed by default: the camera's
// stored x axis is world "right" for a left-handed basis, but the projection
// flips it (E2E_SCREEN=lh renders the plain left-handed way instead).
bool g_rhScreen = true;

void PerspectiveView(V3 cam, V3 fwd, float fovY, int w, int h, const ViewCB* prev, ViewCB& out) {
    std::memset(&out, 0, sizeof(out));
    const V3 F = Norm(fwd);
    const V3 R = Norm(Cross(Mk(0, 1, 0), F));
    const V3 U = Cross(F, R);
    const float ys = 1.0f / std::tan(fovY * 0.5f), xs = ys * float(h) / float(w);
    const V3 ax[3] = {R, U, F};
    for (int i = 0; i < 3; ++i) {
        out.r[i][0] = ax[i].x;
        out.r[i][1] = ax[i].y;
        out.r[i][2] = ax[i].z;
    }
    out.r[3][0] = cam.x;
    out.r[3][1] = cam.y;
    out.r[3][2] = cam.z;
    out.r[3][3] = 1;
    const float Rk[3] = {R.x, R.y, R.z}, Uk[3] = {U.x, U.y, U.z}, Fk[3] = {F.x, F.y, F.z};
    for (int k = 0; k < 3; ++k) {
        out.r[4 + k][0] = Rk[k] * xs * (g_rhScreen ? -1.0f : 1.0f);
        out.r[4 + k][1] = Uk[k] * ys;
        out.r[4 + k][2] = 0;
        out.r[4 + k][3] = Fk[k];
    }
    out.r[7][2] = 0.05f; // reverse-Z, infinite far plane
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out.r[8 + i][j] = prev ? prev->r[4 + i][j] : out.r[4 + i][j];
    for (int j = 0; j < 3; ++j) out.r[15][j] = prev ? prev->r[3][j] : out.r[3][j];
    out.r[33][0] = out.r[33][1] = 1.0f;
    out.r[34][0] = 1.0f / float(w);
    out.r[34][1] = 1.0f / float(h);
    out.r[35][0] = 1e-4f;
    out.r[35][1] = 100000.0f;
}

void OrthoView(V3 lightPos, V3 fwd, float halfWidth, float range, int size, ViewCB& out) {
    std::memset(&out, 0, sizeof(out));
    const V3 F = Norm(fwd);
    const V3 R = Norm(Cross(Mk(0, 1, 0), F));
    const V3 U = Cross(F, R);
    const V3 ax[3] = {R, U, F};
    for (int i = 0; i < 3; ++i) {
        out.r[i][0] = ax[i].x;
        out.r[i][1] = ax[i].y;
        out.r[i][2] = ax[i].z;
    }
    out.r[3][0] = lightPos.x;
    out.r[3][1] = lightPos.y;
    out.r[3][2] = lightPos.z;
    out.r[3][3] = 1;
    const float Rk[3] = {R.x, R.y, R.z}, Uk[3] = {U.x, U.y, U.z}, Fk[3] = {F.x, F.y, F.z};
    for (int k = 0; k < 3; ++k) {
        out.r[4 + k][0] = Rk[k] / halfWidth;
        out.r[4 + k][1] = Uk[k] / halfWidth;
        out.r[4 + k][2] = Fk[k] / range;
        out.r[4 + k][3] = 0;
    }
    out.r[7][3] = 1;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out.r[8 + i][j] = out.r[4 + i][j];
    for (int j = 0; j < 3; ++j) out.r[15][j] = out.r[3][j];
    uint32_t flags = 4096;
    std::memcpy(&out.r[28][3], &flags, 4);
    out.r[33][0] = out.r[33][1] = 1;
    out.r[34][0] = out.r[34][1] = 1.0f / float(size);
    out.r[35][0] = 1e-4f;
}

// ---------------------------------------------------------------- shaders
const char* kHlsl = R"HLSL(
cbuffer View : register(b0) { float4 V[36]; };
cbuffer Draw : register(b15) { float4 uRow0; float4 uRow1; float4 uRow2; float4 uColor; float4 uMisc; uint4 uIds; };

struct VIn { float3 pos : POSITION; float3 nrm : NORMAL; };
struct GV { float4 pos : SV_Position; float4 prevClip : TEXCOORD1; float3 nrm : NORMAL; };

float3 World(float3 p) { return float3(dot(uRow0.xyz, p) + uRow0.w, dot(uRow1.xyz, p) + uRow1.w, dot(uRow2.xyz, p) + uRow2.w); }
float4 Clip(float3 rel, int r) { return rel.x * V[r] + rel.y * V[r + 1] + rel.z * V[r + 2] + V[r + 3]; }

GV VSGame(VIn i) {
    GV o;
    float3 w = World(i.pos);
    o.pos = Clip(w - V[3].xyz, 4);
    o.prevClip = Clip(w + uMisc.xyz - V[15].xyz, 8); // uMisc.xyz: where it was last frame, relative to now
    o.nrm = normalize(float3(dot(uRow0.xyz, i.nrm), dot(uRow1.xyz, i.nrm), dot(uRow2.xyz, i.nrm)));
    return o;
}

uint PackNormal(float3 n) {
    float k = 5791.1675 * rsqrt(abs(n.z) * 8.0 + 8.0);
    uint ex = min((uint)max(n.x * k + 2047.75, 0.0), 4095u);
    uint ey = min((uint)max(n.y * k + 2047.75, 0.0), 4095u);
    return ex | (ey << 12) | (n.z < 0.0 ? (1u << 24) : 0u);
}
uint PackAlbedo(float3 a) {
    float3 s = saturate(sqrt(saturate(a)));
    return min((uint)(s.r * 255.0 + 0.5), 255u) | (min((uint)(s.g * 255.0 + 0.5), 255u) << 8) |
           (min((uint)(s.b * 127.0 + 0.5), 127u) << 16);
}
struct GOut { float depth : SV_Target0; float2 motion : SV_Target1; uint2 g0 : SV_Target2; uint2 g1 : SV_Target3; };

GOut PSGame(GV i) {
    GOut o;
    o.depth = i.pos.w;
    float2 uvPrev = i.prevClip.xy / max(i.prevClip.w, V[35].x) * float2(0.5, -0.5) + 0.5;
    o.motion = (i.pos.xy * V[34].xy - uvPrev) * V[33].xy;
    o.g0 = uint2(PackNormal(normalize(i.nrm)) | (20u << 25), (1u << 22) | (1u << 25));
    o.g1 = uint2(PackAlbedo(uColor.rgb) | (255u << 23) | 0x80000000u, uIds.x << 16);
    return o;
}

struct FV { float4 pos : SV_Position; };
FV VSFull(uint id : SV_VertexID) {
    FV o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

GOut PSSky(FV i) {
    GOut o;
    o.depth = V[35].y;
    o.motion = float2(0, 0);
    o.g0 = uint2(0, 0);          // shading model 0: sky
    o.g1 = uint2(0, 2u << 16);
    return o;
}

FV VSShadow(VIn i) {
    FV o;
    o.pos = Clip(World(i.pos) - V[3].xyz, 4);
    return o;
}
float PSShadowCaster(FV i) : SV_Depth { return i.pos.z * i.pos.w; }

Texture2D<float> gCache : register(t0);
float PSCacheCopy(FV i) : SV_Depth { return gCache.Load(int3(int2(i.pos.xy - uMisc.xy), 0)); }

Texture2D<float4> gHdr : register(t0);
float4 PSComposite(FV i) : SV_Target {
    float3 c = gHdr.Load(int3(int2(i.pos.xy), 0)).rgb;
    c = c / (1.0 + c);
    return float4(pow(saturate(c), 1.0 / 2.2), 1);
}

// Lighting (decodes the G-buffer like CS_ApplyGBufferLighting_Std).
cbuffer Light : register(b0) { float4 L[24]; };
Texture2D<float> gLin : register(t0);
Texture2D<uint2> gG0 : register(t1);
Texture2D<uint2> gG1 : register(t2);
Texture2D<float> gShadow : register(t3);
RWTexture2D<float4> gOut : register(u0);

float3 UnpackNormal(uint x) {
    float fx = float(x & 4095u) * 6.9067e-4 - 1.41421354;
    float fy = float((x >> 12) & 4095u) * 6.9067e-4 - 1.41421354;
    float d = fx * fx + fy * fy;
    float s = sqrt(max(0.0, 1.0 - d * 0.25));
    float z = 1.0 - d * 0.5;
    return float3(fx * s, fy * s, (x & (1u << 24)) ? -z : z);
}

[numthreads(8, 8, 1)]
void CSLight(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(L[0].w) || id.y >= uint(L[1].w)) return;
    uint2 g0 = gG0.Load(int3(id.xy, 0));
    uint2 g1 = gG1.Load(int3(id.xy, 0));
    float depth = gLin.Load(int3(id.xy, 0));
    float2 ndc = (float2(id.xy) + 0.5) / float2(L[0].w, L[1].w) * float2(2, -2) + float2(-1, 1);
    float3 ray = ndc.x / L[2].w * L[1].xyz + ndc.y / L[3].w * L[2].xyz + L[3].xyz;
    if (((g0.y >> 22) & 7u) == 0u) {
        gOut[id.xy] = float4(lerp(float3(0.55, 0.65, 0.8), float3(0.25, 0.4, 0.75), saturate(ray.y)), 1);
        return;
    }
    float3 albedo = float3(g1.x & 255u, (g1.x >> 8) & 255u, (g1.x >> 16) & 127u) / float3(255, 255, 127);
    albedo *= albedo;
    float3 n = UnpackNormal(g0.x);
    float3 p = L[0].xyz + ray * depth;
    // sun shadow: light view rows L[4..7], light position L[8], atlas region A
    float3 rel = p - L[8].xyz;
    float4 lc = rel.x * L[4] + rel.y * L[5] + rel.z * L[6] + L[7];
    float2 suv = lc.xy * float2(0.5, -0.5) + 0.5;
    float lit = 1.0;
    if (all(suv > 0.0) && all(suv < 1.0)) {
        float sd = gShadow.Load(int3(int2(suv * 1024.0), 0));
        lit = lc.z - 0.002 <= sd ? 1.0 : 0.0;
    }
    float3 sunDir = -L[9].xyz;
    float ndl = saturate(dot(n, sunDir));
    float3 v = normalize(-ray);
    float gloss = float(g0.x >> 25) / 127.0;
    float spec = pow(saturate(dot(n, normalize(sunDir + v))), 8.0 + 120.0 * gloss) * gloss;
    float3 c = albedo * (0.18 + 1.6 * ndl * lit) + spec * lit * 0.6;
    gOut[id.xy] = float4(c, 1);
}
)HLSL";

// Diagnostics: the main depth buffer's stencil, copied out as uints.
const char* kStencilHlsl = R"HLSL(
Texture2D<uint2> gStencil : register(t0);
RWTexture2D<uint> gMarks : register(u0);
[numthreads(8, 8, 1)]
void CSStencil(uint3 id : SV_DispatchThreadID) {
    gMarks[id.xy] = gStencil.Load(int3(id.xy, 0)).g;
}
)HLSL";

// Every frame: where Mario is on screen (his pixels in G-buffer 1: no
// material table entry, or the metal cap's) - their count, the sums of their
// x and y, and of the length of their motion vectors (px, x16).
const char* kCentroidHlsl = R"HLSL(
Texture2D<uint2> gIds : register(t0);
Texture2D<float2> gMotion : register(t1);
RWByteAddressBuffer gSums : register(u0);
[numthreads(8, 8, 1)]
void CSCentroid(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gIds.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint m = gIds.Load(int3(id.xy, 0)).y >> 16;
    if (m == 0xFFFFu || m == 0u) {
        uint o;
        gSums.InterlockedAdd(0, 1u, o);
        gSums.InterlockedAdd(4, id.x, o);
        gSums.InterlockedAdd(8, id.y, o);
        float2 mv = gMotion.Load(int3(id.xy, 0)) * float2(w, h);
        gSums.InterlockedAdd(12, uint(min(length(mv), 4000.0) * 16.0), o);
    }
}
)HLSL";

struct Blob {
    std::vector<uint8_t> bytes;
    D3D12_SHADER_BYTECODE Code() const { return {bytes.data(), bytes.size()}; }
    std::string Digest() const {
        char buf[33];
        for (int i = 0; i < 16; ++i) std::snprintf(buf + i * 2, 3, "%02x", bytes[4 + i]);
        return std::string(buf, 32);
    }
};

Blob g_csStencil, g_csCentroid;

bool CompileAll(Blob& vsGame, Blob& psGame, Blob& vsFull, Blob& psSky, Blob& vsShadow, Blob& psCaster, Blob& psCopy,
                Blob& psComposite, Blob& csLight) {
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? reinterpret_cast<decltype(&D3DCompile)>(reinterpret_cast<void*>(GetProcAddress(comp, "D3DCompile")))
                        : nullptr;
    if (!compile) return false;
    struct Job {
        const char* entry;
        const char* target;
        Blob* out;
    } jobs[] = {{"VSGame", "vs_5_1", &vsGame},      {"PSGame", "ps_5_1", &psGame},
                {"VSFull", "vs_5_1", &vsFull},      {"PSSky", "ps_5_1", &psSky},
                {"VSShadow", "vs_5_1", &vsShadow},  {"PSShadowCaster", "ps_5_1", &psCaster},
                {"PSCacheCopy", "ps_5_1", &psCopy}, {"PSComposite", "ps_5_1", &psComposite},
                {"CSLight", "cs_5_1", &csLight}};
    for (auto& j : jobs) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(compile(kHlsl, std::strlen(kHlsl), "mock", nullptr, nullptr, j.entry, j.target, 0, 0, &code, &err))) {
            std::printf("host: shader %s failed: %s\n", j.entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        j.out->bytes.assign(p, p + code->GetBufferSize());
        code->Release();
        if (err) err->Release();
    }
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(compile(kStencilHlsl, std::strlen(kStencilHlsl), "stencil", nullptr, nullptr, "CSStencil", "cs_5_1", 0,
                           0, &code, &err))) {
            std::printf("host: shader CSStencil failed: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        g_csStencil.bytes.assign(p, p + code->GetBufferSize());
        code->Release();
        if (err) err->Release();
    }
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(compile(kCentroidHlsl, std::strlen(kCentroidHlsl), "centroid", nullptr, nullptr, "CSCentroid",
                           "cs_5_1", 0, 0, &code, &err))) {
            std::printf("host: shader CSCentroid failed: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        g_csCentroid.bytes.assign(p, p + code->GetBufferSize());
        code->Release();
        if (err) err->Release();
    }
    return true;
}

// ---------------------------------------------------------------- scene
struct Obj {
    V3 c, s;          // centre, size
    V3 color;
    uint32_t material;
    bool caster;
};
std::vector<Obj> g_static;

void BuildScene() {
    const float x = kP0[0], z = kP0[2];
    g_static.push_back({Mk(x, kFeetY - 0.5f, z + 10), Mk(60, 1, 60), Mk(0.30f, 0.33f, 0.30f), 1, true}); // ground
    for (int i = 0; i < 4; ++i) {                                                                      // stairs
        const float h = 0.25f * float(i + 1);
        g_static.push_back({Mk(x, kFeetY + h * 0.5f, z + 3.25f + 0.5f * float(i)), Mk(3, h, 0.5f), Mk(0.55f, 0.48f, 0.36f), 1,
                            true});
    }
    g_static.push_back({Mk(x, kFeetY + 0.5f, z + 9.0f), Mk(3, 1.0f, 8.0f), Mk(0.55f, 0.48f, 0.36f), 1, true}); // platform
    g_static.push_back({Mk(x, kFeetY + 4.0f, z + 16.5f), Mk(20, 8, 1), Mk(0.45f, 0.30f, 0.25f), 1, true});     // wall
    g_static.push_back({Mk(x + 0.85f, kFeetY + 2.0f, z - 1.6f), Mk(0.35f, 4, 0.35f), Mk(0.6f, 0.6f, 0.65f), 1, true}); // pillar
    g_static.push_back({Mk(x - 4.0f, kFeetY + 0.7f, z + 4.0f), Mk(2, 1.4f, 4.2f), Mk(0.15f, 0.25f, 0.6f), 1, true});  // car
}

struct DrawConsts {
    float row0[4], row1[4], row2[4];
    float color[4];
    float misc[4];
    uint32_t ids[4];
};
static_assert(sizeof(DrawConsts) == 24 * 4, "24 root constants");

DrawConsts BoxConsts(V3 c, V3 s, V3 color, uint32_t material) {
    DrawConsts d{};
    d.row0[0] = s.x;
    d.row0[3] = c.x;
    d.row1[1] = s.y;
    d.row1[3] = c.y;
    d.row2[2] = s.z;
    d.row2[3] = c.z;
    d.color[0] = color.x;
    d.color[1] = color.y;
    d.color[2] = color.z;
    d.ids[0] = material;
    return d;
}

// ---------------------------------------------------------------- D3D12
template <typename T>
void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

ID3D12Device* g_dev = nullptr;

ID3D12Resource* Tex(DXGI_FORMAT f, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
                    const D3D12_CLEAR_VALUE* cv) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, cv, IID_PPV_ARGS(&r)))) {
        std::printf("host: texture %d %ux%u failed\n", int(f), w, h);
        return nullptr;
    }
    return r;
}

ID3D12Resource* Buf(D3D12_HEAP_TYPE t, UINT64 size, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = t;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

struct Renderer {
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* sc = nullptr;
    ID3D12Resource* back[3] = {};
    ID3D12CommandAllocator* alloc[4] = {};
    ID3D12GraphicsCommandList* lists[4] = {};
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    ID3D12RootSignature* rs = nullptr;
    ID3D12RootSignature* rsCompute = nullptr;
    ID3D12PipelineState *psoGame = nullptr, *psoSky = nullptr, *psoCaster = nullptr, *psoCopy = nullptr,
                        *psoComposite = nullptr, *psoLight = nullptr, *psoMark = nullptr, *psoStencil = nullptr,
                        *psoCentroid = nullptr;
    ID3D12Resource* sums = nullptr; // Mario's pixels this frame: count, sum x, sum y (CSCentroid)
    ID3D12DescriptorHeap *heap = nullptr, *samplers = nullptr, *rtvHeap = nullptr, *dsvHeap = nullptr;
    UINT descStride = 0, rtvStride = 0, dsvStride = 0;
    ID3D12Resource* gb[4] = {};
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* hdr = nullptr;
    ID3D12Resource* atlas = nullptr; // 2048 x 1024, R32 typeless (D32 view)
    ID3D12Resource* cache = nullptr; // 1024 x 1024
    ID3D12Resource* upload = nullptr;
    uint8_t* uploadPtr = nullptr;
    ID3D12Resource* vb = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* marks = nullptr; // the stencil, as R32_UINT (diagnostics)
    // descriptor slots
    enum : UINT {
        kGlobalSrv = 0,      // 70 null SRVs (t57.. space1)
        kNullSrv = 80,       // 57 null SRVs (stage SRV tables)
        kNullUav = 140,      // 16 null UAVs
        kCacheSrv = 160,     // PS SRV table for the cache copy (t0 = cache, then nulls)
        kHdrSrv = 220,       // PS SRV table for the composite (t0 = HDR)
        kLightSrv = 280,     // t0..t3 for the lighting CS
        kLightUav = 290,
        kViewCbv = 300,      // 3 frames x (main 14 + light 14)
        kStencilSrv = 440,   // t0 = the depth buffer's stencil, t1..t3 null (diagnostics)
        kMarksUav = 450,     // u0 = marks
        kCentroidSrv = 460,  // t0 = G-buffer 1, t1..t3 null (Mario on screen)
        kSumsUav = 470,      // u0 = sums
    };
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * descStride;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(i) * descStride;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtvStride;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * dsvStride;
        return h;
    }
};

Renderer g;
ViewCB g_view, g_prevView, g_light;
bool g_havePrev = false;
uint64_t g_frame = 0;
// diagnostics
int g_diagRequest = 0;
int g_diagNumber = 0;
constexpr UINT64 kStencilOffset = 20ull * 1024 * 1024; // in the read-back buffer
constexpr UINT64 kSumsOffset = 23ull * 1024 * 1024;    // ... Mario on screen, every frame
constexpr UINT64 kZeroOffset = 60 * 1024;              // 16 zero bytes in the upload buffer
UINT g_stencilPitch = 0;

bool CreateRootSignatures() {
    // The game's graphics layout (Spider-Man2.exe 142cd9750, VS+PS variant).
    D3D12_DESCRIPTOR_RANGE gSrv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 70, 57, 1, 0};
    D3D12_DESCRIPTOR_RANGE stage[4] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 57, 0, 0, 0},
                                       {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 14, 0, 0, 0},
                                       {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 16, 0, 0, 0},
                                       {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0, 0}};
    D3D12_ROOT_PARAMETER p[10]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 15;
    p[0].Constants.Num32BitValues = 24;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &gSrv;
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < 4; ++k) {
            D3D12_ROOT_PARAMETER& q = p[2 + s * 4 + k];
            q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            q.DescriptorTable.NumDescriptorRanges = 1;
            q.DescriptorTable.pDescriptorRanges = &stage[k];
            q.ShaderVisibility = s == 0 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
        }
    D3D12_ROOT_SIGNATURE_DESC d{};
    d.NumParameters = 10;
    d.pParameters = p;
    d.Flags = D3D12_ROOT_SIGNATURE_FLAGS(0x1D);
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rs)))) {
        std::printf("host: root signature failed: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    Rel(blob);
    Rel(err);
    D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER c[3]{};
    c[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    c[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    c[1].DescriptorTable.NumDescriptorRanges = 1;
    c[1].DescriptorTable.pDescriptorRanges = &srv;
    c[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    c[2].DescriptorTable.NumDescriptorRanges = 1;
    c[2].DescriptorTable.pDescriptorRanges = &uav;
    d.NumParameters = 3;
    d.pParameters = c;
    d.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rsCompute))))
        return false;
    Rel(blob);
    Rel(err);
    return true;
}

bool CreatePipelines(const Blob& vsGame, const Blob& psGame, const Blob& vsFull, const Blob& psSky, const Blob& vsShadow,
                     const Blob& psCaster, const Blob& psCopy, const Blob& psComposite, const Blob& csLight) {
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = g.rs;
    p.VS = vsGame.Code();
    p.PS = psGame.Code();
    for (int i = 0; i < 4; ++i) p.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // closed boxes: depth sorts it out
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    // The game marks what it draws in stencil; Mario should get the same.
    p.DepthStencilState.StencilEnable = TRUE;
    p.DepthStencilState.StencilReadMask = 0xFF;
    p.DepthStencilState.StencilWriteMask = 0x01;
    p.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE,
                                     D3D12_COMPARISON_FUNC_ALWAYS};
    p.DepthStencilState.BackFace = p.DepthStencilState.FrontFace;
    p.InputLayout = {layout, 2};
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 4;
    p.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    p.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
    p.RTVFormats[2] = DXGI_FORMAT_R32G32_UINT;
    p.RTVFormats[3] = DXGI_FORMAT_R32G32_UINT;
    p.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    p.SampleDesc.Count = 1;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&g.psoGame)))) return false;
    // The hero's own stencil mark: a depth-only pass of its own after the
    // G-buffer, into the main depth buffer (Spider-Man 2's ModelStencilWrite:
    // its own depth-stencil state and reference) - bit 0x80 on top of the 0x01
    // every G-buffer draw writes.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC w = p;
    w.PS = D3D12_SHADER_BYTECODE{};
    w.NumRenderTargets = 0;
    for (auto& f : w.RTVFormats) f = DXGI_FORMAT_UNKNOWN;
    for (auto& rt : w.BlendState.RenderTarget) rt.RenderTargetWriteMask = 0;
    w.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    w.DepthStencilState.StencilWriteMask = 0x80;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&w, IID_PPV_ARGS(&g.psoMark)))) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC s = p; // sky init: full screen, no depth test
    s.VS = vsFull.Code();
    s.PS = psSky.Code();
    s.InputLayout = {nullptr, 0};
    s.DepthStencilState.DepthEnable = FALSE;
    s.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    s.DepthStencilState.StencilEnable = FALSE;
    s.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&s, IID_PPV_ARGS(&g.psoSky)))) return false;
    // shadow caster (depth only, PS writes z*w like PS_ShadowCaster)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC c{};
    c.pRootSignature = g.rs;
    c.VS = vsShadow.Code();
    c.PS = psCaster.Code();
    c.SampleMask = UINT_MAX;
    c.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    c.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    c.RasterizerState.DepthBias = 50;
    c.RasterizerState.SlopeScaledDepthBias = 1.5f;
    c.RasterizerState.DepthClipEnable = FALSE;
    c.DepthStencilState.DepthEnable = TRUE;
    c.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    c.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    c.InputLayout = {layout, 2};
    c.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    c.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    c.SampleDesc.Count = 1;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&c, IID_PPV_ARGS(&g.psoCaster)))) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC k = c; // cache copy: full screen, writes depth
    k.VS = vsFull.Code();
    k.PS = psCopy.Code();
    k.InputLayout = {nullptr, 0};
    k.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    k.RasterizerState.DepthBias = 0;
    k.RasterizerState.SlopeScaledDepthBias = 0;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&k, IID_PPV_ARGS(&g.psoCopy)))) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC m{}; // composite to the back buffer
    m.pRootSignature = g.rs;
    m.VS = vsFull.Code();
    m.PS = psComposite.Code();
    m.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    m.SampleMask = UINT_MAX;
    m.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    m.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    m.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    m.NumRenderTargets = 1;
    m.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    m.SampleDesc.Count = 1;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&m, IID_PPV_ARGS(&g.psoComposite)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = g.rsCompute;
    cp.CS = csLight.Code();
    if (FAILED(g_dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&g.psoLight)))) return false;
    cp.CS = g_csStencil.Code();
    if (FAILED(g_dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&g.psoStencil)))) return false;
    cp.CS = g_csCentroid.Code();
    if (FAILED(g_dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&g.psoCentroid)))) return false;
    return true;
}

bool CreateResources() {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 512;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = 16;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.samplers)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 16;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 8;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.dsvHeap)))) return false;
    g.descStride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.rtvStride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g.dsvStride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT i = 0; i < 16; ++i) {
        D3D12_SAMPLER_DESC sd{};
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g.samplers->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        g_dev->CreateSampler(&sd, h);
    }
    // G-buffer (created in the state they rest in between frames).
    const DXGI_FORMAT f[4] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_UINT,
                              DXGI_FORMAT_R32G32_UINT};
    for (int i = 0; i < 4; ++i) {
        g.gb[i] = Tex(f[i], kW, kH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      nullptr);
        if (!g.gb[i]) return false;
        g_dev->CreateRenderTargetView(g.gb[i], nullptr, g.Rtv(UINT(i)));
    }
    D3D12_CLEAR_VALUE dcv{};
    dcv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dcv.DepthStencil.Depth = 0.0f;
    g.depth = Tex(DXGI_FORMAT_R32G8X24_TYPELESS, kW, kH, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                  D3D12_RESOURCE_STATE_DEPTH_READ, &dcv);
    if (!g.depth) return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g_dev->CreateDepthStencilView(g.depth, &dv, g.Dsv(0));
    g.hdr = Tex(DXGI_FORMAT_R16G16B16A16_FLOAT, kW, kH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    D3D12_CLEAR_VALUE scv{};
    scv.Format = DXGI_FORMAT_D32_FLOAT;
    scv.DepthStencil.Depth = 1.0f;
    g.atlas = Tex(DXGI_FORMAT_R32_TYPELESS, 2048, 1024, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &scv);
    g.cache = Tex(DXGI_FORMAT_R32_TYPELESS, 1024, 1024, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &scv);
    if (!g.hdr || !g.atlas || !g.cache) return false;
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    g_dev->CreateDepthStencilView(g.atlas, &dv, g.Dsv(1));
    g_dev->CreateDepthStencilView(g.cache, &dv, g.Dsv(2));
    // SRVs / UAVs
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    for (UINT i = 0; i < 70; ++i) g_dev->CreateShaderResourceView(nullptr, &sd, g.Cpu(Renderer::kGlobalSrv + i));
    for (UINT i = 0; i < 57; ++i) {
        g_dev->CreateShaderResourceView(nullptr, &sd, g.Cpu(Renderer::kNullSrv + i));
        g_dev->CreateShaderResourceView(i ? nullptr : g.cache, &sd, g.Cpu(Renderer::kCacheSrv + i));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC hs = sd;
    hs.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(g.hdr, &hs, g.Cpu(Renderer::kHdrSrv));
    for (UINT i = 1; i < 57; ++i) g_dev->CreateShaderResourceView(nullptr, &sd, g.Cpu(Renderer::kHdrSrv + i));
    g_dev->CreateShaderResourceView(g.gb[0], &sd, g.Cpu(Renderer::kLightSrv + 0));
    D3D12_SHADER_RESOURCE_VIEW_DESC us = sd;
    us.Format = DXGI_FORMAT_R32G32_UINT;
    g_dev->CreateShaderResourceView(g.gb[2], &us, g.Cpu(Renderer::kLightSrv + 1));
    g_dev->CreateShaderResourceView(g.gb[3], &us, g.Cpu(Renderer::kLightSrv + 2));
    g_dev->CreateShaderResourceView(g.atlas, &sd, g.Cpu(Renderer::kLightSrv + 3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(g.hdr, nullptr, &ud, g.Cpu(Renderer::kLightUav));
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    for (UINT i = 0; i < 16; ++i) g_dev->CreateUnorderedAccessView(nullptr, nullptr, &ud, g.Cpu(Renderer::kNullUav + i));
    // (diagnostics) the stencil as a uint texture
    g.marks = Tex(DXGI_FORMAT_R32_UINT, kW, kH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    if (!g.marks) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC ss = sd;
    ss.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    ss.Texture2D.PlaneSlice = 1;
    g_dev->CreateShaderResourceView(g.depth, &ss, g.Cpu(Renderer::kStencilSrv));
    for (UINT i = 1; i < 4; ++i) g_dev->CreateShaderResourceView(nullptr, &sd, g.Cpu(Renderer::kStencilSrv + i));
    ud.Format = DXGI_FORMAT_R32_UINT;
    g_dev->CreateUnorderedAccessView(g.marks, nullptr, &ud, g.Cpu(Renderer::kMarksUav));
    // upload ring: view CBs (3 frames x 2 x 1 KB) + light CB
    g.upload = Buf(D3D12_HEAP_TYPE_UPLOAD, 64 * 1024, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RANGE none{0, 0};
    g.upload->Map(0, &none, reinterpret_cast<void**>(&g.uploadPtr));
    // unit cube
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<float> v;
    for (int fidx = 0; fidx < 6; ++fidx) {
        const float* N = n[fidx];
        // two tangents
        float t1[3] = {N[1], N[2], N[0]}, t2[3];
        t2[0] = N[1] * t1[2] - N[2] * t1[1];
        t2[1] = N[2] * t1[0] - N[0] * t1[2];
        t2[2] = N[0] * t1[1] - N[1] * t1[0];
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        const int idx[6] = {0, 2, 1, 0, 3, 2};
        for (int k : idx) {
            for (int a = 0; a < 3; ++a)
                v.push_back(0.5f * (N[a] + corners[k][0] * t1[a] + corners[k][1] * t2[a]));
            for (int a = 0; a < 3; ++a) v.push_back(N[a]);
        }
    }
    g.vb = Buf(D3D12_HEAP_TYPE_UPLOAD, v.size() * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr;
    g.vb->Map(0, &none, &p);
    std::memcpy(p, v.data(), v.size() * 4);
    g.vb->Unmap(0, nullptr);
    g.readback = Buf(D3D12_HEAP_TYPE_READBACK, 24ull * 1024 * 1024, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        // Mario on screen: G-buffer 1 in, three sums out (zeroed from the
        // upload buffer before each frame's count).
        std::memset(g.uploadPtr + kZeroOffset, 0, 16);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 256;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&g.sums))))
            return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC is = sd;
        is.Format = DXGI_FORMAT_R32G32_UINT;
        g_dev->CreateShaderResourceView(g.gb[3], &is, g.Cpu(Renderer::kCentroidSrv));
        D3D12_SHADER_RESOURCE_VIEW_DESC ms = sd;
        ms.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_dev->CreateShaderResourceView(g.gb[1], &ms, g.Cpu(Renderer::kCentroidSrv + 1));
        for (UINT i = 2; i < 4; ++i) g_dev->CreateShaderResourceView(nullptr, &sd, g.Cpu(Renderer::kCentroidSrv + i));
        D3D12_UNORDERED_ACCESS_VIEW_DESC bu{};
        bu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        bu.Format = DXGI_FORMAT_R32_TYPELESS;
        bu.Buffer.NumElements = 64;
        bu.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        g_dev->CreateUnorderedAccessView(g.sums, nullptr, &bu, g.Cpu(Renderer::kSumsUav));
    }
    for (int i = 0; i < 4; ++i) {
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i]));
        g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[i], nullptr, IID_PPV_ARGS(&g.lists[i]));
        g.lists[i]->Close();
    }
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

// Writes a view CB into the ring and fills 14 CBV descriptors (b0..b13) with it.
UINT ViewTable(const ViewCB& cb, UINT slot) {
    const UINT64 off = UINT64(slot) * 1024;
    std::memcpy(g.uploadPtr + off, &cb, sizeof(cb));
    const UINT first = Renderer::kViewCbv + slot * 14;
    for (UINT i = 0; i < 14; ++i) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cd{g.upload->GetGPUVirtualAddress() + off, 1024};
        g_dev->CreateConstantBufferView(&cd, g.Cpu(first + i));
    }
    return first;
}

void BindGame(ID3D12GraphicsCommandList* l, UINT viewTable, UINT psSrvTable) {
    ID3D12DescriptorHeap* heaps[2] = {g.heap, g.samplers};
    l->SetDescriptorHeaps(2, heaps);
    l->SetGraphicsRootSignature(g.rs);
    l->SetGraphicsRootDescriptorTable(1, g.Gpu(Renderer::kGlobalSrv));
    for (int s = 0; s < 2; ++s) {
        l->SetGraphicsRootDescriptorTable(UINT(2 + s * 4), g.Gpu(s == 0 ? Renderer::kNullSrv : psSrvTable));
        l->SetGraphicsRootDescriptorTable(UINT(3 + s * 4), g.Gpu(viewTable));
        l->SetGraphicsRootDescriptorTable(UINT(4 + s * 4), g.Gpu(Renderer::kNullUav));
        l->SetGraphicsRootDescriptorTable(UINT(5 + s * 4), g.samplers->GetGPUDescriptorHandleForHeapStart());
    }
}

void Marker(ID3D12GraphicsCommandList* l, const char* name) {
    l->BeginEvent(1, name, UINT(std::strlen(name) + 1)); // WINPIX_EVENT_ANSI_VERSION
}

void Viewport(ID3D12GraphicsCommandList* l, float x, float y, float w, float h) {
    D3D12_VIEWPORT vp{x, y, w, h, 0, 1};
    D3D12_RECT rc{LONG(x), LONG(y), LONG(x + w), LONG(y + h)};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &rc);
}

void DrawBox(ID3D12GraphicsCommandList* l, const DrawConsts& d) {
    l->SetGraphicsRoot32BitConstants(0, 24, &d, 0);
    l->DrawInstanced(36, 1, 0, 0);
}

V3 HeroPos() { return Mk(g_heroTransform[12], g_heroTransform[13], g_heroTransform[14]); }

// Top of the static geometry under (x, z) at or below `from` (the ground plane
// at least).
float GroundBelow(float x, float z, float from) {
    float best = kFeetY;
    for (const Obj& o : g_static) {
        const float top = o.c.y + o.s.y * 0.5f;
        if (top > from || top <= best) continue;
        if (std::fabs(x - o.c.x) <= o.s.x * 0.5f && std::fabs(z - o.c.z) <= o.s.z * 0.5f) best = top;
    }
    return best;
}

// The game's character controller, like Spider-Man 2's when Mario jumps: it
// puts the hero back on the ground under him every frame (alternately through
// Transform::SetMatrix and an inline write + MarkDirty). Without the mod's pin
// hooks the camera below would stay on the ground.
bool g_snapping = true;
int g_snaps = 0;
int g_carFrames = 0;
// SetMatrixEx/Ex2 calls, and how many left the wrong thing at +0x70 (Ex2: its
// sixth argument didn't arrive)
int g_exCalls = 0, g_exWrong = 0, g_ex2Calls = 0, g_ex2Wrong = 0;
const float kQuat[4] = {0, 0, 0, 1}, kScale[3] = {1, 1, 1}, kExtra[3] = {2.5f, 3.5f, 4.5f};
bool ExtraIs(const float* t, float a, float b, float c) { return t[28] == a && t[29] == b && t[30] == c; }
void SetMatrixEx2(float* t, const float* m) {
    fake_transform_set_matrix_ex2(t, m, kQuat, kScale, 0, kExtra);
    ++g_ex2Calls;
    if (!ExtraIs(t, kExtra[0], kExtra[1], kExtra[2])) ++g_ex2Wrong;
}
void SetMatrixEx(float* t, const float* m) {
    fake_transform_set_matrix_ex(t, m, kQuat, kScale);
    ++g_exCalls;
    if (!ExtraIs(t, 1, 1, 1)) ++g_exWrong;
}
void Locomotion() {
    if (!g_snapping) return;
    float* p = &g_heroTransform[12];
    const float ground = GroundBelow(p[0], p[2], p[1] + 0.3f);
    if (p[1] <= ground + 0.001f) return;
    ++g_snaps;
    alignas(16) float m[16];
    std::memcpy(m, g_heroTransform, sizeof(m));
    m[13] = ground;
    switch (g_frame % 3) {
    case 0:
        p[1] = ground;
        fake_transform_mark_dirty(g_heroTransform, -1);
        break;
    case 1: fake_transform_set_matrix(g_heroTransform, m, nullptr); break;
    default: SetMatrixEx2(g_heroTransform, m); break;
    }
}

V3 g_traceHero, g_traceCam; // this frame: Mario (the hero before the controller ran) and the camera
// The camera's transform (an actor's, written by the camera every frame), and
// where its spring has it.
alignas(16) float g_camTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
// ... and the one it writes after the script's switch (a new camera, as after
// a load): the old one is never written again.
alignas(16) float g_camTransform2[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
bool g_camSwapped = false;
float* CamTransform() { return g_camSwapped ? g_camTransform2 : g_camTransform; }
V3 g_camLag;
bool g_camInit = false;
double g_camTau = 0.35; // s (E2E_CAMERA_TAU)

// A car that drives across the gap between the platform and the wall just
// before Mario walks through it. The mod must know it is moving (the game's
// motion vectors for the rendered world; the actor moving for the game's
// physics): it must not leave an invisible wall behind.
double g_now = 0;
V3 g_prevCar;
bool g_havePrevCar = false;
bool CarAt(V3& c) {
    const double t0 = 11.6, t1 = 12.6; // Mario is on the stairs then, and walks through its lane at ~13 s
    if (g_now < t0 || g_now > t1) return false;
    const float u = float((g_now - t0) / (t1 - t0));
    c = Mk(kP0[0] + 14.0f - 28.0f * u, kFeetY + 0.7f, kP0[2] + 14.4f);
    return true;
}

// ---------------------------------------------------------------- the game's physics (mock)
// What the mock's ray casts hit: the scene's boxes (the static world, with
// the game's physics materials), the hero, the pedestrian and the thug
// (actors: their transforms), and the car while it drives by.
struct PhysBox {
    float lo[3], hi[3];
    int16_t material; // the game's physics material
    uint32_t xform;   // transform id (0: the static world)
};
int16_t StaticMaterial(size_t i) {
    // BuildScene's order: ground, 4 stairs, platform, wall, pillar, car
    if (i == 0) return 2;  // kAsphalt
    if (i <= 4) return 9;  // kConcrete
    if (i == 5) return 28; // kGrass
    if (i == 6) return 4;  // kBuilding
    return 45;             // kMetalMedium
}
void AddBox(std::vector<PhysBox>& out, V3 c, V3 sz, int16_t m, uint32_t x) {
    PhysBox b;
    b.lo[0] = c.x - sz.x * 0.5f;
    b.lo[1] = c.y - sz.y * 0.5f;
    b.lo[2] = c.z - sz.z * 0.5f;
    b.hi[0] = c.x + sz.x * 0.5f;
    b.hi[1] = c.y + sz.y * 0.5f;
    b.hi[2] = c.z + sz.z * 0.5f;
    b.material = m;
    b.xform = x;
    out.push_back(b);
}
void PhysScene(std::vector<PhysBox>& out) {
    for (size_t i = 0; i < g_static.size(); ++i) AddBox(out, g_static[i].c, g_static[i].s, StaticMaterial(i), 0);
    AddBox(out, Mk(kP0[0] - 2.0f, kFeetY + 2.0f, kP0[2] + 2.5f), Mk(0.15f, 4.0f, 0.15f), 45, 0); // the lamp post
    auto actor = [&](const float* x, V3 sz, int16_t m, uint32_t id) {
        AddBox(out, Mk(x[12], x[13] + sz.y * 0.5f, x[14]), sz, m, id);
    };
    actor(g_heroTransform, Mk(0.5f, 1.8f, 0.3f), 15, 1); // kFlesh
    actor(g_pedTransform, Mk(0.45f, 1.7f, 0.3f), 15, 2);
    actor(g_enemyTransform, Mk(0.6f, 1.8f, 0.6f), 15, 3);
    V3 car;
    if (CarAt(car)) AddBox(out, car, Mk(4.2f, 1.4f, 2.0f), 45, 4);
}
// Where the segment a..b enters the box (convex shapes: a ray starting
// inside one doesn't hit it).
bool HitBox(const float* a, const float* b, const PhysBox& bx, float& t, float n[3]) {
    float t0 = 0, t1 = 1, sign = 0;
    int axis = -1;
    for (int k = 0; k < 3; ++k) {
        const float d = b[k] - a[k];
        if (std::fabs(d) < 1e-9f) {
            if (a[k] < bx.lo[k] || a[k] > bx.hi[k]) return false;
            continue;
        }
        float ta = (bx.lo[k] - a[k]) / d, tb = (bx.hi[k] - a[k]) / d, s = -1;
        if (ta > tb) {
            std::swap(ta, tb);
            s = 1;
        }
        if (ta > t0) {
            t0 = ta;
            axis = k;
            sign = s;
        }
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    if (axis < 0) return false;
    t = t0;
    n[0] = n[1] = n[2] = 0;
    n[axis] = sign;
    return true;
}

// The result pool of one physics frame (the game's: 256), and its records.
struct MockHit {
    float t;
    uint32_t unused0;
    uint32_t xform; // +8: transform id | 0x01000000 (an actor's)
    float n[3];     // +0xC
    float p[3];     // +0x18
    uint8_t unused1[0x3A - 0x24];
    int16_t material; // +0x3A
    uint8_t unused2[4];
};
static_assert(sizeof(MockHit) == 64, "a hit record is 64 bytes");
struct MockResult {
    MockHit* records;
    uint16_t unused;
    uint16_t count; // +0xA
    uint8_t rest[4];
};
MockResult g_results[256];
MockHit g_resultHits[256][8];

std::atomic<int> g_physRays{0}, g_physRaysElsewhere{0}, g_physBad{0}, g_physUntagged{0}, g_physPoolFull{0};
// The camera's query (11 = kCamera): rays the mod casts towards its camera,
// and a wall only that query sees, which the script puts between Mario and
// the camera for a moment.
std::atomic<int> g_camRays{0}, g_camRaysHit{0};
volatile bool g_camBlock = false;
float g_camBlockX = 0; // (across where Mario stands when it appears)
std::atomic<int> g_heroLeftOut{0}, g_heroHit{0}, g_physSteps{0}, g_physBusy{0}, g_physFrames{0}, g_waterRays{0};
DWORD g_gameThreadId = 0;
volatile bool g_gameThreadQuit = false;

// Damage requests (DamageSphere): looked at in the next physics step, like the game's damage system does.
struct DamageEntry {
    alignas(16) uint8_t mem[0x300];
    float c[3];
    float r;
    uint32_t query;
    bool live;
};
DamageEntry g_damage[16];
int g_damagePending = 0;
std::atomic<int> g_damageBad{0}, g_damageSeen{0};
int g_thugHits = 0;

template <typename T>
T Field(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

void ProcessDamage() {
    for (int i = 0; i < g_damagePending; ++i) {
        DamageEntry& e = g_damage[i];
        const uint8_t* dr = e.mem + 0x68;
        const bool byActor = Field<uint32_t>(e.mem, 0) == 1;
        bool thug;
        char what[96];
        if (byActor) {
            const uint32_t target = Field<uint32_t>(e.mem, 4);
            thug = target == kEnemyHandle;
            std::snprintf(what, sizeof(what), "actor 0x%x", target);
        } else {
            const float* x = g_enemyTransform;
            const float dx = e.c[0] - x[12], dy = e.c[1] - (x[13] + 0.9f), dz = e.c[2] - x[14];
            thug = std::sqrt(dx * dx + dy * dy + dz * dz) < e.r + 0.5f;
            std::snprintf(what, sizeof(what), "sphere (%.2f %.2f %.2f) r %.2f query %u%s", e.c[0], e.c[1], e.c[2], e.r,
                          e.query, e.live ? "" : " (released request!)");
        }
        const float amount = Field<float>(dr, 0x144);
        std::printf("host: DAMAGE %s | damager 0x%x type %u amount %.1f knockback %u x%.2f flags 0x%x masks 0x%llx "
                    "0x%llx | %s\n",
                    what, Field<uint32_t>(dr, 0x138), Field<uint32_t>(dr, 0x13C), amount, Field<uint32_t>(dr, 0x150),
                    Field<float>(dr, 0x154), Field<uint32_t>(dr, 0x160),
                    static_cast<unsigned long long>(Field<uint64_t>(dr, 0x8)),
                    static_cast<unsigned long long>(Field<uint64_t>(dr, 0x18)), thug ? "the thug" : "nobody");
        ++g_damageSeen;
        if (thug && amount > 0) {
            // The game's damage system: health down, and the thug staggers away.
            float* hp = reinterpret_cast<float*>(g_enemyHealth + 0xD0);
            *hp = std::max(0.0f, *hp - amount);
            g_enemyTransform[12] += 8.0f;
            ++g_thugHits;
        }
    }
    g_damagePending = 0;
    std::fflush(stdout);
}

// The game's main update thread: a physics frame every ~16 ms.
DWORD WINAPI GameThread(void*) {
    g_gameThreadId = GetCurrentThreadId();
    static uint8_t self[64];
    while (!g_gameThreadQuit) {
        fake_phys_frame(self);
        ++g_physFrames;
        Sleep(15);
    }
    return 0;
}

void GBufferTargets(ID3D12GraphicsCommandList* l) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4] = {g.Rtv(0), g.Rtv(1), g.Rtv(2), g.Rtv(3)};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g.Dsv(0);
    l->OMSetRenderTargets(4, rtv, FALSE, &dsv);
}

// The game showing Spider-Man again now and then while Mario stands in (the
// end of a gadget, an interaction): Transform::Unhide on his transform.
int g_unhideCalls = 0, g_unhideShown = 0;

LONGLONG Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
double QpcMs(LONGLONG d) {
    static LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return double(d) * 1000.0 / double(freq);
}

// Mario on screen in the frame rendered last (CSCentroid): his pixels, their
// centre and how far they moved by their motion vectors (px, on average).
int g_marioPixels = 0;
double g_marioX = -1, g_marioY = -1, g_marioMv = 0;
bool g_traceRig = false; // ... and whether it was rendered from the game's other camera
// The last Present (the mod's frame runs inside it): when it started and returned.
LONGLONG g_lastPresentStart = 0, g_lastPresentEnd = 0;

// The game's follow camera, like Spider-Man 2's: it chases its target (2.2 m
// above and 4 m behind it) through a spring - it trails jumps - aimed by the
// player (here: fixed). It writes the camera's transform through
// Transform::SetMatrix (which the mod hooks), and the frame is rendered from
// that transform.
// E2E_CAMERA_FAR=1: after Mario's jumps at the wall (33.2-36.4 s) the game's
// own camera falls 50 m behind its target (as it does when Mario flies or
// falls faster than its spring), smoothly, still looking at him - the mod
// must keep placing it.
bool g_camFar = false;
float CameraFallBack(double now) {
    if (!g_camFar) return 0.0f;
    const double up = std::max(0.0, std::min(1.0, now - 33.2)), down = std::max(0.0, std::min(1.0, 36.4 - now));
    return 50.0f * float(std::min(up, down));
}
// E2E_CAMERA_RIGS=1: the game's other camera (Spider-Man 2 has several, and
// renders from one or another): written every frame 3 m further back and 1 m
// higher than the follow camera's own spot, aimed a degree lower, and
// rendered from in every other frame from 21.0 to 27.5 s - as 0.6's log had
// it in flight, a fifth of the frames from a camera the mod didn't place: the
// camera snapped close and far. The mod must find it from the views rendered
// from it and place it too.
bool g_camRigs = false;
alignas(16) float g_camRigB[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
int g_rigFrames = 0;
bool RigFrame() { return g_camRigs && g_now >= 21.0 && g_now <= 27.5 && (g_frame & 1) != 0; }

void WriteGameCamera(double now) {
    float tp[3];
    TargetSlot(10)(&g_heroTarget, tp);
    const V3 want = Add(Mk(tp[0], tp[1], tp[2]), Mk(0, 2.2f, -4.0f - CameraFallBack(now)));
    static double lastT = -1;
    static uint32_t writes = 0;
    if (!g_camInit) {
        g_camLag = want;
        g_camInit = true;
    } else {
        const double dt = std::max(0.0, std::min(0.25, now - lastT));
        const float a = float(1.0 - std::exp(-dt / g_camTau));
        g_camLag = Add(g_camLag, Mul(Sub(want, g_camLag), a));
    }
    lastT = now;
    const V3 F = Norm(Mk(0, -1.2f, 4.0f)), R = Norm(Cross(Mk(0, 1, 0), F)), U = Cross(F, R);
    alignas(16) float m[16] = {R.x, R.y, R.z, 0, U.x, U.y, U.z, 0, F.x, F.y, F.z, 0, g_camLag.x, g_camLag.y, g_camLag.z, 1};
    // (through SetMatrix and SetMatrixEx2 in turn)
    if (++writes & 1) fake_transform_set_matrix(CamTransform(), m, nullptr);
    else SetMatrixEx2(CamTransform(), m);
    if (g_camRigs) {
        const V3 FB = Norm(Mk(0, -1.2765f, 4.0f)), RB = Norm(Cross(Mk(0, 1, 0), FB)), UB = Cross(FB, RB);
        const V3 pb = Add(g_camLag, Mk(0, 1.0f, -3.0f));
        alignas(16) float mb[16] = {RB.x, RB.y, RB.z, 0, UB.x, UB.y, UB.z, 0, FB.x, FB.y, FB.z, 0, pb.x, pb.y, pb.z, 1};
        fake_transform_set_matrix(g_camRigB, mb, nullptr);
    }
}

// E2E_CAMERA_THREAD=1: the camera written on a thread of its own, like the
// game's main thread in Spider-Man 2 (its frames are presented on another):
// kicked once the frame before has read it, it writes at a random moment -
// before or after the mod's frame (inside Present) of the frame being
// recorded meanwhile, by a race. The mod must keep Mario with his camera
// either way.
bool g_camThreadMode = false;
HANDLE g_camKick = nullptr, g_camDone = nullptr, g_camThreadHandle = nullptr;
std::atomic<double> g_camKickNow{0};
std::atomic<int> g_camWaitUnits{0}; // the next write's wait (0.1 ms)
std::atomic<LONGLONG> g_camWroteAt{0};
LONGLONG g_camKickAt = 0;
double g_camToPresentMs = 4.0; // from the kick to the middle of the Present (learnt)
int g_camBefore = 0, g_camAfter = 0, g_camDuring = 0;
DWORD WINAPI CameraThread(void*) {
    for (;;) {
        WaitForSingleObject(g_camKick, INFINITE);
        if (g_gameThreadQuit) return 0;
        const LONGLONG start = Qpc();
        const double waitMs = g_camWaitUnits.load() / 10.0;
        for (;;) {
            const double left = waitMs - QpcMs(Qpc() - start);
            if (left <= 0) break;
            if (left > 2.0) Sleep(1);
            else SwitchToThread();
        }
        WriteGameCamera(g_camKickNow.load());
        g_camWroteAt.store(Qpc());
        SetEvent(g_camDone);
    }
}

void Frame() {
    ++g_frame;
    ++g_renderFrame;
    g_traceHero = HeroPos(); // where the mod put him at the last Present
    Locomotion();
    {
        // The pedestrian's transform, rewritten as it is every frame through
        // the game's other two setters (nothing for the mod to change).
        alignas(16) float pm[16];
        std::memcpy(pm, g_pedTransform, sizeof(pm));
        if (g_frame & 1) SetMatrixEx(g_pedTransform, pm);
        else SetMatrixEx2(g_pedTransform, pm);
    }
    if (g_now > 6.0 && g_now < 28.5 && g_frame % 40 == 0) {
        fake_transform_unhide(g_heroTransform);
        ++g_unhideCalls;
        if (!HeroHidden()) ++g_unhideShown;
    }
    // cameras: behind the camera target, looking +Z at its tracked joint; sun
    // from above-left-front
    const V3 hero = HeroPos();
    float tp[3], tj[3];
    TargetSlot(10)(&g_heroTarget, tp);
    TargetSlot(18)(&g_heroTarget, tj);
    alignas(16) float tm[16];
    TargetSlot(11)(&g_heroTarget, tm);
    if (std::fabs(tm[13] - tp[1]) > 1e-4f) std::printf("host: camera target matrix and position disagree\n");
    (void)tj;
    if (g_camThreadMode) {
        // The camera written on the game's other thread (CameraThread): the
        // write for this frame, kicked while the frame before was recorded.
        if (!g_camThreadHandle) {
            g_camKick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_camDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_camThreadHandle = CreateThread(nullptr, 0, CameraThread, nullptr, 0, nullptr);
            g_camKickNow.store(g_now);
            g_camWaitUnits.store(0);
            SetEvent(g_camKick);
        }
        if (WaitForSingleObject(g_camDone, 3000) != WAIT_OBJECT_0) std::printf("host: the camera thread didn't write\n");
        // (Against the Present of the frame recorded while it waited: the
        // mod's frame - its plan for the camera - is made inside it.)
        const LONGLONG w = g_camWroteAt.load();
        if (g_lastPresentEnd > 0) {
            if (w < g_lastPresentStart) ++g_camBefore;
            else if (w > g_lastPresentEnd) ++g_camAfter;
            else ++g_camDuring;
        }
    } else {
        WriteGameCamera(g_now);
    }
    const bool rig = RigFrame();
    if (rig) ++g_rigFrames;
    g_traceRig = rig;
    const float* ct = rig ? g_camRigB : CamTransform();
    const V3 cam = Mk(ct[12], ct[13], ct[14]);
    const V3 camFwd = Mk(ct[8], ct[9], ct[10]);
    g_traceCam = cam;
    if (g_camThreadMode) {
        // The next frame's camera: written at a random moment from now on, up
        // to twice as long as it takes to get to the middle of this frame's
        // Present (learnt) - so before the mod's frame about as often as after.
        g_camKickAt = Qpc();
        const double windowMs = std::max(0.5, std::min(40.0, 2.0 * g_camToPresentMs));
        g_camWaitUnits.store(int(std::rand() % std::max(1, int(windowMs * 10.0))));
        g_camKickNow.store(g_now);
        SetEvent(g_camKick);
    }
    PerspectiveView(cam, camFwd, 55.0f * 3.14159265f / 180.0f, kW, kH, g_havePrev ? &g_prevView : nullptr, g_view);
    g_prevView = g_view;
    g_havePrev = true;
    const V3 sun = Norm(Mk(0.45f, -1.0f, 0.55f));
    const V3 focus = Mk(kP0[0], kFeetY, kP0[2] + 6.0f); // fixed light frustum: the cache stays valid
    OrthoView(Sub(focus, Mul(sun, 50.0f)), sun, 14.0f, 100.0f, 1024, g_light);
    const UINT fi = UINT(g_frame % 3);
    const UINT mainTable = ViewTable(g_view, fi * 2);
    const UINT lightTable = ViewTable(g_light, fi * 2 + 1);
    // lighting constants
    float L[24][4] = {};
    L[0][0] = g_view.r[3][0];
    L[0][1] = g_view.r[3][1];
    L[0][2] = g_view.r[3][2];
    L[0][3] = float(kW);
    for (int k = 0; k < 3; ++k) {
        L[1][k] = g_view.r[0][k];
        L[2][k] = g_view.r[1][k];
        L[3][k] = g_view.r[2][k];
    }
    L[1][3] = float(kH);
    L[2][3] = g_view.r[4][0] * g_view.r[0][0] + g_view.r[5][0] * g_view.r[0][1] + g_view.r[6][0] * g_view.r[0][2];
    L[3][3] = g_view.r[4][1] * g_view.r[1][0] + g_view.r[5][1] * g_view.r[1][1] + g_view.r[6][1] * g_view.r[1][2];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) L[4 + i][j] = g_light.r[4 + i][j];
    L[8][0] = g_light.r[3][0];
    L[8][1] = g_light.r[3][1];
    L[8][2] = g_light.r[3][2];
    L[9][0] = sun.x;
    L[9][1] = sun.y;
    L[9][2] = sun.z;
    std::memcpy(g.uploadPtr + 32 * 1024, L, sizeof(L));

    for (int i = 0; i < 4; ++i) {
        g.alloc[i]->Reset();
        g.lists[i]->Reset(g.alloc[i], nullptr);
    }
    const D3D12_VERTEX_BUFFER_VIEW vbv{g.vb->GetGPUVirtualAddress(), 36 * 24, 24};

    // ---- A: sky init + static geometry
    ID3D12GraphicsCommandList* a = g.lists[0];
    {
        D3D12_RESOURCE_BARRIER b[5];
        for (int i = 0; i < 4; ++i)
            b[i] = Tr(g.gb[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        b[4] = Tr(g.depth, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        a->ResourceBarrier(5, b);
    }
    Marker(a, "GBuffer SkyInit");
    a->ClearDepthStencilView(g.Dsv(0), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
    GBufferTargets(a);
    Viewport(a, 0, 0, kW, kH);
    BindGame(a, mainTable, Renderer::kNullSrv);
    a->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a->SetPipelineState(g.psoSky);
    a->DrawInstanced(3, 1, 0, 0);
    a->EndEvent();
    Marker(a, "GBuffer Static");
    a->SetPipelineState(g.psoGame);
    a->OMSetStencilRef(1);
    a->IASetVertexBuffers(0, 1, &vbv);
    for (const Obj& o : g_static) DrawBox(a, BoxConsts(o.c, o.s, o.color, o.material));
    a->EndEvent();
    a->Close();

    // ---- B: dynamic objects; the mod puts Mario at the end of this segment
    ID3D12GraphicsCommandList* b = g.lists[1];
    Marker(b, "GBuffer Dynamic");
    GBufferTargets(b);
    Viewport(b, 0, 0, kW, kH);
    BindGame(b, mainTable, Renderer::kNullSrv);
    b->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    b->SetPipelineState(g.psoGame);
    b->OMSetStencilRef(1);
    b->IASetVertexBuffers(0, 1, &vbv);
    const float t = float(g_frame) * 0.02f;
    const V3 ped = Mk(kP0[0] - 3.0f + 0.5f * std::sin(t), kFeetY + 0.85f, kP0[2] + 1.0f);
    DrawBox(b, BoxConsts(ped, Mk(0.45f, 1.7f, 0.3f), Mk(0.7f, 0.7f, 0.2f), 4)); // a pedestrian
    V3 car;
    if (CarAt(car)) {
        DrawConsts d = BoxConsts(car, Mk(4.2f, 1.4f, 2.0f), Mk(0.7f, 0.1f, 0.1f), 1);
        // E2E_CAR_NO_MOTION=1: the car writes no motion of its own (as if the
        // mod couldn't tell it moves) - the negative control.
        const V3 prev = g_havePrevCar && !std::getenv("E2E_CAR_NO_MOTION") ? g_prevCar : car;
        d.misc[0] = prev.x - car.x;
        d.misc[1] = prev.y - car.y;
        d.misc[2] = prev.z - car.z;
        DrawBox(b, d);
        g_prevCar = car;
        g_havePrevCar = true;
        ++g_carFrames;
        g_carTransform[12] = car.x;
        g_carTransform[13] = car.y - 0.7f;
        g_carTransform[14] = car.z;
    } else {
        g_havePrevCar = false;
        g_carTransform[13] = -1000.0f;
    }
    // (In selfie mode the game hides the hero by a flag of its own - the
    // copy of him stands in - which the mod doesn't touch.)
    if (!HeroHidden() && !g_photoOpen) {
        DrawBox(b, BoxConsts(Add(hero, Mk(0, 0.9f, 0)), Mk(0.5f, 1.8f, 0.3f), Mk(0.8f, 0.05f, 0.05f), 5));
        DrawBox(b, BoxConsts(Add(hero, Mk(0, 0.45f, -0.02f)), Mk(0.52f, 0.9f, 0.32f), Mk(0.05f, 0.1f, 0.6f), 5));
    }
    if (g_doppelSpawned && !(DoppelFlags() & 0x20)) {
        const V3 dp = Mk(g_doppelTransform[12], g_doppelTransform[13], g_doppelTransform[14]);
        DrawBox(b, BoxConsts(Add(dp, Mk(0, 0.9f, 0)), Mk(0.5f, 1.8f, 0.3f), Mk(0.8f, 0.05f, 0.05f), 6));
    }
    GBufferTargets(b); // same targets again: ends the segment (Mario is drawn here)
    // Drawn without setting anything again: the mod must have restored the state.
    DrawBox(b, BoxConsts(Mk(kP0[0] - 2.0f, kFeetY + 2.0f, kP0[2] + 2.5f), Mk(0.15f, 4.0f, 0.15f), Mk(0.2f, 0.2f, 0.2f), 3));
    b->EndEvent();
    // The hero's own mark (0x80) where he is drawn: the mod learns it and
    // gives Mario the same.
    if (!HeroHidden() && !g_photoOpen) {
        Marker(b, "ModelStencilWrite");
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = g.Dsv(0);
        b->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        b->SetPipelineState(g.psoMark);
        b->OMSetStencilRef(0x80);
        DrawBox(b, BoxConsts(Add(hero, Mk(0, 0.9f, 0)), Mk(0.5f, 1.8f, 0.3f), Mk(0.8f, 0.05f, 0.05f), 5));
        DrawBox(b, BoxConsts(Add(hero, Mk(0, 0.45f, -0.02f)), Mk(0.52f, 0.9f, 0.32f), Mk(0.05f, 0.1f, 0.6f), 5));
        b->EndEvent();
    }
    {
        D3D12_RESOURCE_BARRIER x[5];
        for (int i = 0; i < 4; ++i)
            x[i] = Tr(g.gb[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        x[4] = Tr(g.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
        b->ResourceBarrier(5, x);
    }
    b->Close();

    // ---- C: shadows
    ID3D12GraphicsCommandList* c = g.lists[2];
    const D3D12_RESOURCE_STATES srvState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (g_frame % 60 == 1) {
        Marker(c, "Create Shadow Caches");
        D3D12_RESOURCE_BARRIER x = Tr(g.cache, srvState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        c->ResourceBarrier(1, &x);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = g.Dsv(2);
        c->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        c->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        Viewport(c, 0, 0, 1024, 1024);
        BindGame(c, lightTable, Renderer::kNullSrv);
        c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->SetPipelineState(g.psoCaster);
        c->IASetVertexBuffers(0, 1, &vbv);
        for (const Obj& o : g_static)
            if (o.caster) DrawBox(c, BoxConsts(o.c, o.s, o.color, o.material));
        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        c->ResourceBarrier(1, &x);
        c->EndEvent();
    }
    Marker(c, "Create CSM");
    {
        D3D12_RESOURCE_BARRIER x = Tr(g.atlas, srvState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        c->ResourceBarrier(1, &x);
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = g.Dsv(1);
        c->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        if (g_frame == 1) c->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
    BindGame(c, lightTable, Renderer::kCacheSrv);
    c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // region A: cached static depth + dynamic casters
    Viewport(c, 0, 0, 1024, 1024);
    {
        DrawConsts d{};
        d.misc[0] = 0;
        d.misc[1] = 0;
        c->SetPipelineState(g.psoCopy);
        c->SetGraphicsRoot32BitConstants(0, 24, &d, 0);
        c->DrawInstanced(3, 1, 0, 0);
    }
    c->SetPipelineState(g.psoCaster);
    c->IASetVertexBuffers(0, 1, &vbv);
    if (!HeroHidden()) DrawBox(c, BoxConsts(Add(hero, Mk(0, 0.9f, 0)), Mk(0.5f, 1.8f, 0.3f), Mk(1, 1, 1), 5));
    // region B: static casters drawn straight in, every frame, no copy: the
    // mod must leave it alone (it can't know it isn't a cache)
    Viewport(c, 1024, 0, 1024, 1024);
    for (const Obj& o : g_static)
        if (o.caster) DrawBox(c, BoxConsts(o.c, o.s, o.color, o.material));
    c->EndEvent();
    {
        D3D12_RESOURCE_BARRIER x = Tr(g.atlas, D3D12_RESOURCE_STATE_DEPTH_WRITE, srvState);
        c->ResourceBarrier(1, &x);
    }
    c->Close();

    // ---- D: lighting + composite (+ diagnostics)
    ID3D12GraphicsCommandList* d = g.lists[3];
    {
        ID3D12DescriptorHeap* heaps[2] = {g.heap, g.samplers};
        d->SetDescriptorHeaps(2, heaps);
        d->SetComputeRootSignature(g.rsCompute);
        d->SetPipelineState(g.psoLight);
        d->SetComputeRootConstantBufferView(0, g.upload->GetGPUVirtualAddress() + 32 * 1024);
        d->SetComputeRootDescriptorTable(1, g.Gpu(Renderer::kLightSrv));
        d->SetComputeRootDescriptorTable(2, g.Gpu(Renderer::kLightUav));
        d->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        {
            // Mario on screen, every frame: his pixels in G-buffer 1.
            d->CopyBufferRegion(g.sums, 0, g.upload, kZeroOffset, 16);
            D3D12_RESOURCE_BARRIER u = Tr(g.sums, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d->ResourceBarrier(1, &u);
            d->SetPipelineState(g.psoCentroid);
            d->SetComputeRootDescriptorTable(1, g.Gpu(Renderer::kCentroidSrv));
            d->SetComputeRootDescriptorTable(2, g.Gpu(Renderer::kSumsUav));
            d->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
            u = Tr(g.sums, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            d->ResourceBarrier(1, &u);
            d->CopyBufferRegion(g.readback, kSumsOffset, g.sums, 0, 16);
            u = Tr(g.sums, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            d->ResourceBarrier(1, &u);
            d->SetPipelineState(g.psoLight); // (as before, for what follows)
        }
        D3D12_RESOURCE_BARRIER x = Tr(g.hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        d->ResourceBarrier(1, &x);
        const UINT bi = g.sc->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER y = Tr(g.back[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d->ResourceBarrier(1, &y);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.Rtv(8 + bi);
        d->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        Viewport(d, 0, 0, kW, kH);
        BindGame(d, mainTable, Renderer::kHdrSrv);
        d->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d->SetPipelineState(g.psoComposite);
        d->DrawInstanced(3, 1, 0, 0);
        std::swap(y.Transition.StateBefore, y.Transition.StateAfter);
        d->ResourceBarrier(1, &y);
        std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        d->ResourceBarrier(1, &x);
        if (g_diagRequest) {
            // G-buffer 1 (material ids) and both shadow textures to the CPU.
            D3D12_RESOURCE_BARRIER z[3] = {
                Tr(g.gb[3], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Tr(g.atlas, srvState, D3D12_RESOURCE_STATE_COPY_SOURCE), Tr(g.cache, srvState, D3D12_RESOURCE_STATE_COPY_SOURCE)};
            d->ResourceBarrier(3, z);
            auto copy = [&](ID3D12Resource* src, DXGI_FORMAT fmt, UINT w, UINT h, UINT bpp, UINT64 off) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = g.readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = off;
                dst.PlacedFootprint.Footprint = {fmt, w, h, 1, (w * bpp + 255) & ~255u};
                D3D12_TEXTURE_COPY_LOCATION s{};
                s.pResource = src;
                s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                d->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
            };
            copy(g.gb[3], DXGI_FORMAT_R32G32_UINT, kW, kH, 8, 0);
            copy(g.atlas, DXGI_FORMAT_R32_TYPELESS, 2048, 1024, 4, 5ull * 1024 * 1024);
            copy(g.cache, DXGI_FORMAT_R32_TYPELESS, 1024, 1024, 4, 14ull * 1024 * 1024);
            for (auto& q : z) std::swap(q.Transition.StateBefore, q.Transition.StateAfter);
            d->ResourceBarrier(3, z);
            // The stencil: the marks on each pixel, read by a compute shader
            // (Wine's vkd3d can't copy a depth-stencil plane to a buffer).
            {
                D3D12_RESOURCE_BARRIER t = Tr(g.depth, D3D12_RESOURCE_STATE_DEPTH_READ,
                                              D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                d->ResourceBarrier(1, &t);
                d->SetPipelineState(g.psoStencil);
                d->SetComputeRootSignature(g.rsCompute);
                d->SetComputeRootConstantBufferView(0, g.upload->GetGPUVirtualAddress() + 32 * 1024);
                d->SetComputeRootDescriptorTable(1, g.Gpu(Renderer::kStencilSrv));
                d->SetComputeRootDescriptorTable(2, g.Gpu(Renderer::kMarksUav));
                d->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
                std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
                D3D12_RESOURCE_BARRIER u[2] = {
                    t, Tr(g.marks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
                d->ResourceBarrier(2, u);
                copy(g.marks, DXGI_FORMAT_R32_UINT, kW, kH, 4, kStencilOffset);
                std::swap(u[1].Transition.StateBefore, u[1].Transition.StateAfter);
                d->ResourceBarrier(1, &u[1]);
                g_stencilPitch = (kW * 4 + 255) & ~255u;
            }
        }
    }
    d->Close();
    ID3D12CommandList* lists[4] = {a, b, c, d};
    g.queue->ExecuteCommandLists(4, lists);
}

void Diagnostics() {
    // Called after the GPU finished a frame recorded with g_diagRequest set.
    uint8_t* p = nullptr;
    D3D12_RANGE all{0, 24 * 1024 * 1024};
    if (FAILED(g.readback->Map(0, &all, reinterpret_cast<void**>(&p)))) return;
    int mario = 0, lamp = 0, ped = 0, hero = 0, doppel = 0;
    int marioMarks[256] = {}, heroMarks[256] = {}; // their pixels' stencil values
    const UINT pitch = (kW * 8 + 255) & ~255u;
    const uint8_t* stencil = g_stencilPitch ? p + kStencilOffset : nullptr;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            uint32_t g1[2];
            std::memcpy(g1, p + size_t(y) * pitch + size_t(x) * 8, 8);
            const uint32_t id = g1[1] >> 16;
            const uint8_t mark = stencil ? stencil[size_t(y) * g_stencilPitch + size_t(x) * 4] : 0;
            if (id == 0xFFFF || id == 0) { // (0: the metal cap's reflective material)
                ++mario;
                ++marioMarks[mark];
            } else if (id == 3) {
                ++lamp;
            } else if (id == 4) {
                ++ped;
            } else if (id == 5) {
                ++hero;
                ++heroMarks[mark];
            } else if (id == 6) {
                ++doppel;
            }
        }
    // The most common stencil value on someone's pixels, and its share.
    auto common = [](const int* marks, int total) {
        char buf[32];
        if (total <= 0) return std::string("- (0%)");
        int best = 0;
        for (int v = 1; v < 256; ++v)
            if (marks[v] > marks[best]) best = v;
        std::snprintf(buf, sizeof(buf), "0x%02x (%d%%)", best, marks[best] * 100 / total);
        return std::string(buf);
    };
    const std::string marioMark = common(marioMarks, mario), heroMark = common(heroMarks, hero);
    {
        // (all the values seen, for when a check fails)
        std::string all;
        for (int v = 0; v < 256; ++v)
            if (marioMarks[v] + heroMarks[v] > 0) {
                char one[48];
                std::snprintf(one, sizeof(one), " 0x%02x: %d/%d", v, marioMarks[v], heroMarks[v]);
                all += one;
            }
        std::printf("host: DIAG %d stencil values (Mario's pixels/Spider-Man's):%s\n", g_diagNumber, all.c_str());
    }
    const float* atlas = reinterpret_cast<const float*>(p + 5ull * 1024 * 1024);
    const float* cache = reinterpret_cast<const float*>(p + 14ull * 1024 * 1024);
    int shadowA = 0, shadowB = 0;
    for (int y = 0; y < 1024; ++y)
        for (int x = 0; x < 1024; ++x) {
            const float cv = cache[size_t(y) * 1024 + x];
            if (atlas[size_t(y) * 2048 + x] < cv - 1e-4f) ++shadowA;
            if (std::fabs(atlas[size_t(y) * 2048 + 1024 + x] - cv) > 1e-3f) ++shadowB;
        }
    g.readback->Unmap(0, nullptr);
    std::printf("host: DIAG %d frame %llu: G-buffer pixels mario %d hero %d pedestrian %d lamp %d | shadow texels "
                "region A (copied) %d, region B (not copied) %d | hero hidden %d | photo mode's Spider-Man %d | "
                "stencil mario %s hero %s\n",
                g_diagNumber, static_cast<unsigned long long>(g_frame), mario, hero, ped, lamp, shadowA, shadowB,
                int(HeroHidden()), doppel, marioMark.c_str(), heroMark.c_str());
    std::fflush(stdout);
}

void Key(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

void Report(const char* what) {
    std::printf("host: REPORT %s hero (%.2f %.2f %.2f) hidden %d cam (%.2f %.2f %.2f)\n", what, g_heroTransform[12],
                g_heroTransform[13], g_heroTransform[14], int(HeroHidden()), g_traceCam.x, g_traceCam.y, g_traceCam.z);
    std::fflush(stdout);
}

void Shoot(int n) {
    char name[48];
    std::snprintf(name, sizeof(name), "shoot_%d", n);
    FILE* f = std::fopen(name, "w");
    if (f) std::fclose(f);
}

struct Step {
    double at;
    // 0 key down, 1 key up, 2 report, 3 screenshot, 4 diagnostics, 5 trace on, 6 trace off,
    // 7 a pedestrian asks for a picture, 8 ModSettings: open the pause menu, 9 ModSettings: set
    // "LABEL=value", 10 ModSettings: close (save), 11 the game pauses (its physics stops),
    // 12 ... and runs again, 13 left mouse button down, 14 ... up, 15 a wall only the camera's query sees
    // (between Mario at the wall and his camera), 16 ... gone, 17 the game's photo mode opens (in selfie
    // mode: a copy of Spider-Man where the hero is), 18 ... closes, 19 the game hides Spider-Man (a gadget),
    // 20 the game's camera switches to a new transform
    int what;
    WORD vk;
    const char* label;
    bool done;
};

// While tracing, every frame prints where the hero (= Mario) is, so the e2e
// log shows the height profile of the walk over the stairs and the platform.
bool g_trace = false;

Step g_steps[] = {
    {3.0, 4, 0, "before Mario", false},
    {3.5, 3, 0, "1", false},
    {4.6, 19, 0, "", false},                              // the game hides Spider-Man itself (Mario must not keep him hidden)
    {5.0, 0, 'M', "", false},  {5.15, 1, 'M', "", false}, // Mario Mode on
    // (Late enough for the spawn spin to be over even when Wine takes 1.7 s to
    // set up the mod's first draw: an earlier punch went nowhere.)
    {8.0, 13, 0, "", false},   {8.15, 14, 0, "", false},  // punch the thug in front of Mario
    {9.0, 2, 0, "spawned", false},
    {9.5, 4, 0, "mario at spawn", false},
    {9.6, 7, 0, "", false},                               // Mario stands still: he poses for the pedestrian
    {10.0, 3, 0, "2", false},
    {10.4, 0, 'E', "", false}, {10.55, 1, 'E', "", false}, // the game's interact key: Mario answers the pedestrian
    {10.9, 5, 0, "", false},   {16.6, 6, 0, "", false},
    {11.0, 0, 'W', "", false}, {14.4, 1, 'W', "", false}, // up the stairs
    {15.0, 0, 'D', "", false}, {16.2, 1, 'D', "", false}, // strafe right along the wall
    {16.0, 2, 0, "after walking", false},
    {16.5, 4, 0, "mario on the stairs", false},
    {17.0, 3, 0, "3", false},
    {18.0, 0, 'W', "", false}, {24.0, 1, 'W', "", false}, // into the wall
    {19.0, 11, 0, "", false},                             // the game pauses for 1.5 s (W still held)
    {19.6, 2, 0, "paused a", false},
    {20.4, 2, 0, "paused b", false},
    {20.5, 12, 0, "", false},
    {24.3, 5, 0, "", false},   {25.9, 6, 0, "", false},
    {24.5, 0, VK_SPACE, "", false}, {24.9, 1, VK_SPACE, "", false}, // jump: the camera must follow him up
    {26.0, 2, 0, "at the wall", false},
    {26.2, 13, 0, "", false},  {26.35, 14, 0, "", false}, // a punch with nobody in reach
    {26.5, 3, 0, "4", false},
    {27.0, 0, 'P', "", false}, {27.2, 1, 'P', "", false}, // pose key: Mario waves at the camera
    {27.3, 15, 0, "", false},                             // a wall between Mario and his camera ...
    {27.9, 3, 0, "5", false},
    {28.5, 2, 0, "camera blocked", false},
    {28.8, 16, 0, "", false},                             // ... gone again
    // The game's camera becomes another transform (as after a load) while
    // Mario jumps at the wall (its view rises with him): the mod must notice
    // the old one is dead, find the new one and place it again.
    {28.9, 5, 0, "", false},   {36.6, 6, 0, "", false},
    {29.0, 20, 0, "", false},
    {30.8, 0, VK_SPACE, "", false}, {31.0, 1, VK_SPACE, "", false},
    {31.8, 0, VK_SPACE, "", false}, {32.0, 1, VK_SPACE, "", false},
    {32.8, 0, VK_SPACE, "", false}, {33.0, 1, VK_SPACE, "", false},
    // Mario's own menu: F8, volume up one step, down to MOON JUMP, switch it on, Esc
    // (taps last 0.3 s, 0.5 s apart: this mock runs at 10-15 frames a second
    // and hitches now and then, and the menu reads the keys once a frame)
    {37.0, 0, VK_F8, "", false}, {37.3, 1, VK_F8, "", false},
    {37.7, 3, 0, "6", false},
    {38.0, 0, VK_RIGHT, "", false}, {38.3, 1, VK_RIGHT, "", false},
    // down past ATTACK STRENGTH, SHINE, POSE FOR PHOTOS, INFINITE HEALTH, WING CAP, METAL CAP,
    // VANISH CAP to MOON JUMP (8 taps; the camera's settings come after the cheats)
    {38.5, 0, VK_DOWN, "", false}, {38.8, 1, VK_DOWN, "", false},
    {39.0, 0, VK_DOWN, "", false}, {39.3, 1, VK_DOWN, "", false},
    {39.5, 0, VK_DOWN, "", false}, {39.8, 1, VK_DOWN, "", false},
    {40.0, 0, VK_DOWN, "", false}, {40.3, 1, VK_DOWN, "", false},
    {40.5, 0, VK_DOWN, "", false}, {40.8, 1, VK_DOWN, "", false},
    {41.0, 0, VK_DOWN, "", false}, {41.3, 1, VK_DOWN, "", false},
    {41.5, 0, VK_DOWN, "", false}, {41.8, 1, VK_DOWN, "", false},
    {42.0, 0, VK_DOWN, "", false}, {42.3, 1, VK_DOWN, "", false},
    {43.0, 0, VK_RETURN, "", false}, {43.3, 1, VK_RETURN, "", false},
    {43.6, 0, VK_ESCAPE, "", false}, {43.9, 1, VK_ESCAPE, "", false},
    // ModSettings' pause menu: it shows the change, and switches on INFINITE HEALTH
    {44.2, 8, 0, "", false},
    {44.4, 9, 0, "INFINITE HEALTH=1", false},
    {44.6, 10, 0, "", false},
    {44.9, 8, 0, "", false},
    // The game's photo mode: Mario holds still while W moves the photo camera,
    // poses on the pose key and holds it; the selfie's Spider-Man is hidden.
    {45.2, 17, 0, "", false},
    {45.5, 2, 0, "photo open", false},
    {45.7, 0, 'W', "", false}, {46.5, 1, 'W', "", false},
    {46.7, 2, 0, "photo walked", false},
    {46.9, 0, 'P', "", false}, {47.2, 1, 'P', "", false},
    {48.7, 4, 0, "photo mode", false},
    {48.9, 3, 0, "7", false},
    // ... Mario Mode off in the selfie: its Spider-Man is back (the game hides
    // the hero itself there), then on again: hidden again.
    {49.2, 0, 'M', "", false}, {49.4, 1, 'M', "", false},
    {50.2, 4, 0, "photo, Mario off", false},
    {50.5, 0, 'M', "", false}, {50.7, 1, 'M', "", false},
    {52.2, 4, 0, "photo, Mario back", false},
    {52.7, 18, 0, "", false},
    // moon jump: hold jump for 1.5 s
    {53.7, 5, 0, "", false},
    {53.8, 0, VK_SPACE, "", false}, {55.3, 1, VK_SPACE, "", false},
    {57.2, 6, 0, "", false},
    {57.4, 2, 0, "after the moon jump", false},
    {57.7, 0, 'M', "", false}, {57.9, 1, 'M', "", false}, // back to Spider-Man
    {60.2, 2, 0, "mario off", false},
    {60.7, 4, 0, "after Mario", false},
};

void RunScript(double t, HWND hwnd) {
    for (Step& s : g_steps) {
        if (s.done || t < s.at) continue;
        s.done = true;
        SetForegroundWindow(hwnd);
        switch (s.what) {
        case 0: Key(s.vk, true); break;
        case 1: Key(s.vk, false); break;
        case 2: Report(s.label); break;
        case 3: Shoot(std::atoi(s.label)); break;
        case 4:
            ++g_diagNumber;
            g_diagRequest = 1;
            std::printf("host: diagnostics %d (%s)\n", g_diagNumber, s.label);
            break;
        case 5: g_trace = true; break;
        case 6: g_trace = false; break;
        case 7: AskForPicture(); break;
        case 8:
            if (g_ms.open) {
                static char buf[4096];
                const int pages = g_ms.open(buf, sizeof(buf));
                std::printf("host: MODSETTINGS open: %d page(s): %s\n", pages, buf);
            } else {
                std::printf("host: MODSETTINGS not loaded\n");
            }
            break;
        case 9:
            if (g_ms.set) {
                std::string a = s.label;
                const size_t eq = a.find('=');
                const std::string label = a.substr(0, eq);
                const float v = float(std::atof(a.c_str() + eq + 1));
                std::printf("host: MODSETTINGS set %s = %g: %s\n", label.c_str(), double(v),
                            g_ms.set(label.c_str(), v) ? "ok" : "no such item");
            }
            break;
        case 10:
            if (g_ms.save) std::printf("host: MODSETTINGS save: %d callback(s)\n", g_ms.save());
            break;
        case 11:
            g_physPaused = 1;
            std::printf("host: the game pauses\n");
            break;
        case 12:
            g_physPaused = 0;
            std::printf("host: the game runs again\n");
            break;
        case 17: {
            // what PhotomodeSystem::OnActivate and the selfie swap do: the flag,
            // and the doppelganger spawned on the hero's transform
            std::memcpy(g_doppelTransform, g_heroTransform, 16 * sizeof(float));
            DoppelFlags() &= ~0x20u;
            *reinterpret_cast<uint16_t*>(g_actorArray + 7 * 0xC0 + 0x08) = 13;
            *reinterpret_cast<uint32_t*>(g_photoMode + 0x884) = kDoppelHandle;
            g_photoMode[0xCE9] = 1;
            g_doppelSpawned = true;
            g_photoOpen = true;
            std::printf("host: photo mode opens (selfie mode: a copy of Spider-Man where the hero is)\n");
            break;
        }
        case 18:
            g_photoMode[0xCE9] = 0;
            *reinterpret_cast<uint32_t*>(g_photoMode + 0x884) = 0;
            *reinterpret_cast<uint16_t*>(g_actorArray + 7 * 0xC0 + 0x08) = 0;
            g_doppelSpawned = false;
            g_photoOpen = false;
            std::printf("host: photo mode closes\n");
            break;
        case 19:
            fake_transform_hide(g_heroTransform);
            std::printf("host: the game hides Spider-Man (a gadget) - hidden %d\n", int(HeroHidden()));
            break;
        case 20:
            std::memcpy(g_camTransform2, g_camTransform, sizeof(g_camTransform2));
            g_camSwapped = true;
            std::printf("host: the game's camera is a new transform now (the old one is never written again)\n");
            break;
        case 15:
            g_camBlockX = g_heroTransform[12];
            g_camBlock = true;
            std::printf("host: a wall only the camera's query sees, between Mario and his camera\n");
            break;
        case 16:
            g_camBlock = false;
            std::printf("host: the camera's wall is gone\n");
            break;
        case 13:
        case 14: {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = s.what == 13 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            SendInput(1, &in, sizeof(in));
            break;
        }
        }
        std::fflush(stdout);
    }
    if (g_trace) {
        // hero = where the mod put Spider-Man (= Mario) before the game's controller ran
        std::printf("host: TRACE t %.2f hero (%.2f %.2f %.2f) cam (%.2f %.2f %.2f) mario (%.2f %.2f %d) mv %.1f rig %d\n",
                    t, g_traceHero.x, g_traceHero.y, g_traceHero.z, g_traceCam.x, g_traceCam.y, g_traceCam.z, g_marioX,
                    g_marioY, g_marioPixels, g_marioMv, int(g_traceRig));
        std::fflush(stdout);
    }
}

// Key-downs the game's window got for the keys Mario Mode keeps for itself
// (F8 always; Esc, Enter and the arrows while its menu is open; P while Mario
// is on). The script presses them only then, so the game must see none.
int g_menuKeysSeen = 0;
int g_interactSeen = 0;   // the game's interact key (E): it must still get it with Mario on
int g_marioKeysSeen = 0;  // Mario's own keys while he is on: it must not
int g_photoKeysSeen = 0;  // ... except in photo mode (they move its camera)
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        const WPARAM k = w;
        if (k == 'E') ++g_interactSeen;
        if ((k == 'W' || k == 'A' || k == 'S' || k == 'D' || k == VK_SPACE) && g_now > 5.5 && g_now < 57.6) {
            if (g_photoOpen) {
                ++g_photoKeysSeen;
            } else {
                ++g_marioKeysSeen;
                std::printf("host: the game got Mario's key 0x%02X\n", unsigned(k));
            }
        }
        if (k == VK_F8 || k == VK_ESCAPE || k == VK_RETURN || k == VK_UP || k == VK_DOWN || k == VK_LEFT ||
            k == VK_RIGHT || k == 'P') {
            ++g_menuKeysSeen;
            std::printf("host: the game got key 0x%02X\n", unsigned(k));
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void WriteDigests(const Blob& vsShadow, const Blob& psCaster, const Blob& psCopy) {
    CreateDirectoryA("sm2mario", nullptr);
    FILE* f = std::fopen("sm2mario/bindings.user.ini", "w");
    if (!f) return;
    std::fprintf(f, "; written by deferred_host: this mock's shadow shaders\n[Shaders]\nCasterVS = %s\nCasterPS = %s\n"
                    "CacheCopyPS = %s\n",
                 vsShadow.Digest().c_str(), psCaster.Digest().c_str(), psCopy.Digest().c_str());
    std::fclose(f);
}

} // namespace

// ---------------------------------------------------------------- the game's physics, called by mock_game.S
extern "C" {

// CastRayImmediate on the query system (after the thunk's add rcx, 0x90).
void* mock_ray_cast(uint8_t* qs, const uint8_t* req, const float* from, const float* to, const char* tag) {
    if (GetCurrentThreadId() == g_gameThreadId) ++g_physRays;
    else ++g_physRaysElsewhere;
    if (qs != g_physicsMem + 0x90 || !req || Field<int32_t>(req, 0) != 1) {
        ++g_physBad;
        return nullptr;
    }
    if (!tag || std::strncmp(tag, "sm2mario", 8) != 0) ++g_physUntagged;
    int32_t& counter = *reinterpret_cast<int32_t*>(qs + 0x1E880);
    const int slot = counter++;
    if (slot < 0 || slot >= 256) {
        ++g_physPoolFull;
        return nullptr;
    }
    const uint32_t type = Field<uint32_t>(req, 8), ignored = Field<uint32_t>(req, 0x10);
    const int maxHits = std::min<int>(Field<uint16_t>(req, 4), 8);
    MockResult& r = g_results[slot];
    r.records = g_resultHits[slot];
    r.count = 0;
    if (type == 17) { // water: there is none here
        ++g_waterRays;
        return &r;
    }
    std::vector<PhysBox> boxes;
    PhysScene(boxes);
    const bool cameraQuery = type == 11;
    if (cameraQuery) {
        ++g_camRays;
        // (13 m wide across Mario, y 3..14, z -27.2..-26.9: behind him standing at the wall, in front of his camera)
        if (g_camBlock) AddBox(boxes, Mk(g_camBlockX, 8.5f, -27.05f), Mk(13.0f, 11.0f, 0.3f), 4, 0);
    }
    struct H {
        float t, n[3];
        int16_t m;
        uint32_t x;
    } hits[24];
    int nh = 0;
    for (const PhysBox& b : boxes) {
        float t, n[3];
        if (!HitBox(from, to, b, t, n)) continue;
        if (b.xform && b.xform == ignored) {
            if (b.xform == 1) ++g_heroLeftOut;
            continue;
        }
        if (b.xform == 1) ++g_heroHit;
        if (nh < 24) hits[nh++] = {t, {n[0], n[1], n[2]}, b.material, b.xform};
    }
    std::sort(hits, hits + nh, [](const H& a, const H& b) { return a.t < b.t; });
    const int count = std::min(nh, maxHits);
    for (int i = 0; i < count; ++i) {
        const H& h = hits[count - 1 - i]; // the game collects hits in no particular order
        MockHit& rec = r.records[i];
        std::memset(&rec, 0, sizeof(rec));
        rec.t = h.t;
        rec.xform = h.x ? (h.x | 0x01000000u) : 0;
        for (int k = 0; k < 3; ++k) {
            rec.n[k] = h.n[k];
            rec.p[k] = from[k] + (to[k] - from[k]) * h.t;
        }
        rec.material = h.m;
    }
    r.count = uint16_t(count);
    if (cameraQuery && count > 0) ++g_camRaysHit;
    return &r;
}

// The physics step (inside the frame the mod hooks): the frame's query results
// are free again, minus what the game itself casts; damage from the last frame
// is dealt.
void mock_physics_step(void* physics) {
    if (physics != g_physicsMem) ++g_physBad;
    const int n = ++g_physSteps;
    int32_t& used = *reinterpret_cast<int32_t*>(g_physicsMem + 0x90 + 0x1E880);
    used = n % 8 == 0 ? 190 : 3; // now and then the game uses most of the pool itself
    if (used > 100) ++g_physBusy;
    ProcessDamage();
}

void* mock_transform_lookup(void*, uint32_t index) { return index >= 1 && index < 5 ? g_xforms[index] : nullptr; }

uint8_t* mock_damage_alloc(void* system) {
    if (system != g_damageSystem || g_damagePending >= 16) {
        ++g_damageBad;
        return nullptr;
    }
    DamageEntry& e = g_damage[g_damagePending++];
    std::memset(e.mem, 0, sizeof(e.mem));
    return e.mem;
}

uint8_t* mock_damage_fill(uint8_t* entry, const float* centre, float radius, const uint8_t* request) {
    DamageEntry* e = reinterpret_cast<DamageEntry*>(entry); // mem is the first member
    for (int k = 0; k < 3; ++k) e->c[k] = centre[k];
    e->r = radius;
    e->query = Field<uint32_t>(request, 8);
    e->live = Field<int32_t>(request, 0) == 1;
    return entry + 0x68;
}

uint32_t MockCrc(const char* s) {
    // The game's string hash (the mod's Sm2StringCrc32).
    uint32_t crc = 0xEDB88320u;
    for (; *s; ++s) {
        uint32_t c = (static_cast<unsigned char>(*s) ^ crc) & 0xFF;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        crc = (crc >> 8) ^ c;
    }
    return crc;
}
std::atomic<int> g_registryCalls{0}, g_registryFound{0};
bool g_noRegistry = false;

__attribute__((aligned(16))) uintptr_t mock_get_component_info(uintptr_t registry, uint32_t crc) {
    ++g_registryCalls;
    if (g_noRegistry || registry != reinterpret_cast<uintptr_t>(g_componentRegistry)) return 0;
    const MockComponentInfo* known[] = {&g_healthInfo, &g_pedInfo, &g_vehicleInfo, &g_guardHealthInfo};
    for (const MockComponentInfo* i : known)
        if (MockCrc(i->name) == crc) {
            ++g_registryFound;
            return reinterpret_cast<uintptr_t>(i);
        }
    return 0;
}

} // extern "C"

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 30;
    if (const char* sc = std::getenv("E2E_SCREEN")) g_rhScreen = std::strcmp(sc, "lh") != 0;
    std::printf("host: screen %s-handed\n", g_rhScreen ? "right" : "left");
    bool script = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--script") == 0) script = true;
    SetupMock();
    g_noRegistry = std::getenv("E2E_NO_REGISTRY") != nullptr;
    if (g_noRegistry) std::printf("host: the component registry answers nothing (lookups by name)\n");
    if (const char* tau = std::getenv("E2E_CAMERA_TAU")) g_camTau = std::max(0.001, std::atof(tau));
    std::printf("host: the follow camera trails its target by a %.2f s spring\n", g_camTau);
    g_camThreadMode = std::getenv("E2E_CAMERA_THREAD") != nullptr;
    if (g_camThreadMode) std::printf("host: the camera is written on a thread of its own, racing the Present\n");
    g_camFar = std::getenv("E2E_CAMERA_FAR") != nullptr;
    if (g_camFar) std::printf("host: the game's own camera falls 50 m behind Mario after his jumps at the wall\n");
    g_camRigs = std::getenv("E2E_CAMERA_RIGS") != nullptr;
    if (g_camRigs)
        std::printf("host: the game has another camera, rendered from in every other frame from 21.0 to 27.5 s\n");
    std::srand(12345);
    BuildScene();
    Blob vsGame, psGame, vsFull, psSky, vsShadow, psCaster, psCopy, psComposite, csLight;
    if (!CompileAll(vsGame, psGame, vsFull, psSky, vsShadow, psCaster, psCopy, psComposite, csLight)) return 2;
    // E2E_NO_DIGESTS=1: as if a game update changed the shadow shaders.
    if (!std::getenv("E2E_NO_DIGESTS")) WriteDigests(vsShadow, psCaster, psCopy);

    // ModSettings (the mock) is another script, loaded before Mario Mode's script_enable runs.
    if (HMODULE ms = LoadLibraryA("scripts/ModSettings.dll")) {
        g_ms.open = reinterpret_cast<int (*)(char*, int)>(reinterpret_cast<void*>(GetProcAddress(ms, "MockModSettings_Open")));
        g_ms.set = reinterpret_cast<int (*)(const char*, float)>(reinterpret_cast<void*>(GetProcAddress(ms, "MockModSettings_Set")));
        g_ms.save = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(ms, "MockModSettings_Save")));
        g_ms.calls = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(ms, "MockModSettings_Calls")));
        std::printf("host: ModSettings mock loaded\n");
    }
    // Like the game, the mod is loaded before the device exists.
    SetDllDirectoryA("scripts");
    HMODULE mod = LoadLibraryA("sm2mario.dll");
    if (!mod) {
        std::printf("host: LoadLibrary failed (%lu)\n", GetLastError());
        return 1;
    }
    auto enable = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(mod, "script_enable")));
    if (!enable) return 1;
    enable();
    std::printf("host: script_enable returned\n");
    // The mod installs its hooks on its init thread (slow the first time Wine
    // runs in a new prefix): like the game, create the device after that.
    Sleep(1500);
    for (int i = 0; i < 300; ++i) {
        std::string log;
        if (FILE* f = std::fopen("sm2mario/sm2mario.log", "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) log.append(buf, n);
            std::fclose(f);
        }
        if (log.find("init complete") != std::string::npos) break;
        Sleep(100);
    }
    // The game's update thread (its physics frame). E2E_NO_GAME_THREAD=1: the
    // hooked frame never runs (as if a game update moved it).
    if (!std::getenv("E2E_NO_GAME_THREAD")) CreateThread(nullptr, 0, GameThread, nullptr, 0, nullptr);
    else std::printf("host: no game thread (the physics frame never runs)\n");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"deferred_game";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Deferred Game", WS_POPUP | WS_VISIBLE, 0, 0, kW, kH, nullptr,
                                nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    ShowCursor(FALSE);
    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) {
        std::printf("host: no D3D12 device\n");
        return 2;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = kW;
    sd.Height = kH;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &sc1))) return 2;
    sc1->QueryInterface(IID_PPV_ARGS(&g.sc));
    if (!CreateRootSignatures() || !CreateResources() ||
        !CreatePipelines(vsGame, psGame, vsFull, psSky, vsShadow, psCaster, psCopy, psComposite, csLight)) {
        std::printf("host: renderer setup failed\n");
        return 2;
    }
    for (UINT i = 0; i < 3; ++i) {
        g.sc->GetBuffer(i, IID_PPV_ARGS(&g.back[i]));
        g_dev->CreateRenderTargetView(g.back[i], nullptr, g.Rtv(8 + i));
    }
    std::printf("host: deferred renderer ready (%dx%d)\n", kW, kH);
    std::fflush(stdout);

    const DWORD start = GetTickCount();
    int frames = 0;
    double lastT = 0;
    int slowLogged = 0;
    while (GetTickCount() - start < DWORD(seconds) * 1000) {
        const double t = (GetTickCount() - start) / 1000.0;
        // (A slow frame can swallow a key press and its release whole.)
        if (frames > 0 && t - lastT > 0.12 && slowLogged < 40) {
            ++slowLogged;
            std::printf("host: slow frame %d: %.0f ms (t %.2f)\n", frames, (t - lastT) * 1000.0, t);
        }
        lastT = t;
        if (script) RunScript(t, hwnd);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const int diag = g_diagRequest;
        g_now = t;
        Frame();
        g_lastPresentStart = Qpc();
        g.sc->Present(0, 0);
        g_lastPresentEnd = Qpc();
        if (g_camThreadMode && g_camKickAt) {
            const double mid = QpcMs((g_lastPresentStart + g_lastPresentEnd) / 2 - g_camKickAt);
            g_camToPresentMs += (std::max(0.0, std::min(50.0, mid)) - g_camToPresentMs) * 0.1;
        }
        g.queue->Signal(g.fence, ++g.fv);
        g.fence->SetEventOnCompletion(g.fv, g.ev);
        WaitForSingleObject(g.ev, 5000);
        {
            // Where Mario was on screen in that frame.
            uint8_t* p = nullptr;
            D3D12_RANGE r{SIZE_T(kSumsOffset), SIZE_T(kSumsOffset + 16)};
            if (SUCCEEDED(g.readback->Map(0, &r, reinterpret_cast<void**>(&p)))) {
                uint32_t s[4];
                std::memcpy(s, p + kSumsOffset, sizeof(s));
                D3D12_RANGE none{0, 0};
                g.readback->Unmap(0, &none);
                g_marioPixels = int(s[0]);
                g_marioX = s[0] ? double(s[1]) / double(s[0]) : -1.0;
                g_marioY = s[0] ? double(s[2]) / double(s[0]) : -1.0;
                g_marioMv = s[0] ? double(s[3]) / 16.0 / double(s[0]) : 0.0;
            }
        }
        if (diag) {
            g_diagRequest = 0;
            Diagnostics();
        }
        ++frames;
    }
    std::printf("host: presented %d frames in %d s\n", frames, seconds);
    if (g_camThreadMode) {
        std::printf("host: camera thread: %d write(s) before the Present of the frame recorded meanwhile, %d after it, "
                    "%d during it (%.1f ms from a kick to the middle of a Present)\n",
                    g_camBefore, g_camAfter, g_camDuring, g_camToPresentMs);
        g_gameThreadQuit = true;
        SetEvent(g_camKick);
    }
    if (g_camRigs) std::printf("host: %d frame(s) rendered from the game's other camera\n", g_rigFrames);
    std::printf("host: the character controller put the hero back on the ground %d time(s)\n", g_snaps);
    std::printf("host: a car drove by in %d frame(s)\n", g_carFrames);
    std::printf("host: the game got %d key-down(s) of Mario Mode's menu/pose keys\n", g_menuKeysSeen);
    std::printf("host: the game got the interact key %d time(s), Mario's own keys %d time(s) while he was on\n",
                g_interactSeen, g_marioKeysSeen);
    std::printf("host: in photo mode the game got %d of Mario's keys (they move its camera there)\n", g_photoKeysSeen);
    g_gameThreadQuit = true;
    Sleep(50);
    std::printf("host: physics: %d frames, %d steps (%d with the query pool busy), %d ray casts on the game thread, "
                "%d elsewhere, %d water, %d bad, %d untagged, %d with the pool full; the hero left out of %d, hit by "
                "%d; requests still held %d\n",
                g_physFrames.load(), g_physSteps.load(), g_physBusy.load(), g_physRays.load(), g_physRaysElsewhere.load(),
                g_waterRays.load(), g_physBad.load(), g_physUntagged.load(), g_physPoolFull.load(), g_heroLeftOut.load(),
                g_heroHit.load(), g_collLive);
    std::printf("host: camera rays: %d (%d hit something)\n", g_camRays.load(), g_camRaysHit.load());
    std::printf("host: SetMatrixEx called %d time(s), %d left the wrong thing; SetMatrixEx2 called %d time(s), its sixth "
                "argument lost %d time(s)\n",
                g_exCalls, g_exWrong, g_ex2Calls, g_ex2Wrong);
    std::printf("host: the game called Unhide on Spider-Man %d time(s) while Mario was on; he showed %d time(s)\n",
                g_unhideCalls, g_unhideShown);
    std::printf("host: component registry: %d lookup(s), %d answered\n", g_registryCalls.load(),
                g_registryFound.load());
    std::printf("host: damage: %d request(s), %d bad, the thug hit %d time(s), his health %.0f\n", g_damageSeen.load(),
                g_damageBad.load(), g_thugHits, double(*reinterpret_cast<float*>(g_enemyHealth + 0xD0)));
    if (g_ms.calls) std::printf("host: MODSETTINGS callbacks called %d, type checks %d\n", g_ms.calls() / 1000, g_ms.calls() % 1000);
    std::printf("host: done\n");
    std::fflush(stdout);
    ExitProcess(0);
}
