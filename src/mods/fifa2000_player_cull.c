#include <math.h>
#include <stdint.h>

#include "cpu_state.h"
#include "func_override.h"
#include "mod_plugins.h"

/*
 * Players are classified for drawing each frame by 0x80048BB8. Before its
 * screen-space test (widened through [widescreen.cull] in game.toml) it calls
 * the view-cone test at 0x80047918:
 *
 *     angle(camera forward, p) - atan2(radius, |p|) <= half_angle
 *
 * with half_angle a constant sized for the 4:3 view, so players were dropped
 * until they crossed into the old frame. The cone's only free input is the
 * radius argument. Raising it so that atan2(radius', |p|) grows by the extra
 * horizontal half-angle of the widened view widens the cone by exactly that
 * angle while the original code does all of its own math. At 4:3 the
 * override declines and the original runs unchanged.
 */

#define PLUGIN_ID "fifa2000.widescreen"
#define VIEW_CONE_TEST 0x80047918u
/* A radius that puts the angular margin at a right angle: every point in
 * front of the camera passes. */
#define RADIUS_ALL_FRONT 0x100000

static int fifa2000_view_cone_override(CPUState* cpu) {
    const int32_t margin = psx_mod_widescreen_x_margin();
    const int32_t h = (int32_t)(cpu->gte_ctrl[26] & 0xFFFFu);     /* projection distance */
    const int32_t half_width = (int32_t)cpu->gte_ctrl[24] >> 16;  /* OFX, the screen centre */
    if (margin <= 0 || h <= 0 || half_width <= 0) return 0;

    const uint32_t p = cpu->gpr[4];
    const double x = (int16_t)psx_mod_read_half(p);
    const double y = (int16_t)psx_mod_read_half(p + 2u);
    const double z = (int16_t)psx_mod_read_half(p + 4u);
    const double distance = sqrt(x * x + y * y + z * z);
    if (distance <= 0.0) return 0;

    const double extra = atan((double)(half_width + margin) / h) - atan((double)half_width / h);
    const double reach = atan2((double)(int32_t)cpu->gpr[5], distance) + extra;
    int32_t radius = RADIUS_ALL_FRONT;
    if (reach < 1.55) {
        const double widened = ceil(distance * tan(reach));
        if (widened < RADIUS_ALL_FRONT) radius = (int32_t)widened;
    }
    cpu->gpr[5] = (uint32_t)radius;
    func_override_call_original(cpu);
    return 1;
}

PSX_MOD_CONSTRUCTOR(fifa2000_register_player_cull) {
    static const uint32_t view_cone_words[] = {0x27BDFFE0u, 0xAFBF001Cu, 0xAFB20018u, 0xAFB10014u};
    /* The original runs inside the wrap and charges its own cycles. */
    (void)psx_mod_register_function_override(PLUGIN_ID, VIEW_CONE_TEST, fifa2000_view_cone_override,
                                             view_cone_words, 4, FO_CREDIT_SELF);
}
