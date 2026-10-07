#ifdef _WIN32

#include "photo_mode.h"

#include <cstdio>

#include "../common/log.h"
#include "sm2.h"

namespace sm2m {
namespace photo_mode {
namespace {

bool g_installed = false;
uintptr_t g_system = 0; // the PhotomodeSystem object (in the exe's image)
uint32_t g_active = 0xCE9;
uint32_t g_doppelganger = 0x884, g_head = 0x88C, g_phone = 0x888;
std::string g_describe = "not found";

bool OffsetOk(int64_t v) { return v >= 8 && v < 0x4000; }

} // namespace

bool Install(Sm2Game& game, const Ini& b) {
    if (g_installed) return Available();
    g_installed = true;
    const char* kSec = "PhotoMode";
    if (b.GetString(kSec, "pattern", "").empty()) {
        LOGI("photo mode: no [PhotoMode] binding - Mario holds still in it like in any pause");
        g_describe = "no binding";
        return false;
    }
    std::string d;
    const uintptr_t sys = game.ResolveSection(b, kSec, d);
    const int64_t active = b.GetInt(kSec, "active_offset", 0xCE9);
    const int64_t doppel = b.GetInt(kSec, "doppelganger_offset", 0x884);
    const int64_t head = b.GetInt(kSec, "doppelganger_head_offset", 0x88C);
    const int64_t phone = b.GetInt(kSec, "selfie_phone_offset", 0x888);
    if (!sys || !OffsetOk(active) || !OffsetOk(doppel) || !OffsetOk(head) || !OffsetOk(phone) ||
        !game.InImage(sys, size_t(active) + 1)) {
        LOGW("photo mode: [PhotoMode] not found in this game version (%s) - Mario holds still in it like in any "
             "pause, but a selfie shows Spider-Man",
             d.c_str());
        g_describe = "not found";
        return false;
    }
    uint8_t flag = 0xFF;
    if (!SafeReadT(sys + uintptr_t(active), flag) || flag > 1) {
        LOGW("photo mode: the object at exe+0x%llx doesn't look like the game's photo mode (flag %u) - not used",
             static_cast<unsigned long long>(sys - game.ModuleBase()), unsigned(flag));
        g_describe = "doesn't fit";
        return false;
    }
    g_system = sys;
    g_active = uint32_t(active);
    g_doppelganger = uint32_t(doppel);
    g_head = uint32_t(head);
    g_phone = uint32_t(phone);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "PhotomodeSystem @exe+0x%llx, open flag +0x%X",
                  static_cast<unsigned long long>(sys - game.ModuleBase()), g_active);
    g_describe = buf;
    LOGI("photo mode: watching the game's photo mode (%s)", buf);
    return true;
}

bool Available() { return g_system != 0; }

bool Open() {
    if (!g_system) return false;
    uint8_t flag = 0;
    return SafeReadT(g_system + g_active, flag) && flag == 1;
}

StandIns ReadStandIns() {
    StandIns s;
    if (!g_system) return s;
    SafeReadT(g_system + g_doppelganger, s.doppelganger);
    SafeReadT(g_system + g_head, s.head);
    SafeReadT(g_system + g_phone, s.phone);
    return s;
}

std::string Describe() { return g_describe; }

} // namespace photo_mode
} // namespace sm2m

#endif
