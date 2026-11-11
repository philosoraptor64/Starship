#ifndef STARSHIP_HUDFULLVIEW_H
#define STARSHIP_HUDFULLVIEW_H
#include <libultraship/libultra/gbi.h>
#ifdef __cplusplus
extern "C" {
#endif
/* the Versus HUD drawn in [start, end) for `player`'s quarter, zoomed to the full screen (see HudFullView.c) */
void HudFullView_Transform(Gfx* start, Gfx* end, int player);
#ifdef __cplusplus
}
#endif
#endif
