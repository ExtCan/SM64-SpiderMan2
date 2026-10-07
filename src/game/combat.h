// Combat rules shared by the game bridge and the unit tests.
//
// Mario -> enemy: every tick, enemies whose cylinder overlaps Mario's attack
// reach are passed to sm64_mario_attack(), which applies SM64's own facing
// rules, knockback and sounds. When it reports a hit, damage is taken off the
// enemy's Health component as a fraction of its max health.
//
// Enemy -> Mario: enemies still target the (hidden) hero, so a drop in the
// hero's health is converted into SM64 damage wedges and knockback, then the
// hero is healed back up.
#pragma once

#include <cstdint>
#include <string>

#include "../common/vec.h"

namespace sm2m {

enum class AttackKind : int {
    None = 0,
    Punch,
    Kick,
    Trip,
    SlideKick,
    Dive,
    Stomp,
    GroundPound,
    Shockwave, // first frame of ACT_GROUND_POUND_LAND: hits everything nearby
    Count
};

const char* AttackName(AttackKind k);

// What Mario is doing this tick from an attack point of view.
AttackKind ClassifyAttack(uint32_t action, uint32_t flags, float verticalVelocity);

// The game's hit reactions (its Knockback enum), weakest to strongest.
enum class Reaction : int {
    None = 0,
    Twitch,
    Stagger,
    Knockback,
    HeavyKnockback,
    Knockdown,
    FlyBack,
    HeavyKnockdown,
    Airborne,
    PopUp,
    SuperFlyBack,
    Count
};
const char* ReactionName(Reaction r); // "stagger"
// "stagger", "Knockdown", "pop up" ... (spaces and case ignored); false if unknown.
bool ParseReaction(const std::string& s, Reaction& out);

struct CombatTuning {
    float enemyRadius = 0.5f;   // metres
    float enemyHeight = 1.8f;
    float reachBonus = 0.25f;   // extra horizontal reach for punches/kicks
    float shockwaveRadius = 2.5f;
    float damageFraction[int(AttackKind::Count)] = {0, 0.20f, 0.25f, 0.20f, 0.30f, 0.30f, 0.35f, 0.50f, 0.25f};
    float knockback[int(AttackKind::Count)] = {0, 1.0f, 1.5f, 1.0f, 1.5f, 2.0f, 0.0f, 0.5f, 2.0f};
    // How the game's characters react to each attack (when the game's damage
    // system is used).
    Reaction reaction[int(AttackKind::Count)] = {Reaction::None,      Reaction::Stagger,   Reaction::Knockback,
                                                 Reaction::Stagger,   Reaction::Knockdown, Reaction::Knockdown,
                                                 Reaction::Knockdown, Reaction::HeavyKnockdown, Reaction::PopUp};
    float damageMultiplier = 1.0f;
    float incomingScale = 1.0f;
    int hitCooldownTicks = 10;
};

// Mario and enemy positions are feet positions in local SM64 units;
// `unitsPerMetre` converts the metre-based tuning.
bool AttackReaches(AttackKind kind, const Vec3& mario, float marioVy, const Vec3& enemyFeet, const CombatTuning& t,
                   float unitsPerMetre);

// Converts a drop in the hero's health into SM64 damage wedges (1..4).
int IncomingDamageWedges(float lost, float maxHealth, float scale);

} // namespace sm2m
