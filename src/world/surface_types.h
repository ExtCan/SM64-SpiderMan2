// Spider-Man 2's physics materials -> SM64 surface types.
//
// The game's ray casts report the physics material of the surface they hit
// (an enum of 90 values: kNone, kAcid, kAsphalt, ... kWoodThin; the names are
// in Spider-Man2.exe's reflection data). SM64 picks Mario's footstep sound
// from the floor's terrain (grass, stone, snow, sand, wood ("spooky"), water)
// and its surface type, and a few types change how he moves (ice, lava).
#pragma once

#include <cstdint>

namespace sm2m {

struct SurfaceKind {
    int16_t type = 0;      // SURFACE_* (SM64 surface type)
    uint16_t terrain = 1;  // TERRAIN_* (SM64 terrain: footstep sounds)
};

constexpr int kPhysicsMaterialCount = 90;

// What Mario's floors and walls get for a game material (-1 / out of range:
// plain stone).
SurfaceKind SurfaceForMaterial(int material);
// The materials SM64 makes lava of (acid, lava): Mario jumps off them, burnt.
bool BurnsMario(int material);
// "kConcrete" etc. ("?" if unknown).
const char* PhysicsMaterialName(int material);
// kWaterAnkleHard .. kWaterWaistDeep.
bool IsWaterMaterial(int material);
// Water Mario swims in rather than stands on (deep water, the ocean, waterfalls).
bool IsDeepWater(int material);
// Water whose surface is Mario's water level (deep, the ocean, waist-deep) -
// not puddles, ankle-deep water or a waterfall's sheet.
bool IsSwimmableWater(int material);
// The footstep sound SM64 plays on a floor of this kind (SOUND_TERRAIN_*, as
// mario_get_terrain_sound_addend picks it for dry ground).
int FootstepSound(const SurfaceKind& k);
const char* FootstepSoundName(int sound);

// SM64 constants used here (surface_terrains.h / sounds.h).
namespace sm64s {
constexpr int16_t SURFACE_DEFAULT = 0x0000;
constexpr int16_t SURFACE_BURNING = 0x0001;
constexpr int16_t SURFACE_VERY_SLIPPERY = 0x0013;
constexpr int16_t SURFACE_SLIPPERY = 0x0014;
constexpr int16_t SURFACE_NOT_SLIPPERY = 0x0015;
constexpr int16_t SURFACE_NOISE_DEFAULT = 0x0029;
constexpr int16_t SURFACE_ICE = 0x002E;
constexpr uint16_t TERRAIN_GRASS = 0, TERRAIN_STONE = 1, TERRAIN_SNOW = 2, TERRAIN_SAND = 3, TERRAIN_SPOOKY = 4,
                   TERRAIN_WATER = 5, TERRAIN_SLIDE = 6;
constexpr int SOUND_TERRAIN_DEFAULT = 0, SOUND_TERRAIN_GRASS = 1, SOUND_TERRAIN_WATER = 2, SOUND_TERRAIN_STONE = 3,
              SOUND_TERRAIN_SPOOKY = 4, SOUND_TERRAIN_SNOW = 5, SOUND_TERRAIN_ICE = 6, SOUND_TERRAIN_SAND = 7;
} // namespace sm64s

} // namespace sm2m
