#include <stdint.h>

#include "cpu_state.h"
#include "fifa2000_widescreen.h"
#include "func_override.h"
#include "gpu.h"
#include "mod_plugins.h"

/*
 * The match sky is one screen-space gouraud quad, drawn first each frame over
 * the green clear and spanning exactly the 512-pixel buffer down to the
 * horizon. 0x800490A8 sets its bottom edge to the current horizon line and
 * links it into the ordering table; the packet pointer lives at
 * SKY_STATE + 0x9E8 and the horizon line at SKY_STATE. Unprojected, the quad
 * never reaches the native-wide margins, which kept the clear colour.
 *
 * Wrapping the builder lets the original finish the packet, then tags it as
 * an authored background so the native-wide renderer stretches it across the
 * whole frame. No guest data changes; at 4:3 the wrap declines.
 */

#define PLUGIN_ID "fifa2000.widescreen"
#define SKY_BUILDER 0x800490A8u
#define SKY_STATE 0x80109594u
#define SKY_PACKET_POINTER (SKY_STATE + 0x9E8u)

static int fifa2000_sky_override(CPUState* cpu) {
    /* The renderer builds the sky once per drawn match frame, so this is also
     * where the widescreen view learns the 3D match is on screen. */
    fifa2000_note_match_render();
    if (psx_mod_widescreen_x_margin() <= 0) return 0;
    func_override_call_original(cpu);
    /* 0x800490C0: no sky is built while the horizon is at or above the top. */
    if ((int32_t)psx_mod_read_word(SKY_STATE) > 0)
        gpu_ws_tag_background_prim(psx_mod_read_word(SKY_PACKET_POINTER));
    return 1;
}

PSX_MOD_CONSTRUCTOR(fifa2000_register_sky) {
    static const uint32_t sky_builder_words[] = {0x27BDFFE0u, 0xAFB10014u, 0x3C118011u, 0x8E229594u};
    /* The original runs inside the wrap and charges its own cycles. */
    (void)psx_mod_register_function_override(PLUGIN_ID, SKY_BUILDER, fifa2000_sky_override,
                                             sky_builder_words, 4, FO_CREDIT_SELF);
}
