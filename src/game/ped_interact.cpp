#ifdef _WIN32

#include "ped_interact.h"

#include <atomic>
#include <cstdio>

#include "../common/log.h"
#include "MinHook.h"
#include "pattern.h"
#include "rtti.h"
#include "sm2.h"

namespace sm2m {
namespace ped_interact {
namespace {

// The game's activation takes (this, params); the detour passes on every
// register argument and the return value anyway.
using EnterFn = uintptr_t (*)(uintptr_t self, uintptr_t a, uintptr_t b, uintptr_t c);
EnterFn o_enter = nullptr;
uint32_t g_handleOffset = 0x48;
bool g_installed = false;

// Requests from the game's AI threads; read on the Present thread. A slot is
// claimed (head) before it is written: the reader waits for it. Overflow drops
// the oldest.
constexpr int kRing = 32;
std::atomic<uint32_t> g_ring[kRing];
std::atomic<uint32_t> g_head{0}; // next write
uint32_t g_tail = 0;             // next read (Present thread only)
int g_waits = 0;                 // calls spent waiting for a claimed slot

uintptr_t H_Enter(uintptr_t self, uintptr_t a, uintptr_t b, uintptr_t c) {
    const uintptr_t r = o_enter(self, a, b, c);
    uint32_t handle = 0;
    if (self && SafeReadT(self + g_handleOffset, handle) && handle) {
        const uint32_t i = g_head.fetch_add(1, std::memory_order_acq_rel);
        g_ring[i % kRing].store(handle, std::memory_order_release);
    }
    return r;
}

bool LooksRight(uintptr_t fn, const std::string& check) {
    if (check.empty()) return true;
    const Pattern p = Pattern::Parse(check);
    if (!p.valid) return false;
    uint8_t code[64];
    if (!SafeRead(fn, code, sizeof(code))) return false;
    return FindPattern(code, sizeof(code), p) >= 0;
}

} // namespace

bool Install(Sm2Game& game, const Ini& b) {
    if (g_installed) return true;
    const std::string cls = b.GetString("PedestrianInteract", "class", "");
    const int64_t slot = b.GetInt("PedestrianInteract", "enter", -1);
    if (cls.empty() || slot < 0) {
        LOGI("photo poses: no [PedestrianInteract] binding - only the pose key works");
        return false;
    }
    const uintptr_t base = game.ModuleBase();
    const uintptr_t vt = rtti::FindVtable(base, rtti::ImageDataSpans(base), cls.c_str());
    uintptr_t fn = 0;
    if (!vt || !SafeReadT(vt + uintptr_t(slot) * 8, fn) || fn < base) {
        LOGW("photo poses: %s not found in this game version - only the pose key works", cls.c_str());
        return false;
    }
    if (!LooksRight(fn, b.GetString("PedestrianInteract", "check_enter", ""))) {
        LOGW("photo poses: %s slot %lld (exe+0x%llx) doesn't look like it did - not hooked", cls.c_str(),
             static_cast<long long>(slot), static_cast<unsigned long long>(fn - base));
        return false;
    }
    const int64_t off = b.GetInt("PedestrianInteract", "actor_handle_offset", 0x48);
    if (off < 8 || off > 0x1000 || (off & 3)) {
        LOGW("photo poses: actor_handle_offset 0x%llx doesn't look right - not hooked", static_cast<long long>(off));
        return false;
    }
    g_handleOffset = uint32_t(off);
    if (MH_CreateHook(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&H_Enter), reinterpret_cast<void**>(&o_enter)) !=
            MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(fn)) != MH_OK) {
        LOGW("photo poses: hooking %s failed - only the pose key works", cls.c_str());
        return false;
    }
    g_installed = true;
    LOGI("photo poses: watching for pedestrians who ask for a picture (%s @exe+0x%llx)", cls.c_str(),
         static_cast<unsigned long long>(fn - base));
    return true;
}

bool Installed() { return g_installed; }

std::vector<uint32_t> Take() {
    std::vector<uint32_t> out;
    const uint32_t head = g_head.load(std::memory_order_acquire);
    if (head - g_tail > kRing) g_tail = head - kRing;
    while (g_tail != head) {
        const uint32_t h = g_ring[g_tail % kRing].exchange(0, std::memory_order_acq_rel);
        if (!h && ++g_waits < 30) break; // claimed, not written yet: next time
        g_waits = 0;
        if (h) out.push_back(h);
        ++g_tail;
    }
    return out;
}

} // namespace ped_interact
} // namespace sm2m

#endif
