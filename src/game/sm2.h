// Spider-Man 2 bindings: resolves signatures listed in bindings.ini and
// exposes the handful of engine features the mod uses.
//
// Everything known here comes from LDD565's SM2ScriptTemplate
// (github.com/hbgda/SM2ScriptTemplate, GPLv3): HeroSystem -> hero actor,
// the actor pool behind GetActor, Transform::SetPosition, the component
// registry and the Health component layout. Offsets live in bindings.ini so
// a game patch can be handled without recompiling.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../common/ini.h"
#include "../common/vec.h"

namespace sm2m {

struct BindingStatus {
    std::string name;
    bool ok = false;
    bool required = false;
    std::string detail;
};

struct Sm2Layout {
    uint32_t heroHandle = 0x14;
    uint32_t actorTransform = 0x00;
    uint32_t actorFlags = 0x08;
    uint32_t actorSerial = 0x08;       // u16: the handle's serial (bits 20-30), see GetActor
    uint32_t actorIndex = 0x0C;        // u32: the slot's index (handle bits 0-19)
    uint32_t actorName = 0xB0;
    uint32_t actorComponentMode = 0x88;
    uint32_t actorComponentsA = 0x58;
    uint32_t actorComponentsB = 0x80;
    uint32_t actorComponentList = 0x68;
    uint32_t actorComponentCount = 0x70;
    uint32_t componentInfoName = 0x60;
    // ComponentInfo: a type a lookup also matches derived types for (bit 6 of
    // the byte at +0xE4), and each type's list of the types it derives from
    // (pointers at +0x80, count byte at +0xE6) - the game's GetComponent.
    uint32_t componentInfoFlags = 0xE4;
    uint32_t componentInfoParents = 0x80;
    uint32_t componentInfoParentCount = 0xE6;
    uint32_t transformMatrix = 0x00;   // 3 rows of float4 (axes)
    uint32_t transformPosition = 0x30; // float3
    uint32_t transformFlags = 0x5C;    // u32: hidden bits, see Transform::Hide
    uint32_t transformHiddenBit = 0x20;   // the bit Transform::Hide sets
    uint32_t transformHiddenMask = 0x1E0; // any of these hides the actor
    uint32_t healthMax = 0xA0;
    uint32_t healthCurrent = 0xD0;
    uint32_t poolStride = 0xC0;
    int heroForwardRow = 2;            // which transform row is "forward"
    uint32_t actorLiveMask = 0;        // optional: (flags & mask) == mask for live actors (0 = don't check)
};

// Memory helpers that never fault (ReadProcessMemory on our own process).
bool SafeRead(uintptr_t addr, void* out, size_t size);
template <typename T>
bool SafeReadT(uintptr_t addr, T& out) {
    return SafeRead(addr, &out, sizeof(T));
}
bool SafeReadString(uintptr_t addr, std::string& out, size_t maxLen = 96);
bool SafeWrite(uintptr_t addr, const void* data, size_t size);
bool LooksLikePointer(uintptr_t p);

class Sm2Game {
public:
    // Scans the game executable. Returns true if the minimum set (hero +
    // transform access) resolved.
    bool Init(const Ini& bindings);
    const std::vector<BindingStatus>& Status() const { return status_; }
    const Sm2Layout& Layout() const { return layout_; }
    bool Ready() const { return ready_; }
    uintptr_t ModuleBase() const { return moduleBase_; }

    // Hero
    uintptr_t Hero() const;
    // His actor handle (0 if unknown).
    uint32_t HeroHandle() const;
    // Any actor by its handle (GetActor); 0 if it doesn't exist (any more).
    uintptr_t ActorFromHandle(uint32_t handle) const;
    bool GetTransform(uintptr_t actor, uintptr_t& transform) const;
    bool GetPosition(uintptr_t actor, DVec3& pos) const;
    bool GetMatrix(uintptr_t actor, float m[16]) const; // raw 64 bytes at transform
    bool SetMatrixRows(uintptr_t actor, const float rows[12]) const;
    void SetPosition(uintptr_t actor, const DVec3& pos) const;
    bool GetForward(uintptr_t actor, Vec3& fwd) const;
    // true: Transform::SetPosition (game function); false: write the floats.
    void SetUsePositionFunction(bool use) { usePositionFn_ = use; }
    bool PositionFunctionAvailable() const { return setPosition_ && !setPositionBroken_; }

    // Engine visibility: Transform::Hide/Unhide set or clear the transform's
    // "hidden" bit and flag it dirty, so the renderer skips the actor in every
    // pass (G-buffer, shadows, reflections). The game's scripts use the same
    // pair (HideOnEvent, CinematicHide, ...).
    bool HideAvailable() const { return hide_ && unhide_ && !hideBroken_; }
    bool SetHidden(uintptr_t actor, bool hidden) const;
    // The transform's flags dword (hidden bits are layout.transformHiddenMask).
    bool TransformFlags(uintptr_t actor, uint32_t& flags) const;

    // Components
    bool ComponentsAvailable() const {
        return getComponentInfo_ && registry_ && getComponent1_ && getComponent2_ && !componentsBroken_;
    }
    uintptr_t GetComponent(uintptr_t actor, const char* name) const;
    bool ReadHealth(uintptr_t actor, float& current, float& max) const;
    bool WriteHealth(uintptr_t actor, float current) const;
    bool ReadHealthComponent(uintptr_t comp, float& current, float& max) const;
    bool WriteHealthComponent(uintptr_t comp, float current) const;

    // Actor pool
    bool PoolAvailable() const { return poolBasePtr_ && poolCountPtr_; }
    uint32_t PoolCount() const;
    uintptr_t PoolBase() const;
    uintptr_t PoolSlot(uint32_t index) const;
    uint32_t PoolStride() const { return layout_.poolStride; }
    std::string ActorName(uintptr_t actor) const;
    uint32_t ActorSerial(uintptr_t actor) const;
    // The game's ActorHandle for an actor ((serial << 20) | slot), 0 if none.
    uint32_t ActorHandleOf(uintptr_t actor) const;

    // Pure memory checks/lookups - never call game code, so they are safe on
    // pool slots that may be free or stale.
    bool LooksLikeLiveActor(uintptr_t actor) const;
    // Walks the actor's component list for a component by name - of that
    // type or one derived from it (an enemy's health is a subclass of
    // "Health"), the way the game's GetComponent matches.
    uintptr_t FindComponentInList(uintptr_t actor, const char* name) const;
    // The game's ComponentInfo for a component name (cached), or 0.
    uintptr_t ComponentInfoFor(const char* name) const;
    // For actors that aren't known-good: list walk first, game lookup only
    // after LooksLikeLiveActor() passed.
    uintptr_t GetComponentChecked(uintptr_t actor, const char* name) const;

    // Debug dumps (to the log)
    void DumpHeroComponents() const;
    void DumpComponentRegistry(size_t maxEntries) const;
    void DumpActorsNear(const DVec3& center, float radius, size_t maxEntries) const;

    // Address of a pattern described in bindings.ini [section]; 0 if absent.
    // `match` (optional) receives where the pattern itself matched.
    uintptr_t ResolveSection(const Ini& ini, const std::string& section, std::string& detail,
                             uintptr_t* match = nullptr) const;
    // Is [addr, addr + size) inside Spider-Man2.exe's image?
    bool InImage(uintptr_t addr, size_t size) const;
    // Where the game calls `fn` right after "lea rcx, [rip+X]" (fn's object): X, or 0.
    uintptr_t FindRipLoadBeforeCall(uintptr_t fn) const;
    // ResolveSection for a function: logged like the built-in bindings and
    // checked to be the start of a function (0 if it isn't).
    uintptr_t ResolveFunction(const Ini& ini, const std::string& section, std::string& detail,
                              uintptr_t* match = nullptr);
    // Transform::SetPosition, if resolved and not known to be broken.
    uintptr_t SetPositionAddress() const { return setPositionBroken_ ? 0 : setPosition_; }
    // Transform::Unhide, if resolved (with Hide).
    uintptr_t UnhideAddress() const { return hideBroken_ ? 0 : unhide_; }

private:
    bool ScanText(const std::string& patternText, int64_t& offset, int& matches, std::string& error) const;
    void Add(const std::string& name, bool ok, bool required, const std::string& detail);
    // Logs the bytes around a function binding and makes sure it points at a
    // function start (see ValidateFunctionStart). May move or clear `addr`.
    void CheckFunction(const Ini& ini, const char* name, uintptr_t& addr);

    std::vector<BindingStatus> status_;
    Sm2Layout layout_;
    bool ready_ = false;
    uintptr_t moduleBase_ = 0;
    size_t imageSize_ = 0;
    std::vector<std::pair<uintptr_t, size_t>> textSections_;

    uintptr_t heroSystem_ = 0;
    uintptr_t getActor_ = 0;
    uintptr_t poolBasePtr_ = 0;  // address of the global holding the actor array pointer
    uintptr_t poolCountPtr_ = 0; // address of the global holding the slot count
    uintptr_t setPosition_ = 0;
    uintptr_t hide_ = 0;
    uintptr_t unhide_ = 0;
    uintptr_t registry_ = 0;
    uintptr_t getComponentInfo_ = 0;
    uintptr_t getComponent1_ = 0;
    uintptr_t getComponent2_ = 0;
    mutable uintptr_t healthInfo_ = 0;
    // (component type, type asked for) -> derives from it; by name without the registry
    mutable std::map<std::pair<uintptr_t, uintptr_t>, bool> derivedCache_;
    mutable std::map<std::pair<uintptr_t, std::string>, bool> nameCache_;
    bool InfoNamedOrDerived(uintptr_t info, const char* name) const;
    bool usePositionFn_ = true;
    // Set when a call into the game faulted (caught by the crash guard): that
    // binding is not used again this session.
    mutable bool getActorBroken_ = false;
    mutable bool setPositionBroken_ = false;
    mutable bool componentsBroken_ = false;
    mutable bool hideBroken_ = false;
    // ComponentInfo* -> name, for list walks (ComponentInfo objects are static).
    mutable std::vector<std::pair<uintptr_t, std::string>> infoNames_;
    // name -> ComponentInfo* (0: the registry didn't know it)
    mutable std::vector<std::pair<std::string, uintptr_t>> infoByName_;
    bool InfoMatches(uintptr_t entryInfo, uintptr_t target, bool derived) const;
    const std::string* InfoName(uintptr_t info) const;
};

// Same CRC as the game's component registry (per SM2ScriptTemplate): table
// driven CRC-32 with the polynomial as the initial value and no final xor.
uint32_t Sm2StringCrc32(const char* s);

} // namespace sm2m

#endif
