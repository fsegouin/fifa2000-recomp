#include <math.h>
#include <stdint.h>

#include "cpu_state.h"
#include "fifa2000_widescreen.h"
#include "func_override.h"
#include "mod_plugins.h"
#include "pgxp.h"
#include "psx_cycles.h"

/*
 * Pitch markings in the match engine (BECODE_Z.BIN) are clipped in software
 * against the 512-pixel-wide screen, so with a widened view every touchline,
 * box and arc stopped at the old 4:3 edge. Three routines draw them, each
 * turning projected points into thick-line quads written into the marking's
 * own GPU packets:
 *
 *   0x8004373C  outcode(x, y): the Cohen-Sutherland region code used to
 *               reject segments wholly beyond one side of the screen.
 *   0x80043784  line_quad(line, ot): trims a projected polyline to its first
 *               and last drawable points, clips the segment to the screen,
 *               thickens it into a quad written into the line's own GPU
 *               packet and links that packet into the ordering table.
 *   0x800434B0  curve(curve, ot, closed): one quad per segment of a polyline
 *               (the centre circle, penalty arcs and corner arcs).
 *
 * The game computes every quad on integer screen positions, which hold at the
 * PS1's 240 lines but turn into visible jogs at high internal resolution: the
 * ends sit up to a pixel off the projected points, and each arc segment
 * widens along whichever axis is nearer its normal, so the band steps
 * wherever an arc turns past 45 degrees and the semi-transparent quads
 * double-blend where they overlap.
 *
 * All three run here only while the native-wide margin is nonzero. At 4:3
 * they decline and the original code runs, so vanilla behaviour is untouched.
 * outcode and the integer line_quad follow the original instruction by
 * instruction; addresses in the comments name the instructions each step
 * reproduces. While PGXP draws corrected geometry, the lines and arcs are
 * built from the sub-pixel positions the GTE projected instead: the integer
 * packet halves are those positions' floors and each vertex word carries the
 * exact position for the rasterizer, as the GTE's own packets do. Straight
 * lines keep the original clip and thickness rules; arcs become one
 * continuous band along a smooth curve through their points. Without PGXP
 * the arc routine declines too and the original draws the arcs.
 */

#define PLUGIN_ID "fifa2000.widescreen"

#define SCREEN_W 512
#define SCREEN_H 256
#define SCRATCH_XY 0x1F800154u /* projected points: int16 x, int16 y */
#define SCRATCH_Z 0x1F800000u  /* projected depths: int16 per point */

#define PACKET_BYTES 0x28u /* tag + 9-word textured quad (GP0 0x2C..0x2F) */

/* Arc segments are subdivided along a smooth curve through the projected
 * points into pieces of about CURVE_CHORD screen pixels, at most CURVE_PIECES
 * per segment. The first piece uses the game's own packet for the segment;
 * the others come from an arena indexed by the segment's first point in the
 * marking vertex table, which keeps the layout a function of guest data
 * alone. The table holds as many points as the scratchpad depth table, which
 * ends where the XY table starts. */
#define CURVE_POINTS_MAX 64
#define CURVE_TABLE_POINTS ((int)((SCRATCH_XY - SCRATCH_Z) / 2u))
#define CURVE_PIECES 8
#define CURVE_CHORD 6.0

static uint32_t s_curve_arena;

static int32_t mul_sra15(int32_t a, int32_t b) {
    /* mult; mflo; sra 15 */
    return (int32_t)(uint32_t)((int64_t)a * (int64_t)b) >> 15;
}

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

/* Little-endian LWL/SWL, as the R3000A performs them. */
static uint32_t lwl(uint32_t reg, uint32_t address) {
    const uint32_t shift = (3u - (address & 3u)) * 8u;
    const uint32_t mem = psx_mod_read_word(address & ~3u);
    const uint32_t keep = shift == 0 ? 0u : (0xFFFFFFFFu >> (32u - shift));
    return (reg & keep) | (mem << shift);
}

static void swl(uint32_t reg, uint32_t address) {
    const uint32_t shift = (3u - (address & 3u)) * 8u;
    const uint32_t base = address & ~3u;
    const uint32_t mem = psx_mod_read_word(base);
    const uint32_t keep = shift == 0 ? 0u : (0xFFFFFFFFu << (32u - shift));
    psx_mod_write_word(base, (mem & keep) | (reg >> shift));
}

/* addPrim(ot, packet) with lwl/swl, as 0x80043C30..0x80043C3C does it. */
static void add_prim(uint32_t ot, uint32_t packet) {
    const uint32_t link = lwl(0u, ot + 2u);
    swl(packet << 8, ot + 2u);
    swl(link, packet + 2u);
}

static uint32_t outcode(int32_t x, int32_t y, int32_t xl, int32_t xr) {
    uint32_t code = 0;
    if (!(x < xr)) code = 8;          /* 0x8004373C slti a0, 0x200 */
    else if (!(x > xl)) code = 2;     /* 0x80043750 bgtz a0 */
    if (!(y < SCREEN_H)) code |= 1;   /* 0x8004375C slti a1, 0x100 */
    else if (!(y > 0)) code |= 4;     /* 0x80043770 bgtz a1 */
    return code;
}

typedef struct {
    int32_t x, y, z;
} LinePoint;

typedef struct {
    double x, y;
} Vec;

static LinePoint line_point(uint32_t xy, uint32_t z, int32_t index) {
    LinePoint p;
    p.x = (int16_t)psx_mod_read_half(xy + (uint32_t)index * 4u);
    p.y = (int16_t)psx_mod_read_half(xy + (uint32_t)index * 4u + 2u);
    p.z = (int16_t)psx_mod_read_half(z + (uint32_t)index * 2u);
    return p;
}

/* 0x800437FC..0x8004383C: a point the projection can use. */
static int line_point_drawable(LinePoint p) {
    return (uint32_t)(p.x + 1000) < 2001u && p.y >= -1000 && p.y < 1001 && p.z >= 200;
}

/* (x - lo) <u (hi - lo): x in [lo, hi), the sltiu idiom of the original. */
static int in_range(int32_t x, int32_t lo, int32_t hi) {
    return (uint32_t)(x - lo) < (uint32_t)(hi - lo);
}

/* --- sub-pixel positions ------------------------------------------------- */

/* The renderer draws PGXP positions only while the engine is armed and the
 * geometry correction consumes them. */
static int pgxp_drawing(void) { return pgxp_enabled() && gte_geometry_correction_enabled(); }

/* How far a precise axis moved the vertex from the guest integer, as the
 * renderer's tolerance clamp measures it: for a derived half the first pixel
 * above the integer is the guest arithmetic's own rounding, not movement. */
static double moved(double precise, int32_t native, int derived) {
    const double d = precise - native;
    if (!derived) return fabs(d);
    return d < 0.0 ? -d : (d > 1.0 ? d - 1.0 : 0.0);
}

/* The position the GTE projected for the scratchpad SXY word of a point, when
 * its PGXP shadow still describes that word. The window is the renderer's
 * preserve-projection agreement window and the tolerance its clamp, so this
 * is a vertex the GPU would accept. */
static int precise_point(uint32_t xy, int32_t index, LinePoint p, Vec* out) {
    const uint32_t address = xy + (uint32_t)index * 4u;
    PGXPWordShadow shadow;
    if (!pgxp_capture_word_shadow(address, psx_mod_read_word(address), &shadow) ||
        !shadow.valid || (shadow.flags & PGXP_F_VXY) != PGXP_F_VXY)
        return 0;
    out->x = shadow.x16 / 65536.0;
    out->y = shadow.y16 / 65536.0;
    if (!(out->x > p.x - PGXP_PPP_AGREE_BELOW && out->x < p.x + PGXP_PPP_AGREE_ABOVE &&
          out->y > p.y - PGXP_PPP_AGREE_BELOW && out->y < p.y + PGXP_PPP_AGREE_ABOVE))
        return 0;
    const double tolerance = pgxp_tolerance();
    return tolerance < 0.0 ||
           (moved(out->x, p.x, (shadow.flags & PGXP_F_DX) != 0) <= tolerance &&
            moved(out->y, p.y, (shadow.flags & PGXP_F_DY) != 0) <= tolerance);
}

/* Writes one packet vertex: the floors of the position as the halves the GPU
 * parses, and the position itself as the word's PGXP shadow. Deriving the
 * integers from the shadow keeps them in exact truncation agreement. */
static void put_vertex(uint32_t address, Vec v) {
    const int32_t x16 = (int32_t)floor(fmax(-1024.0, fmin(1023.0, v.x)) * 65536.0);
    const int32_t y16 = (int32_t)floor(fmax(-1024.0, fmin(1023.0, v.y)) * 65536.0);
    const uint32_t word = ((uint32_t)(uint16_t)(y16 >> 16) << 16) | (uint16_t)(x16 >> 16);
    psx_mod_write_word(address, word);

    PGXPNativeWord vertex = {0};
    if (!pgxp_capture_word_shadow(address, word, &vertex.word)) return;
    vertex.word.valid = 1u;
    vertex.word.x16 = x16;
    vertex.word.y16 = y16;
    vertex.word.z = 0;
    vertex.word.flags = PGXP_F_VXY;
    (void)pgxp_native_publish_words(&address, &vertex, 1u);
}

/* --- straight lines: 0x80043784 ------------------------------------------ */

/* The integer quad of the original, for when PGXP draws native positions. */
static void line_quad_native(uint32_t packet, LinePoint pa, int a_end, LinePoint pb, int b_end,
                             int32_t xl, int32_t xr, uint32_t ot) {
    int32_t ax = pa.x, ay = pa.y, az = pa.z;
    int32_t bx = pb.x, by = pb.y, bz = pb.z;

    /* 0x800438BC..0x800438DC: both above or both below the screen. */
    if (ay <= 0 && by <= 0) return;
    const int a_above_bottom = ay < SCREEN_H;
    if (!a_above_bottom && !(by < SCREEN_H)) return;

    /* 0x800438E4..0x80043924: an end needs extending to the screen edge
     * unless it is the polyline's own end point and lies strictly inside. */
    int a_open = 1;
    if (in_range(ax, xl + 1, xr) && ay > 0 && a_above_bottom) a_open = !a_end;
    int b_open = 1;
    if (in_range(bx, xl + 1, xr) && by > 0 && by < SCREEN_H) b_open = !b_end;

    /* 0x80043928..0x8004395C: order the ends left to right. */
    if (bx < ax) {
        int32_t t;
        t = ax; ax = bx; bx = t;
        t = ay; ay = by; by = t;
        t = az; az = bz; bz = t;
        t = a_open; a_open = b_open; b_open = t;
    }

    /* 0x80043960, 0x8004396C: wholly right or wholly left of the screen. */
    if (!(ax < xr)) return;
    if (bx <= xl) return;

    const int32_t dx = bx - ax;
    const int32_t dy = by - ay;
    const int32_t sum = abs32(dx + dy);

    if (!((sum >> 7) < abs32(dx))) {
        /* 0x800439A4..0x800439C0: near vertical, extend to the top/bottom. */
        if (a_open) ay = ay < by ? 0 : SCREEN_H;
        if (b_open) by = ay < by ? SCREEN_H : 0;
    } else if (!((sum >> 8) < abs32(dy))) {
        /* 0x800439DC..0x800439F4: near horizontal, extend to the sides. */
        if (a_open) ax = xl;
        if (b_open) bx = xr;
    } else {
        /* 0x800439F8..0x80043A5C: slopes in 1.15 fixed point. */
        const int32_t y_per_x = (int32_t)((uint32_t)(-dy) << 15) / dx;
        const int32_t x_per_y = (int32_t)((uint32_t)(-dx) << 15) / dy;

        /* 0x80043A64..0x80043AA8: extend the left end to the left edge, then
         * back inside the top or bottom edge. */
        if (a_open) {
            ay += mul_sra15(y_per_x, ax - xl);
            if (ay < 0) {
                ax = xl + mul_sra15(x_per_y, ay);
                ay = 0;
            } else if (!(ay < SCREEN_H + 1)) {
                ax = xl + mul_sra15(x_per_y, ay - SCREEN_H);
                ay = SCREEN_H;
            } else {
                ax = xl;
            }
        }
        /* 0x80043AAC..0x80043ABC */
        if (!in_range(ax, xl, xr + 1)) return;
        if (bx < ax) return;

        /* 0x80043AC0..0x80043B18: the same for the right end. */
        if (b_open) {
            by += mul_sra15(y_per_x, bx - xr);
            if (by < 0) {
                bx = mul_sra15(x_per_y, by) + xr;
                by = 0;
            } else if (!(by < SCREEN_H + 1)) {
                bx = mul_sra15(x_per_y, by - SCREEN_H) + xr;
                by = SCREEN_H;
            } else {
                bx = xr;
            }
        }
        /* 0x80043B1C..0x80043B28 */
        if (!in_range(bx, xl, xr + 1)) return;
        if (bx < ax) return;
    }

    /* 0x80043B30..0x80043C2C: thicken into a quad in the line's packet. */
    if (abs32(ax - bx) < abs32(ay - by)) {
        /* Steep: widen horizontally, 6..8 pixels by depth. */
        int32_t wa = 8 - (az >> 11), wb = 8 - (bz >> 11);
        if (wa < 6) wa = 6;
        if (wb < 6) wb = 6;
        const int32_t half = wa >> 1;
        ax -= half;
        bx -= half; /* 0x80043B9C: the original offsets both ends by wa/2 */
        psx_mod_write_half(packet + 0x10u, (uint16_t)(ax + wa));
        psx_mod_write_half(packet + 0x08u, (uint16_t)ax);
        psx_mod_write_half(packet + 0x18u, (uint16_t)bx);
        psx_mod_write_half(packet + 0x20u, (uint16_t)(bx + wb));
        psx_mod_write_half(packet + 0x12u, (uint16_t)ay);
        psx_mod_write_half(packet + 0x0Au, (uint16_t)ay);
        psx_mod_write_half(packet + 0x22u, (uint16_t)by);
        psx_mod_write_half(packet + 0x1Au, (uint16_t)by);
    } else {
        /* Shallow: widen vertically, 2..3 pixels by depth. */
        int32_t wa = 3 - (az >> 12), wb = 3 - (bz >> 12);
        if (wa < 2) wa = 2;
        if (wb < 2) wb = 2;
        ay -= wa >> 1;
        by -= wb >> 1;
        psx_mod_write_half(packet + 0x0Au, (uint16_t)(ay + wa));
        psx_mod_write_half(packet + 0x12u, (uint16_t)ay);
        psx_mod_write_half(packet + 0x1Au, (uint16_t)(by + wb));
        psx_mod_write_half(packet + 0x22u, (uint16_t)by);
        psx_mod_write_half(packet + 0x08u, (uint16_t)ax);
        psx_mod_write_half(packet + 0x10u, (uint16_t)ax);
        psx_mod_write_half(packet + 0x18u, (uint16_t)bx);
        psx_mod_write_half(packet + 0x20u, (uint16_t)bx);
    }
    add_prim(ot, packet);
}

/* The same clip and thickening rules on the projected sub-pixel positions. */
static void line_quad_precise(uint32_t packet, Vec a, int32_t az, int a_end, Vec b, int32_t bz,
                              int b_end, int32_t xl, int32_t xr, uint32_t ot) {
    if (a.y <= 0 && b.y <= 0) return;
    if (!(a.y < SCREEN_H) && !(b.y < SCREEN_H)) return;

    int a_open = !(a_end && a.x >= xl + 1 && a.x < xr && a.y > 0 && a.y < SCREEN_H);
    int b_open = !(b_end && b.x >= xl + 1 && b.x < xr && b.y > 0 && b.y < SCREEN_H);

    if (b.x < a.x) {
        const Vec tv = a; a = b; b = tv;
        const int32_t tz = az; az = bz; bz = tz;
        const int to = a_open; a_open = b_open; b_open = to;
    }

    if (!(a.x < xr)) return;
    if (b.x <= xl) return;

    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double sum = fabs(dx + dy);

    if (!(floor(sum / 128.0) < fabs(dx))) {
        if (a_open) a.y = a.y < b.y ? 0 : SCREEN_H;
        if (b_open) b.y = a.y < b.y ? SCREEN_H : 0;
    } else if (!(floor(sum / 256.0) < fabs(dy))) {
        if (a_open) a.x = xl;
        if (b_open) b.x = xr;
    } else {
        const double y_per_x = -dy / dx;
        const double x_per_y = -dx / dy;
        if (a_open) {
            a.y += y_per_x * (a.x - xl);
            if (a.y < 0) {
                a.x = xl + x_per_y * a.y;
                a.y = 0;
            } else if (a.y > SCREEN_H) {
                a.x = xl + x_per_y * (a.y - SCREEN_H);
                a.y = SCREEN_H;
            } else {
                a.x = xl;
            }
        }
        if (!(a.x >= xl && a.x < xr + 1)) return;
        if (b.x < a.x) return;
        if (b_open) {
            b.y += y_per_x * (b.x - xr);
            if (b.y < 0) {
                b.x = xr + x_per_y * b.y;
                b.y = 0;
            } else if (b.y > SCREEN_H) {
                b.x = xr + x_per_y * (b.y - SCREEN_H);
                b.y = SCREEN_H;
            } else {
                b.x = xr;
            }
        }
        if (!(b.x >= xl && b.x < xr + 1)) return;
        if (b.x < a.x) return;
    }

    Vec v[4];
    if (fabs(a.x - b.x) < fabs(a.y - b.y)) {
        int32_t wa = 8 - (az >> 11), wb = 8 - (bz >> 11);
        if (wa < 6) wa = 6;
        if (wb < 6) wb = 6;
        const int32_t half = wa >> 1;
        a.x -= half;
        b.x -= half;
        v[0] = a;
        v[1] = (Vec){a.x + wa, a.y};
        v[2] = b;
        v[3] = (Vec){b.x + wb, b.y};
    } else {
        int32_t wa = 3 - (az >> 12), wb = 3 - (bz >> 12);
        if (wa < 2) wa = 2;
        if (wb < 2) wb = 2;
        a.y -= wa >> 1;
        b.y -= wb >> 1;
        v[0] = (Vec){a.x, a.y + wa};
        v[1] = a;
        v[2] = (Vec){b.x, b.y + wb};
        v[3] = b;
    }
    for (uint32_t i = 0; i < 4u; ++i) put_vertex(packet + 0x08u + i * 8u, v[i]);
    add_prim(ot, packet);
}

static void line_quad(uint32_t line, uint32_t ot, int32_t xl, int32_t xr) {
    const int32_t first = (int16_t)psx_mod_read_half(line + 2u);
    const int32_t last = (int16_t)psx_mod_read_half(line) - 1;
    const uint32_t xy = SCRATCH_XY + (uint32_t)first * 4u;
    const uint32_t zs = SCRATCH_Z + (uint32_t)first * 2u;

    /* 0x800437D0..0x8004383C: first drawable point from the front. */
    int32_t a = 0;
    LinePoint pa = line_point(xy, zs, a);
    while (!line_point_drawable(pa)) {
        if (!(++a < last)) return;
        pa = line_point(xy, zs, a);
    }
    /* 0x80043840..0x800438B8: last drawable point from the back. */
    int32_t b = last;
    LinePoint pb = line_point(xy, zs, b);
    while (!line_point_drawable(pb)) {
        if (!(a < --b)) return;
        pb = line_point(xy, zs, b);
    }

    const uint32_t packet = line + 4u;
    Vec va, vb;
    if (pgxp_drawing() && precise_point(xy, a, pa, &va) && precise_point(xy, b, pb, &vb))
        line_quad_precise(packet, va, pa.z, a == 0, vb, pb.z, b == last, xl, xr, ot);
    else
        line_quad_native(packet, pa, a == 0, pb, b == last, xl, xr, ot);
}

/* --- arcs: 0x800434B0 ---------------------------------------------------- */

/* A point the arc can bend through: in front of the camera and inside the
 * range the projection keeps meaningful (the original's own limits). */
static int curve_point_usable(LinePoint p) {
    return p.z >= 201 && abs32(p.x) < 1001 && abs32(p.y) < 1001;
}

typedef struct {
    int n, closed;
    LinePoint p[CURVE_POINTS_MAX];
    Vec v[CURVE_POINTS_MAX];
} Curve;

static Vec vsub(Vec a, Vec b) { return (Vec){a.x - b.x, a.y - b.y}; }
static Vec vadd(Vec a, Vec b) { return (Vec){a.x + b.x, a.y + b.y}; }
static Vec vscale(Vec a, double s) { return (Vec){a.x * s, a.y * s}; }
static double vlen(Vec a) { return sqrt(a.x * a.x + a.y * a.y); }

/* Neighbour of point k along the arc, or -1 past an open end. */
static int curve_neighbour(const Curve* c, int k, int step) {
    const int j = k + step;
    if (j >= 0 && j < c->n) return j;
    return c->closed ? (j + c->n) % c->n : -1;
}

/* The neighbour the spline bends towards: the real one when usable, else the
 * reflection of the opposite neighbour, which keeps the end straight. */
static Vec curve_side(const Curve* c, int k, int step) {
    const int j = curve_neighbour(c, k, step);
    if (j >= 0 && curve_point_usable(c->p[j])) return c->v[j];
    const int o = curve_neighbour(c, k, -step);
    if (o >= 0 && curve_point_usable(c->p[o])) return vsub(vscale(c->v[k], 2.0), c->v[o]);
    return c->v[k];
}

/* Centripetal knot interval between two points. */
static double knot(Vec a, Vec b) { return sqrt(vlen(vsub(b, a))); }

/* Tangent of the centripetal Catmull-Rom spline at point k, per unit knot
 * parameter. It depends on point k and its neighbours only, so both segments
 * meeting at k bend through the same direction and share their joint. */
static Vec curve_tangent(const Curve* c, int k) {
    const Vec p0 = curve_side(c, k, -1), p1 = c->v[k], p2 = curve_side(c, k, 1);
    const double t0 = knot(p0, p1), t1 = knot(p1, p2);
    if (t0 < 1e-6 || t1 < 1e-6) return vsub(p2, p0);
    return vadd(vsub(vscale(vsub(p1, p0), 1.0 / t0), vscale(vsub(p2, p0), 1.0 / (t0 + t1))),
                vscale(vsub(p2, p1), 1.0 / t1));
}

/*
 * Full band across the line at one point, from the original's thickness
 * rules: a line running across the screen is widened vertically by
 * 3 - z/4096 pixels (at least 2) and one running up the screen horizontally
 * by 5 - z/2048 (at least 3). Here the steps are evened out over depth and
 * the widening follows the tangent: the band is the image of a constant-width
 * ground stripe whose horizontal and vertical screen widths are those two, so
 * it equals the original at both axes and turns smoothly between them.
 */
static Vec curve_band(Vec tangent, double z) {
    const double wv = fmax(2.0, fmin(3.0, 3.5 - z / 4096.0));
    const double wh = fmax(3.0, fmin(5.0, 5.5 - z / 2048.0));
    const double d = sqrt(wv * wv * tangent.x * tangent.x + wh * wh * tangent.y * tangent.y);
    if (d < 1e-9) return (Vec){0.0, wv};
    return (Vec){-wh * wh * tangent.y / d, wv * wv * tangent.x / d};
}

/* Cubic Hermite point and derivative on one segment. */
static Vec hermite(Vec p1, Vec m1, Vec p2, Vec m2, double t, Vec* derivative) {
    const double t2 = t * t, t3 = t2 * t;
    *derivative = vadd(vadd(vscale(p1, 6.0 * t2 - 6.0 * t), vscale(m1, 3.0 * t2 - 4.0 * t + 1.0)),
                       vadd(vscale(p2, 6.0 * t - 6.0 * t2), vscale(m2, 3.0 * t2 - 2.0 * t)));
    return vadd(vadd(vscale(p1, 2.0 * t3 - 3.0 * t2 + 1.0), vscale(m1, t3 - 2.0 * t2 + t)),
                vadd(vscale(p2, 3.0 * t2 - 2.0 * t3), vscale(m2, t3 - t2)));
}

/* Links one band quad from p (band bp) to q (band bq), v0/v2 on the +band
 * side: right of the direction of travel on screen, so the UVs' u = 240 edge
 * stays on one side along the whole arc. The original instead keeps it below
 * a shallow segment and left of a steep one whatever the direction, which
 * agrees only where the arc runs rightward or downward; the band texture is
 * symmetric across its width. An arena packet first takes the game packet's
 * length, command, colour, texture page, CLUT and UVs. */
static void curve_quad(uint32_t packet, uint32_t game_packet, Vec p, Vec bp, Vec q, Vec bq,
                       uint32_t ot) {
    if (packet != game_packet) {
        psx_mod_write_word(packet, psx_mod_read_word(game_packet) & 0xFF000000u);
        for (uint32_t w = 1; w < PACKET_BYTES / 4u; w += 2u)
            psx_mod_write_word(packet + w * 4u, psx_mod_read_word(game_packet + w * 4u));
    }
    const Vec hp = vscale(bp, 0.5), hq = vscale(bq, 0.5);
    put_vertex(packet + 0x08u, vadd(p, hp));
    put_vertex(packet + 0x10u, vsub(p, hp));
    put_vertex(packet + 0x18u, vadd(q, hq));
    put_vertex(packet + 0x20u, vsub(q, hq));
    add_prim(ot, packet);
}

/* Returns the segments drawn, or -1 to leave the arc to the original.
 * *walked receives the segments the original's loop visits. */
static int curve(uint32_t marking, uint32_t ot, int closed, int32_t xl, int32_t xr,
                 int* walked) {
    Curve c;
    c.n = (int16_t)psx_mod_read_half(marking);
    c.closed = closed;
    const int32_t first = (int16_t)psx_mod_read_half(marking + 2u);
    const uint32_t packets = psx_mod_read_word(marking + 4u);
    const uint32_t xy = SCRATCH_XY + (uint32_t)first * 4u;
    const uint32_t zs = SCRATCH_Z + (uint32_t)first * 2u;
    const int segments = closed ? c.n : c.n - 1;
    *walked = 0;
    if (c.n > CURVE_POINTS_MAX || first < 0 || first + c.n > CURVE_TABLE_POINTS) return -1;

    /* 0x8004356C: an arc without segments is not drawn. */
    if (segments <= 0) return 0;
    for (int k = 0; k < c.n; ++k) {
        c.p[k] = line_point(xy, zs, k);
        if (!precise_point(xy, k, c.p[k], &c.v[k])) c.v[k] = (Vec){c.p[k].x, c.p[k].y};
    }
    /* 0x8004350C..0x8004354C: nor an open arc whose first point is out of
     * range. */
    if (!closed && !(abs32(c.p[0].x) < 1001 && abs32(c.p[0].y) < 1001)) return 0;
    *walked = segments;

    int drawn = 0;
    for (int i = 0; i < segments; ++i) {
        const int a = i, b = (i + 1) % c.n;
        /* 0x8004357C..0x800435BC: skip a segment wholly beyond one screen
         * edge, or one starting behind the near plane. */
        if (outcode(c.p[a].x, c.p[a].y, xl, xr) & outcode(c.p[b].x, c.p[b].y, xl, xr)) continue;
        if (c.p[a].z < 201) continue;
        ++drawn;

        const Vec pa = c.v[a], pb = c.v[b];
        const double za = c.p[a].z, zb = c.p[b].z;
        const int bends = curve_point_usable(c.p[a]) && curve_point_usable(c.p[b]);
        const double chord = vlen(vsub(pb, pa));
        int pieces = bends && s_curve_arena ? (int)ceil(chord / CURVE_CHORD) : 1;
        if (pieces < 1) pieces = 1;
        if (pieces > CURVE_PIECES) pieces = CURVE_PIECES;

        /* Hermite form of the centripetal spline: knot tangents scaled to
         * this segment's own knot interval. A segment that cannot bend keeps
         * its chord and bands from its own direction. */
        const double interval = knot(pa, pb);
        const Vec ma = bends ? vscale(curve_tangent(&c, a), interval) : vsub(pb, pa);
        const Vec mb = bends ? vscale(curve_tangent(&c, b), interval) : vsub(pb, pa);

        const uint32_t game_packet = packets + (uint32_t)i * PACKET_BYTES;
        Vec p = pa, band = curve_band(ma, za);
        for (int j = 1; j <= pieces; ++j) {
            const double t = (double)j / pieces;
            Vec tangent;
            const Vec q = j == pieces ? pb : hermite(pa, ma, pb, mb, t, &tangent);
            if (j == pieces) tangent = mb;
            const Vec next_band = curve_band(tangent, za + (zb - za) * t);
            const uint32_t extra = (uint32_t)(first + i) * (CURVE_PIECES - 1) + (uint32_t)(j - 2);
            curve_quad(j == 1 ? game_packet : s_curve_arena + extra * PACKET_BYTES, game_packet, p,
                       band, q, next_band, ot);
            p = q;
            band = next_band;
        }
    }
    return drawn;
}

/* --- overrides ----------------------------------------------------------- */

static int fifa2000_outcode_override(CPUState* cpu) {
    const int32_t m = psx_mod_widescreen_x_margin();
    if (m <= 0) return 0;
    cpu->gpr[2] = outcode((int32_t)cpu->gpr[4], (int32_t)cpu->gpr[5], -m, SCREEN_W + m);
    return 1;
}

static int fifa2000_line_quad_override(CPUState* cpu) {
    const int32_t m = psx_mod_widescreen_x_margin();
    if (m <= 0) return 0;
    line_quad(cpu->gpr[4], cpu->gpr[5], -m, SCREEN_W + m);
    return 1;
}

static int fifa2000_curve_override(CPUState* cpu) {
    const int32_t m = psx_mod_widescreen_x_margin();
    if (m <= 0 || !pgxp_drawing()) return 0;
    int walked;
    const int drawn = curve(cpu->gpr[4], cpu->gpr[5], cpu->gpr[6] != 0, -m, SCREEN_W + m, &walked);
    if (drawn < 0) return 0;
    /* The original's prologue and epilogue, then per segment its loop visits
     * its outcode pair and, for a drawn one, the thickening and the packet
     * writes. */
    psx_advance_cycles(40u + 35u * (uint32_t)walked + 40u * (uint32_t)drawn);
    return 1;
}

void fifa2000_pitch_lines_activate(void) {
    /* Process lifetime, like every GPU-DMA arena: a rematch reuses it. */
    if (!s_curve_arena)
        s_curve_arena = psx_mod_alloc_gpu_dma_memory(
            CURVE_TABLE_POINTS * (CURVE_PIECES - 1) * PACKET_BYTES, 4u);
}

PSX_MOD_CONSTRUCTOR(fifa2000_register_pitch_lines) {
    static const uint32_t outcode_words[] = {0x28820200u, 0x14400003u, 0x00001821u, 0x08010DD7u};
    static const uint32_t line_quad_words[] = {0x27BDFFF8u, 0x0080C821u, 0x3C041F80u, 0x34840154u};
    static const uint32_t curve_words[] = {0x27BDFFC8u, 0xAFBE0030u, 0x0080F021u, 0xAFA5003Cu};
    /* Credits approximate the originals' instruction counts on their common
     * paths: the outcode leaf, and the line routine's point searches, two
     * divides, the clip multiplies and the packet writes. The arc routine's
     * cost grows with its segments, so its override charges its own. */
    (void)psx_mod_register_function_override(PLUGIN_ID, 0x8004373Cu, fifa2000_outcode_override,
                                             outcode_words, 4, 10);
    (void)psx_mod_register_function_override(PLUGIN_ID, 0x80043784u, fifa2000_line_quad_override,
                                             line_quad_words, 4, 200);
    (void)psx_mod_register_function_override(PLUGIN_ID, 0x800434B0u, fifa2000_curve_override,
                                             curve_words, 4, FO_CREDIT_SELF);
}
