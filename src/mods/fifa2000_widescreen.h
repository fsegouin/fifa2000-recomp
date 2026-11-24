#ifndef FIFA2000_WIDESCREEN_H
#define FIFA2000_WIDESCREEN_H

/* Called by the match renderer hooks each time the 3D match draws a frame.
 * The widescreen view presents wide only while that keeps happening, so 2D
 * screens shown with the match engine resident (loading, controls) stay 4:3. */
void fifa2000_note_match_render(void);

/* Allocates the pitch-line arena; called from the widescreen activation. */
void fifa2000_pitch_lines_activate(void);

#endif
