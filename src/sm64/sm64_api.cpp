#include "sm64_api.h"

#ifdef _WIN32
#include "../common/platform.h"
#endif

namespace sm2m {

bool Sm64Api::Complete() const {
    return global_init && audio_init && audio_tick && static_surfaces_load && mario_create && mario_tick &&
           mario_delete && set_mario_position && set_mario_faceangle && set_mario_velocity &&
           set_mario_invincibility && set_mario_water_level && set_mario_health && mario_take_damage &&
           mario_heal && mario_attack && set_mario_action && surface_find_floor_height;
}

#ifdef _WIN32
namespace {
template <typename T>
void Bind(HMODULE mod, const char* name, T& fn) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(mod, name)));
}
} // namespace

bool LoadSm64Api(const std::wstring& dllPath, Sm64Api& api, std::string& error) {
    HMODULE mod = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        error = "could not load sm64.dll (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    Bind(mod, "sm64_register_debug_print_function", api.register_debug_print_function);
    Bind(mod, "sm64_global_init", api.global_init);
    Bind(mod, "sm64_global_terminate", api.global_terminate);
    Bind(mod, "sm64_audio_init", api.audio_init);
    Bind(mod, "sm64_audio_tick", api.audio_tick);
    Bind(mod, "sm64_static_surfaces_load", api.static_surfaces_load);
    Bind(mod, "sm64_mario_create", api.mario_create);
    Bind(mod, "sm64_mario_tick", api.mario_tick);
    Bind(mod, "sm64_mario_delete", api.mario_delete);
    Bind(mod, "sm64_set_mario_action", api.set_mario_action);
    Bind(mod, "sm64_set_mario_position", api.set_mario_position);
    Bind(mod, "sm64_set_mario_faceangle", api.set_mario_faceangle);
    Bind(mod, "sm64_set_mario_velocity", api.set_mario_velocity);
    Bind(mod, "sm64_set_mario_forward_velocity", api.set_mario_forward_velocity);
    Bind(mod, "sm64_set_mario_invincibility", api.set_mario_invincibility);
    Bind(mod, "sm64_set_mario_water_level", api.set_mario_water_level);
    Bind(mod, "sm64_set_mario_health", api.set_mario_health);
    Bind(mod, "sm64_mario_take_damage", api.mario_take_damage);
    Bind(mod, "sm64_mario_heal", api.mario_heal);
    Bind(mod, "sm64_mario_attack", api.mario_attack);
    Bind(mod, "sm64_play_music", api.play_music);
    Bind(mod, "sm64_stop_background_music", api.stop_background_music);
    Bind(mod, "sm64_get_current_background_music", api.get_current_background_music);
    Bind(mod, "sm64_set_sound_volume", api.set_sound_volume);
    Bind(mod, "sm64_surface_find_floor_height", api.surface_find_floor_height);
    Bind(mod, "sm64_surface_find_wall_collision", api.surface_find_wall_collision);
    Bind(mod, "sm64_set_mario_animation", api.set_mario_animation);
    Bind(mod, "sm64_set_mario_anim_frame", api.set_mario_anim_frame);
    Bind(mod, "sm64_set_mario_state", api.set_mario_state);
    Bind(mod, "sm64_mario_interact_cap", api.mario_interact_cap);
    Bind(mod, "sm64_mario_extend_cap", api.mario_extend_cap);
    Bind(mod, "sm64_play_sound_global", api.play_sound_global);
    if (!api.Complete()) {
        error = "sm64.dll is missing exports (it is older than libsm64 commit fd11813)";
        return false;
    }
    return true;
}
#endif

} // namespace sm2m
