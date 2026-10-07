#ifdef _WIN32

#include "sm2.h"

#include <cmath>
#include <cstring>

#include "../common/log.h"
#include "../common/platform.h"
#include "../win/guard.h"
#include "pattern.h"

namespace sm2m {

// ---------------------------------------------------------------- memory

bool LooksLikePointer(uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFF0000ull && (p & 3) == 0; }

bool SafeRead(uintptr_t addr, void* out, size_t size) {
    if (!LooksLikePointer(addr & ~uintptr_t(3))) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
}

bool SafeWrite(uintptr_t addr, const void* data, size_t size) {
    if (!LooksLikePointer(addr & ~uintptr_t(3))) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), data, size, &put) && put == size;
}

bool SafeReadString(uintptr_t addr, std::string& out, size_t maxLen) {
    out.clear();
    char buf[128];
    if (maxLen > sizeof(buf) - 1) maxLen = sizeof(buf) - 1;
    // Read in small steps so a string near the end of a page still works.
    size_t got = 0;
    while (got < maxLen) {
        size_t chunk = std::min<size_t>(16, maxLen - got);
        if (!SafeRead(addr + got, buf + got, chunk)) break;
        bool end = false;
        for (size_t i = 0; i < chunk; ++i)
            if (buf[got + i] == 0) end = true;
        got += chunk;
        if (end) break;
    }
    if (got == 0) return false;
    buf[got] = 0;
    for (size_t i = 0; i < got && buf[i]; ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c < 0x20 || c > 0x7E) return !out.empty();
        out.push_back(char(c));
    }
    return !out.empty();
}

uint32_t Sm2StringCrc32(const char* s) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xEDB88320u;
    for (; *s; ++s) crc = (crc >> 8) ^ table[(static_cast<unsigned char>(*s) ^ crc) & 0xFF];
    return crc;
}

// ---------------------------------------------------------------- scanning

void Sm2Game::Add(const std::string& name, bool ok, bool required, const std::string& detail) {
    status_.push_back({name, ok, required, detail});
    if (ok) LOGI("binding %-22s ok   %s", name.c_str(), detail.c_str());
    else if (required) LOGE("binding %-22s FAIL %s", name.c_str(), detail.c_str());
    else LOGW("binding %-22s --   %s", name.c_str(), detail.c_str());
}

bool Sm2Game::ScanText(const std::string& text, int64_t& offset, int& matches, std::string& error) const {
    Pattern p = Pattern::Parse(text);
    if (!p.valid) {
        error = "invalid pattern";
        return false;
    }
    matches = 0;
    offset = -1;
    for (const auto& sec : textSections_) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(sec.first);
        int64_t off = FindPattern(data, sec.second, p);
        if (off < 0) continue;
        if (matches == 0) offset = int64_t(sec.first - moduleBase_) + off;
        matches += CountPattern(data, sec.second, p, 4);
    }
    if (matches == 0) {
        error = "pattern not found (game updated?)";
        return false;
    }
    return true;
}

bool Sm2Game::InImage(uintptr_t addr, size_t size) const {
    return moduleBase_ && addr >= moduleBase_ && addr + size <= moduleBase_ + imageSize_ && addr + size >= addr;
}

uintptr_t Sm2Game::ResolveSection(const Ini& ini, const std::string& section, std::string& detail,
                                  uintptr_t* match) const {
    if (match) *match = 0;
    std::string text = ini.GetString(section, "pattern");
    if (text.empty()) {
        detail = "no pattern in bindings.ini";
        return 0;
    }
    int64_t off;
    int matches;
    if (!ScanText(text, off, matches, detail)) return 0;
    Resolve mode;
    if (!ParseResolve(ini.GetString(section, "resolve", "direct"), mode)) {
        detail = "bad 'resolve' value";
        return 0;
    }
    Pattern p = Pattern::Parse(text);
    if (match) *match = moduleBase_ + uintptr_t(off);
    uintptr_t addr = ResolveMatch(moduleBase_ + uintptr_t(off), p, mode, ini.GetInt(section, "offset", 0));
    char buf[160];
    std::snprintf(buf, sizeof(buf), "exe+0x%llx%s", (unsigned long long)(addr - moduleBase_),
                  matches > 1 ? " (WARNING: pattern matches more than once, using the first)" : "");
    detail = buf;
    return addr;
}

uintptr_t Sm2Game::ResolveFunction(const Ini& ini, const std::string& section, std::string& detail,
                                   uintptr_t* match) {
    uintptr_t addr = ResolveSection(ini, section, detail, match);
    Add(section, addr != 0, false, detail);
    CheckFunction(ini, section.c_str(), addr);
    return addr;
}

void Sm2Game::CheckFunction(const Ini& ini, const char* name, uintptr_t& addr) {
    if (!addr) return;
    uint8_t ctx[16 + 24];
    if (!SafeRead(addr - 16, ctx, sizeof(ctx))) {
        LOGE("binding %s: code at exe+0x%llx is unreadable - not using it", name,
             (unsigned long long)(addr - moduleBase_));
        addr = 0;
        return;
    }
    // Bytes around the match: lets a log from any game version be checked by hand.
    char before[16 * 3 + 1], after[24 * 3 + 1];
    for (int i = 0; i < 16; ++i) std::snprintf(before + i * 3, 4, "%02X ", ctx[i]);
    for (int i = 0; i < 24; ++i) std::snprintf(after + i * 3, 4, "%02X ", ctx[16 + i]);
    LOGI("  %-20s %s| %s", name, before, after);
    if (!ini.GetBool(name, "validate", true)) {
        LOGW("binding %s: start-of-function check disabled in bindings.ini", name);
        return;
    }
    const uintptr_t start = ValidateFunctionStart(addr, ctx);
    if (start == addr) return;
    if (start) {
        LOGW("binding %s: the pattern matched %d bytes into a function (this game version added prologue code); "
             "using the function start exe+0x%llx",
             name, int(addr - start), (unsigned long long)(start - moduleBase_));
        addr = start;
    } else {
        LOGE("binding %s: exe+0x%llx is not the start of a function in this game version - not calling it", name,
             (unsigned long long)(addr - moduleBase_));
        addr = 0;
    }
}

bool Sm2Game::Init(const Ini& ini) {
    status_.clear();
    HMODULE exe = GetModuleHandleW(nullptr);
    moduleBase_ = reinterpret_cast<uintptr_t>(exe);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase_);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(moduleBase_ + dos->e_lfanew);
    imageSize_ = size_t(nt->OptionalHeader.SizeOfImage);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            textSections_.push_back({moduleBase_ + sec[i].VirtualAddress, size_t(sec[i].Misc.VirtualSize)});
    }
    LOGI("game module base %p, %zu code section(s)", reinterpret_cast<void*>(moduleBase_), textSections_.size());

    // Layout overrides.
    auto lay = [&](const char* key, uint32_t& field) {
        if (ini.Has("Layout", key)) field = uint32_t(ini.GetInt("Layout", key, field));
    };
    lay("hero_handle", layout_.heroHandle);
    lay("actor_transform", layout_.actorTransform);
    lay("actor_flags", layout_.actorFlags);
    lay("actor_serial", layout_.actorSerial);
    lay("actor_index", layout_.actorIndex);
    lay("actor_name", layout_.actorName);
    lay("actor_component_mode", layout_.actorComponentMode);
    lay("actor_components_a", layout_.actorComponentsA);
    lay("actor_components_b", layout_.actorComponentsB);
    lay("actor_component_list", layout_.actorComponentList);
    lay("actor_component_count", layout_.actorComponentCount);
    lay("component_info_name", layout_.componentInfoName);
    lay("component_info_flags", layout_.componentInfoFlags);
    lay("component_info_parents", layout_.componentInfoParents);
    lay("component_info_parent_count", layout_.componentInfoParentCount);
    lay("transform_matrix", layout_.transformMatrix);
    lay("transform_position", layout_.transformPosition);
    lay("transform_flags", layout_.transformFlags);
    lay("transform_hidden_bit", layout_.transformHiddenBit);
    lay("transform_hidden_mask", layout_.transformHiddenMask);
    lay("health_max", layout_.healthMax);
    lay("health_current", layout_.healthCurrent);
    lay("pool_stride", layout_.poolStride);
    lay("actor_live_mask", layout_.actorLiveMask);
    layout_.heroForwardRow = int(ini.GetInt("Layout", "hero_forward_row", layout_.heroForwardRow));

    std::string d;
    heroSystem_ = ResolveSection(ini, "HeroSystem", d);
    Add("HeroSystem", heroSystem_ != 0, true, d);

    getActor_ = ResolveSection(ini, "GetActor", d);
    Add("GetActor", getActor_ != 0, true, d);
    if (getActor_) {
        // add rcx,[rip+X] / cmp r8d,[rip+Y] inside GetActor give the pool.
        int64_t baseDisp = ini.GetInt("GetActor", "pool_base_disp", 35);
        int64_t countDisp = ini.GetInt("GetActor", "pool_count_disp", 42);
        poolBasePtr_ = RipTarget(getActor_ + uintptr_t(baseDisp));
        poolCountPtr_ = RipTarget(getActor_ + uintptr_t(countDisp));
        char buf[128];
        std::snprintf(buf, sizeof(buf), "base@exe+0x%llx count@exe+0x%llx",
                      (unsigned long long)(poolBasePtr_ - moduleBase_),
                      (unsigned long long)(poolCountPtr_ - moduleBase_));
        Add("ActorPool", true, false, buf);
        // movzx eax, word ptr [rcx+N] in GetActor: where an actor keeps the
        // serial its handle is checked against (0.4 had it wrong).
        // ([Layout] actor_serial, if set, wins - with a warning when the code
        // disagrees.)
        const int64_t serialDisp = ini.GetInt("GetActor", "serial_disp", -1);
        uint8_t code[4] = {};
        if (serialDisp > 0 && serialDisp < 64 && SafeRead(getActor_ + uintptr_t(serialDisp) - 3, code, 4) &&
            code[0] == 0x0F && code[1] == 0xB7 && code[2] == 0x41 && code[3] != layout_.actorSerial) {
            if (ini.Has("Layout", "actor_serial")) {
                LOGW("layout: the game's GetActor reads an actor's serial at +0x%X, but [Layout] actor_serial says +0x%X "
                     "- using the setting", code[3], layout_.actorSerial);
            } else {
                LOGW("layout: the game's GetActor reads an actor's serial at +0x%X, not +0x%X - using +0x%X", code[3],
                     layout_.actorSerial, code[3]);
                layout_.actorSerial = code[3];
            }
        }
        // Only after the pool offsets (relative to the match) were taken.
        CheckFunction(ini, "GetActor", getActor_);
    }

    setPosition_ = ResolveSection(ini, "TransformSetPosition", d);
    Add("TransformSetPosition", setPosition_ != 0, false, setPosition_ ? d : d + " (falling back to direct writes)");
    CheckFunction(ini, "TransformSetPosition", setPosition_);

    hide_ = ResolveSection(ini, "TransformHide", d);
    Add("TransformHide", hide_ != 0, false, hide_ ? d : d + " (Spider-Man can't be hidden)");
    CheckFunction(ini, "TransformHide", hide_);
    unhide_ = ResolveSection(ini, "TransformUnhide", d);
    Add("TransformUnhide", unhide_ != 0, false, d);
    CheckFunction(ini, "TransformUnhide", unhide_);
    if (!hide_ || !unhide_) hide_ = unhide_ = 0; // only ever use them as a pair

    registry_ = ResolveSection(ini, "ComponentRegistry", d);
    Add("ComponentRegistry", registry_ != 0, false, d);
    getComponentInfo_ = ResolveSection(ini, "GetComponentInfo", d);
    Add("GetComponentInfo", getComponentInfo_ != 0, false, d);
    CheckFunction(ini, "GetComponentInfo", getComponentInfo_);
    getComponent1_ = ResolveSection(ini, "GetComponent1", d);
    Add("GetComponent1", getComponent1_ != 0, false, d);
    CheckFunction(ini, "GetComponent1", getComponent1_);
    getComponent2_ = ResolveSection(ini, "GetComponent2", d);
    Add("GetComponent2", getComponent2_ != 0, false, d);
    CheckFunction(ini, "GetComponent2", getComponent2_);

    ready_ = heroSystem_ && getActor_;
    if (!getActor_ && heroSystem_) LOGE("GetActor is unusable in this game version - Mario Mode can't find Spider-Man");
    return ready_;
}

// ---------------------------------------------------------------- actors

uintptr_t Sm2Game::Hero() const {
    if (!heroSystem_ || !getActor_ || getActorBroken_) return 0;
    uint32_t handle = 0;
    if (!SafeReadT(heroSystem_ + layout_.heroHandle, handle) || handle == 0) return 0;
    return ActorFromHandle(handle);
}

uint32_t Sm2Game::HeroHandle() const {
    uint32_t handle = 0;
    if (!heroSystem_ || !SafeReadT(heroSystem_ + layout_.heroHandle, handle)) return 0;
    return handle;
}

uintptr_t Sm2Game::FindRipLoadBeforeCall(uintptr_t fn) const {
    if (!fn) return 0;
    for (const auto& sec : textSections_) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(sec.first);
        for (size_t i = 7; i + 5 <= sec.second; ++i) {
            if (p[i] != 0xE8) continue;
            int32_t rel;
            std::memcpy(&rel, p + i + 1, 4);
            if (sec.first + i + 5 + uintptr_t(intptr_t(rel)) != fn) continue;
            if (p[i - 7] != 0x48 || p[i - 6] != 0x8D || p[i - 5] != 0x0D) continue; // lea rcx, [rip+X]
            return RipTarget(sec.first + i - 4);
        }
    }
    return 0;
}

uintptr_t Sm2Game::ActorFromHandle(uint32_t handle) const {
    if (!getActor_ || getActorBroken_ || handle == 0) return 0;
    using GetActorFn = uintptr_t (*)(const uint32_t*);
    const uintptr_t fn = getActor_;
    uintptr_t actor = 0;
    auto call = [&] { actor = reinterpret_cast<GetActorFn>(fn)(&handle); };
    guard::Fault fault;
    {
        guard::PhaseScope phase("GetActor");
        if (guard::Call(call, &fault)) return LooksLikePointer(actor) ? actor : 0;
    }
    getActorBroken_ = true;
    LOGE("GetActor crashed and is now disabled: %s", guard::Describe(fault).c_str());
    return 0;
}

bool Sm2Game::GetTransform(uintptr_t actor, uintptr_t& t) const {
    if (!actor) return false;
    if (!SafeReadT(actor + layout_.actorTransform, t)) return false;
    return LooksLikePointer(t);
}

bool Sm2Game::GetPosition(uintptr_t actor, DVec3& pos) const {
    uintptr_t t;
    if (!GetTransform(actor, t)) return false;
    float p[3];
    if (!SafeRead(t + layout_.transformPosition, p, sizeof(p))) return false;
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    pos = DVec3(p[0], p[1], p[2]);
    return true;
}

bool Sm2Game::GetMatrix(uintptr_t actor, float m[16]) const {
    uintptr_t t;
    if (!GetTransform(actor, t)) return false;
    return SafeRead(t + layout_.transformMatrix, m, 64);
}

bool Sm2Game::SetMatrixRows(uintptr_t actor, const float rows[12]) const {
    uintptr_t t;
    if (!GetTransform(actor, t)) return false;
    return SafeWrite(t + layout_.transformMatrix, rows, 48);
}

void Sm2Game::SetPosition(uintptr_t actor, const DVec3& pos) const {
    uintptr_t t;
    if (!GetTransform(actor, t)) return;
    float p[3] = {float(pos.x), float(pos.y), float(pos.z)};
    if (setPosition_ && usePositionFn_ && !setPositionBroken_) {
        using SetPosFn = void (*)(uintptr_t, const float*);
        const uintptr_t fn = setPosition_;
        auto call = [&] { reinterpret_cast<SetPosFn>(fn)(t, p); };
        guard::Fault fault;
        {
            guard::PhaseScope phase("Transform::SetPosition");
            if (guard::Call(call, &fault)) return;
        }
        setPositionBroken_ = true;
        LOGE("Transform::SetPosition crashed; switching to direct position writes: %s", guard::Describe(fault).c_str());
    }
    SafeWrite(t + layout_.transformPosition, p, sizeof(p));
}

bool Sm2Game::TransformFlags(uintptr_t actor, uint32_t& flags) const {
    uintptr_t t;
    return GetTransform(actor, t) && SafeReadT(t + layout_.transformFlags, flags);
}

bool Sm2Game::SetHidden(uintptr_t actor, bool hidden) const {
    if (!HideAvailable()) return false;
    uintptr_t t;
    if (!GetTransform(actor, t)) return false;
    // Both functions are leaf functions (flag bit + dirty bitset, which the
    // game updates with a locked OR), safe to call from the present thread.
    using Fn = void (*)(uintptr_t);
    const uintptr_t fn = hidden ? hide_ : unhide_;
    auto call = [&] { reinterpret_cast<Fn>(fn)(t); };
    guard::Fault fault;
    {
        guard::PhaseScope phase(hidden ? "Transform::Hide" : "Transform::Unhide");
        if (guard::Call(call, &fault)) return true;
    }
    hideBroken_ = true;
    LOGE("Transform::%s crashed and is now disabled: %s", hidden ? "Hide" : "Unhide", guard::Describe(fault).c_str());
    return false;
}

bool Sm2Game::GetForward(uintptr_t actor, Vec3& fwd) const {
    float m[16];
    if (!GetMatrix(actor, m)) return false;
    int r = layout_.heroForwardRow;
    if (r < 0 || r > 2) r = 2;
    fwd = Normalize(Vec3(m[4 * r], m[4 * r + 1], m[4 * r + 2]));
    return IsFinite(fwd) && Length(fwd) > 0.5f;
}

uint32_t Sm2Game::PoolCount() const {
    uint32_t n = 0;
    SafeReadT(poolCountPtr_, n);
    return n > 1000000 ? 0 : n;
}

uintptr_t Sm2Game::PoolBase() const {
    uintptr_t base = 0;
    if (!SafeReadT(poolBasePtr_, base) || !LooksLikePointer(base)) return 0;
    return base;
}

uintptr_t Sm2Game::PoolSlot(uint32_t index) const {
    const uintptr_t base = PoolBase();
    return base ? base + uintptr_t(index) * layout_.poolStride : 0;
}

bool Sm2Game::LooksLikeLiveActor(uintptr_t actor) const {
    if (!LooksLikePointer(actor)) return false;
    uint8_t head[0x18];
    if (!SafeRead(actor, head, sizeof(head))) return false;
    uintptr_t transform;
    uint32_t flags, index;
    uint16_t serial;
    std::memcpy(&transform, head + layout_.actorTransform, sizeof(transform));
    std::memcpy(&flags, head + layout_.actorFlags, sizeof(flags));
    std::memcpy(&serial, head + layout_.actorSerial, sizeof(serial));
    std::memcpy(&index, head + layout_.actorIndex, sizeof(index));
    if (!LooksLikePointer(transform)) return false;
    // GetActor: a handle is (serial << 20) | slot index, and it takes the
    // slot only if the slot's serial (the u16 at +8) is the handle's - never
    // 0. The slot also keeps its own index (+0xC).
    if ((serial & 0x7FF) == 0) return false;
    const uintptr_t base = PoolBase();
    if (base && actor >= base && layout_.poolStride &&
        (actor - base) / layout_.poolStride != uintptr_t(index & 0xFFFFF))
        return false;
    if (layout_.actorLiveMask && (flags & layout_.actorLiveMask) != layout_.actorLiveMask) return false;
    uint8_t mode = 0xFF;
    if (!SafeReadT(actor + layout_.actorComponentMode, mode) || mode > 1) return false;
    DVec3 p;
    return GetPosition(actor, p);
}

const std::string* Sm2Game::InfoName(uintptr_t info) const {
    for (const auto& kv : infoNames_)
        if (kv.first == info) return &kv.second;
    uintptr_t namePtr = 0;
    std::string fresh;
    if (!SafeReadT(info + layout_.componentInfoName, namePtr) || !SafeReadString(namePtr, fresh)) return nullptr;
    if (infoNames_.size() >= 4096) infoNames_.erase(infoNames_.begin());
    infoNames_.push_back({info, fresh});
    return &infoNames_.back().second;
}

uintptr_t Sm2Game::ComponentInfoFor(const char* name) const {
    for (const auto& kv : infoByName_)
        if (kv.first == name) return kv.second;
    uintptr_t info = 0;
    if (registry_ && getComponentInfo_ && !componentsBroken_) {
        using InfoFn = uintptr_t (*)(uintptr_t, uint32_t);
        const uint32_t crc = Sm2StringCrc32(name);
        auto call = [&] { info = reinterpret_cast<InfoFn>(getComponentInfo_)(registry_, crc); };
        guard::Fault fault;
        bool ok;
        {
            guard::PhaseScope phase("GetComponentInfo");
            ok = guard::Call(call, &fault);
        }
        if (!ok) {
            componentsBroken_ = true;
            LOGE("the game's component lookup crashed and is now disabled: %s", guard::Describe(fault).c_str());
            return 0;
        }
        if (!LooksLikePointer(info)) info = 0;
    }
    if (infoByName_.size() < 64) infoByName_.push_back({name, info});
    return info;
}

// The game's GetComponent: the entry's type is the one asked for, or - for a
// type that matches its subclasses - one of the entry type's base types is.
bool Sm2Game::InfoMatches(uintptr_t entryInfo, uintptr_t target, bool derived) const {
    if (entryInfo == target) return true;
    if (!derived || !LooksLikePointer(entryInfo)) return false;
    // (Component types are registered once: their base types never change.)
    const auto key = std::make_pair(entryInfo, target);
    const auto hit = derivedCache_.find(key);
    if (hit != derivedCache_.end()) return hit->second;
    bool match = false;
    uint8_t n = 0;
    if (SafeReadT(entryInfo + layout_.componentInfoParentCount, n) && n > 0) {
        uintptr_t parents[64];
        const size_t count = std::min<size_t>(n, 64);
        if (SafeRead(entryInfo + layout_.componentInfoParents, parents, count * sizeof(uintptr_t)))
            for (size_t i = 0; i < count && !match; ++i) match = parents[i] == target;
    }
    if (derivedCache_.size() > 16384) derivedCache_.clear();
    derivedCache_[key] = match;
    return match;
}

bool Sm2Game::InfoNamedOrDerived(uintptr_t info, const char* name) const {
    const auto key = std::make_pair(info, std::string(name));
    const auto hit = nameCache_.find(key);
    if (hit != nameCache_.end()) return hit->second;
    const std::string* known = InfoName(info);
    bool match = known && *known == name;
    if (!match) {
        uint8_t n = 0;
        uintptr_t parents[64];
        if (SafeReadT(info + layout_.componentInfoParentCount, n) && n > 0 &&
            SafeRead(info + layout_.componentInfoParents, parents, std::min<size_t>(n, 64) * sizeof(uintptr_t)))
            for (size_t k = 0; k < std::min<size_t>(n, 64) && !match; ++k)
                if (LooksLikePointer(parents[k]))
                    if (const std::string* pn = InfoName(parents[k])) match = *pn == name;
    }
    if (!known) return match; // (unreadable now: not cached)
    if (nameCache_.size() > 16384) nameCache_.clear();
    nameCache_[key] = match;
    return match;
}

uintptr_t Sm2Game::FindComponentInList(uintptr_t actor, const char* name) const {
    uintptr_t list = 0;
    uint16_t count = 0;
    if (!SafeReadT(actor + layout_.actorComponentList, list) || !SafeReadT(actor + layout_.actorComponentCount, count))
        return 0;
    if (!LooksLikePointer(list) || count == 0 || count > 512) return 0;
    struct Entry {
        uintptr_t info, component;
    };
    Entry entries[512];
    if (!SafeRead(list, entries, sizeof(Entry) * count)) return 0;
    const uintptr_t target = ComponentInfoFor(name);
    if (target) {
        uint8_t flags = 0;
        SafeReadT(target + layout_.componentInfoFlags, flags);
        const bool derived = (flags & 0x40) != 0;
        for (uint16_t i = 0; i < count; ++i)
            if (InfoMatches(entries[i].info, target, derived))
                return LooksLikePointer(entries[i].component) ? entries[i].component : 0;
        return 0;
    }
    // No registry: by name - the entry's type, or one of its base types.
    for (uint16_t i = 0; i < count; ++i) {
        const uintptr_t info = entries[i].info;
        if (!LooksLikePointer(info)) continue;
        if (InfoNamedOrDerived(info, name)) return LooksLikePointer(entries[i].component) ? entries[i].component : 0;
    }
    return 0;
}

uintptr_t Sm2Game::GetComponentChecked(uintptr_t actor, const char* name) const {
    if (!LooksLikeLiveActor(actor)) return 0;
    const uintptr_t c = FindComponentInList(actor, name);
    if (c) return c;
    // Actors in the alternative component storage (mode byte set) aren't in
    // the list; only then fall back to the game's own lookup.
    uint8_t mode = 0;
    if (SafeReadT(actor + layout_.actorComponentMode, mode) && mode == 1) return GetComponent(actor, name);
    return 0;
}

std::string Sm2Game::ActorName(uintptr_t actor) const {
    uintptr_t p = 0;
    std::string s;
    if (SafeReadT(actor + layout_.actorName, p) && LooksLikePointer(p)) SafeReadString(p, s);
    return s;
}

uint32_t Sm2Game::ActorHandleOf(uintptr_t actor) const {
    uint16_t serial = 0;
    uint32_t index = 0;
    if (!SafeReadT(actor + layout_.actorSerial, serial) || !SafeReadT(actor + layout_.actorIndex, index)) return 0;
    if ((serial & 0x7FF) == 0) return 0;
    return (uint32_t(serial & 0x7FF) << 20) | (index & 0xFFFFF);
}

// The actor's handle: it changes when the slot is reused (a new serial).
uint32_t Sm2Game::ActorSerial(uintptr_t actor) const { return ActorHandleOf(actor); }

// ---------------------------------------------------------------- components

uintptr_t Sm2Game::GetComponent(uintptr_t actor, const char* name) const {
    if (!actor || !ComponentsAvailable()) return 0;
    using InfoFn = uintptr_t (*)(uintptr_t, uint32_t);
    using CompFn = uintptr_t (*)(uintptr_t, uintptr_t);
    uint8_t mode = 0;
    if (!SafeReadT(actor + layout_.actorComponentMode, mode)) return 0;
    const bool isHealth = std::strcmp(name, "Health") == 0;
    const uint32_t crc = Sm2StringCrc32(name);
    uintptr_t comp = 0;
    auto call = [&] {
        uintptr_t info = isHealth ? healthInfo_ : 0;
        if (!info) {
            info = reinterpret_cast<InfoFn>(getComponentInfo_)(registry_, crc);
            if (isHealth && LooksLikePointer(info)) healthInfo_ = info;
        }
        if (!LooksLikePointer(info)) return;
        comp = mode == 0 ? reinterpret_cast<CompFn>(getComponent1_)(actor + layout_.actorComponentsA, info)
                         : reinterpret_cast<CompFn>(getComponent2_)(actor + layout_.actorComponentsB, info);
    };
    guard::Fault fault;
    {
        guard::PhaseScope phase("GetComponent");
        if (guard::Call(call, &fault)) return LooksLikePointer(comp) ? comp : 0;
    }
    componentsBroken_ = true;
    LOGE("the game's component lookup crashed and is now disabled: %s", guard::Describe(fault).c_str());
    return 0;
}

bool Sm2Game::ReadHealthComponent(uintptr_t comp, float& cur, float& max) const {
    if (!comp) return false;
    if (!SafeReadT(comp + layout_.healthCurrent, cur) || !SafeReadT(comp + layout_.healthMax, max)) return false;
    return std::isfinite(cur) && std::isfinite(max) && max > 0 && max < 1e7f && cur >= -1e6f && cur <= max * 4;
}

bool Sm2Game::WriteHealthComponent(uintptr_t comp, float cur) const {
    if (!comp) return false;
    return SafeWrite(comp + layout_.healthCurrent, &cur, sizeof(cur));
}

bool Sm2Game::ReadHealth(uintptr_t actor, float& cur, float& max) const {
    return ReadHealthComponent(GetComponent(actor, "Health"), cur, max);
}

bool Sm2Game::WriteHealth(uintptr_t actor, float cur) const {
    return WriteHealthComponent(GetComponent(actor, "Health"), cur);
}

// ---------------------------------------------------------------- dumps

void Sm2Game::DumpHeroComponents() const {
    uintptr_t hero = Hero();
    LOGI("=== hero actor %p name='%s'", reinterpret_cast<void*>(hero), hero ? ActorName(hero).c_str() : "");
    if (!hero) return;
    uintptr_t t = 0;
    GetTransform(hero, t);
    float m[16] = {};
    GetMatrix(hero, m);
    LOGI("transform %p", reinterpret_cast<void*>(t));
    for (int r = 0; r < 4; ++r) LOGI("  row%d % .4f % .4f % .4f % .4f", r, m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    uintptr_t list = 0;
    uint16_t count = 0;
    SafeReadT(hero + layout_.actorComponentList, list);
    SafeReadT(hero + layout_.actorComponentCount, count);
    LOGI("components: %u at %p", count, reinterpret_cast<void*>(list));
    for (uint32_t i = 0; i < count && i < 256; ++i) {
        uintptr_t entry[2] = {0, 0}; // {ComponentInfo*, Component*}
        if (!SafeRead(list + i * 16, entry, sizeof(entry))) break;
        uintptr_t namePtr = 0;
        std::string name;
        if (SafeReadT(entry[0] + layout_.componentInfoName, namePtr)) SafeReadString(namePtr, name);
        LOGI("  [%3u] %-40s component=%p", i, name.c_str(), reinterpret_cast<void*>(entry[1]));
    }
}

void Sm2Game::DumpComponentRegistry(size_t maxEntries) const {
    if (!registry_ || !getComponentInfo_) {
        LOGW("component registry not resolved");
        return;
    }
    // HashTable<uint32,uint32>: keys*, values*, count, max_count (SM2ScriptTemplate)
    uintptr_t keys = 0;
    uint32_t maxCount = 0;
    SafeReadT(registry_, keys);
    SafeReadT(registry_ + 0x14, maxCount);
    LOGI("=== component registry: %u slots", maxCount);
    using InfoFn = uintptr_t (*)(uintptr_t, uint32_t);
    size_t printed = 0;
    for (uint32_t i = 0; i < maxCount && i < 100000 && printed < maxEntries; ++i) {
        uint32_t key = 0;
        if (!SafeReadT(keys + i * 4ull, key) || key == 0) continue;
        uintptr_t info = reinterpret_cast<InfoFn>(getComponentInfo_)(registry_, key);
        uintptr_t namePtr = 0;
        std::string name;
        if (LooksLikePointer(info) && SafeReadT(info + layout_.componentInfoName, namePtr)) SafeReadString(namePtr, name);
        LOGI("  %08x %s", key, name.c_str());
        ++printed;
    }
}

void Sm2Game::DumpActorsNear(const DVec3& c, float radius, size_t maxEntries) const {
    if (!PoolAvailable()) {
        LOGW("actor pool not resolved");
        return;
    }
    const uint32_t n = PoolCount();
    LOGI("=== actors within %.1f of (%.2f %.2f %.2f), pool slots=%u", radius, c.x, c.y, c.z, n);
    size_t printed = 0;
    for (uint32_t i = 0; i < n && printed < maxEntries; ++i) {
        uintptr_t a = PoolSlot(i);
        DVec3 p;
        if (!a || !GetPosition(a, p)) continue;
        double d = Length(p - c);
        if (d > radius) continue;
        float cur = 0, mx = 0;
        bool hasHealth = ReadHealth(a, cur, mx);
        uint32_t flags = 0;
        SafeReadT(a + layout_.actorFlags, flags);
        LOGI("  #%-5u %6.2fm flags=%08x %-48s pos=(%.2f %.2f %.2f)%s", i, d, flags, ActorName(a).c_str(), p.x, p.y,
             p.z, hasHealth ? (" health " + std::to_string(int(cur)) + "/" + std::to_string(int(mx))).c_str() : "");
        ++printed;
    }
}

} // namespace sm2m

#endif
