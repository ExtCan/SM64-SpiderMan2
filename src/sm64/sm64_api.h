// Function table for libsm64. Filled from sm64.dll at runtime on Windows
// (see LoadSm64Api) or by fakes in unit tests.
#pragma once

#include <string>

#include "libsm64.h"

namespace sm2m {

struct Sm64Api {
    decltype(&sm64_register_debug_print_function) register_debug_print_function = nullptr;
    decltype(&sm64_global_init) global_init = nullptr;
    decltype(&sm64_global_terminate) global_terminate = nullptr;
    decltype(&sm64_audio_init) audio_init = nullptr;
    decltype(&sm64_audio_tick) audio_tick = nullptr;
    decltype(&sm64_static_surfaces_load) static_surfaces_load = nullptr;
    decltype(&sm64_mario_create) mario_create = nullptr;
    decltype(&sm64_mario_tick) mario_tick = nullptr;
    decltype(&sm64_mario_delete) mario_delete = nullptr;
    decltype(&sm64_set_mario_action) set_mario_action = nullptr;
    decltype(&sm64_set_mario_position) set_mario_position = nullptr;
    decltype(&sm64_set_mario_faceangle) set_mario_faceangle = nullptr;
    decltype(&sm64_set_mario_velocity) set_mario_velocity = nullptr;
    decltype(&sm64_set_mario_forward_velocity) set_mario_forward_velocity = nullptr;
    decltype(&sm64_set_mario_invincibility) set_mario_invincibility = nullptr;
    decltype(&sm64_set_mario_water_level) set_mario_water_level = nullptr;
    decltype(&sm64_set_mario_health) set_mario_health = nullptr;
    decltype(&sm64_mario_take_damage) mario_take_damage = nullptr;
    decltype(&sm64_mario_heal) mario_heal = nullptr;
    decltype(&sm64_mario_attack) mario_attack = nullptr;
    decltype(&sm64_play_music) play_music = nullptr;
    decltype(&sm64_stop_background_music) stop_background_music = nullptr;
    decltype(&sm64_get_current_background_music) get_current_background_music = nullptr;
    decltype(&sm64_set_sound_volume) set_sound_volume = nullptr;
    decltype(&sm64_surface_find_floor_height) surface_find_floor_height = nullptr;
    decltype(&sm64_surface_find_wall_collision) surface_find_wall_collision = nullptr; // optional (debug trace)
    // optional: poses and cheats
    decltype(&sm64_set_mario_animation) set_mario_animation = nullptr;
    decltype(&sm64_set_mario_anim_frame) set_mario_anim_frame = nullptr;
    decltype(&sm64_set_mario_state) set_mario_state = nullptr;
    decltype(&sm64_mario_interact_cap) mario_interact_cap = nullptr;
    decltype(&sm64_mario_extend_cap) mario_extend_cap = nullptr;
    decltype(&sm64_play_sound_global) play_sound_global = nullptr;

    bool Complete() const;
};

#ifdef _WIN32
// Loads sm64.dll from `dllPath`. On failure returns false and sets `error`.
bool LoadSm64Api(const std::wstring& dllPath, Sm64Api& api, std::string& error);
#endif

} // namespace sm2m
