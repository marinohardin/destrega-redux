/* Sphere-vs-triangle collision against stage geometry.
 *
 * Readable replacement for the recompiled function at 0x800465a4
 * (analysis/overrides.txt). Recovered from the Ghidra decompilation and the
 * generated C; behaviour is checked against the original with
 * `destrega --verify-overrides`.
 *
 * Not touched by this file: the visible stage mesh. The game tests moving
 * spheres (characters, weapon tips) against a separate list of collision
 * triangles that carry a precomputed normal, so replacing render models
 * does not change what this code sees.
 */
#include "override.h"
#include "game_syms.h"

/* ---- guest data layouts (byte offsets into guest RAM) --------------------- */

/* Collision polygon. Despite the "triangle" name it holds up to four vertices:
 * the bounding-box and polygon tests below always look at all four. */
enum {
    POLY_FLAGS  = 0x00,   /* u16; low nibble = surface/material id for the polygon tests */
    POLY_NORMAL = 0x04,   /* u32 guest pointer -> Vec3s, unit-length in 4.12 fixed point */
    POLY_VERTS  = 0x08,   /* 4 x u32 guest pointers -> Vec3s */
    POLY_NUM_VERTS = 4,
};

/* Moving sphere. */
enum {
    SPHERE_CENTER = 0x00, /* 3 x s32 (x, y, z) */
    SPHERE_HIT    = 0x10, /* 3 x s32, written only when a hit is reported */
    SPHERE_RADIUS = 0x30, /* s16 */
};

/* Components of a Vec3s / Vec3 */
enum { AXIS_X = 0, AXIS_Y = 1, AXIS_Z = 2 };

typedef struct { s32 x, y, z; } Vec3;

/* ---- guest memory helpers ------------------------------------------------- */

static s32 read_s32(u32 addr) { return (s32)mem_r32(addr); }
static s32 read_s16(u32 addr) { return (s16)mem_r16(addr); }

static Vec3 read_vec3(u32 addr) {      /* 3 x s32 */
    return (Vec3){ read_s32(addr), read_s32(addr + 4), read_s32(addr + 8) };
}

static void write_vec3(u32 addr, Vec3 v) {
    mem_w32(addr, (u32)v.x);
    mem_w32(addr + 4, (u32)v.y);
    mem_w32(addr + 8, (u32)v.z);
}

static Vec3 read_vec3s(u32 addr) {     /* 3 x s16 */
    return (Vec3){ read_s16(addr), read_s16(addr + 2), read_s16(addr + 4) };
}

static u32 poly_vertex_addr(u32 poly, int i) { return mem_r32(poly + POLY_VERTS + 4 * i); }

/* ---- arithmetic the original performs (32-bit MIPS semantics) ------------- */

/* a * b with the product truncated to 32 bits, as `mult` + `mflo` does. */
static s32 mul32(s32 a, s32 b) { return (s32)((u32)a * (u32)b); }

/* (a * b) / 4096, rounding toward zero. */
static s32 fx12_mul(s32 a, s32 b) {
    s32 p = mul32(a, b);
    if (p < 0) p += 0xfff;
    return p >> 12;
}

/* Signed division with the MIPS `div` results for the two cases C leaves
 * undefined (the game does not trap on them). */
static s32 div32(s32 n, s32 d) {
    if (d == 0) return n >= 0 ? -1 : 1;
    if (n == INT32_MIN && d == -1) return INT32_MIN;
    return n / d;
}

static s32 abs32(s32 v) { return v < 0 ? (s32)(0u - (u32)v) : v; }
static s32 add32(s32 a, s32 b) { return (s32)((u32)a + (u32)b); }
static s32 sub32(s32 a, s32 b) { return (s32)((u32)a - (u32)b); }

/* ---- calls into guest code that has not been rewritten yet ---------------- */

/* Put `p` in guest memory for a callee and lower the guest stack pointer below
 * it. The MIPS o32 convention reserves 16 bytes at the callee's entry sp for
 * spilling its register arguments, so those must stay free: the point lives at
 * sp+16 of the new frame, exactly as in the original function's stack frame.
 * The caller restores g_cpu.r[29]. */
static u32 push_point(Vec3 p) {
    g_cpu.r[29] -= 32;
    u32 addr = g_cpu.r[29] + 16;
    write_vec3(addr, p);
    return addr;
}

/* Is `p` inside the polygon, ignoring height (floor/ceiling-style surfaces)?
 * The callee also reads a fourth register argument, the normal's Y component
 * (sign decides the winding direction of its edge tests); the original leaves
 * it in a3 without any prototype saying so. */
static u32 guest_point_in_floor_poly(u32 verts, Vec3 p, u32 material, s32 normal_y) {
    u32 sp = g_cpu.r[29];
    u32 p_addr = push_point(p);
    u32 r = guest_call(f_80044918, verts, p_addr, material, (u32)normal_y);
    g_cpu.r[29] = sp;
    return r;
}

/* Same for polygons with a horizontal normal (walls). */
static u32 guest_point_in_wall_poly(u32 normal, u32 verts, Vec3 p, u32 material) {
    u32 sp = g_cpu.r[29];
    u32 p_addr = push_point(p);
    u32 r = guest_call(f_80045414, normal, verts, p_addr, material);
    g_cpu.r[29] = sp;
    return r;
}

/* Integer square root (libgte SquareRoot0). */
static s32 guest_isqrt(s32 v) { return (s32)guest_call(f_80095188, (u32)v, 0, 0, 0); }

/* ---- the collision test --------------------------------------------------- */

/* Cycle accounting. The original is recompiled MIPS whose cost is charged per
 * basic block; COST(n) charges the n instructions it executes at this point
 * (CYC() turns that into 2 cycles each). It is placed exactly where the
 * original charges it, because guest callees check for pending interrupts at
 * their entry and so see the cycle counter. --verify-overrides compares the
 * counter against the original on every call. */
#define COST(n) CYC(n)

/* Smallest and largest value of one coordinate over the polygon's vertices.
 * Returns the number of MIPS instructions the original spends finding them
 * (data dependent: the original updates its running min/max with branches).
 * For the Y axis this excludes the 20 instructions of function prologue and
 * argument loading the original folds into the same basic block. */
static u32 poly_extent(u32 poly, int axis, s32 *lo, s32 *hi) {
    s32 v[POLY_NUM_VERTS];
    for (int i = 0; i < POLY_NUM_VERTS; i++) v[i] = read_s16(poly_vertex_addr(poly, i) + 2 * axis);

    const bool y_axis = axis == AXIS_Y;
    u32 cost;

    /* vertices 0 and 1 */
    *lo = v[0] < v[1] ? v[0] : v[1];
    *hi = v[0] < v[1] ? v[1] : v[0];
    cost = y_axis ? (v[1] < v[0] ? 3 : 2) : 10;

    /* vertex 2 */
    cost += 9 + (v[2] < *lo);
    if (v[2] < *lo) *lo = v[2];
    if (v[2] > *hi) *hi = v[2];

    /* vertex 3 (X/Z fold the following bounds compare into this block) */
    cost += y_axis ? 9 + (v[3] < *lo) : 9 + (v[3] > *hi) + 2 * (v[3] < *lo);
    if (v[3] < *lo) *lo = v[3];
    if (v[3] > *hi) *hi = v[3];
    return cost;
}

/* Returns non-zero (the polygon-test result) and stores the contact point in
 * the sphere when `sphere` touches `poly`; returns 0 otherwise.
 *
 * `loose` is the third argument of the original. When non-zero the checks
 * against the polygon's lower height bound and the wall polygon test are
 * skipped; callers use it for probes that should also catch geometry beneath
 * the sphere. (Exact gameplay meaning not recovered yet.) */
static u32 sphere_vs_poly(u32 poly, u32 sphere, u32 loose) {
    /* Every early reject leaves through the original's epilogue (12 instructions). */
#define REJECT() do { COST(12); return 0; } while (0)

    const Vec3 center = read_vec3(sphere + SPHERE_CENTER);
    const s32 radius = read_s16(sphere + SPHERE_RADIUS);

    /* 1. Cheap reject on height. */
    s32 y_lo, y_hi;
    COST(20 + poly_extent(poly, AXIS_Y, &y_lo, &y_hi));
    COST(2);
    if (!loose) {
        COST(7);
        if (add32(center.y, radius) < y_lo) REJECT();
        COST(1);
    } else {
        COST(4);
    }
    COST(3);
    if (sub32(center.y, radius) > y_hi) REJECT();

    /* 2. Signed distance from the sphere centre to the polygon's plane
     *    (4.12 fixed point). Negative: the centre is behind the polygon.
     *    Each product is rounded toward zero, which costs the original one
     *    extra instruction when the product is negative. */
    const u32 normal_addr = mem_r32(poly + POLY_NORMAL);
    const Vec3 n = read_vec3s(normal_addr);
    const Vec3 v0 = read_vec3s(poly_vertex_addr(poly, 0));
    const s32 px = mul32(sub32(v0.x, center.x), n.x);
    const s32 pz = mul32(sub32(v0.z, center.z), n.z);
    const s32 py = mul32(sub32(v0.y, center.y), n.y);
    COST(29 + (px < 0));
    COST(6 + (pz < 0));
    COST(7 + (py < 0));
    const s32 dist = add32(add32(fx12_mul(sub32(v0.x, center.x), n.x),
                                 fx12_mul(sub32(v0.z, center.z), n.z)),
                           fx12_mul(sub32(v0.y, center.y), n.y));
    COST(4);
    if (dist < 0) REJECT();

    /* 3. Project the centre onto the plane. Dividing by |n|^2 (4.12) instead of
     *    assuming unit length keeps the result correct for slightly
     *    non-normalised normals. */
    const s32 n_len_sq = ((n.x * n.x) >> 12) + ((n.y * n.y) >> 12) + ((n.z * n.z) >> 12);
    const Vec3 hit = {
        add32(center.x, div32(mul32(n.x, dist), n_len_sq)),
        add32(center.y, div32(mul32(n.y, dist), n_len_sq)),
        add32(center.z, div32(mul32(n.z, dist), n_len_sq)),
    };

    /* 4. The projected point must lie inside the polygon's bounding box. */
    COST(58);
    if (!loose) {
        COST(3);
        if (hit.y < y_lo) REJECT();
    }
    COST(3);
    if (hit.y > y_hi) REJECT();
    s32 x_lo, x_hi, z_lo, z_hi;
    COST(poly_extent(poly, AXIS_X, &x_lo, &x_hi));
    COST(2);
    if (hit.x < x_lo) REJECT();
    COST(3);
    if (hit.x > x_hi) REJECT();
    COST(poly_extent(poly, AXIS_Z, &z_lo, &z_hi));
    COST(2);
    if (hit.z < z_lo) REJECT();
    COST(3);
    if (hit.z > z_hi) REJECT();

    /* 5. Manhattan-distance early out: |d|x + |d|y + |d|z >= 2r cannot be a hit.
     *    (Each negation of a negative difference costs one more instruction.) */
    const s32 ddx = sub32(hit.x, center.x), ddy = sub32(hit.y, center.y), ddz = sub32(hit.z, center.z);
    const s32 dx = abs32(ddx), dy = abs32(ddy), dz = abs32(ddz);
    COST(5 + (ddx < 0));
    COST(3 + (ddy < 0));
    COST(5 + (ddz < 0));
    COST(6);
    if (!(add32(add32(dx, dy), dz) < radius * 2)) { COST(2); REJECT(); }

    /* 6. Exact test that the contact point is inside the polygon's outline.
     *    Surfaces with a vertical component use the floor test; purely
     *    horizontal normals (walls) use the wall test unless `loose`. */
    const u32 verts = poly + POLY_VERTS;
    const u32 material = mem_r16(poly + POLY_FLAGS) & 0xf;
    u32 inside;
    COST(9);
    if (n.y != 0) {
        COST(4);
        inside = guest_point_in_floor_poly(verts, hit, material, n.y);
        COST(1);
    } else {
        COST(2);
        inside = 1;
        if (!loose) {
            COST(4);
            inside = guest_point_in_wall_poly(normal_addr, verts, hit, material);
            COST(2);
        }
    }
    COST(2);

    /* 7. Euclidean distance check, then report the contact point. */
    if (inside) {
        COST(12);
        s32 distance = guest_isqrt(add32(add32(mul32(dx, dx), mul32(dy, dy)), mul32(dz, dz)));
        COST(5);
        if (radius < distance) {
            inside = 0;
            COST(1);
        } else {
            write_vec3(sphere + SPHERE_HIT, hit);
            COST(4);
        }
    }
    COST(1);          /* common exit: result move */
    COST(12);         /* epilogue */
    return inside;
#undef REJECT
}

/* ---- guest entry point (MIPS calling convention) -------------------------- */

/* a0 = polygon, a1 = sphere, a2 = loose; v0 = result. */
static void sphere_vs_poly_guest(void) {
    g_cpu.r[2] = sphere_vs_poly(g_cpu.r[4], g_cpu.r[5], g_cpu.r[6]);
}

void f_800465a4(void) {
    FN_ENTER(0x800465a4);
    EVT_CHECK();
    override_call("sphere_vs_poly", f_800465a4_orig, sphere_vs_poly_guest);
    FN_RET();
}
