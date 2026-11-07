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
#include "gen_protos.h"

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

/* Smallest and largest value of one coordinate over the polygon's vertices. */
static void poly_extent(u32 poly, int axis, s32 *lo, s32 *hi) {
    *lo = *hi = read_s16(poly_vertex_addr(poly, 0) + 2 * axis);
    for (int i = 1; i < POLY_NUM_VERTS; i++) {
        s32 v = read_s16(poly_vertex_addr(poly, i) + 2 * axis);
        if (v < *lo) *lo = v;
        if (v > *hi) *hi = v;
    }
}

/* Returns non-zero (the polygon-test result) and stores the contact point in
 * the sphere when `sphere` touches `poly`; returns 0 otherwise.
 *
 * `loose` is the third argument of the original. When non-zero the checks
 * against the polygon's lower height bound and the wall polygon test are
 * skipped; callers use it for probes that should also catch geometry beneath
 * the sphere. (Exact gameplay meaning not recovered yet.) */
static u32 sphere_vs_poly(u32 poly, u32 sphere, u32 loose) {
    const Vec3 center = read_vec3(sphere + SPHERE_CENTER);
    const s32 radius = read_s16(sphere + SPHERE_RADIUS);

    /* 1. Cheap reject on height. */
    s32 y_lo, y_hi;
    poly_extent(poly, AXIS_Y, &y_lo, &y_hi);
    if (!loose && center.y + radius < y_lo) return 0;
    if (center.y - radius > y_hi) return 0;

    /* 2. Signed distance from the sphere centre to the polygon's plane
     *    (4.12 fixed point). Negative: the centre is behind the polygon. */
    const u32 normal_addr = mem_r32(poly + POLY_NORMAL);
    const Vec3 n = read_vec3s(normal_addr);
    const Vec3 v0 = read_vec3s(poly_vertex_addr(poly, 0));
    const s32 dist = fx12_mul(v0.x - center.x, n.x) + fx12_mul(v0.z - center.z, n.z) +
                     fx12_mul(v0.y - center.y, n.y);
    if (dist < 0) return 0;

    /* 3. Project the centre onto the plane. Dividing by |n|^2 (4.12) instead of
     *    assuming unit length keeps the result correct for slightly
     *    non-normalised normals. */
    const s32 n_len_sq = ((n.x * n.x) >> 12) + ((n.y * n.y) >> 12) + ((n.z * n.z) >> 12);
    const Vec3 hit = {
        center.x + div32(mul32(n.x, dist), n_len_sq),
        center.y + div32(mul32(n.y, dist), n_len_sq),
        center.z + div32(mul32(n.z, dist), n_len_sq),
    };

    /* 4. The projected point must lie inside the polygon's bounding box. */
    if (!loose && hit.y < y_lo) return 0;
    if (hit.y > y_hi) return 0;
    s32 x_lo, x_hi, z_lo, z_hi;
    poly_extent(poly, AXIS_X, &x_lo, &x_hi);
    if (hit.x < x_lo || hit.x > x_hi) return 0;
    poly_extent(poly, AXIS_Z, &z_lo, &z_hi);
    if (hit.z < z_lo || hit.z > z_hi) return 0;

    /* 5. Manhattan-distance early out: |d|x + |d|y + |d|z >= 2r cannot be a hit. */
    const s32 dx = abs32(hit.x - center.x);
    const s32 dy = abs32(hit.y - center.y);
    const s32 dz = abs32(hit.z - center.z);
    if (!(dx + dy + dz < radius * 2)) return 0;

    /* 6. Exact test that the contact point is inside the polygon's outline.
     *    Surfaces with a vertical component use the floor test; purely
     *    horizontal normals (walls) use the wall test unless `loose`. */
    const u32 verts = poly + POLY_VERTS;
    const u32 material = mem_r16(poly + POLY_FLAGS) & 0xf;
    u32 inside;
    if (n.y != 0) {
        inside = guest_point_in_floor_poly(verts, hit, material, n.y);
    } else {
        inside = 1;
        if (!loose) inside = guest_point_in_wall_poly(normal_addr, verts, hit, material);
    }
    if (!inside) return 0;

    /* 7. Euclidean distance check, then report the contact point. */
    s32 distance = guest_isqrt(mul32(dx, dx) + mul32(dy, dy) + mul32(dz, dz));
    if (radius < distance) return 0;
    write_vec3(sphere + SPHERE_HIT, hit);
    return inside;
}

/* ---- guest entry point (MIPS calling convention) -------------------------- */

/* a0 = polygon, a1 = sphere, a2 = loose; v0 = result. */
static void sphere_vs_poly_guest(void) {
    CYC(100);   /* the original costs ~40-300 cycles depending on how early it rejects */
    g_cpu.r[2] = sphere_vs_poly(g_cpu.r[4], g_cpu.r[5], g_cpu.r[6]);
}

void f_800465a4(void) {
    FN_ENTER(0x800465a4);
    EVT_CHECK();
    override_call("sphere_vs_poly", f_800465a4_orig, sphere_vs_poly_guest);
    FN_RET();
}
