// Desktop check of the renderer and layout: writes every panel of a few wall states as PPM.
//   make && ./dust_host out/
#include "dust_render.h"
#include "dust_layout.h"
#include "dust_warp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *alloc(size_t n) { return calloc(1, n); }

static void write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", DUST_PANEL_PX, DUST_PANEL_PX);
    for (int i = 0; i < DUST_PANEL_PX * DUST_PANEL_PX; i++) {
        uint16_t c = fb[i];
        unsigned char rgb[3] = { (unsigned char)((c >> 8) & 0xF8), (unsigned char)((c >> 3) & 0xFC), (unsigned char)((c << 3) & 0xF8) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

typedef struct {
    const char *name; float t, talk; uint16_t lifted, missing;
    int form; const char *text; dust_fx_t fx;
} scene_t;

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    if (!dust_render_init(alloc)) { fprintf(stderr, "alloc failed\n"); return 1; }
    uint16_t *fb = malloc(DUST_PANEL_PX * DUST_PANEL_PX * 2);
    const float gap = 40;
    const scene_t scenes[] = {
        { "full",    6.0f, 0, 0, 0, 0, NULL, {0} },
        { "corner",  6.0f, 0, 0, (1u << 2) | (1u << 5), 0, NULL, {0} },
        { "sq2x2",   6.0f, 0, 0, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 8), 0, NULL, {0} },
        { "lift56",  6.0f, 0, (1u << 4) | (1u << 5), 0, 0, NULL, {0} },
        { "centre",  6.0f, 0, 1u << 5, 1u << 4, 0, NULL, {0} },
        { "column",  6.0f, 0, 0, (1u << 0) | (1u << 3) | (1u << 6) | (1u << 1) | (1u << 7), 0, NULL, {0} },
        { "ring",    7.0f, 0, 0, 1u << 8, DUST_FORM_RING, NULL, {0} },
        { "text",    7.0f, 0, 0, 0, DUST_FORM_TEXT, "HI JOSH", {0} },
        { "swirl",   7.0f, 0, 0, 0, 0, NULL, { DUST_FX_SWIRL, 1.0f, 0.28f, 2.2f, 0.5f, 0.32f, 0, 0, 0 } },
        { "gather",  7.0f, 0, 0, 0, 0, NULL, { DUST_FX_GATHER, 0.9f, 0.3f, 0, 0.2f, 0.75f, 0, 0, 0 } },
    };
    double worst = 0;
    for (size_t s = 0; s < sizeof scenes / sizeof scenes[0]; s++) {
        const scene_t *sc = &scenes[s];
        uint16_t gone = sc->lifted | sc->missing;
        dust_frame_t fr = dust_fit((uint16_t)(0x1FF & ~gone), gap);
        printf("%-7s frame c=(%.2f,%.2f) s=(%.2f,%.2f)\n", sc->name, fr.cx, fr.cy, fr.sx, fr.sy);
        for (int slot = 0; slot < 9; slot++) {
            dust_view_t v = { .t = sc->t, .talk = sc->talk, .assemble = 1.2f,
                              .frame_cx = fr.cx, .frame_cy = fr.cy, .frame_sx = fr.sx, .frame_sy = fr.sy,
                              .slot = slot, .gap_px = gap };
            if (gone) v.warp = dust_warp_field((uint16_t)(0x1FF & ~gone), gap);
            for (int g = 0; g < 9; g++) { v.take[g] = (gone >> g & 1) ? 1 : 0; v.pres[g] = (gone >> g & 1) ? 0 : 1; }
            if (sc->form) {
                v.form_a.id = v.form_b.id = (uint8_t)sc->form;
                if (sc->text) { strncpy(v.form_a.text, sc->text, DUST_TEXT_MAX); strncpy(v.form_b.text, sc->text, DUST_TEXT_MAX); }
                v.form_amt = 1; v.form_swap = 1.2f;
            }
            v.fx[0] = sc->fx;
            char path[512];
            snprintf(path, sizeof path, "%s/%s_%d.ppm", out, sc->name, slot);
            if (sc->missing >> slot & 1) { remove(path); continue; }
            if (sc->lifted >> slot & 1) { v.slot = -1; v.home_slot = slot; v.ball_in = 1.2f; }
            clock_t c0 = clock();
            dust_render(&v, fb, false);
            double ms = (clock() - c0) * 1000.0 / CLOCKS_PER_SEC; if (ms > worst) worst = ms;
            write_ppm(path, fb);
        }
    }
    printf("worst frame on this machine: %.1f ms\n", worst);
    free(fb);
    return 0;
}
