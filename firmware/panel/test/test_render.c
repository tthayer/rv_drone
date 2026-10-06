// Host render check: draws the three displays from the same render.c into
// PBM files (one per display, 128x64, ASCII "P1") for eyeballing, and sanity
// checks dirty-page tracking. cc -I.. test_render.c ../render.c ../fb.c ../font5x7.c
// Usage: test_render [outdir]   (default: current directory)
#include <stdio.h>
#include <string.h>

#include "render.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void write_pbm(const fb_t *fb, const char *dir, const char *name) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s.pbm", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); fails++; return; }
    fprintf(f, "P1\n%d %d\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) fputc(((fb->buf[y >> 3][x] >> (y & 7)) & 1) ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    panel_state_t s;
    memset(&s, 0, sizeof s);
    s.enc_total[0] = 1234;  s.enc_delta[0] = 4;
    s.enc_total[1] = -57;   s.enc_delta[1] = -1;
    s.enc_total[2] = 99999; s.enc_delta[2] = 12;   // too wide for the large font
    s.enc_total[3] = 0;
    s.enc_total[4] = -2000000; s.enc_total[5] = 30;
    s.sw_down[1] = true;                            // right half of OLED 0 inverted
    s.midi_last_ms = 1000;
    snprintf(s.midi_text, sizeof s.midi_text, "N60");

    fb_t scratch, shown;
    memset(&shown, 0, sizeof shown);
    char name[16];
    for (unsigned d = 0; d < N_OLED; d++) {
        render_display(&scratch, d, &s, 1050);
        fb_update(&shown, &scratch);
        snprintf(name, sizeof name, "oled%u", d);
        write_pbm(&shown, dir, name);
    }
    render_test_pattern(&scratch, 1, 0x3D);
    write_pbm(&scratch, dir, "testpattern");

    // Dirty tracking: identical re-render dirties nothing; one counter change
    // dirties only the pages it touches (not the header page).
    memset(&shown, 0, sizeof shown);
    render_display(&scratch, 0, &s, 1050);
    fb_update(&shown, &scratch);
    CHECK(shown.dirty != 0);
    shown.dirty = 0;
    render_display(&scratch, 0, &s, 1050);
    fb_update(&shown, &scratch);
    CHECK(shown.dirty == 0);
    s.enc_total[0] = 1235;
    render_display(&scratch, 0, &s, 1050);
    fb_update(&shown, &scratch);
    CHECK(shown.dirty != 0 && !(shown.dirty & 1u));
    // MIDI box timing
    CHECK(render_midi_lit(&s, 1050));
    CHECK(!render_midi_lit(&s, 1000 + MIDI_FLASH_MS));

    printf("render: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
