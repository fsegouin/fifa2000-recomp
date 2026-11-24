#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fifa2000_widescreen.h"
#include "mod_plugins.h"

/*
 * Widescreen is owned by the mod catalog on PSX: this plugin's activation
 * selects the display aspect before the renderer and window exist, and tells
 * the native-wide renderer which frames are the 3D match. Those render into a
 * wider frame with more field of view; menus, loading screens and FMVs keep
 * 4:3.
 */
#define PKG "fifa2000.enhancement.widescreen"
#define FEATURE "widescreen"

/*
 * The match engine (BECODE_Z.BIN) is loaded whole at 0x8003EE64; the front
 * end (FECODE_Z.BIN) overwrites the same region. Three words spread across
 * the image, taken from the disc file, prove the match engine is resident so
 * the 3D match presents wide while every front-end screen keeps 4:3.
 */
static const struct {
    uint32_t address;
    uint32_t word;
} k_match_engine_guard[] = {
    {0x8003EE6Cu, 0x6F636F6Du}, /* "moco" of "mocore.viv" in its file table */
    {0x80042E64u, 0x85040002u},
    {0x8009EE64u, 0x00002021u},
};

static int fifa2000_match_engine_resident(void) {
    for (size_t i = 0; i < sizeof k_match_engine_guard / sizeof k_match_engine_guard[0]; ++i) {
        if (psx_mod_read_word(k_match_engine_guard[i].address) !=
            k_match_engine_guard[i].word)
            return 0;
    }
    return 1;
}

/*
 * The match engine stays resident behind its own 2D screens (controls,
 * loading), so residency alone would present those wide with stale 3D in the
 * margins. The match renderer reports every frame it draws; a frame counts as
 * the 3D match only while that has happened within the last few VBlanks. The
 * game draws at most every other VBlank and can drop to one in four, so eight
 * covers slow frames without holding wide over a 2D screen for long. This is
 * presentation state only: it never touches guest memory.
 */
#define MATCH_RENDER_WINDOW 8u

static uint32_t s_vblanks;
static uint32_t s_last_match_render;
static int s_match_rendered;

void fifa2000_note_match_render(void) {
    s_last_match_render = s_vblanks;
    s_match_rendered = 1;
}

static void fifa2000_widescreen_vblank(void) { ++s_vblanks; }

static int fifa2000_match_scene(void) {
    return s_match_rendered && s_vblanks - s_last_match_render <= MATCH_RENDER_WINDOW &&
           fifa2000_match_engine_resident();
}

static void fifa2000_widescreen_activate(void) {
    char aspect[16];
    if (!psx_mod_option_value(PKG, FEATURE, "aspect", aspect, sizeof aspect) ||
        aspect[0] == '\0')
        strcpy(aspect, "16:9");

    psx_mod_set_world_scene_predicate(fifa2000_match_scene);
    fifa2000_pitch_lines_activate();

    /* The fixed aspect also shapes the first window; Fit then follows every
     * resize from 4:3 upward with no upper limit. */
    (void)psx_mod_set_fixed_display_aspect(16u, 9u);
    if (strcmp(aspect, "Fit") == 0)
        (void)psx_mod_set_adaptive_display_aspect(0u, 0u);
}

PSX_MOD_CONSTRUCTOR(fifa2000_register_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin("fifa2000.widescreen",
                                             fifa2000_widescreen_activate);
    (void)psx_mod_register_vblank_plugin("fifa2000.widescreen", fifa2000_widescreen_vblank);
}
