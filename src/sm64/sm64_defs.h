// The few SM64 constants this mod needs, copied from libsm64's
// src/decomp/include/sm64.h and surface_terrains.h (values verified there).
#pragma once

#include <cstdint>

namespace sm2m::sm64 {

// Mario flags
constexpr uint32_t MARIO_NORMAL_CAP = 0x00000001;
constexpr uint32_t MARIO_VANISH_CAP = 0x00000002;
constexpr uint32_t MARIO_METAL_CAP = 0x00000004;
constexpr uint32_t MARIO_WING_CAP = 0x00000008;
constexpr uint32_t MARIO_CAP_ON_HEAD = 0x00000010;
constexpr uint32_t MARIO_CAP_IN_HAND = 0x00000020;
constexpr uint32_t MARIO_SPECIAL_CAPS = MARIO_VANISH_CAP | MARIO_METAL_CAP | MARIO_WING_CAP;
constexpr uint32_t MARIO_PUNCHING = 0x00100000;
constexpr uint32_t MARIO_KICKING = 0x00200000;
constexpr uint32_t MARIO_TRIPPING = 0x00400000;

// Action groups / flags
constexpr uint32_t ACT_ID_MASK = 0x000001FF;
constexpr uint32_t ACT_GROUP_MASK = 0x000001C0;
constexpr uint32_t ACT_GROUP_STATIONARY = 0x000;
constexpr uint32_t ACT_GROUP_MOVING = 0x040;
constexpr uint32_t ACT_GROUP_AIRBORNE = 0x080;
constexpr uint32_t ACT_GROUP_SUBMERGED = 0x0C0;
constexpr uint32_t ACT_GROUP_CUTSCENE = 0x100;
constexpr uint32_t ACT_GROUP_AUTOMATIC = 0x140;
constexpr uint32_t ACT_GROUP_OBJECT = 0x180;

constexpr uint32_t ACT_FLAG_AIR = 1u << 11;
constexpr uint32_t ACT_FLAG_INTANGIBLE = 1u << 12;
constexpr uint32_t ACT_FLAG_SWIMMING = 1u << 13;
constexpr uint32_t ACT_FLAG_INVULNERABLE = 1u << 17;
constexpr uint32_t ACT_FLAG_DIVING = 1u << 19;
constexpr uint32_t ACT_FLAG_ATTACKING = 1u << 23;

// Actions
constexpr uint32_t ACT_IDLE = 0x0C400201;
constexpr uint32_t ACT_WALKING = 0x04000440;
constexpr uint32_t ACT_JUMP = 0x03000880;
constexpr uint32_t ACT_DOUBLE_JUMP = 0x03000881;
constexpr uint32_t ACT_TRIPLE_JUMP = 0x01000882;
constexpr uint32_t ACT_BACKFLIP = 0x01000883;
constexpr uint32_t ACT_WALL_KICK_AIR = 0x03000886;
constexpr uint32_t ACT_SIDE_FLIP = 0x01000887;
constexpr uint32_t ACT_WATER_JUMP = 0x01000889;
constexpr uint32_t ACT_HOLD_JUMP = 0x030008A0;
constexpr uint32_t ACT_GROUND_BONK = 0x00020466;
constexpr uint32_t ACT_SOFT_BONK = 0x010208B6;
constexpr uint32_t ACT_AIR_HIT_WALL = 0x000008A7;
constexpr uint32_t ACT_BACKWARD_AIR_KB = 0x010208B0;
constexpr uint32_t ACT_HARD_BACKWARD_AIR_KB = 0x010208B3;
constexpr uint32_t ACT_LONG_JUMP = 0x03000888;
constexpr uint32_t ACT_LONG_JUMP_LAND = 0x00000479;
constexpr uint32_t ACT_FLYING = 0x10880899;
constexpr uint32_t ACT_PUTTING_ON_CAP = 0x0000133D;
constexpr uint32_t ACT_STAR_DANCE_NO_EXIT = 0x00001307;
constexpr uint32_t ACT_END_WAVING_CUTSCENE = 0x0000131A; // stubbed in libsm64: holds Mario still, plays any animation
constexpr uint32_t ACT_FREEFALL = 0x0100088C;
constexpr uint32_t ACT_LAVA_BOOST = 0x010208B7;
constexpr uint32_t ACT_SPAWN_SPIN_AIRBORNE = 0x00001924;
constexpr uint32_t ACT_GROUND_POUND = 0x008008A9;
constexpr uint32_t ACT_GROUND_POUND_LAND = 0x0080023C;
constexpr uint32_t ACT_DIVE = 0x0188088A;
constexpr uint32_t ACT_DIVE_SLIDE = 0x00880456;
constexpr uint32_t ACT_SLIDE_KICK = 0x018008AA;
constexpr uint32_t ACT_SLIDE_KICK_SLIDE = 0x0080045A;
constexpr uint32_t ACT_JUMP_KICK = 0x018008AC;
constexpr uint32_t ACT_PUNCHING = 0x00800380;
constexpr uint32_t ACT_MOVE_PUNCHING = 0x00800457;
constexpr uint32_t ACT_TWIRLING = 0x108008A4;
constexpr uint32_t ACT_WATER_PUNCH = 0x300024E1;
constexpr uint32_t ACT_STANDING_DEATH = 0x00021311;
constexpr uint32_t ACT_QUICKSAND_DEATH = 0x00021312;
constexpr uint32_t ACT_ELECTROCUTION = 0x00021313;
constexpr uint32_t ACT_SUFFOCATION = 0x00021314;
constexpr uint32_t ACT_DEATH_ON_STOMACH = 0x00021315;
constexpr uint32_t ACT_DEATH_ON_BACK = 0x00021316;
constexpr uint32_t ACT_DROWNING = 0x300032C4;
constexpr uint32_t ACT_WATER_DEATH = 0x300032C7;

// Animations (mario_animation_ids.h)
constexpr int32_t MARIO_ANIM_CREDITS_WAVING = 0x1D;
constexpr int32_t MARIO_ANIM_CREDITS_PEACE_SIGN = 0x27;
constexpr int32_t MARIO_ANIM_STAR_DANCE = 0xCD;

// Sounds (audio_defines.h): SOUND_ARG_LOAD(bank, playFlags, soundID, priority, flags2)
constexpr int32_t SoundArg(uint32_t bank, uint32_t playFlags, uint32_t id, uint32_t prio, uint32_t flags2) {
    return int32_t((bank << 28) | (playFlags << 24) | (id << 16) | (prio << 8) | (flags2 << 4) | 1u);
}
constexpr int32_t SOUND_MARIO_HERE_WE_GO = SoundArg(2, 4, 0x0C, 0x80, 8);
constexpr int32_t SOUND_MARIO_YAHOO = SoundArg(2, 4, 0x04, 0x80, 8);
constexpr int32_t SOUND_MARIO_HELLO = SoundArg(2, 4, 0x32, 0xFF, 8);

// Surfaces / terrain
constexpr int16_t SURFACE_DEFAULT = 0x0000;
constexpr int16_t SURFACE_NOT_SLIPPERY = 0x0015;
constexpr uint16_t TERRAIN_STONE = 0x0001;

// Health: 8 wedges, 0x880 = full.
constexpr int16_t HEALTH_FULL = 0x880;
inline int HealthWedges(int16_t health) { return health > 0 ? (health >> 8) : 0; }

// One SM64 frame.
constexpr double TICK_SECONDS = 1.0 / 30.0;

inline uint32_t ActionGroup(uint32_t action) { return action & ACT_GROUP_MASK; }
// In the air: the airborne actions, and the cutscene ones that fly or fall
// (ACT_FLAG_AIR) - like the spawn spin every (re)spawn starts with, which can
// drop a long way when the game has no floor under the spot yet.
inline bool IsAirborne(uint32_t action) {
    return ActionGroup(action) == ACT_GROUP_AIRBORNE ||
           ((action & ACT_FLAG_AIR) != 0 && ActionGroup(action) == ACT_GROUP_CUTSCENE);
}
inline bool IsSubmerged(uint32_t action) { return ActionGroup(action) == ACT_GROUP_SUBMERGED; }
// Left the ground on purpose (a jump of some kind, a dive, flying).
inline bool IsJump(uint32_t action) {
    switch (action) {
    case ACT_JUMP:
    case ACT_DOUBLE_JUMP:
    case ACT_TRIPLE_JUMP:
    case ACT_BACKFLIP:
    case ACT_WALL_KICK_AIR:
    case ACT_SIDE_FLIP:
    case ACT_LONG_JUMP:
    case ACT_WATER_JUMP:
    case ACT_HOLD_JUMP:
    case ACT_DIVE:
    case ACT_FLYING:
    case ACT_JUMP_KICK:
        return true;
    default:
        return false;
    }
}
// Knocked back off something solid (not by a hit: the mod checks his health).
inline bool IsBonk(uint32_t action) {
    return action == ACT_GROUND_BONK || action == ACT_SOFT_BONK || action == ACT_AIR_HIT_WALL ||
           action == ACT_BACKWARD_AIR_KB || action == ACT_HARD_BACKWARD_AIR_KB;
}
inline bool IsDying(uint32_t action) {
    switch (action) {
    case ACT_STANDING_DEATH:
    case ACT_QUICKSAND_DEATH:
    case ACT_ELECTROCUTION:
    case ACT_SUFFOCATION:
    case ACT_DEATH_ON_STOMACH:
    case ACT_DEATH_ON_BACK:
    case ACT_DROWNING:
    case ACT_WATER_DEATH:
        return true;
    default:
        return false;
    }
}

} // namespace sm2m::sm64
