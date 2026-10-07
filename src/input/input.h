// Input: reads keyboard / XInput / DualSense (libScePad) for Mario, and
// installs a shim that hides those inputs from the game while Mario is
// active (so Spider-Man doesn't move or attack) while leaving the camera
// (mouse movement, right stick) and pause buttons working.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <vector>

#include "../common/ini.h"
#include "../common/platform.h"

namespace sm2m {

struct KeyCombo {
    int vk = 0;
    bool ctrl = false, shift = false, alt = false;
};

// "M", "CTRL+F8", "LMB", "SPACE" ... ; multiple alternatives separated by commas.
bool ParseKeyCombo(const std::string& text, KeyCombo& out);
std::vector<KeyCombo> ParseKeyList(const std::string& text);
int VirtualKeyFromName(const std::string& name);

// XInput-style button bits used for all pads (DualSense is translated).
enum PadButton : uint32_t {
    PAD_DPAD_UP = 0x0001,
    PAD_DPAD_DOWN = 0x0002,
    PAD_DPAD_LEFT = 0x0004,
    PAD_DPAD_RIGHT = 0x0008,
    PAD_START = 0x0010,
    PAD_BACK = 0x0020,
    PAD_LS = 0x0040,
    PAD_RS = 0x0080,
    PAD_LB = 0x0100,
    PAD_RB = 0x0200,
    PAD_A = 0x1000,
    PAD_B = 0x2000,
    PAD_X = 0x4000,
    PAD_Y = 0x8000,
    PAD_LT = 0x10000, // synthesised from the analog triggers
    PAD_RT = 0x20000,
};
uint32_t PadButtonsFromNames(const std::string& list);

struct PadState {
    bool connected = false;
    float lx = 0, ly = 0; // -1..1, up = +1
    float rx = 0, ry = 0;
    uint32_t buttons = 0;
};

enum class Hotkey {
    Toggle,
    DebugOverlay,
    Dump,
    Rescan,
    NextCamera,
    PrevCamera,
    FovUp,
    FovDown,
    Mirror,
    Respawn,
    Calibrate,
    Menu,
    Pose,
    Count
};

// Menu navigation this frame (edge-triggered; arrows and the d-pad repeat
// while held).
struct MenuNav {
    bool up = false, down = false, left = false, right = false, accept = false, back = false;
};

struct MarioButtons {
    float stickX = 0, stickY = 0; // libsm64 convention: +x right, +y towards the camera (down)
    bool a = false, b = false, z = false;
};

class InputManager {
public:
    void Configure(const Ini& cfg);
    // Once per frame. `window` is the game's window (focus check).
    void Poll(HWND window);

    bool Pressed(Hotkey h) const { return pressed_[int(h)]; }
    // Keyboard arrows / Enter / Space / Esc / Backspace and the pad's d-pad / A / B.
    const MenuNav& Nav() const { return nav_; }
    // Any of the navigation keys / buttons is held down.
    bool NavHeld() const { return navHeld_; }
    const MarioButtons& Mario() const { return mario_; }
    bool Focused() const { return focused_; }
    bool CursorVisible() const { return cursorVisible_; }
    const PadState& Pad() const { return pad_; }

    // Keys/buttons the shim should keep from the game while Mario is active.
    const std::vector<int>& PassThroughKeys() const { return passKeys_; }
    // Mario's own controls (the shim's selective mode keeps just these from the game).
    std::vector<int> MarioKeys() const;
    // Every hotkey combo (toggle, menu, respawn, dump ...).
    std::vector<KeyCombo> Hotkeys() const;
    uint32_t MarioPadButtons() const { return padA_ | padB_ | padZ_; }
    // The interact button (Triangle / Y, or its key) went down this frame.
    bool InteractPressed() const { return interactPressed_; }

private:
    bool ComboDown(const KeyCombo& k) const;
    bool AnyDown(const std::vector<KeyCombo>& list) const;
    bool PadAny(uint32_t mask) const { return (pad_.buttons & mask) != 0; }

    std::vector<KeyCombo> hotkeys_[int(Hotkey::Count)];
    uint32_t padToggle_ = 0;
    uint32_t padMenu_ = 0, padPose_ = 0;
    bool padMenuWas_ = false, padPoseWas_ = false;
    MenuNav nav_;
    bool navHeld_ = false;
    struct Repeat {
        bool was = false;
        double next = 0;
    } navRepeat_[6];
    bool wasDown_[int(Hotkey::Count)] = {};
    bool pressed_[int(Hotkey::Count)] = {};
    bool padToggleWas_ = false;

    std::vector<KeyCombo> keyUp_, keyDown_, keyLeft_, keyRight_, keyA_, keyB_, keyZ_, keyWalk_;
    std::vector<KeyCombo> keyInteract_;
    uint32_t padInteract_ = PAD_Y;
    bool interactWas_ = false, interactPressed_ = false;
    uint32_t padA_ = PAD_A, padB_ = PAD_X | PAD_B, padZ_ = PAD_LT | PAD_RT | PAD_LB;
    float deadzone_ = 0.2f;
    bool invertPadY_ = false;

    MarioButtons mario_;
    PadState pad_;
    bool focused_ = false;
    bool cursorVisible_ = false;
    std::vector<int> passKeys_;
};

// ---- input shim (hooks) ----
namespace input_shim {
// Installs hooks that are available now; call again later (cheap) to pick up
// DLLs loaded after start-up (xinput, libScePad).
void InstallHooks();
void SetBlocking(bool block);
bool Blocking();
// Mario's menu is open: arrows, Enter, Esc, Backspace, Space and the pad's
// d-pad / A / B don't reach the game.
void SetMenuCapture(bool on);
// Keys the game never sees (toggle key etc.), and keys that always pass.
void SetAlwaysBlocked(const std::vector<int>& vks);
// ... and these too, for now (photo mode: the pose key, which the game gets
// in a pause). Replaces the last call's.
void SetAlsoBlocked(const std::vector<int>& vks);
void SetPassThrough(const std::vector<int>& vks);
// onlyMario: while blocking, the game loses only Mario's keys, pad buttons and
// (if `leftStick`) the left stick - everything else (menus, prompts, the
// d-pad, the camera) still reaches it. Else everything but SetPassThrough's keys.
void SetMarioControls(bool onlyMario, const std::vector<int>& keys, uint32_t padButtons, bool leftStick);
// Mario Mode's own hotkeys (Ctrl+R, Ctrl+F9, F10...): while blocking, their
// key doesn't reach the game while their modifiers are held (Ctrl being
// Mario's walk key, the game would otherwise see a bare R).
void SetHotkeys(const std::vector<KeyCombo>& combos);
void SubclassWindow(HWND hwnd);
// Called (on the window's thread) when the game window gets WM_DESTROY.
void SetClosingCallback(void (*fn)());
// Latest DualSense state seen by the libScePad hook (for Mario).
bool LatestScePad(PadState& out);
// Unfiltered XInput state for user 0 (calls the original function).
bool ReadXInput(PadState& out);
const char* Status();
} // namespace input_shim

} // namespace sm2m

#endif
