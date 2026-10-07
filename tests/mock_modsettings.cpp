// A stand-in for the ModSettings mod (nexusmods.com/marvelsspiderman2/mods/579)
// for the end-to-end test. It exports the functions Mario Mode looks for under
// their MSVC-decorated names (mock_modsettings.def) and treats what it is given
// the way MSVC-compiled code does:
//  * a registered menu is an object whose first field is a vtable of Create,
//    GetName, GetDescription, GetType (BaseMenu's virtuals, then ModMenu's);
//  * a std::function<void()> argument is passed by address, its implementation
//    pointer in its last 8 bytes; the callee moves it into its own storage
//    (stealing a heap implementation, calling _Move for a local one) and
//    destroys the argument; calling it goes through vtable slot 2 (_Do_call),
//    destroying through slot 4 (_Delete_this).
// The host (deferred_host.cpp) drives it like a player in the pause menu:
// MockModSettings_Open builds the pages, MockModSettings_Set changes an item and
// calls its callback, MockModSettings_Save asks the mods to save.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct MsvcFunc {
    void* storage[7];
    void* impl;
};
using FnCopy = void* (*)(void* self, void* where);
using FnMove = void* (*)(void* self, void* where);
using FnCall = void (*)(void* self);
using FnType = const void* (*)(void* self);
using FnDelete = void (*)(void* self, bool deallocate);
void** Vtbl(void* impl) { return *static_cast<void***>(impl); }

bool IsLocal(const MsvcFunc& f) { return f.impl == static_cast<const void*>(&f); }
void Destroy(MsvcFunc& f) {
    if (!f.impl) return;
    reinterpret_cast<FnDelete>(Vtbl(f.impl)[4])(f.impl, !IsLocal(f));
    f.impl = nullptr;
}
// std::function's move constructor (MSVC): steal a heap implementation, move a local one.
void MoveInto(MsvcFunc& dst, MsvcFunc& src) {
    std::memset(&dst, 0, sizeof(dst));
    if (!src.impl) return;
    if (IsLocal(src)) {
        dst.impl = reinterpret_cast<FnMove>(Vtbl(src.impl)[1])(src.impl, &dst);
        Destroy(src);
    } else {
        dst.impl = src.impl;
        src.impl = nullptr;
    }
}
int g_calls = 0, g_typeChecks = 0;
// What MSVC's typeid(...).name() reads: a type descriptor's name, 16 bytes in.
bool TypeNameIs(const void* typeDescriptor, const char* name) {
    return typeDescriptor && std::strcmp(static_cast<const char*>(typeDescriptor) + 16, name) == 0;
}
void Call(MsvcFunc& f) {
    if (!f.impl) return;
    // _Target_type must answer (std::function::target_type() would call it).
    if (TypeNameIs(reinterpret_cast<FnType>(Vtbl(f.impl)[3])(f.impl), ".?AVMarioModeCallback@@")) ++g_typeChecks;
    reinterpret_cast<FnCall>(Vtbl(f.impl)[2])(f.impl);
    ++g_calls;
}

struct SliderOpts {
    float min, max;
    int displayMin, displayMax;
    float step;
};

struct Item {
    char kind; // H header, T toggle, O option, S slider
    std::string label, desc;
    int* ivalue = nullptr;
    float* fvalue = nullptr;
    std::vector<std::string> options;
    SliderOpts slider{};
    MsvcFunc* cb = nullptr; // a stable address: a local implementation lives inside it
};

struct Page {
    void* menu = nullptr;
    std::vector<Item> items;
};
std::vector<Page> g_pages;
std::vector<MsvcFunc*> g_saves;
int g_nextId = 1;
std::string g_errors;

Page* PageOf(void* menu) {
    for (Page& p : g_pages)
        if (p.menu == menu) return &p;
    g_errors += "item added to an unregistered menu; ";
    return nullptr;
}

void ClearPage(Page& p) {
    for (Item& it : p.items)
        if (it.cb) {
            Destroy(*it.cb);
            delete it.cb;
        }
    p.items.clear();
}

// Takes over a callback argument the way the callee of a by-value
// std::function parameter does.
MsvcFunc* Keep(MsvcFunc* arg) {
    auto* kept = new MsvcFunc;
    MoveInto(*kept, *arg);
    Destroy(*arg); // callee-destroyed argument (empty after the move)
    return kept;
}
void Drop(MsvcFunc* f) {
    Destroy(*f);
    delete f;
}

bool Readable(const char* s) { return s && !IsBadReadPtr(s, 1) && std::strlen(s) < 512; }

// What typeid() and dynamic_cast<ModMenu*>() would find through vtable[-1]
// (MSVC x64: a complete object locator with offsets from (itself - pSelf)).
std::string RttiOf(void* object) {
    struct Locator {
        uint32_t signature, offset, cdOffset, typeDescriptor, classDescriptor, self;
    };
    struct Hierarchy {
        uint32_t signature, attributes, numBases, baseArray;
    };
    struct BaseClass {
        uint32_t typeDescriptor, containedBases;
        int32_t mdisp, pdisp, vdisp;
        uint32_t attributes, classDescriptor;
    };
    void** vt = *static_cast<void***>(object);
    const auto* col = static_cast<const Locator*>(vt[-1]);
    if (!col || IsBadReadPtr(col, sizeof(Locator))) return "no RTTI";
    if (col->signature != 1 || col->offset != 0) return "bad locator";
    const uintptr_t base = reinterpret_cast<uintptr_t>(col) - col->self;
    if (base + col->self != reinterpret_cast<uintptr_t>(col)) return "bad self";
    std::string r = static_cast<const char*>(reinterpret_cast<const void*>(base + col->typeDescriptor + 16));
    const auto* chd = reinterpret_cast<const Hierarchy*>(base + col->classDescriptor);
    if (chd->attributes != 0 || chd->numBases < 1 || chd->numBases > 8) return "bad hierarchy";
    const auto* arr = reinterpret_cast<const uint32_t*>(base + chd->baseArray);
    for (uint32_t i = 0; i < chd->numBases; ++i) {
        const auto* bcd = reinterpret_cast<const BaseClass*>(base + arr[i]);
        if (bcd->mdisp != 0 || bcd->pdisp != -1 || bcd->containedBases != chd->numBases - 1 - i) return "bad base " + std::to_string(i);
        r += i == 0 ? " = " : " : ";
        r += static_cast<const char*>(reinterpret_cast<const void*>(base + bcd->typeDescriptor + 16));
    }
    return r;
}

// BaseMenu's layout: vptr, int id, UISystemMenu*, std::vector<UISystemMenuItem>.
struct BaseMenuLayout {
    void** vtbl;
    int32_t id;
    void* menu;
    void* items[3];
};
void* const kPure[4] = {nullptr, nullptr, nullptr, nullptr};

} // namespace

extern "C" {

// ---- the ModSettings API (exported under the MSVC names by the .def file)

void* BaseMenuCtor(void* self) {
    auto* m = static_cast<BaseMenuLayout*>(self);
    m->vtbl = const_cast<void**>(kPure);
    m->id = g_nextId++;
    m->menu = nullptr;
    m->items[0] = m->items[1] = m->items[2] = nullptr;
    return self;
}

void BaseMenuHeader(void* self, const char* label) {
    if (!Readable(label)) g_errors += "bad header label; ";
    if (Page* p = PageOf(self)) {
        Item it;
        it.kind = 'H';
        it.label = label ? label : "";
        p->items.push_back(std::move(it));
    }
}

void BaseMenuToggle(void* self, const char* label, const char* desc, int* toggled, MsvcFunc* onChange) {
    if (!Readable(label) || !Readable(desc) || !toggled) g_errors += "bad toggle; ";
    MsvcFunc* cb = Keep(onChange);
    if (Page* p = PageOf(self)) {
        Item it;
        it.kind = 'T';
        it.label = label;
        it.desc = desc;
        it.ivalue = toggled;
        it.cb = cb;
        p->items.push_back(std::move(it));
    } else {
        Drop(cb);
    }
}

void BaseMenuOption(void* self, const char* label, const char* desc, int* selected, const char** options, int count,
                    MsvcFunc* onChange) {
    if (!Readable(label) || !Readable(desc) || !selected || !options || count < 1 || count > 32)
        g_errors += "bad option; ";
    MsvcFunc* cb = Keep(onChange);
    if (Page* p = PageOf(self)) {
        Item it;
        it.kind = 'O';
        it.label = label;
        it.desc = desc;
        it.ivalue = selected;
        for (int i = 0; i < count; ++i) {
            if (!Readable(options[i])) g_errors += "bad option name; ";
            else it.options.push_back(options[i]);
        }
        it.cb = cb;
        p->items.push_back(std::move(it));
    } else {
        Drop(cb);
    }
}

void BaseMenuSlider(void* self, const char* label, const char* desc, float* value, SliderOpts* opts, MsvcFunc* onChange) {
    if (!Readable(label) || !Readable(desc) || !value || !opts) g_errors += "bad slider; ";
    MsvcFunc* cb = Keep(onChange);
    if (Page* p = PageOf(self)) {
        Item it;
        it.kind = 'S';
        it.label = label;
        it.desc = desc;
        it.fvalue = value;
        it.slider = *opts;
        it.cb = cb;
        p->items.push_back(std::move(it));
    } else {
        Drop(cb);
    }
}

void ModSettingsRegisterMenu(void* menu) {
    Page p;
    p.menu = menu;
    g_pages.push_back(std::move(p));
}

void ModSettingsRegisterSaveCallback(MsvcFunc* cb) { g_saves.push_back(Keep(cb)); }

// ---- driven by the test host

// Opens the pause menu: every registered page is (re)built. Writes a report.
__declspec(dllexport) int MockModSettings_Open(char* out, int size) {
    std::string r;
    char buf[256];
    for (Page& p : g_pages) {
        ClearPage(p);
        void** vt = *static_cast<void***>(p.menu);
        using CreateFn = void (*)(void*);
        using NameFn = const char* (*)(void*);
        using TypeFn = int (*)(void*);
        reinterpret_cast<CreateFn>(vt[0])(p.menu);
        const char* name = reinterpret_cast<NameFn>(vt[1])(p.menu);
        const char* desc = reinterpret_cast<NameFn>(vt[2])(p.menu);
        const int type = reinterpret_cast<TypeFn>(vt[3])(p.menu);
        int h = 0, t = 0, o = 0, s = 0;
        for (const Item& it : p.items) {
            h += it.kind == 'H';
            t += it.kind == 'T';
            o += it.kind == 'O';
            s += it.kind == 'S';
        }
        std::snprintf(buf, sizeof(buf), "page '%s' (type %d, id %d, %s description): %d headers, %d toggles, %d options, %d sliders;",
                      Readable(name) ? name : "?", type, static_cast<BaseMenuLayout*>(p.menu)->id,
                      Readable(desc) ? "with" : "no", h, t, o, s);
        r += buf;
        r += " rtti " + RttiOf(p.menu) + ";";
        for (const Item& it : p.items) {
            if (it.kind == 'T' || it.kind == 'O')
                std::snprintf(buf, sizeof(buf), " %s=%d", it.label.c_str(), *it.ivalue);
            else if (it.kind == 'S')
                std::snprintf(buf, sizeof(buf), " %s=%.2f[%g..%g|%d..%d]", it.label.c_str(), double(*it.fvalue),
                              double(it.slider.min), double(it.slider.max), it.slider.displayMin, it.slider.displayMax);
            else
                continue;
            r += buf;
        }
    }
    if (!g_errors.empty()) r += " ERRORS: " + g_errors;
    std::snprintf(out, size_t(size), "%s", r.c_str());
    return int(g_pages.size());
}

// A player changes an item: the value is written through the item's pointer,
// then its callback runs.
__declspec(dllexport) int MockModSettings_Set(const char* label, float value) {
    for (Page& p : g_pages)
        for (Item& it : p.items) {
            if (it.label != label) continue;
            if (it.ivalue) *it.ivalue = int(value);
            if (it.fvalue) *it.fvalue = value;
            if (it.cb) Call(*it.cb);
            return 1;
        }
    return 0;
}

// The pause menu closes: the mods are asked to save.
__declspec(dllexport) int MockModSettings_Save() {
    for (MsvcFunc* f : g_saves) Call(*f);
    return int(g_saves.size());
}

__declspec(dllexport) int MockModSettings_Calls() { return g_calls * 1000 + g_typeChecks; }

} // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
