#include <libultraship.h>
#include "HudFullView.h"
#include "SfNet.h"

/*
 * Online Versus views (SfNet_FullView): the game draws its Versus HUD with quarter-screen coordinates (player 1
 * top-left, 2 top-right, 3/4 below), but this device shows only its own pilots, each in its view rectangle
 * (SfNet_ViewRect). This rewrites the HUD's 2D rectangles in the display list just emitted: moved from the
 * player's quarter into its view and scaled to it (one pilot: the full screen, positions x2 and texture steps /2;
 * 2-4 pilots: quarter-size views, moved only). 3D-projected HUD parts (radar marks) aren't touched.
 */

#define QUARTER_W 160 /* SCREEN_WIDTH / 2 */
#define QUARTER_H 120 /* SCREEN_HEIGHT / 2 */

static int32_t Sext24(uint32_t v) {
    v &= 0xFFFFFF;
    return (v & 0x800000) ? (int32_t) (v | 0xFF000000) : (int32_t) v;
}

static int sScale = 2; /* the view's size in quarters: 2 = full screen, 1 = a quarter-size view */

static uint32_t HalveSteps(uint32_t w) {
    /* dsdx (high 16) and dtdy (low 16), s5.10 */
    int16_t dsdx = (int16_t) (w >> 16), dtdy = (int16_t) (w & 0xFFFF);
    return ((uint32_t) (uint16_t) (dsdx / sScale) << 16) | (uint16_t) (dtdy / sScale);
}

void HudFullView_Transform(Gfx* start, Gfx* end, int player) {
    /* coordinates in quarter pixels (10.2) */
    const int32_t qx = (player % 2) * QUARTER_W * 4, qy = (player / 2) * QUARTER_H * 4;
    const int view = SfNet_ViewOf(player);
    float fx, fy, fw, fh;
    Gfx* g;
    if (view < 0) {
        return;
    }
    SfNet_ViewRect(view, &fx, &fy, &fw, &fh);
    sScale = fw >= 2 * QUARTER_W ? 2 : 1;
    const int32_t vx = (int32_t) fx * 4, vy = (int32_t) fy * 4;
#define TX(x) (sScale * ((x) - qx) + vx)
#define TY(y) (sScale * ((y) - qy) + vy)
    for (g = start; g < end; g++) {
        uint32_t op = g->words.w0 >> 24;
        if (op == G_TEXRECT_WIDE && g + 2 < end) {
            int32_t xh = Sext24(g[0].words.w0), yh = Sext24(g[0].words.w1);
            int32_t xl = Sext24(g[1].words.w0), yl = Sext24(g[1].words.w1);
            g[0].words.w0 = (G_TEXRECT_WIDE << 24) | ((uint32_t) TX(xh) & 0xFFFFFF);
            g[0].words.w1 = (uint32_t) TY(yh) & 0xFFFFFF;
            g[1].words.w0 = (g[1].words.w0 & 0xFF000000) | ((uint32_t) TX(xl) & 0xFFFFFF);
            g[1].words.w1 = (uint32_t) TY(yl) & 0xFFFFFF;
            g[2].words.w1 = HalveSteps(g[2].words.w1);
            g += 2;
        } else if ((op == G_TEXRECT || op == G_TEXRECTFLIP) && g + 2 < end) {
            int32_t xh = (g[0].words.w0 >> 12) & 0xFFF, yh = g[0].words.w0 & 0xFFF;
            int32_t xl = (g[0].words.w1 >> 12) & 0xFFF, yl = g[0].words.w1 & 0xFFF;
            int32_t nxh = TX(xh), nyh = TY(yh), nxl = TX(xl), nyl = TY(yl);
            if (nxl < 0 || nyl < 0 || nxh > 0xFFF || nyh > 0xFFF) {
                g += 2; /* doesn't fit the 12-bit form: leave it */
                continue;
            }
            g[0].words.w0 = (op << 24) | ((uint32_t) nxh << 12) | (uint32_t) nyh;
            g[0].words.w1 = (g[0].words.w1 & 0xFF000000) | ((uint32_t) nxl << 12) | (uint32_t) nyl;
            if ((g[2].words.w0 >> 24) == (G_RDPHALF_2 & 0xFF)) {
                g[2].words.w1 = HalveSteps(g[2].words.w1);
            }
            g += 2;
        } else if (op == G_FILLRECT) {
            int32_t lrx = (g->words.w0 >> 14) & 0x3FF, lry = (g->words.w0 >> 2) & 0x3FF;
            int32_t ulx = (g->words.w1 >> 14) & 0x3FF, uly = (g->words.w1 >> 2) & 0x3FF;
            lrx = TX(lrx * 4) / 4; lry = TY(lry * 4) / 4; ulx = TX(ulx * 4) / 4; uly = TY(uly * 4) / 4;
            if (ulx >= 0 && uly >= 0 && lrx < 1024 && lry < 1024) {
                g->words.w0 = (G_FILLRECT << 24) | ((uint32_t) lrx << 14) | ((uint32_t) lry << 2);
                g->words.w1 = ((uint32_t) ulx << 14) | ((uint32_t) uly << 2);
            }
        }
    }
#undef TX
#undef TY
}
