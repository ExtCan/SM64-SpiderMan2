#ifdef _WIN32

#include "modsettings_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

#include "../common/log.h"
#include "../common/platform.h"
#include "../win/guard.h"

extern "C" BOOL WINAPI K32EnumProcessModules(HANDLE process, HMODULE* modules, DWORD cb, LPDWORD needed);

namespace sm2m {
namespace modsettings {
namespace {

// ---- MSVC's run-time type information (x64), for the objects ModSettings
// gets from us: a type descriptor is what typeid() returns (writable: name()
// caches the readable name in it); a vtable's slot -1 points at a complete
// object locator, whose offsets are relative to (its address - pSelf).
struct RttiTypeDescriptor {
    const void* vftable; // type_info's; only its virtual destructor would use it
    void* spare;         // name() cache
    char name[32];
};
struct RttiBaseClass {
    uint32_t typeDescriptor;
    uint32_t containedBases;
    int32_t mdisp, pdisp, vdisp;
    uint32_t attributes;
    uint32_t classDescriptor;
};
struct RttiHierarchy {
    uint32_t signature;
    uint32_t attributes; // 0: single inheritance
    uint32_t numBases;
    uint32_t baseArray;
};
struct RttiLocator {
    uint32_t signature; // 1: offsets relative to (this - self)
    uint32_t offset;
    uint32_t cdOffset;
    uint32_t typeDescriptor;
    uint32_t classDescriptor;
    uint32_t self;
};
// Mario's page is a ModMenu (a BaseMenu), in case ModSettings ever uses
// typeid or dynamic_cast on it; the std::function implementation has a type too.
struct RttiBlock {
    RttiLocator locator;
    RttiTypeDescriptor types[4]; // MarioModeMenu, ModMenu, BaseMenu, MarioModeCallback
    RttiBaseClass bases[3];
    RttiHierarchy hierarchies[3];
    uint32_t baseArray[3];
};
RttiBlock g_rtti = {
    {},
    {{nullptr, nullptr, ".?AVMarioModeMenu@@"},
     {nullptr, nullptr, ".?AVModMenu@@"},
     {nullptr, nullptr, ".?AVBaseMenu@@"},
     {nullptr, nullptr, ".?AVMarioModeCallback@@"}},
    {},
    {},
    {},
};
uint32_t Rva(const void* p) {
    return uint32_t(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(&g_rtti));
}
void BuildRtti() {
    RttiBlock& r = g_rtti;
    for (int i = 0; i < 3; ++i) {
        // bases[i]: class i, followed by its own (2 - i) bases in the array.
        r.bases[i] = {Rva(&r.types[i]), uint32_t(2 - i), 0, -1, 0, 0x40u, Rva(&r.hierarchies[i])};
        r.hierarchies[i] = {0, 0, uint32_t(3 - i), Rva(&r.baseArray[i])};
        r.baseArray[i] = Rva(&r.bases[i]);
    }
    r.locator = {1, 0, 0, Rva(&r.types[0]), Rva(&r.hierarchies[0]), Rva(&r.locator)};
}

// ---- MSVC std::function<void()> (x64): 8 pointers, the implementation
// object's address in the last one; that object starts with a vtable of
// _Copy, _Move, _Do_call, _Target_type, _Delete_this, _Get. (By value it is
// passed as the address of a caller-made copy, which MSVC aligns to 16.)
struct alignas(16) MsvcFunc {
    void* storage[7];
    void* impl;
};

struct FuncImpl {
    const void* const* vtbl;
    void (*fn)();
};

// Member functions in the Microsoft x64 convention: `this` comes first.
FuncImpl* FuncCopy(const FuncImpl* self, void*) { return new (std::nothrow) FuncImpl(*self); }
FuncImpl* FuncMove(FuncImpl* self, void* where) { return new (where) FuncImpl(*self); }
void FuncCall(FuncImpl* self) {
    if (self->fn) self->fn();
}
const void* FuncType(const FuncImpl*) { return &g_rtti.types[3]; }
void FuncDelete(FuncImpl* self, bool deallocate) {
    if (deallocate) delete self;
}
const void* FuncGet(const FuncImpl* self) { return &self->fn; }

const void* const kFuncVtbl[6] = {reinterpret_cast<const void*>(&FuncCopy),   reinterpret_cast<const void*>(&FuncMove),
                                  reinterpret_cast<const void*>(&FuncCall),   reinterpret_cast<const void*>(&FuncType),
                                  reinterpret_cast<const void*>(&FuncDelete), reinterpret_cast<const void*>(&FuncGet)};

// An argument: the callee owns (and destroys) it.
MsvcFunc MakeFunc(void (*fn)()) {
    MsvcFunc f;
    std::memset(&f, 0, sizeof(f));
    if (fn) {
        FuncImpl* impl = new (std::nothrow) FuncImpl;
        if (impl) {
            impl->vtbl = kFuncVtbl;
            impl->fn = fn;
            f.impl = impl;
        }
    }
    return f;
}

// ---- class ModMenu : BaseMenu (MSVC layout): vptr, int id, UISystemMenu*,
// std::vector<UISystemMenuItem>; virtuals Create, GetName, GetDescription,
// GetType in that order.
struct MsvcMenu {
    const void* const* vtbl;
    int32_t id;
    int32_t pad;
    void* menu;
    void* items[3];
    uint8_t slack[64]; // in case a newer ModSettings keeps more in its BaseMenu
};

struct SliderOpts {
    float min, max;
    int displayMin, displayMax;
    float step;
};

using CtorFn = void* (*)(void*);
using HeaderFn = void (*)(void*, const char*);
using ToggleFn = void (*)(void*, const char*, const char*, int*, MsvcFunc*);
using OptionFn = void (*)(void*, const char*, const char*, int*, const char**, int, MsvcFunc*);
using SliderFn = void (*)(void*, const char*, const char*, float*, SliderOpts*, MsvcFunc*);
using RegisterMenuFn = void (*)(void*);
using RegisterSaveFn = void (*)(MsvcFunc*);

struct Exports {
    CtorFn ctor = nullptr;
    HeaderFn header = nullptr;
    ToggleFn toggle = nullptr;
    OptionFn option = nullptr;
    SliderFn slider = nullptr;
    RegisterMenuFn registerMenu = nullptr;
    RegisterSaveFn registerSave = nullptr;
};
Exports g_x;
MsvcMenu* g_menu = nullptr;
std::atomic<bool> g_registered{false}, g_tried{false};
Mutex g_statusMu;
std::string g_status; // under g_statusMu

void SetStatus(const std::string& s) {
    LockGuard lock(g_statusMu);
    g_status = s;
}

// What the menu edits (ModSettings writes through these pointers), and what
// was last put there (so only the items the player changed are taken back).
int g_ints[kSetCount];
float g_floats[kSetCount];
int g_shownInts[kSetCount];
float g_shownFloats[kSetCount];
std::atomic<bool> g_dirty{false}, g_save{false};
Mutex g_mu;
LiveSettings g_published;

void OnChange() { g_dirty.store(true, std::memory_order_release); }
void OnSave() { g_save.store(true, std::memory_order_release); }

// g_mu held. Items the player changed on the page and Mario Mode hasn't
// taken yet keep their new value.
void ShowValues(const LiveSettings& s) {
    for (const SettingItem& it : SettingItems()) {
        if (it.id < 0) continue;
        if (g_ints[it.id] != g_shownInts[it.id] || g_floats[it.id] != g_shownFloats[it.id]) continue;
        if (it.kind == SettingItem::Toggle) g_ints[it.id] = GetSetting(s, it.id) >= 0.5f ? 1 : 0;
        else if (it.kind == SettingItem::Choice) g_ints[it.id] = int(GetSetting(s, it.id) + 0.5f);
        else if (it.kind == SettingItem::Slider) g_floats[it.id] = GetSetting(s, it.id);
        g_shownInts[it.id] = g_ints[it.id];
        g_shownFloats[it.id] = g_floats[it.id];
    }
}

void MenuCreate(MsvcMenu* self) {
    {
        LockGuard lock(g_mu);
        ShowValues(g_published);
    }
    for (const SettingItem& it : SettingItems()) {
        switch (it.kind) {
        case SettingItem::Header:
            g_x.header(self, it.label);
            break;
        case SettingItem::Toggle: {
            MsvcFunc fn = MakeFunc(&OnChange);
            g_x.toggle(self, it.label, it.desc, &g_ints[it.id], &fn);
            break;
        }
        case SettingItem::Choice: {
            MsvcFunc fn = MakeFunc(&OnChange);
            g_x.option(self, it.label, it.desc, &g_ints[it.id], const_cast<const char**>(it.choices.data()),
                       int(it.choices.size()), &fn);
            break;
        }
        case SettingItem::Slider: {
            SliderOpts o{it.min, it.max, it.displayMin, it.displayMax, 1.0f};
            MsvcFunc fn = MakeFunc(&OnChange);
            g_x.slider(self, it.label, it.desc, &g_floats[it.id], &o, &fn);
            break;
        }
        }
    }
}
const char* MenuName(MsvcMenu*) { return "MARIO MODE"; }
const char* MenuDescription(MsvcMenu*) { return "Super Mario 64's Mario in Spider-Man 2: sound, attacks, cheats."; }
int MenuType(MsvcMenu*) { return 0; } // ModMenu::Pause

// vtable[-1]: the RTTI locator (built in Register).
const void* const kMenuVtblStore[5] = {&g_rtti.locator, reinterpret_cast<const void*>(&MenuCreate),
                                       reinterpret_cast<const void*>(&MenuName),
                                       reinterpret_cast<const void*>(&MenuDescription),
                                       reinterpret_cast<const void*>(&MenuType)};

template <typename T>
bool Bind(HMODULE m, const char* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return out != nullptr;
}

HMODULE FindModSettings() {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
    const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < n; ++i)
        if (GetProcAddress(mods[i], "?RegisterMenu@ModSettings@@YAXPEAVModMenu@@@Z")) return mods[i];
    return nullptr;
}

} // namespace

static void RegisterBody() {
    HMODULE m = FindModSettings();
    if (!m) {
        SetStatus("not installed");
        return;
    }
    const bool ok = Bind(m, "??0BaseMenu@@QEAA@XZ", g_x.ctor) && Bind(m, "?Header@BaseMenu@@QEAAXPEBD@Z", g_x.header) &&
                    Bind(m, "?Toggle@BaseMenu@@QEAAXPEBD0PEAHV?$function@$$A6AXXZ@std@@@Z", g_x.toggle) &&
                    Bind(m, "?Option@BaseMenu@@QEAAXPEBD0PEAHPEAPEBDHV?$function@$$A6AXXZ@std@@@Z", g_x.option) &&
                    Bind(m, "?Slider@BaseMenu@@QEAAXPEBD0PEAMAEAUSliderOpts@1@V?$function@$$A6AXXZ@std@@@Z", g_x.slider) &&
                    Bind(m, "?RegisterMenu@ModSettings@@YAXPEAVModMenu@@@Z", g_x.registerMenu);
    Bind(m, "?RegisterSaveCallback@ModSettings@@YAXV?$function@$$A6AXXZ@std@@@Z", g_x.registerSave);
    if (!ok) {
        SetStatus("found, but its exports don't match this version of Mario Mode");
        return;
    }
    g_menu = static_cast<MsvcMenu*>(std::calloc(1, sizeof(MsvcMenu)));
    if (!g_menu) return;
    guard::Install(); // also covers script_enable's thread
    guard::Fault fault;
    auto reg = [&] {
        g_x.ctor(g_menu);
        g_menu->vtbl = kMenuVtblStore + 1;
        g_x.registerMenu(g_menu);
        if (g_x.registerSave) {
            MsvcFunc fn = MakeFunc(&OnSave);
            g_x.registerSave(&fn);
        }
    };
    if (!guard::Call(reg, &fault)) {
        SetStatus("registering crashed (" + guard::Describe(fault) + ") - not used");
        return;
    }
    g_registered = true;
    SetStatus("Mario Mode page added to the pause menu");
}

void Register(bool logResult) {
    if (g_tried.exchange(true)) return;
    BuildRtti();
    RegisterBody();
    if (logResult) LOGI("ModSettings: %s", Status().c_str());
}

std::string Status() {
    LockGuard lock(g_statusMu);
    return g_status;
}
bool Registered() { return g_registered.load(); }

void Publish(const LiveSettings& s) {
    LockGuard lock(g_mu);
    g_published = s;
    // If ModSettings built the page once and keeps it, its items still point
    // here: show changes made in the F8 menu (ShowValues leaves alone what
    // the player changed on the page and hasn't been taken yet).
    ShowValues(s);
}

bool TakeChanges(LiveSettings& s) {
    if (!g_dirty.exchange(false, std::memory_order_acq_rel)) return false;
    LockGuard lock(g_mu);
    const LiveSettings before = s;
    for (const SettingItem& it : SettingItems()) {
        switch (it.kind) {
        case SettingItem::Toggle:
        case SettingItem::Choice:
            if (g_ints[it.id] != g_shownInts[it.id]) {
                SetSetting(s, it.id, float(g_ints[it.id]));
                g_shownInts[it.id] = g_ints[it.id];
            }
            break;
        case SettingItem::Slider:
            // (Untouched sliders aren't re-read: a value between the
            // slider's steps would snap to one.)
            if (g_floats[it.id] != g_shownFloats[it.id]) {
                SetSetting(s, it.id, g_floats[it.id]);
                g_shownFloats[it.id] = g_floats[it.id];
            }
            break;
        default:
            break;
        }
    }
    return s != before;
}

bool TakeSaveRequest() { return g_save.exchange(false, std::memory_order_acq_rel); }

} // namespace modsettings
} // namespace sm2m

#endif
