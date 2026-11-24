#include <stdint.h>

#include "cpu_state.h"
#include "mod_plugins.h"

/*
 * Pitch markings in the match engine (BECODE_Z.BIN) are clipped in software
 * against the 512-pixel-wide screen, so with a widened view every touchline,
 * box and arc stopped at the old 4:3 edge. Two routines own that clip:
 *
 *   0x8004373C  outcode(x, y): the Cohen-Sutherland region code used to
 *               reject segments wholly beyond one side of the screen.
 *   0x80043784  line_quad(line, ot): trims a projected polyline to its first
 *               and last drawable points, clips the segment to the screen,
 *               thickens it into a quad written into the line's own GPU
 *               packet and links that packet into the ordering table.
 *
 * Both are reimplemented here with the horizontal screen bounds as parameters
 * and run only while the native-wide margin is nonzero. At 4:3 they decline
 * and the original code runs, so vanilla behaviour is untouched. The C
 * follows the original instruction by instruction; addresses in the comments
 * name the instructions each step reproduces.
 */

#define PLUGIN_ID "fifa2000.widescreen"

#define SCREEN_W 512
#define SCREEN_H 256
#define SCRATCH_XY 0x1F800154u /* projected points: int16 x, int16 y */
#define SCRATCH_Z 0x1F800000u  /* projected depths: int16 per point */

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

    int32_t ax = pa.x, ay = pa.y, az = pa.z;
    int32_t bx = pb.x, by = pb.y, bz = pb.z;

    /* 0x800438BC..0x800438DC: both above or both below the screen. */
    if (ay <= 0 && by <= 0) return;
    const int a_above_bottom = ay < SCREEN_H;
    if (!a_above_bottom && !(by < SCREEN_H)) return;

    /* 0x800438E4..0x80043924: an end needs extending to the screen edge
     * unless it is the polyline's own end point and lies strictly inside. */
    int a_open = 1;
    if (in_range(ax, xl + 1, xr) && ay > 0 && a_above_bottom) a_open = a != 0;
    int b_open = 1;
    if (in_range(bx, xl + 1, xr) && by > 0 && by < SCREEN_H) b_open = b != last;

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
    const uint32_t packet = line + 4u;
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

    /* 0x80043C30..0x80043C3C: addPrim(ot, packet) with lwl/swl. */
    const uint32_t link = lwl(0u, ot + 2u);
    swl(packet << 8, ot + 2u);
    swl(link, packet + 2u);
}

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

PSX_MOD_CONSTRUCTOR(fifa2000_register_pitch_lines) {
    static const uint32_t outcode_words[] = {0x28820200u, 0x14400003u, 0x00001821u, 0x08010DD7u};
    static const uint32_t line_quad_words[] = {0x27BDFFF8u, 0x0080C821u, 0x3C041F80u, 0x34840154u};
    /* Credits approximate the originals' instruction counts on their common
     * paths: the outcode leaf, and the line routine's point searches, two
     * divides, the clip multiplies and the packet writes. */
    (void)psx_mod_register_function_override(PLUGIN_ID, 0x8004373Cu, fifa2000_outcode_override,
                                             outcode_words, 4, 10);
    (void)psx_mod_register_function_override(PLUGIN_ID, 0x80043784u, fifa2000_line_quad_override,
                                             line_quad_words, 4, 200);
}
