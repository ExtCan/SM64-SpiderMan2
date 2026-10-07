#include "surface_types.h"

namespace sm2m {

namespace {

using namespace sm64s;

// Spider-Man2.exe's physics material enum, in order (its reflection table).
const char* const kNames[kPhysicsMaterialCount] = {
    "kNone", "kAcid", "kAsphalt", "kBlood", "kBuilding", "kCanvas", "kCarpet", "kCarpetIndustrial", "kCarpetSoft",
    "kConcrete", "kConcreteDirty", "kConcreteWet", "kCloud", "kDirt", "kEnergy", "kFlesh", "kFleshExotic", "kFoliage",
    "kFoliageThick", "kFoliageThin", "kGenericManMade", "kGlassBroken", "kGlassExoticUnbreakable", "kGlassMedium",
    "kGlassThick", "kGlassThin", "kGlassUnbreakable", "kGoo", "kGrass", "kGrassThick", "kGravel", "kGravelWet",
    "kGrindRail", "kHoverboardAutoKill", "kIce", "kIceThick", "kIceThin", "kIceWall", "kLavaActive", "kLavaLazy",
    "kMagBoot", "kMetalCable", "kMetalGrate", "kMetalChainLink", "kMetalHollow", "kMetalMedium", "kMetalPipe",
    "kMetalRail", "kMetalThick", "kMetalThin", "kMetalWet", "kMetalFloorGrate", "kMetalFloorHollow",
    "kMetalFloorMedium", "kMetalFloorThick", "kMetalFloorThin", "kMud", "kMudWet", "kNarrowPlatform", "kOil", "kPaper",
    "kPlasticHard", "kPlasticSoft", "kRubber", "kSand", "kSandThin", "kSnow", "kSnowDeep", "kSnowHard",
    "kStoneBrittle", "kStoneMedium", "kStoneSolid", "kTar", "kTarp", "kVinyl", "kWaterAnkleHard", "kWaterAnkleSoft",
    "kWaterDeep", "kWaterfall", "kWaterOcean", "kWaterPuddleHard", "kWaterPuddleSoft", "kWaterWaistDeep", "kWeb",
    "kWoodCreaky", "kWoodHard", "kWoodHollow", "kWoodMedium", "kWoodThick", "kWoodThin",
};

constexpr SurfaceKind kStone{SURFACE_DEFAULT, TERRAIN_STONE};    // concrete, asphalt, metal, glass: "stone" steps
constexpr SurfaceKind kWood{SURFACE_DEFAULT, TERRAIN_SPOOKY};    // wooden steps
constexpr SurfaceKind kGrass{SURFACE_NOISE_DEFAULT, TERRAIN_GRASS}; // rustling grass
constexpr SurfaceKind kDirt{SURFACE_DEFAULT, TERRAIN_GRASS};     // soft ground: the plain step
constexpr SurfaceKind kSand{SURFACE_DEFAULT, TERRAIN_SAND};
constexpr SurfaceKind kSnow{SURFACE_DEFAULT, TERRAIN_SNOW};
constexpr SurfaceKind kIce{SURFACE_ICE, TERRAIN_SNOW};           // slides like SM64's ice
constexpr SurfaceKind kWet{SURFACE_DEFAULT, TERRAIN_WATER};      // puddles, shallow water
constexpr SurfaceKind kSlick{SURFACE_SLIPPERY, TERRAIN_STONE};   // oil
constexpr SurfaceKind kLava{SURFACE_BURNING, TERRAIN_STONE};     // lava (and acid): Mario jumps off it, burnt

SurfaceKind Classify(int m) {
    switch (m) {
    case 1: return kLava;                                       // acid
    case 5: case 6: case 7: case 8: case 60: case 62: case 63: case 73: case 74: return kDirt; // fabric, paper, rubber
    case 13: case 30: case 31: case 56: case 57: case 15: case 16: case 3: case 27: case 83: return kDirt;
    case 17: case 18: case 19: case 28: case 29: return kGrass;
    case 34: case 35: case 36: case 37: return kIce;
    case 38: case 39: return kLava;
    case 59: return kSlick;
    case 64: case 65: return kSand;
    case 66: case 67: case 68: return kSnow;
    case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 82: return kWet;
    case 84: case 85: case 86: case 87: case 88: case 89: return kWood;
    default: return kStone;
    }
}

} // namespace

SurfaceKind SurfaceForMaterial(int material) {
    if (material < 0 || material >= kPhysicsMaterialCount) return kStone;
    return Classify(material);
}

bool BurnsMario(int material) { return SurfaceForMaterial(material).type == SURFACE_BURNING; }

bool IsWaterMaterial(int m) { return m >= 75 && m <= 82; }
bool IsDeepWater(int m) { return m == 77 || m == 78 || m == 79 || m == 82; }
bool IsSwimmableWater(int m) { return m == 77 || m == 79 || m == 82; }

const char* PhysicsMaterialName(int material) {
    if (material < 0) return "none";
    if (material >= kPhysicsMaterialCount) return "?";
    return kNames[material];
}

int FootstepSound(const SurfaceKind& k) {
    // sTerrainSounds[terrain][floor sound type] (mario.c), for the types used here.
    static const int kTable[7][6] = {
        {SOUND_TERRAIN_DEFAULT, SOUND_TERRAIN_STONE, SOUND_TERRAIN_GRASS, SOUND_TERRAIN_GRASS, SOUND_TERRAIN_GRASS,
         SOUND_TERRAIN_DEFAULT},
        {SOUND_TERRAIN_STONE, SOUND_TERRAIN_STONE, SOUND_TERRAIN_STONE, SOUND_TERRAIN_STONE, SOUND_TERRAIN_GRASS,
         SOUND_TERRAIN_GRASS},
        {SOUND_TERRAIN_SNOW, SOUND_TERRAIN_ICE, SOUND_TERRAIN_SNOW, SOUND_TERRAIN_ICE, SOUND_TERRAIN_STONE,
         SOUND_TERRAIN_STONE},
        {SOUND_TERRAIN_SAND, SOUND_TERRAIN_STONE, SOUND_TERRAIN_SAND, SOUND_TERRAIN_SAND, SOUND_TERRAIN_STONE,
         SOUND_TERRAIN_STONE},
        {SOUND_TERRAIN_SPOOKY, SOUND_TERRAIN_SPOOKY, SOUND_TERRAIN_SPOOKY, SOUND_TERRAIN_SPOOKY, SOUND_TERRAIN_STONE,
         SOUND_TERRAIN_STONE},
        {SOUND_TERRAIN_DEFAULT, SOUND_TERRAIN_STONE, SOUND_TERRAIN_GRASS, SOUND_TERRAIN_ICE, SOUND_TERRAIN_STONE,
         SOUND_TERRAIN_ICE},
        {SOUND_TERRAIN_STONE, SOUND_TERRAIN_STONE, SOUND_TERRAIN_STONE, SOUND_TERRAIN_STONE, SOUND_TERRAIN_ICE,
         SOUND_TERRAIN_ICE},
    };
    int col = 0;
    switch (k.type) {
    case SURFACE_NOT_SLIPPERY: col = 1; break;
    case SURFACE_SLIPPERY: col = 2; break;
    case SURFACE_VERY_SLIPPERY: case SURFACE_ICE: col = 3; break;
    case SURFACE_NOISE_DEFAULT: col = 4; break;
    default: col = 0; break;
    }
    const int t = k.terrain < 7 ? k.terrain : 1;
    return kTable[t][col];
}

const char* FootstepSoundName(int sound) {
    switch (sound) {
    case SOUND_TERRAIN_DEFAULT: return "default";
    case SOUND_TERRAIN_GRASS: return "grass";
    case SOUND_TERRAIN_WATER: return "water";
    case SOUND_TERRAIN_STONE: return "stone";
    case SOUND_TERRAIN_SPOOKY: return "wood";
    case SOUND_TERRAIN_SNOW: return "snow";
    case SOUND_TERRAIN_ICE: return "ice";
    case SOUND_TERRAIN_SAND: return "sand";
    default: return "?";
    }
}

} // namespace sm2m
