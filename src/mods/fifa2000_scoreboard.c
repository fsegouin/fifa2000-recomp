#include <stdint.h>

#include "cpu_state.h"
#include "func_override.h"
#include "mod_plugins.h"

/*
 * The match scoreboard (clock, team names, score) sits in the top-left corner
 * of the 4:3 frame. 0x800580D0(ot) links its packets into the front HUD layer
 * each frame: the main panel (plus the stoppage-time panel under it while
 * stoppage time shows), then runs of glyph quads from one fixed
 * array. The clock, names and score use glyphs [0, $gp - 0x594C); the
 * stoppage-time line below them continues the array up to the halfword at
 * 0x8010C6E0 (linked only while flags 0x14 are both set). Wrapping it anchors every
 * one of those packets to the left edge, so the native-wide renderer moves
 * the whole scoreboard into the wide frame's corner at its original distance
 * from the edge. No guest data changes; at 4:3 the wrap declines.
 */

#define PLUGIN_ID "fifa2000.widescreen"
#define SCOREBOARD_LINKER 0x800580D0u
#define STOPPAGE_PANEL 0x8010C710u
#define PANEL 0x8010C734u
#define GLYPHS 0x8010C758u
#define GLYPH_STRIDE 0x28u
#define GLYPH_COUNT_GP_OFFSET (-0x594C)
#define GLYPH_END_WITH_STOPPAGE 0x8010C6E0u
#define ANCHOR_LEFT (-1)

static int fifa2000_scoreboard_override(CPUState* cpu) {
    if (psx_mod_widescreen_x_margin() <= 0) return 0;
    const uint32_t glyph_count_address = cpu->gpr[28] + (uint32_t)GLYPH_COUNT_GP_OFFSET;
    func_override_call_original(cpu);

    /* P_TAG addresses: each packet's tag word sits right before its GP0
     * colour command. Unlinked packets are never drawn, so anchoring the
     * whole run covers every flag combination the linker chose. */
    psx_mod_anchor_hud_primitive(STOPPAGE_PANEL, ANCHOR_LEFT);
    psx_mod_anchor_hud_primitive(PANEL, ANCHOR_LEFT);
    uint32_t glyphs = psx_mod_read_half(glyph_count_address);
    const uint32_t with_stoppage = psx_mod_read_half(GLYPH_END_WITH_STOPPAGE);
    if (with_stoppage > glyphs) glyphs = with_stoppage;
    for (uint32_t i = 0; i < glyphs; ++i)
        psx_mod_anchor_hud_primitive(GLYPHS + i * GLYPH_STRIDE, ANCHOR_LEFT);
    return 1;
}

PSX_MOD_CONSTRUCTOR(fifa2000_register_scoreboard) {
    static const uint32_t linker_words[] = {0x3C028011u, 0x8C45C6E4u, 0x00000000u, 0x30A20001u};
    /* The original runs inside the wrap and charges its own cycles. */
    (void)psx_mod_register_function_override(PLUGIN_ID, SCOREBOARD_LINKER, fifa2000_scoreboard_override,
                                             linker_words, 4, FO_CREDIT_SELF);
}
