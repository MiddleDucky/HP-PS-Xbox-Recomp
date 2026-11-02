/**
 * Characters taken out of the world: never drawn.
 *
 * The cloisters corridor pupil (entity "GCFloor1CloistersCorridorPupil", the castle grounds) walks a route and
 * stops at an authored height of 900 above the grass beside the wall, standing in the air; she is left out
 * altogether. Entities are a list through +0 from the player's ([0x3565B0]), each with its name at [+0x0C] and
 * itself at +8; +0x28 is its skeleton root, a scene node with the 4x4 world matrix at +0x40 (translation +0x70).
 * Her meshes are scene nodes at the root's exact position, drawn by the mesh type's draw callback
 * sub_0016FAC0(node, ...) (registered by sub_00170210); a node standing exactly where a listed character's root
 * stands is not drawn (the callback's own early out, 0x16FFB0, draws nothing either). Not when her root is at the
 * world's origin (put away: the level's own meshes stand there, and the whole room went undrawn), and at most 16
 * distinct meshes at one place ([HIDE] says when that stops it).
 *
 * Built into the recompile with --exclude-manual port/src/hle_world.c (regen.sh); until a regen the generated body
 * of sub_0016FAC0 (recomp_0004.c) is renamed _gen by hand. RECOMP_HIDE_CHARACTERS=0 draws them.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#define ARG(i) MEM32(g_esp + 4 + 4 * (i))

enum { kPlayer = 0x3565B0, kMaxEntities = 8192 };

extern void sub_0016FAC0_gen(void);

static const char *const kHidden[] = {
    "GCFloor1CloistersCorridorPupil",
};
#define NHIDDEN (sizeof kHidden / sizeof kHidden[0])

static uint32_t g_ent[NHIDDEN];  /* the entity found for each name, 0 none (yet) */
static uint32_t g_calls;

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HIDE_CHARACTERS");
        on = !(e && *e == '0');
    }
    return on;
}

static int guest_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x84000000u; }

static int is_entity(uint32_t e, const char *name)
{
    if (!guest_ptr(e) || MEM32(e + 8) != e) return 0;
    uint32_t s = MEM32(e + 0x0C);
    if (!guest_ptr(s)) return 0;
    return strncmp((const char *)XBOX_PTR(s), name, 64) == 0;
}

/* the entity list from the player's, around once */
static void find(void)
{
    uint32_t start = MEM32(kPlayer);
    if (!guest_ptr(start)) return;
    uint32_t e = start;
    for (int n = 0; n < kMaxEntities; n++) {
        for (unsigned i = 0; i < NHIDDEN; i++)
            if (!g_ent[i] && is_entity(e, kHidden[i])) g_ent[i] = e;
        e = MEM32(e);
        if (!guest_ptr(e) || e == start) break;
    }
}

/* the mesh type's draw callback (node, ...) */
void sub_0016FAC0(void)
{
    uint32_t node = ARG(0);
    if (enabled() && guest_ptr(node)) {
        int missing = 0;
        for (unsigned i = 0; i < NHIDDEN; i++) {
            if (g_ent[i] && !is_entity(g_ent[i], kHidden[i])) g_ent[i] = 0;  /* gone with its level */
            missing |= !g_ent[i];
        }
        if (missing && (g_calls++ & 1023) == 0) find();
        for (unsigned i = 0; i < NHIDDEN; i++) {
            if (!g_ent[i]) continue;
            uint32_t root = MEM32(g_ent[i] + 0x28);
            if (!guest_ptr(root) || node == root) continue;
            const uint32_t rx = MEM32(root + 0x70), ry = MEM32(root + 0x74), rz = MEM32(root + 0x78);
            /* a root at the world's origin is a character put away: the level's own meshes stand there (their
             * vertices are in world space), and matching them hid the whole room (the Erised cutscene in night3
             * and night3 after it, 2026-11-02) */
            if (!((rx | ry | rz) & 0x7FFFFFFFu)) continue;
            if (MEM32(node + 0x70) != rx || MEM32(node + 0x74) != ry || MEM32(node + 0x78) != rz) continue;
            /* and at most a character's worth of meshes at one place: past 16 distinct nodes it is not her, and
             * nothing is hidden there until her root moves */
            static uint32_t at[NHIDDEN][3], nodes[NHIDDEN][16], nnodes[NHIDDEN];
            static int off[NHIDDEN];
            if (at[i][0] != rx || at[i][1] != ry || at[i][2] != rz)
                at[i][0] = rx, at[i][1] = ry, at[i][2] = rz, nnodes[i] = 0, off[i] = 0;
            if (off[i]) continue;
            unsigned k = 0;
            while (k < nnodes[i] && nodes[i][k] != node) k++;
            if (k == nnodes[i]) {
                if (nnodes[i] == 16) {
                    off[i] = 1;
                    fprintf(stderr, "[HIDE] %s: over 16 meshes at her place (%.1f %.1f %.1f), not her: none hidden "
                            "there until she moves\n", kHidden[i], MEMF(root + 0x70), MEMF(root + 0x74),
                            MEMF(root + 0x78));
                    continue;
                }
                nodes[i][nnodes[i]++] = node;
            }
            g_esp += 4;  /* the callback's ret, undrawn */
            return;
        }
    }
    sub_0016FAC0_gen();
}
