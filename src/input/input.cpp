#ifdef _WIN32

#include "input.h"

#include <cmath>
#include <cstring>
#include <xinput.h>

#include "../common/log.h"

namespace sm2m {

// ------------------------------------------------------------ key names

int VirtualKeyFromName(const std::string& raw) {
    std::string n = Ini::Lower(Ini::Trim(raw));
    if (n.size() == 1) {
        char c = n[0];
        if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
        if (c >= '0' && c <= '9') return c;
        switch (c) {
        case '[': return VK_OEM_4;
        case ']': return VK_OEM_6;
        case ';': return VK_OEM_1;
        case '\'': return VK_OEM_7;
        case ',': return VK_OEM_COMMA;
        case '.': return VK_OEM_PERIOD;
        case '/': return VK_OEM_2;
        case '\\': return VK_OEM_5;
        case '`': return VK_OEM_3;
        case '-': return VK_OEM_MINUS;
        case '=': return VK_OEM_PLUS;
        default: break;
        }
    }
    if (n.size() >= 2 && n[0] == 'f') {
        int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    if (n.rfind("numpad", 0) == 0 && n.size() == 7 && n[6] >= '0' && n[6] <= '9') return VK_NUMPAD0 + (n[6] - '0');
    struct Named {
        const char* name;
        int vk;
    };
    static const Named table[] = {
        {"space", VK_SPACE},       {"enter", VK_RETURN},       {"return", VK_RETURN},   {"esc", VK_ESCAPE},
        {"escape", VK_ESCAPE},     {"tab", VK_TAB},            {"backspace", VK_BACK},  {"capslock", VK_CAPITAL},
        {"shift", VK_SHIFT},       {"lshift", VK_LSHIFT},      {"rshift", VK_RSHIFT},   {"ctrl", VK_CONTROL},
        {"control", VK_CONTROL},   {"lctrl", VK_LCONTROL},     {"rctrl", VK_RCONTROL},  {"alt", VK_MENU},
        {"lalt", VK_LMENU},        {"ralt", VK_RMENU},         {"up", VK_UP},           {"down", VK_DOWN},
        {"left", VK_LEFT},         {"right", VK_RIGHT},        {"insert", VK_INSERT},   {"delete", VK_DELETE},
        {"home", VK_HOME},         {"end", VK_END},            {"pageup", VK_PRIOR},    {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT},     {"pgdn", VK_NEXT},          {"lmb", VK_LBUTTON},     {"mouse1", VK_LBUTTON},
        {"rmb", VK_RBUTTON},       {"mouse2", VK_RBUTTON},     {"mmb", VK_MBUTTON},     {"mouse3", VK_MBUTTON},
        {"mouse4", VK_XBUTTON1},   {"xbutton1", VK_XBUTTON1},  {"mouse5", VK_XBUTTON2}, {"xbutton2", VK_XBUTTON2},
        {"minus", VK_OEM_MINUS},   {"equals", VK_OEM_PLUS},    {"plus", VK_OEM_PLUS},   {"lbracket", VK_OEM_4},
        {"rbracket", VK_OEM_6},    {"semicolon", VK_OEM_1},    {"apostrophe", VK_OEM_7}, {"comma", VK_OEM_COMMA},
        {"period", VK_OEM_PERIOD}, {"slash", VK_OEM_2},        {"backslash", VK_OEM_5}, {"grave", VK_OEM_3},
        {"tilde", VK_OEM_3},       {"printscreen", VK_SNAPSHOT}, {"lwin", VK_LWIN},     {"rwin", VK_RWIN},
    };
    for (const auto& e : table)
        if (n == e.name) return e.vk;
    return 0;
}

bool ParseKeyCombo(const std::string& text, KeyCombo& out) {
    out = KeyCombo{};
    std::vector<std::string> parts = Ini::SplitList(text, '+');
    if (parts.empty()) return false;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string m = Ini::Lower(parts[i]);
        if (m == "ctrl" || m == "control") out.ctrl = true;
        else if (m == "shift") out.shift = true;
        else if (m == "alt") out.alt = true;
        else return false;
    }
    out.vk = VirtualKeyFromName(parts.back());
    return out.vk != 0;
}

std::vector<KeyCombo> ParseKeyList(const std::string& text) {
    std::vector<KeyCombo> out;
    for (const auto& item : Ini::SplitList(text)) {
        KeyCombo k;
        if (ParseKeyCombo(item, k)) out.push_back(k);
        else if (!item.empty() && Ini::Lower(item) != "none") LOGW("unknown key '%s' in config", item.c_str());
    }
    return out;
}

uint32_t PadButtonsFromNames(const std::string& list) {
    uint32_t mask = 0;
    struct Named {
        const char* name;
        uint32_t bit;
    };
    static const Named table[] = {
        {"a", PAD_A},         {"b", PAD_B},           {"x", PAD_X},         {"y", PAD_Y},
        {"lb", PAD_LB},       {"rb", PAD_RB},         {"lt", PAD_LT},       {"rt", PAD_RT},
        {"back", PAD_BACK},   {"start", PAD_START},   {"ls", PAD_LS},       {"rs", PAD_RS},
        {"up", PAD_DPAD_UP},  {"down", PAD_DPAD_DOWN}, {"left", PAD_DPAD_LEFT}, {"right", PAD_DPAD_RIGHT},
        {"cross", PAD_A},     {"circle", PAD_B},      {"square", PAD_X},    {"triangle", PAD_Y},
        {"l1", PAD_LB},       {"r1", PAD_RB},         {"l2", PAD_LT},       {"r2", PAD_RT},
        {"options", PAD_START}, {"touchpad", PAD_BACK}, {"l3", PAD_LS},     {"r3", PAD_RS},
    };
    for (const auto& item : Ini::SplitList(list, '+')) {
        for (const auto& sub : Ini::SplitList(item, ',')) {
            std::string n = Ini::Lower(sub);
            for (const auto& e : table)
                if (n == e.name) mask |= e.bit;
        }
    }
    return mask;
}

// ------------------------------------------------------------ manager

void InputManager::Configure(const Ini& c) {
    auto keys = [&](const char* key, const char* def) { return ParseKeyList(c.GetString("Controls", key, def)); };
    hotkeys_[int(Hotkey::Toggle)] = keys("ToggleKey", "M");
    hotkeys_[int(Hotkey::DebugOverlay)] = keys("DebugOverlayKey", "F10");
    hotkeys_[int(Hotkey::Dump)] = keys("DumpKey", "CTRL+F9");
    hotkeys_[int(Hotkey::Rescan)] = keys("RescanCameraKey", "CTRL+F8");
    hotkeys_[int(Hotkey::NextCamera)] = keys("NextCameraKey", "CTRL+PAGEDOWN");
    hotkeys_[int(Hotkey::PrevCamera)] = keys("PrevCameraKey", "CTRL+PAGEUP");
    hotkeys_[int(Hotkey::FovUp)] = keys("FovUpKey", "CTRL+]");
    hotkeys_[int(Hotkey::FovDown)] = keys("FovDownKey", "CTRL+[");
    hotkeys_[int(Hotkey::Mirror)] = keys("MirrorKey", "CTRL+F7");
    hotkeys_[int(Hotkey::Respawn)] = keys("RespawnKey", "CTRL+R");
    hotkeys_[int(Hotkey::Calibrate)] = keys("CalibrateKey", "CTRL+F11");
    hotkeys_[int(Hotkey::Menu)] = keys("MenuKey", "F8");
    hotkeys_[int(Hotkey::Pose)] = keys("PoseKey", "P");
    padToggle_ = PadButtonsFromNames(c.GetString("Controls", "PadToggle", ""));
    padMenu_ = PadButtonsFromNames(c.GetString("Controls", "PadMenu", ""));
    padPose_ = PadButtonsFromNames(c.GetString("Controls", "PadPose", ""));

    keyUp_ = keys("KeyForward", "W");
    keyDown_ = keys("KeyBack", "S");
    keyLeft_ = keys("KeyLeft", "A");
    keyRight_ = keys("KeyRight", "D");
    keyA_ = keys("KeyJump", "SPACE");
    keyB_ = keys("KeyPunch", "LMB");
    keyZ_ = keys("KeyCrouch", "LSHIFT, RMB");
    keyWalk_ = keys("KeyWalk", "LCTRL");
    keyInteract_ = keys("KeyInteract", "E, F");
    padInteract_ = PadButtonsFromNames(c.GetString("Controls", "PadInteract", "Y"));
    padA_ = PadButtonsFromNames(c.GetString("Controls", "PadJump", "A"));
    padB_ = PadButtonsFromNames(c.GetString("Controls", "PadPunch", "X, B"));
    padZ_ = PadButtonsFromNames(c.GetString("Controls", "PadCrouch", "LT, RT, LB"));
    deadzone_ = float(c.GetFloat("Controls", "StickDeadzone", 0.2));
    invertPadY_ = c.GetBool("Controls", "InvertStickY", false);

    passKeys_.clear();
    for (const auto& k : ParseKeyList(c.GetString("Controls", "PassThroughKeys",
                                                  "ESC, ENTER, UP, DOWN, LEFT, RIGHT, F1, F2, F3, F4, F5, F6, F11, F12, "
                                                  "ALT, LALT, RALT, LWIN, RWIN, PRINTSCREEN, TAB")))
        passKeys_.push_back(k.vk);
}

std::vector<int> InputManager::MarioKeys() const {
    std::vector<int> v;
    for (const auto* list : {&keyUp_, &keyDown_, &keyLeft_, &keyRight_, &keyA_, &keyB_, &keyZ_, &keyWalk_})
        for (const KeyCombo& k : *list) v.push_back(k.vk);
    // ... and the pose key (a plain letter the game may use too).
    for (const KeyCombo& k : hotkeys_[int(Hotkey::Pose)])
        if (!k.ctrl && !k.shift && !k.alt) v.push_back(k.vk);
    return v;
}

std::vector<KeyCombo> InputManager::Hotkeys() const {
    std::vector<KeyCombo> v;
    for (int h = 0; h < int(Hotkey::Count); ++h) v.insert(v.end(), hotkeys_[h].begin(), hotkeys_[h].end());
    return v;
}

bool InputManager::ComboDown(const KeyCombo& k) const {
    if (!(GetAsyncKeyState(k.vk) & 0x8000)) return false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (k.ctrl && !ctrl) return false;
    if (k.shift && !shift) return false;
    if (k.alt && !alt) return false;
    return true;
}

bool InputManager::AnyDown(const std::vector<KeyCombo>& list) const {
    for (const auto& k : list)
        if (ComboDown(k)) return true;
    return false;
}

static int Modifiers(const KeyCombo& k) { return int(k.ctrl) + int(k.shift) + int(k.alt); }

static float ApplyDeadzone(float v, float dz) {
    float a = std::fabs(v);
    if (a < dz) return 0;
    float s = (a - dz) / (1.0f - dz);
    return v < 0 ? -s : s;
}

void InputManager::Poll(HWND window) {
    HWND fg = GetForegroundWindow();
    focused_ = window && (fg == window || GetAncestor(fg, GA_ROOTOWNER) == GetAncestor(window, GA_ROOTOWNER));
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    cursorVisible_ = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);

    // Pad: DualSense via the libScePad hook, else XInput.
    PadState ps;
    if (!input_shim::LatestScePad(ps) || !ps.connected) input_shim::ReadXInput(ps);
    pad_ = ps;

    // Which combo of each hotkey is down. A combo with modifiers wins over
    // the same key without them: CTRL+F8 (rescan) doesn't also open the menu (F8).
    const KeyCombo* downCombo[int(Hotkey::Count)] = {};
    for (int h = 0; h < int(Hotkey::Count); ++h) {
        if (!focused_) continue;
        for (const KeyCombo& k : hotkeys_[h])
            if (ComboDown(k) && (!downCombo[h] || Modifiers(k) > Modifiers(*downCombo[h]))) downCombo[h] = &k;
    }
    for (int h = 0; h < int(Hotkey::Count); ++h) {
        const bool down = downCombo[h] != nullptr;
        bool shadowed = false;
        for (int o = 0; down && o < int(Hotkey::Count); ++o)
            if (o != h && downCombo[o] && downCombo[o]->vk == downCombo[h]->vk &&
                Modifiers(*downCombo[o]) > Modifiers(*downCombo[h]))
                shadowed = true;
        // (Still counts as held: letting go of CTRL first doesn't fire F8.)
        pressed_[h] = down && !shadowed && !wasDown_[h];
        wasDown_[h] = down;
    }
    // Pad combos: one that contains another wins while both are held
    // (LB+RB+Y toggling doesn't also pose with LB+Y).
    {
        struct Combo {
            uint32_t mask;
            Hotkey hotkey;
            bool* was;
            bool down;
        } combos[] = {{padToggle_, Hotkey::Toggle, &padToggleWas_, false},
                      {padMenu_, Hotkey::Menu, &padMenuWas_, false},
                      {padPose_, Hotkey::Pose, &padPoseWas_, false}};
        for (Combo& c : combos) c.down = c.mask && focused_ && (pad_.buttons & c.mask) == c.mask;
        for (Combo& c : combos) {
            if (!c.mask) continue;
            bool shadowed = false;
            for (const Combo& o : combos)
                if (&o != &c && o.down && o.mask != c.mask && (o.mask & c.mask) == c.mask) shadowed = true;
            if (c.down && !shadowed && !*c.was) pressed_[int(c.hotkey)] = true;
            *c.was = c.down;
        }
    }
    // Menu navigation (the menu decides whether it is listening).
    {
        auto key = [&](int vk) { return focused_ && (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const bool held[6] = {
            key(VK_UP) || (focused_ && (pad_.buttons & PAD_DPAD_UP)),
            key(VK_DOWN) || (focused_ && (pad_.buttons & PAD_DPAD_DOWN)),
            key(VK_LEFT) || (focused_ && (pad_.buttons & PAD_DPAD_LEFT)),
            key(VK_RIGHT) || (focused_ && (pad_.buttons & PAD_DPAD_RIGHT)),
            key(VK_RETURN) || key(VK_SPACE) || (focused_ && (pad_.buttons & PAD_A)),
            key(VK_ESCAPE) || key(VK_BACK) || (focused_ && (pad_.buttons & PAD_B)),
        };
        bool out[6];
        const double now = NowSeconds();
        navHeld_ = false;
        for (int i = 0; i < 6; ++i) {
            navHeld_ = navHeld_ || held[i];
            Repeat& r = navRepeat_[i];
            out[i] = held[i] && !r.was;
            if (out[i]) r.next = now + 0.35;
            else if (held[i] && i < 4 && now >= r.next) {
                out[i] = true; // auto-repeat for the arrows
                r.next = now + 0.07;
            }
            r.was = held[i];
        }
        nav_.up = out[0];
        nav_.down = out[1];
        nav_.left = out[2];
        nav_.right = out[3];
        nav_.accept = out[4];
        nav_.back = out[5];
    }

    {
        const bool down = focused_ && (AnyDown(keyInteract_) || (padInteract_ && PadAny(padInteract_)));
        interactPressed_ = down && !interactWas_;
        interactWas_ = down;
    }

    MarioButtons m;
    if (focused_) {
        float kx = 0, ky = 0;
        if (AnyDown(keyRight_)) kx += 1;
        if (AnyDown(keyLeft_)) kx -= 1;
        if (AnyDown(keyUp_)) ky -= 1; // libsm64: -y = away from the camera
        if (AnyDown(keyDown_)) ky += 1;
        float len = std::sqrt(kx * kx + ky * ky);
        if (len > 1) {
            kx /= len;
            ky /= len;
        }
        if (len > 0 && AnyDown(keyWalk_)) {
            kx *= 0.45f;
            ky *= 0.45f;
        }
        float px = ApplyDeadzone(pad_.lx, deadzone_);
        float py = ApplyDeadzone(pad_.ly, deadzone_) * (invertPadY_ ? 1.0f : -1.0f);
        m.stickX = std::fabs(px) + std::fabs(py) > std::fabs(kx) + std::fabs(ky) ? px : kx;
        m.stickY = std::fabs(px) + std::fabs(py) > std::fabs(kx) + std::fabs(ky) ? py : ky;
        m.a = AnyDown(keyA_) || PadAny(padA_);
        m.b = AnyDown(keyB_) || PadAny(padB_);
        m.z = AnyDown(keyZ_) || PadAny(padZ_);
    }
    mario_ = m;
}

} // namespace sm2m

#endif
