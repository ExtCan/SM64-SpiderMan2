#include "config.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sm2m {

namespace {
// Shipped defaults that later versions changed: the old default, and the
// version whose default differs.
struct Migration {
    const char* section;
    const char* key;
    const char* oldDefault;
    const char* changedIn;
};
const Migration kMigrations[] = {
    {"Render", "Gloss", "0.45", "0.3.0"},       // 0.2.0's: too shiny in game
    {"Render", "Specular", "0.04", "0.3.0"},    // 0.2.0's
    {"Render", "Gloss", "0.15", "0.4.0"},       // 0.3.0's (unchanged so far: kept for the record)
    {"Render", "Specular", "0.03", "0.4.0"},    // 0.3.0's: still too shiny
    {"Collision", "Source", "world", "0.4.0"},  // 0.3.0's: the game's own collision replaces the rendered world
    {"Controls", "KeyPunch", "LMB, E", "0.4.0"}, // 0.3.0's: E is left to the game (its interact key)
    {"Mario", "RespawnAfterFallSeconds", "12", "0.5.0"}, // 0.4.0's: a jump off a tower falls for ~20 s
    {"Collision", "RoofSearchHeight", "20", "0.6.0"},     // 0.5's: awnings and trees overhead used up the floor rays' hits
    {"Collision", "RaysPerFrame", "24", "0.6.0"},          // 0.5's: running and long jumps outran the answers
    {"Camera", "VerticalSmoothing", "0.15", "0.6.0"},      // 0.5's: the camera lagged behind his jumps
};

bool SameValue(const std::string& a, const std::string& b) {
    double x, y;
    if (Ini::ParseFloat(a, x) && Ini::ParseFloat(b, y)) return std::fabs(x - y) <= 1e-6;
    return Ini::Lower(a) == Ini::Lower(b);
}

bool ReadText(const std::string& path, std::string& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
} // namespace

uint64_t SettingsVersionNumber(const std::string& v) {
    uint64_t n = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 3) {
        uint64_t p = 0;
        bool digits = false;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            p = p * 10 + uint64_t(v[i] - '0');
            if (p > 99999) return 0;
            digits = true;
            ++i;
        }
        if (!digits && i < v.size()) return 0;
        n = n * 100000 + p;
        ++parts;
        if (i < v.size() && v[i] == '.') ++i;
        else break;
    }
    for (; parts < 3; ++parts) n *= 100000;
    return n;
}

std::vector<std::string> MigrateUserIni(const std::string& shippedPath, const std::string& userPath,
                                        const std::string& version) {
    std::vector<std::string> changed;
    std::string shipped, user;
    if (!ReadText(shippedPath, shipped) || !ReadText(userPath, user)) return changed;
    std::string out = user;
    std::string fileVersion;
    Ini::GetValueInText(out, "General", "SettingsVersion", fileVersion);
    const uint64_t from = SettingsVersionNumber(fileVersion), to = SettingsVersionNumber(version);
    for (const Migration& m : kMigrations) {
        if (SettingsVersionNumber(m.changedIn) <= from) continue; // the file is from after this change
        std::string cur, now;
        if (!Ini::GetValueInText(out, m.section, m.key, cur) || !Ini::GetValueInText(shipped, m.section, m.key, now))
            continue;
        if (!SameValue(cur, m.oldDefault)) continue;
        if (SameValue(now, m.oldDefault)) continue; // default unchanged
        out = Ini::SetValueInText(out, m.section, m.key, now);
        changed.push_back(std::string("[") + m.section + "] " + m.key + " " + cur + " -> " + now +
                          " (new default; your file had the old one)");
    }
    // 0.3's one cap at a time ("Cap = wing") becomes the caps that combine.
    std::string cap;
    if (Ini::GetValueInText(out, "Cheats", "Cap", cap)) {
        std::string any;
        const bool hasNew = Ini::GetValueInText(out, "Cheats", "WingCap", any) ||
                            Ini::GetValueInText(out, "Cheats", "MetalCap", any) ||
                            Ini::GetValueInText(out, "Cheats", "VanishCap", any);
        if (!hasNew) {
            const std::string c = Ini::Lower(cap);
            out = Ini::SetValueInText(out, "Cheats", "WingCap", c == "wing" ? "true" : "false");
            out = Ini::SetValueInText(out, "Cheats", "MetalCap", c == "metal" ? "true" : "false");
            out = Ini::SetValueInText(out, "Cheats", "VanishCap", c == "vanish" ? "true" : "false");
        }
        out = Ini::RemoveKeyInText(out, "Cheats", "Cap");
        changed.push_back("[Cheats] Cap = " + cap + " -> WingCap / MetalCap / VanishCap (caps combine now)");
    }
    if (to > from) out = Ini::SetValueInText(out, "General", "SettingsVersion", version);
    if (out != user) {
        std::ofstream f(std::filesystem::u8path(userPath), std::ios::binary | std::ios::trunc);
        if (f) f << out;
    }
    return changed;
}

ModConfig LoadModConfig(const Ini& ini) {
    ModConfig c;
    auto str = [&](const char* s, const char* k, std::string& v) { v = ini.GetString(s, k, v); };
    auto low = [&](const char* s, const char* k, std::string& v) { v = Ini::Lower(ini.GetString(s, k, v)); };
    auto flt = [&](const char* s, const char* k, float& v) { v = float(ini.GetFloat(s, k, v)); };
    auto dbl = [&](const char* s, const char* k, double& v) { v = ini.GetFloat(s, k, v); };
    auto bol = [&](const char* s, const char* k, bool& v) { v = ini.GetBool(s, k, v); };
    auto num = [&](const char* s, const char* k, int& v) { v = int(ini.GetInt(s, k, v)); };

    low("General", "LogLevel", c.logLevel);
    bol("General", "ShowHud", c.showHud);
    bol("General", "StartupMessage", c.showStartupToast);

    str("Mario", "RomFile", c.romFile);
    low("Mario", "FallDamage", c.fallDamage);
    low("Mario", "OnDeath", c.onDeath);
    flt("Mario", "RegenSeconds", c.regenSeconds);
    bol("Mario", "PoseForPhotos", c.photoPoses);
    flt("Mario", "RespawnAfterFallSeconds", c.respawnAfterFallSeconds);
    flt("Mario", "Brightness", c.brightness);
    flt("Mario", "Ambient", c.ambient);

    std::string up(1, c.upAxis);
    str("World", "UpAxis", up);
    c.upAxis = (!up.empty() && (up[0] == 'Z' || up[0] == 'z')) ? 'Z' : 'Y';
    low("World", "Mirror", c.mirror);
    dbl("World", "Sm64UnitsPerMetre", c.unitsPerMetre);
    std::string water = ini.GetString("World", "WaterHeight", "none");
    double wh;
    if (Ini::ParseFloat(water, wh)) {
        c.hasWater = true;
        c.waterHeight = wh;
    }
    flt("World", "TeleportDistance", c.teleportDistance);
    low("World", "OnHeroTeleport", c.onTeleport);
    flt("World", "OriginHorizontalLimit", c.origin.horizontalLimit);
    flt("World", "OriginFallLimit", c.origin.airborneFallLimit);

    low("Collision", "Source", c.collisionSource);
    if (c.collisionSource == "raycast") c.collisionSource = "physics"; // 0.3's name for it (never worked)
    if (c.collisionSource != "flat" && c.collisionSource != "world") c.collisionSource = "physics";
    {
        PhysicsWorldParams& ph = c.physics;
        num("Collision", "RaysPerFrame", c.physicsRaysPerFrame);
        c.physicsRaysPerFrame = std::max(4, std::min(64, c.physicsRaysPerFrame));
        num("Collision", "QueryHeadroom", c.physicsPoolHeadroom);
        c.physicsPoolHeadroom = std::max(16, std::min(240, c.physicsPoolHeadroom));
        flt("Collision", "WallRayLength", ph.ringRadius);
        ph.ringRadius = std::max(1.5f, std::min(12.0f, ph.ringRadius));
        num("Collision", "WallRayDirections", ph.ringDirs);
        flt("Collision", "RoofSearchHeight", ph.castAbove);
        int t = ph.floorType;
        num("Collision", "FloorQuery", t);
        ph.floorType = uint8_t(std::max(0, std::min(31, t)));
        t = ph.wallType;
        num("Collision", "WallQuery", t);
        ph.wallType = uint8_t(std::max(0, std::min(31, t)));
        t = ph.waterType;
        num("Collision", "WaterQuery", t);
        ph.waterType = uint8_t(std::max(0, std::min(31, t)));
        if (ini.Has("Collision", "NotSolidComponents")) {
            c.notSolidComponents.clear();
            for (const auto& s : Ini::SplitList(ini.GetString("Collision", "NotSolidComponents", "")))
                c.notSolidComponents.push_back(s);
        }
        if (ini.Has("Collision", "SolidComponents")) {
            c.solidComponents.clear();
            for (const auto& s : Ini::SplitList(ini.GetString("Collision", "SolidComponents", "")))
                c.solidComponents.push_back(s);
        }
    }
    flt("Collision", "WorldRange", c.worldRange);
    flt("Collision", "WorldCellSize", c.worldCell);
    CollisionParams& p = c.collision;
    flt("Collision", "CellSize", p.cellSize);
    num("Collision", "GridRadiusCells", p.gridRadiusCells);
    flt("Collision", "ProbeUp", p.probeUp);
    flt("Collision", "ProbeDown", p.probeDown);
    num("Collision", "MaxLayers", p.maxLayers);
    flt("Collision", "StepHeight", p.stepHeight);
    flt("Collision", "MinClearance", p.minClearance);
    num("Collision", "EdgeProbeRadiusCells", p.edgeProbeRadiusCells);
    flt("Collision", "CoplanarTolerance", p.coplanarTolerance);
    flt("Collision", "TallWallExtra", p.tallWallExtra);
    num("Collision", "WallProbeDirections", p.wallProbeDirs);
    flt("Collision", "WallProbeDistance", p.wallProbeDist);
    flt("Collision", "RebuildDistance", c.rebuildDistance);
    flt("Collision", "RebuildVertical", c.rebuildVertical);
    flt("Collision", "RebuildSeconds", c.rebuildSeconds);

    low("Camera", "Source", c.cameraSource);
    str("Camera", "ActorName", c.cameraActorName);
    flt("Camera", "FovDegrees", c.fovDegrees);
    bol("Camera", "UseScannedFov", c.useScannedFov);
    bol("Camera", "FlipRight", c.flipRight);
    flt("Camera", "LookAtHeight", c.lookAtHeight);
    flt("Camera", "NearPlane", c.nearPlane);
    num("Camera", "ScanThreads", c.scanThreads);
    num("Camera", "MaxRegionMB", c.maxRegionMB);
    bol("Camera", "FollowMarioHeight", c.followMarioHeight);
    bol("Camera", "Override", c.cameraOverride);
    flt("Camera", "VerticalSmoothing", c.cameraVerticalSmoothing);
    c.cameraVerticalSmoothing = std::max(0.0f, std::min(1.0f, c.cameraVerticalSmoothing));
    flt("Camera", "Distance", c.cameraDistance);
    if (!(c.cameraDistance == c.cameraDistance)) c.cameraDistance = 1.5f;
    c.cameraDistance = std::max(0.5f, std::min(3.0f, c.cameraDistance));
    bol("Camera", "Collision", c.cameraCollision);
    bol("Camera", "PlaceOtherCameras", c.placeOtherCameras);
    num("Camera", "CollisionQuery", c.cameraQuery);
    c.cameraQuery = std::max(0, std::min(255, c.cameraQuery));
    flt("Camera", "FollowLead", c.followLead);
    c.followLead = std::max(0.0f, std::min(2.0f, c.followLead));
    flt("Camera", "FollowLeadMax", c.followLeadMax);
    c.followLeadMax = std::max(0.0f, std::min(10.0f, c.followLeadMax));

    low("Hero", "HideHero", c.hideHero);
    // v0.1 configs said "scale" (which never worked): the engine's own hide replaces it.
    if (c.hideHero != "write" && c.hideHero != "none") c.hideHero = "engine";
    bol("Hero", "KeepHeroAlive", c.keepHeroAlive);
    low("Hero", "PositionWrites", c.positionWrites);
    bol("Hero", "HoldDuringFrame", c.holdDuringFrame);
    bol("Hero", "KeepHidden", c.keepHeroHidden);
    flt("Hero", "InteractionRelease", c.interactRelease);
    c.interactRelease = std::max(0.0f, std::min(15.0f, c.interactRelease));
    flt("Hero", "HoldRadius", c.holdRadius);

    bol("Combat", "Enabled", c.combat);
    flt("Combat", "ScanRadius", c.enemyScanRadius);
    num("Combat", "SlotsPerTick", c.enemySlotsPerTick);
    for (const auto& s : Ini::SplitList(ini.GetString("Combat", "EnemyNameInclude", ""))) c.enemyInclude.push_back(Ini::Lower(s));
    for (const auto& s : Ini::SplitList(ini.GetString("Combat", "EnemyNameExclude", ""))) c.enemyExclude.push_back(Ini::Lower(s));
    if (ini.Has("Combat", "HealthComponents")) {
        c.healthComponents.clear();
        for (const auto& s : Ini::SplitList(ini.GetString("Combat", "HealthComponents", ""))) c.healthComponents.push_back(s);
    }
    if (ini.Has("Combat", "PersonComponents")) {
        c.personComponents.clear();
        for (const auto& s : Ini::SplitList(ini.GetString("Combat", "PersonComponents", ""))) c.personComponents.push_back(s);
    }
    CombatTuning& t = c.tuning;
    flt("Combat", "EnemyRadius", t.enemyRadius);
    flt("Combat", "EnemyHeight", t.enemyHeight);
    flt("Combat", "ReachBonus", t.reachBonus);
    flt("Combat", "ShockwaveRadius", t.shockwaveRadius);
    flt("Combat", "DamageMultiplier", t.damageMultiplier);
    flt("Combat", "IncomingDamageScale", t.incomingScale);
    num("Combat", "HitCooldownTicks", t.hitCooldownTicks);
    const char* kinds[] = {"", "Punch", "Kick", "Trip", "SlideKick", "Dive", "Stomp", "GroundPound", "Shockwave"};
    for (int k = 1; k < int(AttackKind::Count); ++k) {
        std::string key = std::string(kinds[k]) + "Damage";
        flt("Combat", key.c_str(), t.damageFraction[k]);
        key = std::string(kinds[k]) + "Knockback";
        flt("Combat", key.c_str(), t.knockback[k]);
        key = std::string(kinds[k]) + "Reaction";
        Reaction r;
        if (ini.Has("Combat", key.c_str()) && ParseReaction(ini.GetString("Combat", key.c_str(), ""), r)) t.reaction[k] = r;
    }
    bol("Combat", "UseGameDamage", c.gameDamage);
    bol("Combat", "HitPeople", c.hitPeople);
    bol("Combat", "LogHits", c.logHits);
    flt("Combat", "AttackStrength", c.attackStrength);
    if (!(c.attackStrength >= 0.25f)) c.attackStrength = 0.25f;
    if (c.attackStrength > 4.0f) c.attackStrength = 4.0f;

    bol("Cheats", "InfiniteHealth", c.infiniteHealth);
    if (ini.Has("Cheats", "WingCap") || ini.Has("Cheats", "MetalCap") || ini.Has("Cheats", "VanishCap")) {
        bol("Cheats", "WingCap", c.wingCap);
        bol("Cheats", "MetalCap", c.metalCap);
        bol("Cheats", "VanishCap", c.vanishCap);
    } else {
        // 0.3: one cap at a time ("Cap = wing").
        const std::string cap = Ini::Lower(ini.GetString("Cheats", "Cap", "normal"));
        c.wingCap = cap == "wing";
        c.metalCap = cap == "metal";
        c.vanishCap = cap == "vanish";
    }
    bol("Cheats", "MoonJump", c.moonJump);
    bol("Cheats", "BljAnywhere", c.bljAnywhere);

    bol("Audio", "Enabled", c.audio);
    flt("Audio", "Volume", c.volume);

    bol("Input", "PauseWhenCursorVisible", c.pauseWhenCursorVisible);
    bol("Input", "PauseWithGame", c.pauseWithGame);
    bol("Input", "BlockOnlyMarioControls", c.onlyMarioControls);
    bol("Input", "BlockGameInput", c.blockGameInput);

    low("Render", "Mode", c.renderMode);
    if (c.renderMode != "overlay") c.renderMode = "world";
    bol("Render", "Shadows", c.shadows);
    low("Render", "ShadowRegions", c.shadowRegions);
    if (c.shadowRegions != "refreshed" && c.shadowRegions != "steady") c.shadowRegions = "auto";
    bol("Render", "FrameReport", c.frameReport);
    flt("Render", "FallbackSeconds", c.fallbackSeconds);
    flt("Render", "Gloss", c.gloss);
    flt("Render", "Specular", c.specular);
    num("Render", "MetalMaterial", c.metalMaterial);
    if (c.metalMaterial < -1 || c.metalMaterial > 0xFFFE) c.metalMaterial = -1;
    flt("Render", "AlbedoScale", c.albedoScale);
    flt("Render", "ShadowBias", c.shadowBias);
    bol("Render", "StencilMatch", c.stencilMatch);
    low("Render", "MarioStencil", c.marioStencil);
    num("Render", "StencilCensusFrames", c.stencilCensusFrames);
    c.stencilCensusFrames = std::max(30, std::min(100000, c.stencilCensusFrames));
    bol("Render", "AlignToCamera", c.alignToCamera);
    if (ini.Has("Render", "GBufferMarkers")) {
        c.gbufferMarkers.clear();
        for (const auto& m : Ini::SplitList(ini.GetString("Render", "GBufferMarkers", ""))) c.gbufferMarkers.push_back(m);
    }
    if (ini.Has("Render", "GBufferFormats")) {
        std::vector<uint32_t> f;
        for (const auto& t : Ini::SplitList(ini.GetString("Render", "GBufferFormats", ""))) {
            double v;
            if (Ini::ParseFloat(t, v) && v > 0 && v < 200) f.push_back(uint32_t(v));
        }
        if (!f.empty()) c.gbufferFormats = f;
    }
    num("Render", "CaptureWidth", c.captureWidth);
    flt("Render", "PaperWhiteNits", c.paperWhiteNits);

    bol("Debug", "ShowOverlay", c.debugOverlayOnStart);
    bol("Debug", "Calibrate", c.calibrateOnStart);
    bol("Debug", "TraceCollision", c.traceCollision);
    bol("Debug", "TraceCamera", c.traceCamera);
    return c;
}

} // namespace sm2m
