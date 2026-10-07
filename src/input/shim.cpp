#ifdef _WIN32

#include <atomic>
#include <cstring>
#include <xinput.h>

#include "../common/log.h"
#include "input.h"
#include "MinHook.h"

namespace sm2m::input_shim {
namespace {

std::atomic<bool> g_block{false};
std::atomic<bool> g_menu{false}; // Mario's menu is open: it gets the navigation keys
bool g_alwaysBlocked[256] = {};
std::atomic<bool> g_alsoBlocked[256]; // for now (photo mode: the pose key)
bool g_pass[256] = {};
// Only Mario's own controls are kept from the game (else: everything but g_pass).
std::atomic<bool> g_selective{false};
bool g_marioKey[256] = {};
std::atomic<uint32_t> g_marioPad{0}; // PadButton bits (PAD_LT / PAD_RT: the triggers)
std::atomic<bool> g_marioStick{true};
// Hotkeys: per key, the modifier sets (bit 0 Ctrl, 1 Shift, 2 Alt) that make
// it one of Mario Mode's; bit 7 set = in use.
uint8_t g_hotkey[256][4] = {};
std::string g_status = "not installed";

bool MenuKey(unsigned vk) {
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN || vk == VK_ESCAPE ||
           vk == VK_BACK || vk == VK_SPACE;
}

// A key (or mouse button) the game doesn't get right now.
bool Blocked(unsigned vk) {
    if (vk >= 256) return false;
    if (!g_block.load(std::memory_order_relaxed)) return false;
    return g_selective.load(std::memory_order_relaxed) ? g_marioKey[vk] : !g_pass[vk];
}

// One of Mario Mode's hotkeys is being pressed with this key.
bool HotkeyHeld(unsigned vk) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    for (uint8_t m : g_hotkey[vk]) {
        if (!(m & 0x80)) continue;
        if (((m & 1) && !ctrl) || ((m & 2) && !shift) || ((m & 4) && !alt)) continue;
        return true;
    }
    return false;
}

bool BlockKeyDown(unsigned vk) {
    if (vk >= 256) return false;
    if (g_alwaysBlocked[vk] || g_alsoBlocked[vk].load(std::memory_order_relaxed)) return true;
    if (g_menu.load(std::memory_order_relaxed) && MenuKey(vk)) return true;
    if (g_block.load(std::memory_order_relaxed) && HotkeyHeld(vk)) return true;
    return Blocked(vk);
}

// ---------------------------------------------------------------- XInput
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xiOrig[3] = {};
XInputGetStateFn g_xiRead = nullptr; // unfiltered function used for Mario
bool g_xiHooked[3] = {};
const wchar_t* const kXiDlls[3] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};

void FilterXInput(XINPUT_STATE* st) {
    if (!st) return;
    if (g_menu.load(std::memory_order_relaxed))
        st->Gamepad.wButtons &= ~WORD(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
                                      XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B);
    if (!g_block.load(std::memory_order_relaxed)) return;
    XINPUT_GAMEPAD& g = st->Gamepad;
    if (!g_selective.load(std::memory_order_relaxed)) {
        g.wButtons &= (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK);
        g.sThumbLX = 0;
        g.sThumbLY = 0;
        g.bLeftTrigger = 0;
        g.bRightTrigger = 0;
        return;
    }
    // Only what Mario uses: his buttons, his triggers, the left stick. The
    // game keeps the rest (menus, prompts, the d-pad, the camera).
    const uint32_t mario = g_marioPad.load(std::memory_order_relaxed);
    g.wButtons &= ~WORD(mario & 0xFFFF); // PadButton bits match XInput's
    if (mario & PAD_LT) g.bLeftTrigger = 0;
    if (mario & PAD_RT) g.bRightTrigger = 0;
    if (g_marioStick.load(std::memory_order_relaxed)) {
        g.sThumbLX = 0;
        g.sThumbLY = 0;
    }
}

template <int N>
DWORD WINAPI HookXInputGetState(DWORD user, XINPUT_STATE* st) {
    DWORD r = g_xiOrig[N](user, st);
    if (r == ERROR_SUCCESS) FilterXInput(st);
    return r;
}

// ---------------------------------------------------------------- libScePad
// First 12 bytes of ScePadData (PS4 SDK layout): u32 buttons, u8 lx, ly, rx, ry, l2, r2.
enum : uint32_t {
    SCE_L3 = 0x0002, SCE_R3 = 0x0004, SCE_OPTIONS = 0x0008, SCE_UP = 0x0010, SCE_RIGHT = 0x0020,
    SCE_DOWN = 0x0040, SCE_LEFT = 0x0080, SCE_L2 = 0x0100, SCE_R2 = 0x0200, SCE_L1 = 0x0400,
    SCE_R1 = 0x0800, SCE_TRIANGLE = 0x1000, SCE_CIRCLE = 0x2000, SCE_CROSS = 0x4000, SCE_SQUARE = 0x8000,
    SCE_TOUCHPAD = 0x100000,
};
using ScePadReadStateFn = int (*)(int, void*);
using ScePadReadFn = int (*)(int, void*, int);
ScePadReadStateFn g_sceReadStateOrig = nullptr;
ScePadReadFn g_sceReadOrig = nullptr;
bool g_sceHooked = false;
Mutex g_sceMutex;
PadState g_scePad;
double g_scePadTime = -1;

void CaptureAndFilterScePad(uint8_t* d) {
    uint32_t b;
    std::memcpy(&b, d, 4);
    PadState p;
    p.connected = true;
    p.lx = (float(d[4]) - 128.0f) / 127.5f;
    p.ly = -(float(d[5]) - 128.0f) / 127.5f;
    p.rx = (float(d[6]) - 128.0f) / 127.5f;
    p.ry = -(float(d[7]) - 128.0f) / 127.5f;
    struct Map {
        uint32_t sce, pad;
    };
    static const Map map[] = {{SCE_CROSS, PAD_A},     {SCE_CIRCLE, PAD_B},    {SCE_SQUARE, PAD_X},
                              {SCE_TRIANGLE, PAD_Y},  {SCE_L1, PAD_LB},       {SCE_R1, PAD_RB},
                              {SCE_OPTIONS, PAD_START}, {SCE_TOUCHPAD, PAD_BACK}, {SCE_L3, PAD_LS},
                              {SCE_R3, PAD_RS},       {SCE_UP, PAD_DPAD_UP},  {SCE_DOWN, PAD_DPAD_DOWN},
                              {SCE_LEFT, PAD_DPAD_LEFT}, {SCE_RIGHT, PAD_DPAD_RIGHT}};
    for (const auto& m : map)
        if (b & m.sce) p.buttons |= m.pad;
    if (d[8] > 128 || (b & SCE_L2)) p.buttons |= PAD_LT;
    if (d[9] > 128 || (b & SCE_R2)) p.buttons |= PAD_RT;
    {
        LockGuard lock(g_sceMutex);
        g_scePad = p;
        g_scePadTime = NowSeconds();
    }
    if (g_menu.load(std::memory_order_relaxed)) {
        b &= ~uint32_t(SCE_UP | SCE_DOWN | SCE_LEFT | SCE_RIGHT | SCE_CROSS | SCE_CIRCLE);
        std::memcpy(d, &b, 4);
    }
    if (g_block.load(std::memory_order_relaxed)) {
        if (!g_selective.load(std::memory_order_relaxed)) {
            b &= (SCE_OPTIONS | SCE_TOUCHPAD);
            std::memcpy(d, &b, 4);
            d[4] = 128;
            d[5] = 128;
            d[8] = 0;
            d[9] = 0;
            return;
        }
        const uint32_t mario = g_marioPad.load(std::memory_order_relaxed);
        for (const auto& m : map)
            if (mario & m.pad) b &= ~m.sce;
        if (mario & PAD_LT) {
            b &= ~uint32_t(SCE_L2);
            d[8] = 0;
        }
        if (mario & PAD_RT) {
            b &= ~uint32_t(SCE_R2);
            d[9] = 0;
        }
        std::memcpy(d, &b, 4);
        if (g_marioStick.load(std::memory_order_relaxed)) {
            d[4] = 128;
            d[5] = 128;
        }
    }
}

int HookScePadReadState(int handle, void* data) {
    int r = g_sceReadStateOrig(handle, data);
    if (r == 0 && data) CaptureAndFilterScePad(static_cast<uint8_t*>(data));
    return r;
}

int HookScePadRead(int handle, void* data, int num) {
    int r = g_sceReadOrig(handle, data, num);
    // Only the first entry is touched: the PC struct stride isn't documented.
    if (r > 0 && data) CaptureAndFilterScePad(static_cast<uint8_t*>(data));
    return r;
}

// ---------------------------------------------------------------- Raw Input
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetRawInputDataFn g_ridOrig = nullptr;
GetRawInputBufferFn g_ribOrig = nullptr;

void FilterRaw(RAWINPUT* ri) {
    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        RAWKEYBOARD& k = ri->data.keyboard;
        const bool up = (k.Flags & RI_KEY_BREAK) != 0;
        if (!up && BlockKeyDown(k.VKey)) {
            k.VKey = 0xFF;
            k.MakeCode = 0;
        }
    } else if (ri->header.dwType == RIM_TYPEMOUSE && g_block.load(std::memory_order_relaxed)) {
        USHORT& f = ri->data.mouse.usButtonFlags;
        if (Blocked(VK_LBUTTON)) f &= ~USHORT(RI_MOUSE_LEFT_BUTTON_DOWN);
        if (Blocked(VK_RBUTTON)) f &= ~USHORT(RI_MOUSE_RIGHT_BUTTON_DOWN);
        if (Blocked(VK_MBUTTON)) f &= ~USHORT(RI_MOUSE_MIDDLE_BUTTON_DOWN);
        if (Blocked(VK_XBUTTON1)) f &= ~USHORT(RI_MOUSE_BUTTON_4_DOWN);
        if (Blocked(VK_XBUTTON2)) f &= ~USHORT(RI_MOUSE_BUTTON_5_DOWN);
    }
}

UINT WINAPI HookGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header) {
    UINT r = g_ridOrig(h, cmd, data, size, header);
    if (cmd == RID_INPUT && data && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER)) FilterRaw(static_cast<RAWINPUT*>(data));
    return r;
}

UINT WINAPI HookGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header) {
    UINT r = g_ribOrig(data, size, header);
    if (data && r != UINT(-1) && r > 0) {
        PRAWINPUT cur = data;
        for (UINT i = 0; i < r; ++i) {
            FilterRaw(cur);
            // NEXTRAWINPUTBLOCK: entries are 8-byte aligned on x64.
            cur = reinterpret_cast<PRAWINPUT>((reinterpret_cast<ULONG_PTR>(cur) + cur->header.dwSize + 7) & ~ULONG_PTR(7));
        }
    }
    return r;
}

// ---------------------------------------------------------------- WndProc
std::atomic<WNDPROC> g_prevWndProc{nullptr};
HWND g_hwnd = nullptr;
void (*g_onClosing)() = nullptr;

LRESULT CALLBACK ShimWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (BlockKeyDown(unsigned(wp))) return 0;
        break;
    case WM_CHAR:
    case WM_SYSCHAR: {
        SHORT vk = VkKeyScanW(WCHAR(wp));
        if (vk != -1 && BlockKeyDown(unsigned(vk & 0xFF))) return 0;
        break;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (Blocked(VK_LBUTTON)) return 0;
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        if (Blocked(VK_RBUTTON)) return 0;
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        if (Blocked(VK_MBUTTON)) return 0;
        break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        if (Blocked(GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1)) return TRUE;
        break;
    case WM_DESTROY:
        // The game is closing: let the mod stand down before the engine tears
        // things down.
        if (g_onClosing) g_onClosing();
        break;
    default:
        break;
    }
    WNDPROC prev = g_prevWndProc;
    if (!prev) return DefWindowProcW(hwnd, msg, wp, lp);
    return CallWindowProcW(prev, hwnd, msg, wp, lp);
}

bool HookExport(HMODULE mod, const char* name, void* detour, void** original, void** targetOut = nullptr) {
    void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
    if (targetOut) *targetOut = target;
    if (!target) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    return MH_EnableHook(target) == MH_OK;
}

} // namespace

void InstallHooks() {
    static bool rawDone = false;
    if (!rawDone) {
        rawDone = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        bool a = user32 && HookExport(user32, "GetRawInputData", reinterpret_cast<void*>(&HookGetRawInputData),
                                      reinterpret_cast<void**>(&g_ridOrig));
        bool b = user32 && HookExport(user32, "GetRawInputBuffer", reinterpret_cast<void*>(&HookGetRawInputBuffer),
                                      reinterpret_cast<void**>(&g_ribOrig));
        LOGI("input: raw input hooks %s/%s", a ? "ok" : "failed", b ? "ok" : "failed");
    }
    // XInput: hook every loaded flavour; load 1.4 ourselves so Mario can read pads.
    static void* const detours[3] = {reinterpret_cast<void*>(&HookXInputGetState<0>),
                                     reinterpret_cast<void*>(&HookXInputGetState<1>),
                                     reinterpret_cast<void*>(&HookXInputGetState<2>)};
    bool anyLoaded = false;
    for (int i = 0; i < 3; ++i) {
        HMODULE m = GetModuleHandleW(kXiDlls[i]);
        if (!m) continue;
        anyLoaded = true;
        if (g_xiHooked[i]) continue;
        g_xiHooked[i] = true;
        void* target = nullptr;
        if (HookExport(m, "XInputGetState", detours[i], reinterpret_cast<void**>(&g_xiOrig[i]), &target)) {
            // If Mario was reading this very export, it now runs through the
            // filter: switch to the trampoline so Mario keeps seeing the pad.
            if (!g_xiRead || reinterpret_cast<void*>(g_xiRead) == target) g_xiRead = g_xiOrig[i];
            LOGI("input: hooked XInputGetState in %ls", kXiDlls[i]);
        }
    }
    if (!anyLoaded && !g_xiRead) {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (m) g_xiRead = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
    }
    if (!g_sceHooked) {
        HMODULE sce = GetModuleHandleW(L"libScePad.dll");
        if (sce) {
            g_sceHooked = true;
            bool a = HookExport(sce, "scePadReadState", reinterpret_cast<void*>(&HookScePadReadState),
                                reinterpret_cast<void**>(&g_sceReadStateOrig));
            bool b = HookExport(sce, "scePadRead", reinterpret_cast<void*>(&HookScePadRead),
                                reinterpret_cast<void**>(&g_sceReadOrig));
            LOGI("input: libScePad hooks %s/%s", a ? "ok" : "missing", b ? "ok" : "missing");
        }
    }
    g_status = std::string("raw input") + (g_xiRead ? ", xinput" : "") + (g_sceHooked ? ", libScePad" : "");
}

void SetBlocking(bool block) { g_block.store(block); }
void SetMenuCapture(bool on) { g_menu.store(on); }
void SetClosingCallback(void (*fn)()) { g_onClosing = fn; }
bool Blocking() { return g_block.load(); }

void SetAlwaysBlocked(const std::vector<int>& vks) {
    std::memset(g_alwaysBlocked, 0, sizeof(g_alwaysBlocked));
    for (int vk : vks)
        if (vk > 0 && vk < 256) g_alwaysBlocked[vk] = true;
}

void SetAlsoBlocked(const std::vector<int>& vks) {
    bool want[256] = {};
    for (int vk : vks)
        if (vk > 0 && vk < 256) want[vk] = true;
    for (int i = 0; i < 256; ++i) g_alsoBlocked[i].store(want[i], std::memory_order_relaxed);
}

void SetMarioControls(bool onlyMario, const std::vector<int>& keys, uint32_t padButtons, bool leftStick) {
    std::memset(g_marioKey, 0, sizeof(g_marioKey));
    for (int vk : keys)
        if (vk > 0 && vk < 256) g_marioKey[vk] = true;
    // Raw input reports the generic modifier codes.
    if (g_marioKey[VK_LSHIFT] || g_marioKey[VK_RSHIFT]) g_marioKey[VK_SHIFT] = true;
    if (g_marioKey[VK_LCONTROL] || g_marioKey[VK_RCONTROL]) g_marioKey[VK_CONTROL] = true;
    if (g_marioKey[VK_LMENU] || g_marioKey[VK_RMENU]) g_marioKey[VK_MENU] = true;
    g_marioPad.store(padButtons);
    g_marioStick.store(leftStick);
    g_selective.store(onlyMario);
}

void SetHotkeys(const std::vector<KeyCombo>& combos) {
    std::memset(g_hotkey, 0, sizeof(g_hotkey));
    for (const KeyCombo& c : combos) {
        if (c.vk <= 0 || c.vk >= 256) continue;
        const uint8_t m = uint8_t(0x80 | (c.ctrl ? 1 : 0) | (c.shift ? 2 : 0) | (c.alt ? 4 : 0));
        for (uint8_t& slot : g_hotkey[c.vk]) {
            if (slot == m) break;
            if (slot == 0) {
                slot = m;
                break;
            }
        }
    }
}

void SetPassThrough(const std::vector<int>& vks) {
    std::memset(g_pass, 0, sizeof(g_pass));
    for (int vk : vks)
        if (vk > 0 && vk < 256) g_pass[vk] = true;
    // Generic modifier codes arrive in raw input as VK_SHIFT/VK_CONTROL/VK_MENU.
    if (g_pass[VK_LMENU] || g_pass[VK_RMENU]) g_pass[VK_MENU] = true;
}

void SubclassWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_hwnd) return;
    if (g_hwnd && IsWindow(g_hwnd)) return; // already attached to the game's window
    const LONG_PTR self = reinterpret_cast<LONG_PTR>(&ShimWndProc);
    const LONG_PTR current = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (current == self) {
        g_hwnd = hwnd;
        return;
    }
    // Publish the previous procedure before ours can receive a message.
    g_prevWndProc.store(reinterpret_cast<WNDPROC>(current));
    const LONG_PTR replaced = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, self);
    if (!replaced) {
        g_prevWndProc.store(nullptr);
        LOGW("input: could not subclass the game window (%lu)", GetLastError());
        return;
    }
    if (replaced != current && replaced != self) g_prevWndProc.store(reinterpret_cast<WNDPROC>(replaced));
    g_hwnd = hwnd;
    LOGI("input: subclassed game window %p", reinterpret_cast<void*>(hwnd));
}

bool LatestScePad(PadState& out) {
    LockGuard lock(g_sceMutex);
    if (g_scePadTime < 0 || NowSeconds() - g_scePadTime > 0.5) return false;
    out = g_scePad;
    return true;
}

bool ReadXInput(PadState& out) {
    out = PadState{};
    if (!g_xiRead) return false;
    XINPUT_STATE st{};
    if (g_xiRead(0, &st) != ERROR_SUCCESS) return false;
    const XINPUT_GAMEPAD& g = st.Gamepad;
    out.connected = true;
    out.lx = float(g.sThumbLX) / 32767.0f;
    out.ly = float(g.sThumbLY) / 32767.0f;
    out.rx = float(g.sThumbRX) / 32767.0f;
    out.ry = float(g.sThumbRY) / 32767.0f;
    out.buttons = g.wButtons;
    if (g.bLeftTrigger > 128) out.buttons |= PAD_LT;
    if (g.bRightTrigger > 128) out.buttons |= PAD_RT;
    return true;
}

const char* Status() { return g_status.c_str(); }

} // namespace sm2m::input_shim

#endif
