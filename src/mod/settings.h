// Settings the player changes while playing - from the mod's own menu (F8) or
// the ModSettings mod's pause menu - and that are saved back into the user's
// sm2mario.ini. One list of items describes both menus.
// Platform independent (unit tested).
#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace sm2m {

struct LiveSettings {
    float volume = 0.8f;          // [Audio] Volume, 0..1
    float attackStrength = 1.0f;  // [Combat] AttackStrength: scales damage and knockback
    bool infiniteHealth = false;  // [Cheats] InfiniteHealth
    // [Cheats] WingCap / MetalCap / VanishCap: any combination, as in SM64
    // (one cap timer for all of them).
    bool wingCap = false;
    bool metalCap = false;
    bool vanishCap = false;
    bool moonJump = false;        // [Cheats] MoonJump: hold jump in the air to keep rising
    bool bljAnywhere = false;     // [Cheats] BljAnywhere: jump again during a backwards long jump
    float shine = 0.15f;          // [Render] Gloss
    bool followHeight = true;     // [Camera] FollowMarioHeight
    float cameraDistance = 1.5f;  // [Camera] Distance: times the game's own framing (0.5..3)
    bool photoPoses = true;       // [Mario] PoseForPhotos

    bool operator==(const LiveSettings& o) const;
    bool operator!=(const LiveSettings& o) const { return !(*this == o); }
};

LiveSettings LiveFromConfig(const ModConfig& c);

// A menu line. Values are floats for every kind (toggles 0/1, choices 0..n-1).
struct SettingItem {
    enum Kind { Header, Toggle, Choice, Slider };
    Kind kind = Header;
    int id = -1;                 // which setting (Get/SetSetting); -1 for headers
    const char* label = "";      // upper case, like the game's menus
    const char* desc = "";
    float min = 0, max = 1;      // sliders
    int displayMin = 0, displayMax = 10; // the slider's displayed range (min..max mapped linearly)
    std::vector<const char*> choices;    // choices
};

enum SettingId {
    kSetVolume,
    kSetAttack,
    kSetInfiniteHealth,
    kSetWingCap,
    kSetMetalCap,
    kSetVanishCap,
    kSetMoonJump,
    kSetBlj,
    kSetShine,
    kSetFollowHeight,
    kSetPhotoPoses,
    kSetCameraDistance,
    kSetCount
};

const std::vector<SettingItem>& SettingItems();
float GetSetting(const LiveSettings& s, int id);
void SetSetting(LiveSettings& s, int id, float v); // clamped / rounded to the item's range
// The value one step left (-1) or right (+1) of the current one.
float StepSetting(const LiveSettings& s, int id, int dir);
// "80%", "ON", "WING CAP"...
std::string FormatSetting(const LiveSettings& s, int id);

// The mod's own menu (F8): a cursor over SettingItems() (headers skipped).
class SettingsMenu {
public:
    bool Open() const { return open_; }
    void Show(const LiveSettings& current);
    void Hide() { open_ = false; }
    void Up() { Move(-1); }
    void Down() { Move(+1); }
    // Left / right change the selected value; accept flips toggles and
    // cycles choices. They return true if `s` changed.
    bool Left(LiveSettings& s) { return Change(s, -1); }
    bool Right(LiveSettings& s) { return Change(s, +1); }
    bool Accept(LiveSettings& s);
    int Cursor() const { return cursor_; } // index into SettingItems()
    // Settings as they were when the menu opened (to save only if changed).
    const LiveSettings& AtOpen() const { return atOpen_; }

private:
    void Move(int dir);
    bool Change(LiveSettings& s, int dir);
    bool open_ = false;
    int cursor_ = 1;
    LiveSettings atOpen_;
};

// Writes every live setting into the user's ini (comments and other keys
// kept). False with a reason if the file can't be written.
bool SaveLiveSettings(const std::string& userIni, const LiveSettings& s, std::string* error = nullptr);

} // namespace sm2m
