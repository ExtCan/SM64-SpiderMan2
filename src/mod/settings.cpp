#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../common/ini.h"

namespace sm2m {

namespace {

std::vector<SettingItem> BuildItems() {
    std::vector<SettingItem> v;
    auto header = [&](const char* label) {
        SettingItem i;
        i.kind = SettingItem::Header;
        i.label = label;
        v.push_back(i);
    };
    auto toggle = [&](int id, const char* label, const char* desc) {
        SettingItem i;
        i.kind = SettingItem::Toggle;
        i.id = id;
        i.label = label;
        i.desc = desc;
        v.push_back(i);
    };
    auto slider = [&](int id, const char* label, const char* desc, float mn, float mx, int dmn, int dmx) {
        SettingItem i;
        i.kind = SettingItem::Slider;
        i.id = id;
        i.label = label;
        i.desc = desc;
        i.min = mn;
        i.max = mx;
        i.displayMin = dmn;
        i.displayMax = dmx;
        v.push_back(i);
    };
    header("MARIO");
    slider(kSetVolume, "MARIO VOLUME", "Mario's voice, sounds and cap music.", 0.0f, 1.0f, 0, 20);
    slider(kSetAttack, "ATTACK STRENGTH", "How hard Mario's punches, kicks and stomps hit enemies.", 0.25f, 4.0f, 1, 16);
    slider(kSetShine, "SHINE", "How glossy Mario looks under the city's lights.", 0.0f, 1.0f, 0, 20);
    toggle(kSetPhotoPoses, "POSE FOR PHOTOS", "Mario waves or strikes a pose when people ask for a picture.");
    header("CHEATS");
    toggle(kSetInfiniteHealth, "INFINITE HEALTH", "Mario's power meter never runs out.");
    toggle(kSetWingCap, "WING CAP", "Triple jump to fly. Combines with the other caps, as in SM64.");
    toggle(kSetMetalCap, "METAL CAP", "Heavy, invincible and shiny. Combines with the other caps.");
    toggle(kSetVanishCap, "VANISH CAP", "See-through, like a ghost. Combines with the other caps.");
    toggle(kSetMoonJump, "MOON JUMP", "Hold jump in the air to keep rising.");
    toggle(kSetBlj, "BLJ ANYWHERE", "During a backwards long jump, hold crouch and press jump again to speed up.");
    header("CAMERA");
    slider(kSetCameraDistance, "CAMERA DISTANCE",
           "How far behind Mario the camera sits. 1x (10 on the pause menu's slider) is the game's own distance for Spider-Man.",
           0.5f, 3.0f, 5, 30);
    toggle(kSetFollowHeight, "CAMERA FOLLOWS JUMPS", "The camera stays on Mario, up and down too (off: the game's own camera).");
    return v;
}

const SettingItem* Find(int id) {
    for (const SettingItem& i : SettingItems())
        if (i.id == id) return &i;
    return nullptr;
}

std::string Num(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.3g", double(v));
    return b;
}
} // namespace

bool LiveSettings::operator==(const LiveSettings& o) const {
    return volume == o.volume && attackStrength == o.attackStrength && infiniteHealth == o.infiniteHealth &&
           wingCap == o.wingCap && metalCap == o.metalCap && vanishCap == o.vanishCap && moonJump == o.moonJump &&
           bljAnywhere == o.bljAnywhere && shine == o.shine && followHeight == o.followHeight &&
           photoPoses == o.photoPoses && cameraDistance == o.cameraDistance;
}

LiveSettings LiveFromConfig(const ModConfig& c) {
    LiveSettings s;
    s.volume = std::min(1.0f, std::max(0.0f, c.volume));
    s.attackStrength = std::min(4.0f, std::max(0.25f, c.attackStrength));
    s.infiniteHealth = c.infiniteHealth;
    s.wingCap = c.wingCap;
    s.metalCap = c.metalCap;
    s.vanishCap = c.vanishCap;
    s.moonJump = c.moonJump;
    s.bljAnywhere = c.bljAnywhere;
    s.shine = std::min(1.0f, std::max(0.0f, c.gloss));
    s.followHeight = c.followMarioHeight;
    s.photoPoses = c.photoPoses;
    s.cameraDistance = std::min(3.0f, std::max(0.5f, c.cameraDistance));
    return s;
}

const std::vector<SettingItem>& SettingItems() {
    static const std::vector<SettingItem> items = BuildItems();
    return items;
}

float GetSetting(const LiveSettings& s, int id) {
    switch (id) {
    case kSetVolume: return s.volume;
    case kSetAttack: return s.attackStrength;
    case kSetInfiniteHealth: return s.infiniteHealth ? 1.0f : 0.0f;
    case kSetWingCap: return s.wingCap ? 1.0f : 0.0f;
    case kSetMetalCap: return s.metalCap ? 1.0f : 0.0f;
    case kSetVanishCap: return s.vanishCap ? 1.0f : 0.0f;
    case kSetMoonJump: return s.moonJump ? 1.0f : 0.0f;
    case kSetBlj: return s.bljAnywhere ? 1.0f : 0.0f;
    case kSetShine: return s.shine;
    case kSetFollowHeight: return s.followHeight ? 1.0f : 0.0f;
    case kSetPhotoPoses: return s.photoPoses ? 1.0f : 0.0f;
    case kSetCameraDistance: return s.cameraDistance;
    default: return 0.0f;
    }
}

void SetSetting(LiveSettings& s, int id, float v) {
    const SettingItem* it = Find(id);
    if (!it || !std::isfinite(v)) return;
    if (it->kind == SettingItem::Slider) {
        // Snap to the slider's steps (what the menus can show).
        const int steps = std::max(1, it->displayMax - it->displayMin);
        const float t = std::min(1.0f, std::max(0.0f, (v - it->min) / (it->max - it->min)));
        v = it->min + (it->max - it->min) * std::round(t * float(steps)) / float(steps);
    }
    const bool on = v >= 0.5f;
    switch (id) {
    case kSetVolume: s.volume = v; break;
    case kSetAttack: s.attackStrength = v; break;
    case kSetInfiniteHealth: s.infiniteHealth = on; break;
    case kSetWingCap: s.wingCap = on; break;
    case kSetMetalCap: s.metalCap = on; break;
    case kSetVanishCap: s.vanishCap = on; break;
    case kSetMoonJump: s.moonJump = on; break;
    case kSetBlj: s.bljAnywhere = on; break;
    case kSetShine: s.shine = v; break;
    case kSetFollowHeight: s.followHeight = on; break;
    case kSetPhotoPoses: s.photoPoses = on; break;
    case kSetCameraDistance: s.cameraDistance = v; break;
    default: break;
    }
}

float StepSetting(const LiveSettings& s, int id, int dir) {
    const SettingItem* it = Find(id);
    const float cur = GetSetting(s, id);
    if (!it) return cur;
    switch (it->kind) {
    case SettingItem::Toggle: return cur >= 0.5f ? 0.0f : 1.0f;
    case SettingItem::Choice: {
        const int n = int(it->choices.size());
        return float(((int(std::lround(cur)) + dir) % n + n) % n);
    }
    case SettingItem::Slider: {
        const float step = (it->max - it->min) / float(std::max(1, it->displayMax - it->displayMin));
        return std::min(it->max, std::max(it->min, cur + step * float(dir)));
    }
    default: return cur;
    }
}

std::string FormatSetting(const LiveSettings& s, int id) {
    const SettingItem* it = Find(id);
    const float v = GetSetting(s, id);
    if (!it) return "";
    char b[32];
    switch (it->kind) {
    case SettingItem::Toggle: return v >= 0.5f ? "ON" : "OFF";
    case SettingItem::Choice: return it->choices[size_t(std::min<int>(int(it->choices.size()) - 1, std::max(0, int(v))))];
    case SettingItem::Slider:
        if (id == kSetAttack) std::snprintf(b, sizeof(b), "%.2fx", double(v));
        else if (id == kSetCameraDistance) std::snprintf(b, sizeof(b), "%.1fx", double(v));
        else std::snprintf(b, sizeof(b), "%d%%", int(std::lround(v * 100.0f)));
        return b;
    default: return "";
    }
}

void SettingsMenu::Show(const LiveSettings& current) {
    open_ = true;
    atOpen_ = current;
    const auto& items = SettingItems();
    if (cursor_ < 0 || cursor_ >= int(items.size()) || items[size_t(cursor_)].kind == SettingItem::Header) {
        cursor_ = 0;
        Move(+1);
    }
}

void SettingsMenu::Move(int dir) {
    const auto& items = SettingItems();
    const int n = int(items.size());
    for (int k = 0; k < n; ++k) {
        cursor_ = ((cursor_ + dir) % n + n) % n;
        if (items[size_t(cursor_)].kind != SettingItem::Header) return;
    }
}

bool SettingsMenu::Change(LiveSettings& s, int dir) {
    const auto& items = SettingItems();
    if (cursor_ < 0 || cursor_ >= int(items.size())) return false;
    const SettingItem& it = items[size_t(cursor_)];
    if (it.kind == SettingItem::Header) return false;
    const LiveSettings before = s;
    SetSetting(s, it.id, StepSetting(s, it.id, dir));
    return s != before;
}

bool SettingsMenu::Accept(LiveSettings& s) {
    const auto& items = SettingItems();
    if (cursor_ < 0 || cursor_ >= int(items.size())) return false;
    const SettingItem& it = items[size_t(cursor_)];
    if (it.kind != SettingItem::Toggle && it.kind != SettingItem::Choice) return false;
    return Change(s, +1);
}

bool SaveLiveSettings(const std::string& path, const LiveSettings& s, std::string* error) {
    const std::vector<Ini::Edit> edits = {
        {"Audio", "Volume", Num(s.volume)},
        {"Combat", "AttackStrength", Num(s.attackStrength)},
        {"Cheats", "InfiniteHealth", s.infiniteHealth ? "true" : "false"},
        {"Cheats", "WingCap", s.wingCap ? "true" : "false"},
        {"Cheats", "MetalCap", s.metalCap ? "true" : "false"},
        {"Cheats", "VanishCap", s.vanishCap ? "true" : "false"},
        {"Cheats", "MoonJump", s.moonJump ? "true" : "false"},
        {"Cheats", "BljAnywhere", s.bljAnywhere ? "true" : "false"},
        {"Render", "Gloss", Num(s.shine)},
        {"Camera", "FollowMarioHeight", s.followHeight ? "true" : "false"},
        {"Camera", "Distance", Num(s.cameraDistance)},
        {"Mario", "PoseForPhotos", s.photoPoses ? "true" : "false"},
    };
    // 0.3's single "Cap = wing|metal|vanish" is replaced by the three keys above.
    if (Ini::SetValuesInFile(path, edits, {{"Cheats", "Cap"}})) return true;
    if (error) *error = "couldn't write " + path;
    return false;
}

} // namespace sm2m
