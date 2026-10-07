/* A stand-in for libsm64's sm64.dll used by the Wine smoke test. It exports the
 * same functions and behaves just enough like Mario: walks relative to the
 * camera, jumps, punches (setting SM64's punch action/flags so the mod's combat
 * bridge runs), falls onto the loaded floor surfaces, and outputs a small
 * coloured model so the renderer has real geometry. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SM64_LIB_EXPORT
#include "libsm64.h"

static struct SM64Surface* g_surfs;
static uint32_t g_count;
static float g_pos[3], g_vel[3], g_face;
static int16_t g_health = 0x880;
static int g_punch, g_hurt;
static uint32_t g_action = 0x0C400201; /* ACT_IDLE */
static int g_created;

SM64_LIB_FN void sm64_register_debug_print_function(SM64DebugPrintFunctionPtr f) { (void)f; }
SM64_LIB_FN void sm64_register_play_sound_function(SM64PlaySoundFunctionPtr f) { (void)f; }
SM64_LIB_FN void sm64_global_init(const uint8_t* rom, uint8_t* tex) {
    (void)rom;
    for (int i = 0; i < SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT; ++i) {
        tex[i * 4 + 0] = 255; tex[i * 4 + 1] = 255; tex[i * 4 + 2] = 255; tex[i * 4 + 3] = 0;
    }
}
SM64_LIB_FN void sm64_global_terminate(void) {}
SM64_LIB_FN void sm64_audio_init(const uint8_t* rom) { (void)rom; }
SM64_LIB_FN uint32_t sm64_audio_tick(uint32_t q, uint32_t d, int16_t* buf) {
    (void)q; (void)d;
    memset(buf, 0, 544 * 2 * 2 * sizeof(int16_t));
    return 544;
}
SM64_LIB_FN void sm64_static_surfaces_load(const struct SM64Surface* s, uint32_t n) {
    free(g_surfs);
    g_surfs = (struct SM64Surface*)malloc(sizeof(*s) * (n ? n : 1));
    memcpy(g_surfs, s, sizeof(*s) * n);
    g_count = n;
}

/* Highest up-facing surface at or below y + 78 (same rule as SM64). */
SM64_LIB_FN float sm64_surface_find_floor_height(float x, float y, float z) {
    float best = -11000.0f;
    for (uint32_t i = 0; i < g_count; ++i) {
        const int32_t(*v)[3] = g_surfs[i].vertices;
        float nx = (float)((v[1][1] - v[0][1]) * (v[2][2] - v[1][2]) - (v[1][2] - v[0][2]) * (v[2][1] - v[1][1]));
        float ny = (float)((v[1][2] - v[0][2]) * (v[2][0] - v[1][0]) - (v[1][0] - v[0][0]) * (v[2][2] - v[1][2]));
        float nz = (float)((v[1][0] - v[0][0]) * (v[2][1] - v[1][1]) - (v[1][1] - v[0][1]) * (v[2][0] - v[1][0]));
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len < 1.0f) continue;
        nx /= len; ny /= len; nz /= len;
        if (ny <= 0.01f) continue;
        float x1 = (float)v[0][0], z1 = (float)v[0][2], x2 = (float)v[1][0], z2 = (float)v[1][2], x3 = (float)v[2][0], z3 = (float)v[2][2];
        if ((z1 - z) * (x2 - x1) - (x1 - x) * (z2 - z1) < 0) continue;
        if ((z2 - z) * (x3 - x2) - (x2 - x) * (z3 - z2) < 0) continue;
        if ((z3 - z) * (x1 - x3) - (x3 - x) * (z1 - z3) < 0) continue;
        float oo = -(nx * v[0][0] + ny * v[0][1] + nz * v[0][2]);
        float h = -(x * nx + nz * z + oo) / ny;
        if (y - (h - 78.0f) < 0) continue;
        if (h > best) best = h;
    }
    return best;
}

SM64_LIB_FN int32_t sm64_mario_create(float x, float y, float z) {
    g_pos[0] = x; g_pos[1] = y; g_pos[2] = z;
    g_vel[0] = g_vel[1] = g_vel[2] = 0;
    g_health = 0x880;
    g_created = 1;
    return 0;
}
SM64_LIB_FN void sm64_mario_delete(int32_t id) { (void)id; g_created = 0; }

static void emit_box(struct SM64MarioGeometryBuffers* b, float cx, float cy, float cz, float hx, float hy, float hz,
                     float r, float g, float bl) {
    static const int f[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    static const float n[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    float c[8][3];
    for (int i = 0; i < 8; ++i) {
        c[i][0] = cx + ((i & 4) ? hx : -hx);
        c[i][1] = cy + ((i & 2) ? hy : -hy);
        c[i][2] = cz + ((i & 1) ? hz : -hz);
    }
    for (int k = 0; k < 6; ++k) {
        const int t[2][3] = {{f[k][0], f[k][1], f[k][2]}, {f[k][0], f[k][2], f[k][3]}};
        for (int q = 0; q < 2; ++q) {
            for (int j = 0; j < 3; ++j) {
                int v = b->numTrianglesUsed * 3 + j;
                memcpy(&b->position[v * 3], c[t[q][j]], 12);
                memcpy(&b->normal[v * 3], n[k], 12);
                b->color[v * 3 + 0] = r; b->color[v * 3 + 1] = g; b->color[v * 3 + 2] = bl;
                b->uv[v * 2 + 0] = 1.0f; b->uv[v * 2 + 1] = 1.0f;
            }
            b->numTrianglesUsed++;
        }
    }
}

SM64_LIB_FN void sm64_mario_tick(int32_t id, const struct SM64MarioInputs* in, struct SM64MarioState* out,
                                 struct SM64MarioGeometryBuffers* buf) {
    (void)id;
    /* stick relative to the camera like SM64: -stickY = away from the camera */
    float l = sqrtf(in->camLookX * in->camLookX + in->camLookZ * in->camLookZ);
    float fx = l > 0 ? in->camLookX / l : 0, fz = l > 0 ? in->camLookZ / l : 1;
    float rx = -fz, rz = fx; /* right of the look direction in a right-handed Y-up world */
    float mx = fx * -in->stickY + rx * in->stickX, mz = fz * -in->stickY + rz * in->stickX;
    float floorY = sm64_surface_find_floor_height(g_pos[0], g_pos[1], g_pos[2]);
    int grounded = g_pos[1] <= floorY + 1.0f;
    g_pos[0] += mx * 20.0f;
    g_pos[2] += mz * 20.0f;
    if (mx * mx + mz * mz > 0.01f) g_face = atan2f(mx, mz);
    if (grounded && in->buttonA) g_vel[1] = 42.0f;
    g_vel[1] -= 4.0f;
    g_pos[1] += g_vel[1];
    floorY = sm64_surface_find_floor_height(g_pos[0], g_pos[1], g_pos[2]);
    if (g_pos[1] < floorY) { g_pos[1] = floorY; g_vel[1] = 0; }
    if (in->buttonB && grounded && g_punch == 0) g_punch = 6;
    if (g_punch > 0) g_punch--;
    g_action = g_punch > 0 ? 0x00800380 /* ACT_PUNCHING */ : (g_pos[1] > floorY + 1 ? 0x0100088C : 0x0C400201);
    if (g_hurt > 0) { g_hurt--; g_action = 0x00020462; /* backward ground kb */ }
    out->position[0] = g_pos[0]; out->position[1] = g_pos[1]; out->position[2] = g_pos[2];
    out->velocity[0] = mx * 20.0f; out->velocity[1] = g_vel[1]; out->velocity[2] = mz * 20.0f;
    out->faceAngle = g_face;
    out->forwardVelocity = sqrtf(mx * mx + mz * mz) * 20.0f;
    out->health = g_health;
    out->action = g_action;
    out->flags = 0x10 | (g_punch > 0 ? 0x00100000u : 0u); /* cap on head (+ MARIO_PUNCHING) */
    out->invincTimer = 0;
    buf->numTrianglesUsed = 0;
    emit_box(buf, g_pos[0], g_pos[1] + 40, g_pos[2], 25, 40, 18, 0.1f, 0.2f, 0.9f);        /* overalls */
    emit_box(buf, g_pos[0], g_pos[1] + 110, g_pos[2], 28, 30, 22, 0.9f, 0.1f, 0.1f);       /* shirt */
    emit_box(buf, g_pos[0], g_pos[1] + 160, g_pos[2], 22, 22, 22, 1.0f, 0.8f, 0.6f);       /* head */
    emit_box(buf, g_pos[0] + sinf(g_face) * 30, g_pos[1] + 160, g_pos[2] + cosf(g_face) * 30, 6, 6, 6,
             1.0f, 0.7f, 0.5f);                                                            /* nose (facing) */
    emit_box(buf, g_pos[0], g_pos[1] + 186, g_pos[2], 26, 8, 26, 0.9f, 0.1f, 0.1f);        /* cap */
}

SM64_LIB_FN void sm64_set_mario_action(int32_t id, uint32_t a) { (void)id; g_action = a; }
SM64_LIB_FN void sm64_set_mario_action_arg(int32_t id, uint32_t a, uint32_t arg) { (void)id; (void)arg; g_action = a; }
SM64_LIB_FN void sm64_set_mario_animation(int32_t id, int32_t a) { (void)id; (void)a; }
SM64_LIB_FN void sm64_set_mario_anim_frame(int32_t id, int16_t f) { (void)id; (void)f; }
SM64_LIB_FN void sm64_set_mario_state(int32_t id, uint32_t f) { (void)id; (void)f; }
SM64_LIB_FN void sm64_set_mario_position(int32_t id, float x, float y, float z) {
    (void)id; g_pos[0] = x; g_pos[1] = y; g_pos[2] = z;
}
SM64_LIB_FN void sm64_set_mario_angle(int32_t id, float x, float y, float z) { (void)id; (void)x; (void)z; g_face = y; }
SM64_LIB_FN void sm64_set_mario_faceangle(int32_t id, float y) { (void)id; g_face = y; }
SM64_LIB_FN void sm64_set_mario_velocity(int32_t id, float x, float y, float z) {
    (void)id; g_vel[0] = x; g_vel[1] = y; g_vel[2] = z;
}
SM64_LIB_FN void sm64_set_mario_forward_velocity(int32_t id, float v) { (void)id; (void)v; }
SM64_LIB_FN void sm64_set_mario_invincibility(int32_t id, int16_t t) { (void)id; (void)t; }
SM64_LIB_FN void sm64_set_mario_water_level(int32_t id, signed int l) { (void)id; (void)l; }
SM64_LIB_FN void sm64_set_mario_gas_level(int32_t id, signed int l) { (void)id; (void)l; }
SM64_LIB_FN void sm64_set_mario_health(int32_t id, uint16_t h) { (void)id; g_health = (int16_t)h; }
SM64_LIB_FN void sm64_mario_take_damage(int32_t id, uint32_t dmg, uint32_t sub, float x, float y, float z) {
    (void)id; (void)sub; (void)x; (void)y; (void)z;
    g_health = (int16_t)(g_health - (int)dmg * 0x100);
    if (g_health < 0xFF) g_health = 0xFF;
    g_hurt = 8;
}
SM64_LIB_FN void sm64_mario_heal(int32_t id, uint8_t c) { (void)id; g_health = (int16_t)(g_health + c * 0x40); if (g_health > 0x880) g_health = 0x880; }
SM64_LIB_FN void sm64_mario_kill(int32_t id) { (void)id; g_health = 0xFF; }
SM64_LIB_FN void sm64_mario_interact_cap(int32_t id, uint32_t f, uint16_t t, uint8_t m) { (void)id; (void)f; (void)t; (void)m; }
SM64_LIB_FN void sm64_mario_extend_cap(int32_t id, uint16_t t) { (void)id; (void)t; }
SM64_LIB_FN bool sm64_mario_attack(int32_t id, float x, float y, float z, float h) {
    (void)id; (void)y; (void)h;
    /* SM64 rule: the target must be within 60 degrees of where Mario faces */
    float a = atan2f(x - g_pos[0], z - g_pos[2]) - g_face;
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return g_punch > 0 && fabsf(a) <= 1.0472f;
}
SM64_LIB_FN uint32_t sm64_surface_object_create(const struct SM64SurfaceObject* o) { (void)o; return 0; }
SM64_LIB_FN void sm64_surface_object_move(uint32_t id, const struct SM64ObjectTransform* t) { (void)id; (void)t; }
SM64_LIB_FN void sm64_surface_object_delete(uint32_t id) { (void)id; }
SM64_LIB_FN void sm64_play_music(uint8_t p, uint16_t a, uint16_t f) { (void)p; (void)a; (void)f; }
SM64_LIB_FN void sm64_stop_background_music(uint16_t s) { (void)s; }
SM64_LIB_FN uint16_t sm64_get_current_background_music(void) { return 0; }
SM64_LIB_FN void sm64_set_sound_volume(float v) { (void)v; }
