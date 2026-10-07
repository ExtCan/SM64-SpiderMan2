#ifdef _WIN32

#include "sm2_physics.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>

#include "../common/log.h"
#include "../common/platform.h"
#include "../win/guard.h"
#include "MinHook.h"
#include "pattern.h"
#include "sm2.h"

namespace sm2m {
namespace game_physics {
namespace {

// NQueryResult* CastRayImmediate(PhysicsSystem*, const CollRequest*, const Vec3* from, const Vec3* to, const char* tag)
using RayFn = void* (*)(uintptr_t physics, void* request, const float* from, const float* to, const char* tag);
using ReqInitFn = void* (*)(void* request, uint32_t type);   // CollRequest::CollRequest(type)
using ReqReleaseFn = void (*)(void* request);               // CollRequest::~CollRequest
using ReqIgnoreFn = void (*)(void* request, uintptr_t actor); // CollRequest::AddIgnoreActor(Actor*)
using ResultActorFn = uintptr_t (*)(void* result, int index); // the hit's Actor* (or null)
using FrameFn = void (*)(uintptr_t self);
// DamageRequest* DamageSystem::DamageSphere(DamageSystem*, const Vec3& center, float radius, const CollRequest&)
using DamageSphereFn = uint8_t* (*)(uintptr_t system, const float* center, float radius, const void* request);
// DamageRequest* DamageSystem::DamageActor(DamageSystem*, const ActorHandle*)
using DamageActorFn = uint8_t* (*)(uintptr_t system, const uint32_t* target);

const char kTag[] = "sm2mario: Mario's collision";
constexpr int kMaxBatch = 64;
constexpr size_t kMaxResults = 1024; // unread answers kept (the mod reads them every frame)

struct Layout {
    uint32_t maxHits = 4;     // CollRequest: u16 number of hits to collect
    uint32_t records = 0x0;   // result: pointer to the hit records
    uint32_t count = 0xA;     // result: u16 number of hits
    uint32_t stride = 0x40;   // hit record size
    uint32_t fraction = 0x0;  // record: f32 along the ray (0..1)
    uint32_t normal = 0xC;    // record: 3 x f32
    uint32_t position = 0x18; // record: 3 x f32
    uint32_t material = 0x3A; // record: s16 physics material
};

struct Bindings {
    uintptr_t ray = 0, physics = 0, reqInit = 0, reqRelease = 0, reqIgnore = 0, resultActor = 0, frame = 0;
    uintptr_t pausedFlag = 0, timestep = 0, timescale = 0;
    uint32_t queryOffset = 0; // the ray cast adds this to the physics system pointer (its query system)
    uint32_t poolCounter = 0; // offset (in the query system) of the frame's result counter
    uint32_t poolLimit = 0;   // results per physics frame
    Layout lay;
};

Bindings g_b;
Options g_opt;

// The DamageRequest's fields: byte offset, and the bit each sets in the
// request's two field masks (which fields were filled in).
struct Field {
    uint32_t offset = 0;
    int bit = -1;
};
struct DamageBinding {
    uintptr_t sphere = 0, actor = 0, system = 0;
    uint8_t query = 9; // CollRequest kDamage
    uint32_t masks[2] = {0x8, 0x18};
    Field damager, type, amount, knockback, knockbackAmount, flags;
};
DamageBinding g_d;
std::atomic<bool> g_damage{false};
std::deque<DamageOrder> g_damageQueue;
bool g_installed = false;
std::atomic<bool> g_available{false};
std::atomic<bool> g_pauseDetection{false};
FrameFn o_frame = nullptr;
// The query pool (game thread only): results the game used in its last frame
// (before the frame freed them, less the mod's own), and a running average.
int g_poolLastGame = 0;
double g_poolAverage = 0;
int g_castLastFrame = 0; // the mod's rays in the frame just ended
bool g_poolSeen = false;

std::atomic<bool> g_active{false};
std::atomic<uintptr_t> g_hero{0};
std::atomic<bool> g_broken{false};
std::atomic<uint64_t> g_calls{0}, g_steps{0};
std::atomic<double> g_lastCall{-1e9}; // when the frame hook last ran (NowSeconds)

Mutex g_mu; // the queue, the answers and the stats
std::deque<GameRay> g_queue;
std::vector<RawResult> g_results;
Stats g_stats;

template <typename T>
T Load(uintptr_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}

// The world is running this frame: the same test the game makes before
// stepping its physics (a "world paused" flag, and timestep x time scale > 0).
bool WorldRunning() {
    if (!g_pauseDetection.load(std::memory_order_relaxed)) return true;
    const uint8_t flag = Load<uint8_t>(g_b.pausedFlag);
    const float dt = Load<float>(g_b.timestep), scale = Load<float>(g_b.timescale);
    return flag == 0 && dt * scale > 0.0f;
}

void ReadResult(void* res, RawResult& out) {
    const Layout& L = g_b.lay;
    const uintptr_t r = reinterpret_cast<uintptr_t>(res);
    const uintptr_t recs = Load<uintptr_t>(r + L.records);
    const int count = std::min<int>(Load<uint16_t>(r + L.count), kMaxGameHits);
    if (!LooksLikePointer(recs) || count <= 0) return;
    struct Hit {
        float t;
        GameHit h;
        uintptr_t actor;
    } hits[kMaxGameHits];
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const uintptr_t rec = recs + uintptr_t(i) * L.stride;
        const float t = Load<float>(rec + L.fraction);
        float nrm[3], pos[3];
        std::memcpy(nrm, reinterpret_cast<const void*>(rec + L.normal), sizeof(nrm));
        std::memcpy(pos, reinterpret_cast<const void*>(rec + L.position), sizeof(pos));
        bool finite = std::isfinite(t);
        for (int k = 0; k < 3; ++k) finite = finite && std::isfinite(nrm[k]) && std::isfinite(pos[k]);
        if (!finite) continue;
        Hit& h = hits[n++];
        h.t = t;
        h.h.pos = DVec3(pos[0], pos[1], pos[2]);
        const Vec3 nv(nrm[0], nrm[1], nrm[2]);
        const float len = Length(nv);
        h.h.normal = len > 1e-6f ? nv * (1.0f / len) : Vec3(0, 1, 0);
        h.h.material = Load<int16_t>(rec + L.material);
        h.actor = 0;
        if (g_b.resultActor) {
            const uintptr_t a = reinterpret_cast<ResultActorFn>(g_b.resultActor)(res, i);
            h.actor = LooksLikePointer(a) ? a : 0;
        }
    }
    // The game collects every hit along the ray, not in order.
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && hits[j].t < hits[j - 1].t; --j) std::swap(hits[j], hits[j - 1]);
    for (int i = 0; i < n; ++i) {
        out.r.hits[i] = hits[i].h;
        out.actors[i] = hits[i].actor;
    }
    out.r.count = n;
}

struct Request {
    uint8_t type = 0;
    bool ready = false;
    alignas(16) uint8_t mem[64];
};

// AddIgnoreActor reads the actor's transform and its id: only for an actor
// that is still there (the hero can change, or be gone for a moment).
bool HeroReadable(uintptr_t hero) {
    uintptr_t transform = 0;
    uint32_t id = 0;
    return SafeReadT(hero, transform) && LooksLikePointer(transform) && SafeReadT(transform + 0x64, id);
}

// Casts `n` rays (the game's thread, right after its physics frame).
void CastBatch(const GameRay* rays, int n, uintptr_t physics, std::vector<RawResult>& out) {
    Request reqs[4];
    int nreq = 0;
    const uintptr_t hero = g_hero.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const GameRay& ray = rays[i];
        Request* rq = nullptr;
        for (int k = 0; k < nreq; ++k)
            if (reqs[k].type == ray.type) rq = &reqs[k];
        if (!rq) {
            if (nreq == 4) continue; // more query types than the mod uses: not cast (asked again later)
            rq = &reqs[nreq++];
            rq->type = ray.type;
            std::memset(rq->mem, 0, sizeof(rq->mem));
            reinterpret_cast<ReqInitFn>(g_b.reqInit)(rq->mem, ray.type);
            rq->ready = true;
            if (hero && g_b.reqIgnore && HeroReadable(hero))
                reinterpret_cast<ReqIgnoreFn>(g_b.reqIgnore)(rq->mem, hero);
        }
        const uint16_t maxHits = uint16_t(std::max(1, std::min(kMaxGameHits, int(ray.maxHits))));
        std::memcpy(rq->mem + g_b.lay.maxHits, &maxHits, sizeof(maxHits));
        const float from[3] = {float(ray.from.x), float(ray.from.y), float(ray.from.z)};
        const float to[3] = {float(ray.to.x), float(ray.to.y), float(ray.to.z)};
        RawResult rr;
        rr.r.tag = ray.tag;
        void* res = reinterpret_cast<RayFn>(g_b.ray)(physics, rq->mem, from, to, kTag);
        if (res) ReadResult(res, rr);
        out.push_back(rr);
    }
    for (int k = 0; k < nreq; ++k)
        if (reqs[k].ready) reinterpret_cast<ReqReleaseFn>(g_b.reqRelease)(reqs[k].mem);
}

void Pump() {
    if (!g_available.load(std::memory_order_relaxed) || !g_active.load(std::memory_order_acquire) ||
        g_broken.load(std::memory_order_relaxed))
        return;
    GameRay batch[kMaxBatch];
    int n = 0;
    {
        LockGuard lock(g_mu);
        const int want = std::min(kMaxBatch, std::max(1, g_opt.raysPerFrame));
        while (n < want && !g_queue.empty()) {
            batch[n++] = g_queue.front();
            g_queue.pop_front();
        }
    }
    if (n == 0) return;
    const double t0 = NowSeconds();
    const uintptr_t physics = Load<uintptr_t>(g_b.physics); // a global in the exe's image
    int allowed = LooksLikePointer(physics) ? n : 0;
    int used = -1;
    if (allowed && g_b.poolCounter) {
        // The results the mod's rays get come out of the pool the game's own
        // queries use next frame: leave it what it used last frame (or on
        // average lately, if that's more), and the headroom on top.
        if (g_poolSeen) {
            used = std::max(g_poolLastGame, int(g_poolAverage + 0.5));
            allowed = std::max(0, std::min(n, int(g_b.poolLimit) - g_opt.poolHeadroom - used));
        } else {
            allowed = 0; // the physics system isn't readable (being torn down?)
        }
    }
    std::vector<RawResult> out;
    out.reserve(size_t(allowed));
    bool ok = true;
    if (allowed > 0) {
        guard::Fault fault;
        auto cast = [&] { CastBatch(batch, allowed, physics, out); };
        guard::PhaseScope phase("game physics rays");
        ok = guard::Call(cast, &fault);
        if (!ok) {
            g_broken.store(true);
            LOGE("game physics: a ray cast crashed (%s) - Mario's collision falls back to what he can see",
                 guard::Describe(fault).c_str());
        }
    }
    g_castLastFrame = ok ? int(out.size()) : 0;
    LockGuard lock(g_mu);
    for (int i = n - 1; i >= allowed; --i) g_queue.push_front(batch[i]); // later, in the same order
    g_stats.deferred += uint64_t(n - allowed);
    if (!ok) return;
    for (const RawResult& r : out) {
        ++g_stats.cast;
        g_stats.hits += uint64_t(r.r.count);
        if (r.r.count == 0) ++g_stats.empty;
        g_results.push_back(r);
    }
    if (g_results.size() > kMaxResults) g_results.erase(g_results.begin(), g_results.end() - long(kMaxResults));
    if (allowed > 0) {
        const double ms = (NowSeconds() - t0) * 1000.0;
        ++g_stats.frames;
        g_stats.totalMs += ms;
        g_stats.maxFrameMs = std::max(g_stats.maxFrameMs, ms);
    }
    g_stats.poolUsedMax = std::max(g_stats.poolUsedMax, used);
    g_stats.thread = GetCurrentThreadId();
}

void SetField(uint8_t* dr, const Field& f, const void* value) {
    if (f.bit < 0 || f.bit > 63) return;
    std::memcpy(dr + f.offset, value, 4);
    for (uint32_t m : g_d.masks) {
        uint64_t mask;
        std::memcpy(&mask, dr + m, sizeof(mask));
        mask |= uint64_t(1) << f.bit;
        std::memcpy(dr + m, &mask, sizeof(mask));
    }
}

// Sends the orders; counts those that went by actor handle and those the
// game had no room for.
void SendDamage(const DamageOrder* orders, int n, int& byActor, int& refused) {
    for (int i = 0; i < n; ++i) {
        const DamageOrder& d = orders[i];
        uint8_t* dr = nullptr;
        if (d.target && g_d.actor) {
            const uint32_t target = d.target;
            dr = reinterpret_cast<DamageActorFn>(g_d.actor)(g_d.system, &target);
            ++byActor;
        } else if (g_d.sphere && g_b.reqInit && g_b.reqRelease) {
            alignas(16) uint8_t req[64];
            std::memset(req, 0, sizeof(req));
            reinterpret_cast<ReqInitFn>(g_b.reqInit)(req, g_d.query);
            const float c[3] = {float(d.center.x), float(d.center.y), float(d.center.z)};
            dr = reinterpret_cast<DamageSphereFn>(g_d.sphere)(g_d.system, c, d.radius, req);
            reinterpret_cast<ReqReleaseFn>(g_b.reqRelease)(req);
        } else {
            continue;
        }
        if (!LooksLikePointer(reinterpret_cast<uintptr_t>(dr))) {
            ++refused;
            continue;
        }
        const uint32_t type = uint32_t(d.type), kb = uint32_t(d.knockback);
        if (d.damager) SetField(dr, g_d.damager, &d.damager);
        SetField(dr, g_d.type, &type);
        SetField(dr, g_d.amount, &d.amount);
        SetField(dr, g_d.knockback, &kb);
        SetField(dr, g_d.knockbackAmount, &d.knockbackAmount);
        SetField(dr, g_d.flags, &d.flags);
    }
}

void PumpDamage() {
    if (!g_damage.load(std::memory_order_relaxed) || g_broken.load(std::memory_order_relaxed)) return;
    DamageOrder orders[16];
    int n = 0;
    {
        LockGuard lock(g_mu);
        while (n < 16 && !g_damageQueue.empty()) {
            orders[n++] = g_damageQueue.front();
            g_damageQueue.pop_front();
        }
    }
    if (n == 0) return;
    guard::Fault fault;
    int byActor = 0, refused = 0;
    auto send = [&] { SendDamage(orders, n, byActor, refused); };
    guard::PhaseScope phase("game damage");
    if (!guard::Call(send, &fault)) {
        g_damage.store(false);
        LOGE("game damage: a damage request crashed (%s) - Mario's hits write health directly from now on",
             guard::Describe(fault).c_str());
        return;
    }
    LockGuard lock(g_mu);
    g_stats.damage += uint64_t(n);
    g_stats.damageActor += uint64_t(byActor);
    g_stats.damageRefused += uint64_t(refused);
}

// How much of the query pool the frame that is ending used (read before the
// frame frees it): the game's own queries, plus the mod's rays from last time.
void NotePoolUse() {
    if (!g_b.poolCounter || !g_available.load(std::memory_order_relaxed)) return;
    const uintptr_t physics = Load<uintptr_t>(g_b.physics);
    int32_t counter = 0;
    if (!LooksLikePointer(physics) || !SafeReadT(physics + g_b.queryOffset + g_b.poolCounter, counter) || counter < 0 ||
        counter > 1000000) {
        g_poolSeen = false;
        return;
    }
    g_poolLastGame = std::max(0, int(counter) - g_castLastFrame);
    g_poolAverage = g_poolSeen ? g_poolAverage * 0.9 + 0.1 * g_poolLastGame : double(g_poolLastGame);
    g_poolSeen = true;
    g_castLastFrame = 0;
}

// The game's end of physics frame (its main update thread).
void H_Frame(uintptr_t self) {
    const bool running = WorldRunning();
    NotePoolUse();
    o_frame(self);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_lastCall.store(NowSeconds(), std::memory_order_relaxed);
    if (!running) return;
    g_steps.fetch_add(1, std::memory_order_relaxed);
    Pump();
    PumpDamage();
}

bool ParseField(const Ini& b, const char* section, const char* key, Field& f) {
    const std::vector<std::string> parts = Ini::SplitList(b.GetString(section, key, ""));
    double off = 0, bit = 0;
    if (parts.size() != 2 || !Ini::ParseFloat(parts[0], off) || !Ini::ParseFloat(parts[1], bit)) return false;
    if (off < 0 || off > 0x1000 || bit < 0 || bit > 63) return false;
    f.offset = uint32_t(off);
    f.bit = int(bit);
    return true;
}

uintptr_t Global(Sm2Game& game, uintptr_t match, const Ini& b, const char* section, const char* key, int trailing,
                 size_t size) {
    const int64_t disp = b.GetInt(section, key, -1);
    if (!match || disp < 0 || disp > 256) return 0;
    const uintptr_t a = RipTarget(match + uintptr_t(disp), trailing);
    return game.InImage(a, size) ? a : 0;
}

std::string Hex(Sm2Game& game, uintptr_t a) {
    if (!a) return "--";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "exe+0x%llx", static_cast<unsigned long long>(a - game.ModuleBase()));
    return buf;
}

std::string g_describe;

} // namespace

bool Install(Sm2Game& game, const Ini& b, const Options& opt) {
    if (g_installed) return g_available;
    g_installed = true;
    g_opt = opt;
    std::string d;
    uintptr_t rayMatch = 0, frameMatch = 0, poolMatch = 0;
    g_b.ray = game.ResolveFunction(b, "PhysicsRaycast", d, &rayMatch);
    g_b.physics = Global(game, rayMatch, b, "PhysicsRaycast", "physics_disp", 0, 8);
    g_b.frame = game.ResolveFunction(b, "PhysicsFrame", d, &frameMatch);
    if (!g_b.physics) g_b.physics = Global(game, frameMatch, b, "PhysicsFrame", "physics_disp", 0, 8);
    g_b.pausedFlag = Global(game, frameMatch, b, "PhysicsFrame", "paused_flag_disp", 1, 1);
    g_b.timestep = Global(game, frameMatch, b, "PhysicsFrame", "timestep_disp", 0, 4);
    g_b.timescale = Global(game, frameMatch, b, "PhysicsFrame", "timescale_disp", 0, 4);
    g_b.reqInit = game.ResolveFunction(b, "CollRequestInit", d);
    g_b.reqRelease = game.ResolveFunction(b, "CollRequestRelease", d);
    g_b.reqIgnore = game.ResolveFunction(b, "CollRequestIgnoreActor", d);
    g_b.resultActor = game.ResolveFunction(b, "QueryResultActor", d);
    g_b.lay.maxHits = uint32_t(std::max<int64_t>(0, std::min<int64_t>(60, b.GetInt("PhysicsRaycast", "request_max_hits", 4))));

    // The thunk the game calls: add rcx, imm32 (the query system) ; jmp.
    if (g_b.ray) {
        uint8_t code[8] = {};
        if (SafeRead(g_b.ray, code, sizeof(code))) {
            if (code[0] == 0x48 && code[1] == 0x81 && code[2] == 0xC1) std::memcpy(&g_b.queryOffset, code + 3, 4);
            else if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xC1) g_b.queryOffset = code[3];
        }
    }
    // The per-frame result pool: lock xadd [rcx+counter], eax ; cmp eax, limit.
    if (game.ResolveSection(b, "PhysicsQueryPool", d, &poolMatch) && poolMatch && g_b.queryOffset) {
        const int64_t cd = b.GetInt("PhysicsQueryPool", "counter_disp", -1);
        const int64_t ld = b.GetInt("PhysicsQueryPool", "limit_disp", -1);
        if (cd >= 0 && ld >= 0 && cd < 128 && ld < 128) {
            int32_t counter = 0, limit = 0;
            if (SafeReadT(poolMatch + uintptr_t(cd), counter) && SafeReadT(poolMatch + uintptr_t(ld), limit) &&
                counter > 0 && counter < 0x1000000 && limit >= 32 && limit <= 65536) {
                g_b.poolCounter = uint32_t(counter);
                g_b.poolLimit = uint32_t(limit);
            }
        }
    }
    g_pauseDetection = g_b.pausedFlag && g_b.timestep && g_b.timescale;

    // The damage system: DamageSphere / DamageActor, and its object (where
    // the game calls them: lea rcx, [rip+X]).
    g_d.sphere = game.ResolveFunction(b, "DamageSphere", d);
    g_d.actor = game.ResolveFunction(b, "DamageActor", d);
    if (g_d.sphere) g_d.system = game.FindRipLoadBeforeCall(g_d.sphere);
    if (!g_d.system && g_d.actor) g_d.system = game.FindRipLoadBeforeCall(g_d.actor);
    if (g_d.system && !game.InImage(g_d.system, 8)) g_d.system = 0;
    g_d.query = uint8_t(b.GetInt("DamageSphere", "query", 9));
    {
        const std::vector<std::string> m = Ini::SplitList(b.GetString("DamageSphere", "masks", "0x8, 0x18"));
        double a = 0, c = 0;
        if (m.size() == 2 && Ini::ParseFloat(m[0], a) && Ini::ParseFloat(m[1], c) && a >= 0 && c >= 0 && a < 0x1000 &&
            c < 0x1000) {
            g_d.masks[0] = uint32_t(a);
            g_d.masks[1] = uint32_t(c);
        }
    }
    const bool fields = ParseField(b, "DamageSphere", "damager", g_d.damager) &&
                        ParseField(b, "DamageSphere", "type", g_d.type) &&
                        ParseField(b, "DamageSphere", "amount", g_d.amount) &&
                        ParseField(b, "DamageSphere", "knockback", g_d.knockback) &&
                        ParseField(b, "DamageSphere", "knockback_amount", g_d.knockbackAmount) &&
                        ParseField(b, "DamageSphere", "flags", g_d.flags);

    const bool callable = g_b.ray && g_b.physics && g_b.reqInit && g_b.reqRelease;
    const bool damageCallable =
        g_d.system && fields && (g_d.actor || (g_d.sphere && g_b.reqInit && g_b.reqRelease));
    if ((callable || damageCallable) && g_b.frame) {
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(g_b.frame), reinterpret_cast<void*>(&H_Frame),
                                    reinterpret_cast<void**>(&o_frame));
        if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(g_b.frame));
        if (s != MH_OK) {
            LOGW("game physics: hooking the physics frame failed (MinHook %d)", int(s));
            MH_RemoveHook(reinterpret_cast<void*>(g_b.frame));
            o_frame = nullptr;
        }
    }
    g_available = callable && o_frame != nullptr;
    if (!o_frame) g_pauseDetection = false;
    g_damage = o_frame && damageCallable;
    if ((g_d.sphere || g_d.actor) && !g_d.system)
        LOGW("game damage: the damage system object wasn't found next to a call of DamageSphere / DamageActor");
    if (g_d.sphere && !fields) LOGW("game damage: bindings.ini [DamageSphere] has no usable field offsets");

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "ray cast %s, physics %s, frame hook %s, requests %s/%s, ignore hero %s, hit actors %s, query pool "
                  "%s, pause detection %s, damage system %s",
                  Hex(game, g_b.ray).c_str(), Hex(game, g_b.physics).c_str(), o_frame ? Hex(game, g_b.frame).c_str() : "--",
                  Hex(game, g_b.reqInit).c_str(), Hex(game, g_b.reqRelease).c_str(), g_b.reqIgnore ? "yes" : "no",
                  g_b.resultActor ? "yes" : "no",
                  g_b.poolCounter ? (std::to_string(g_b.poolLimit) + " per frame").c_str() : "unknown",
                  g_pauseDetection ? "yes" : "no",
                  g_damage ? (Hex(game, g_d.system) + (g_d.actor ? " (by actor and sphere)" : " (sphere)")).c_str()
                           : "no");
    g_describe = buf;
    if (g_available) LOGI("game physics: %s", buf);
    else LOGW("game physics unavailable (%s) - Mario's collision comes from what the camera sees", buf);
    return g_available;
}

bool Available() { return g_available && !g_broken.load(std::memory_order_relaxed); }
// (Damage goes out from the frame hook: if the game stopped calling it - a
// loading screen, or the binding found the wrong function - hits fall back to
// health changes rather than waiting in the queue.)
bool DamageAvailable() {
    return g_damage && !g_broken.load(std::memory_order_relaxed) &&
           NowSeconds() - g_lastCall.load(std::memory_order_relaxed) < 1.0;
}

bool DamageActorAvailable() { return DamageAvailable() && g_d.actor != 0; }

void SubmitDamage(const DamageOrder& d) {
    if (!g_damage) return;
    LockGuard lock(g_mu);
    if (g_damageQueue.size() < 64) g_damageQueue.push_back(d);
}
bool PauseDetection() { return g_pauseDetection; }
std::string Describe() { return g_describe; }

void SetActive(bool on, uintptr_t hero) {
    g_hero.store(hero, std::memory_order_release);
    g_active.store(on && g_available, std::memory_order_release);
}

void SetHero(uintptr_t hero) { g_hero.store(hero, std::memory_order_release); }

int Queued() {
    LockGuard lock(g_mu);
    return int(g_queue.size());
}

void Submit(const std::vector<GameRay>& rays) {
    if (rays.empty()) return;
    LockGuard lock(g_mu);
    for (const GameRay& r : rays) g_queue.push_back(r);
    // Never more than a few frames' worth (the scheduler asks again for what is dropped).
    const size_t cap = size_t(std::max(1, g_opt.raysPerFrame)) * 8;
    while (g_queue.size() > cap) g_queue.pop_front();
}

void SubmitFirst(const std::vector<GameRay>& rays, uint32_t replace) {
    LockGuard lock(g_mu);
    if (replace)
        g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(),
                                     [&](const GameRay& r) { return (r.tag & replace) == replace; }),
                      g_queue.end());
    for (size_t i = rays.size(); i-- > 0;) g_queue.push_front(rays[i]);
}

void Take(std::vector<RawResult>& out) {
    LockGuard lock(g_mu);
    out.insert(out.end(), g_results.begin(), g_results.end());
    g_results.clear();
}

void Clear() {
    LockGuard lock(g_mu);
    g_queue.clear();
    g_results.clear();
    g_damageQueue.clear();
}

Heartbeat GetHeartbeat() {
    Heartbeat h;
    h.calls = g_calls.load(std::memory_order_relaxed);
    h.steps = g_steps.load(std::memory_order_relaxed);
    return h;
}

Stats TakeStats() {
    LockGuard lock(g_mu);
    Stats s = g_stats;
    s.broken = g_broken.load();
    const uint32_t thread = g_stats.thread;
    g_stats = Stats();
    g_stats.thread = thread;
    return s;
}

} // namespace game_physics
} // namespace sm2m

#endif
