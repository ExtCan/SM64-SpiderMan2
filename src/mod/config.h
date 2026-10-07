// Parsed user configuration (sm2mario.ini). Platform independent so the
// defaults can be unit tested against the shipped file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../common/ini.h"
#include "../game/combat.h"
#include "../world/collision.h"
#include "../world/coords.h"
#include "../world/physics_world.h"

namespace sm2m {

struct ModConfig {
    // [General]
    std::string logLevel = "info";
    bool showHud = true;
    bool showStartupToast = true;

    // [Mario]
    std::string romFile = "sm64.us.z64";
    std::string fallDamage = "capped"; // off | capped | sm64
    std::string onDeath = "respawn";   // respawn | hero
    float regenSeconds = 8.0f;
    bool photoPoses = true;            // pose when pedestrians ask for a picture
    float respawnAfterFallSeconds = 30.0f; // (0.4: 12 - too short for a jump off the tallest towers)
    float brightness = 1.0f;
    float ambient = 0.55f;

    // [World]
    char upAxis = 'Y';
    std::string mirror = "auto";       // auto | on | off
    double unitsPerMetre = 100.0;      // SM64 units per game unit
    bool hasWater = false;
    double waterHeight = 0.0;
    float teleportDistance = 12.0f;
    std::string onTeleport = "follow"; // follow | off
    OriginPolicy origin;

    // [Collision]
    std::string collisionSource = "physics"; // physics (the game's own collision) | world (its rendered depth) | flat
    PhysicsWorldParams physics;
    int physicsRaysPerFrame = 32;            // rays cast per game frame
    int physicsPoolHeadroom = 96;            // query results per frame left to the game
    // Actors with one of these components aren't solid for Mario (people)...
    std::vector<std::string> notSolidComponents{"Health", "Pedestrian", "Ragdoll"};
    // ... unless they also have one of these (vehicles).
    std::vector<std::string> solidComponents{"Vehicle", "LandVehicle", "VehicleBase"};
    float worldRange = 30.0f;               // metres of remembered world around Mario
    float worldCell = 0.25f;
    CollisionParams collision;
    float rebuildDistance = 1.5f;
    float rebuildVertical = 3.0f;
    float rebuildSeconds = 1.0f;

    // [Camera]
    std::string cameraSource = "auto";
    std::string cameraActorName;
    float fovDegrees = 60.0f;
    bool useScannedFov = true;
    bool flipRight = false;
    float lookAtHeight = 1.0f;
    float nearPlane = 0.05f;
    int scanThreads = 3;
    int maxRegionMB = 4096;
    bool followMarioHeight = true;     // the game camera's target reports Mario's position
    bool cameraOverride = true;        // the mod places the game's camera around Mario (the game still aims it)
    float cameraVerticalSmoothing = 0.05f; // s: ... its height follows Mario's jumps this smoothly
    float cameraDistance = 1.5f;       // ... this many times as far from Mario as the game's framing
    bool cameraCollision = true;       // ... and comes in front of walls (the game's physics, its camera query)
    bool placeOtherCameras = true;     // ... and the game's other cameras it is seen rendering from (0.6.1)
    int cameraQuery = 11;              // the game's query type for that (11 = kCamera)
    float followLead = 1.0f;           // how much of the camera's lag behind Mario's jumps is made up (0 = off)
    float followLeadMax = 3.0f;        // metres

    // [Hero]
    std::string hideHero = "engine";   // engine | write | none
    bool keepHeroAlive = true;
    std::string positionWrites = "function"; // function (Transform::SetPosition) | direct
    bool holdDuringFrame = true;       // undo the game's own moves of Spider-Man off Mario
    bool keepHeroHidden = true;        // refuse the game's Transform::Unhide on the hidden Spider-Man
    float interactRelease = 8.0f;      // s the game may move Spider-Man after the player answers a pedestrian's prompt
    float holdRadius = 3.0f;           // ...up to this far horizontally (metres); farther = a teleport

    // [Combat]
    bool combat = true;
    float enemyScanRadius = 25.0f;
    int enemySlotsPerTick = 768;
    std::vector<std::string> enemyInclude;
    std::vector<std::string> enemyExclude;
    // Who can be hit: actors with one of these components (or one derived
    // from it: enemies carry BotHealth, a Health), first found wins ...
    std::vector<std::string> healthComponents{"Health", "BotHealth"};
    // ... and of those, passers-by (they react to Mario's hits, unhurt).
    std::vector<std::string> personComponents{"Pedestrian", "Civilian"};
    CombatTuning tuning;
    bool logHits = false;
    float attackStrength = 1.0f;       // scales damage and knockback of Mario's attacks
    bool gameDamage = true;            // hits go through the game's damage system (hit reactions); else health writes
    bool hitPeople = true;             // ... and people Mario hits react too (they take no damage)

    // [Cheats]
    bool infiniteHealth = false;
    bool wingCap = false, metalCap = false, vanishCap = false; // any combination
    bool moonJump = false;
    bool bljAnywhere = false;

    // [Audio]
    bool audio = true;
    float volume = 0.8f;

    // [Input]
    bool pauseWhenCursorVisible = true;
    bool pauseWithGame = true;         // Mario stops while the game is paused (its physics frame doesn't step)
    bool onlyMarioControls = true;     // the game keeps every control Mario doesn't use (menus, prompts, d-pad)
    bool blockGameInput = true;

    // [Render]
    std::string renderMode = "world";  // world (inside the game's frame) | overlay
    bool shadows = true;
    std::string shadowRegions = "auto"; // auto | refreshed | steady (see sm2mario.ini)
    bool frameReport = true;
    float fallbackSeconds = 3.0f;      // world -> overlay if Mario can't be put in the frame
    float gloss = 0.15f;               // SHINE (gloss, reflections and F0 together; render/gbuffer_format.h)
    float specular = 0.02f;            // F0 at SHINE 50%
    int metalMaterial = 0;             // metal cap: the game's material table entry for its reflections (-1 = none)
    float albedoScale = 0.85f;
    float shadowBias = 0.0f;
    bool stencilMatch = true;
    std::string marioStencil = "auto"; // auto (Spider-Man's mark, learnt) | keep | copy | a number 0-255
    int stencilCensusFrames = 150;     // auto: frames with Spider-Man on screen, and with Mario, before it decides
    bool alignToCamera = true;         // Mario drawn where the camera of each view had him (render/injector.h)
    std::vector<std::string> gbufferMarkers{"GBuffer Dynamic", "GBuffer Animated"};
    std::vector<uint32_t> gbufferFormats{41, 34, 17, 17};
    int captureWidth = 480;
    float paperWhiteNits = 200.0f;

    // [Debug]
    bool debugOverlayOnStart = false;
    bool calibrateOnStart = false;
    bool traceCollision = false; // log Mario's moves, his collision and the world model near him (debugging)
    bool traceCamera = false;    // log each view the game rendered from somewhere the mod didn't put a camera
};

// Reads everything from `ini` (missing keys keep the defaults above).
ModConfig LoadModConfig(const Ini& ini);

// Settings whose shipped default changed: a user file from before the change
// still holding the old default gets the new one (in place, comments kept).
// The file then records the version it was brought up to ([General]
// SettingsVersion), so a value the user picks again afterwards stays.
// Returns what changed.
std::vector<std::string> MigrateUserIni(const std::string& shippedPath, const std::string& userPath,
                                        const std::string& version);
// "0.4.0" -> comparable number (missing parts 0; unparseable: 0).
uint64_t SettingsVersionNumber(const std::string& v);

} // namespace sm2m
