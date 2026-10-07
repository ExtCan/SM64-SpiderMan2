#include "combat.h"

#include <cmath>

#include "../sm64/sm64_defs.h"

namespace sm2m {

const char* AttackName(AttackKind k) {
    switch (k) {
    case AttackKind::None: return "none";
    case AttackKind::Punch: return "punch";
    case AttackKind::Kick: return "kick";
    case AttackKind::Trip: return "trip";
    case AttackKind::SlideKick: return "slide kick";
    case AttackKind::Dive: return "dive";
    case AttackKind::Stomp: return "stomp";
    case AttackKind::GroundPound: return "ground pound";
    case AttackKind::Shockwave: return "shockwave";
    default: return "?";
    }
}

namespace {
const char* const kReactionNames[] = {"none",           "twitch",    "stagger",         "knockback",
                                      "heavy knockback", "knockdown", "fly back",        "heavy knockdown",
                                      "airborne",       "pop up",    "super fly back"};
std::string Squash(const std::string& s) {
    std::string o;
    for (char c : s)
        if (c != ' ' && c != '_' && c != '-') o += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return o;
}
} // namespace

const char* ReactionName(Reaction r) {
    const int i = int(r);
    return i >= 0 && i < int(Reaction::Count) ? kReactionNames[i] : "?";
}

bool ParseReaction(const std::string& s, Reaction& out) {
    std::string k = Squash(s);
    if (k.size() > 1 && k[0] == 'k') {
        // kStagger (the game's own name)
        for (int i = 0; i < int(Reaction::Count); ++i)
            if (k == "k" + Squash(kReactionNames[i])) {
                out = Reaction(i);
                return true;
            }
    }
    for (int i = 0; i < int(Reaction::Count); ++i)
        if (k == Squash(kReactionNames[i])) {
            out = Reaction(i);
            return true;
        }
    return false;
}

AttackKind ClassifyAttack(uint32_t action, uint32_t flags, float vy) {
    using namespace sm64;
    if (action == ACT_GROUND_POUND) return vy < 0 ? AttackKind::GroundPound : AttackKind::None;
    if (action == ACT_GROUND_POUND_LAND) return AttackKind::Shockwave;
    if (action == ACT_DIVE || action == ACT_DIVE_SLIDE) return AttackKind::Dive;
    if (action == ACT_SLIDE_KICK || action == ACT_SLIDE_KICK_SLIDE) return AttackKind::SlideKick;
    if (flags & MARIO_KICKING) return AttackKind::Kick;
    if (flags & MARIO_PUNCHING) return AttackKind::Punch;
    if (flags & MARIO_TRIPPING) return AttackKind::Trip;
    if ((action & ACT_FLAG_AIR) && vy < 0) return AttackKind::Stomp;
    return AttackKind::None;
}

bool AttackReaches(AttackKind kind, const Vec3& m, float vy, const Vec3& e, const CombatTuning& t, float u) {
    if (kind == AttackKind::None) return false;
    const float marioRadius = 37.0f;  // SM64 Mario hitbox
    const float marioHeight = 160.0f;
    const float er = t.enemyRadius * u;
    const float eh = t.enemyHeight * u;
    const float dx = e.x - m.x, dz = e.z - m.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);

    if (kind == AttackKind::Shockwave) {
        return horiz <= t.shockwaveRadius * u && std::fabs(e.y - m.y) < 1.0f * u;
    }
    if (kind == AttackKind::Stomp || kind == AttackKind::GroundPound) {
        // Feet must come down on the enemy's head.
        if (vy >= 0) return false;
        const float head = e.y + eh;
        return horiz <= marioRadius + er && m.y <= head + 60.0f && m.y >= e.y + eh * 0.5f;
    }
    float reach = marioRadius + er;
    if (kind == AttackKind::Punch || kind == AttackKind::Kick || kind == AttackKind::Trip)
        reach += t.reachBonus * u;
    if (horiz > reach) return false;
    // Vertical overlap of the two cylinders.
    return m.y < e.y + eh && e.y < m.y + marioHeight;
}

int IncomingDamageWedges(float lost, float maxHealth, float scale) {
    if (!(lost > 0) || !(maxHealth > 0)) return 0;
    float wedges = lost / maxHealth * 8.0f * scale;
    int w = int(std::ceil(wedges - 1e-4f));
    if (w < 1) w = 1;
    if (w > 4) w = 4;
    return w;
}

} // namespace sm2m
