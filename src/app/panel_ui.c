#include "panel_ui.h"
#include "fb.h"
#include "panel_link.h"
#include "rvpanel.h"
#include "ui.h"

#define FRAME_MS     33          /* ~30 Hz redraw */
#define REFRESH_MS   1000        /* resend every page: lost packets, Pico B reboot */

static fb_t drawn;
static uint8_t sent[UI_DISPLAYS][FB_PAGES][FB_W];
static uint8_t sent_valid[UI_DISPLAYS];   /* bit p: sent[d][p] reached the TX ring */
static uint64_t next_frame, next_refresh;

static int page_equal(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < FB_W; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

void panel_ui_init(void)
{
    next_frame = 0;
    next_refresh = 0;
}

void panel_ui_service(uint64_t now)
{
    panel_event_t e;
    while (panel_link_next_event(&e)) {
        switch (e.type) {
        case RVPANEL_ENC: ui_enc(e.a, (int8_t)e.b); break;
        case RVPANEL_SW:  ui_sw(e.a, e.b); break;
        case RVPANEL_CLOCK: ui_clock(e.a, e.t); break;   /* RVPANEL_CLK_* == ENGINE_CLK_* */
        case RVPANEL_MIDI: {
            uint8_t m[3] = { e.b, e.c, e.d };
            ui_midi(m, e.a, now);
            break;
        }
        }
    }
    if (now < next_frame)
        return;
    next_frame = now + FRAME_MS;
    if (now >= next_refresh) {
        next_refresh = now + REFRESH_MS;
        for (unsigned d = 0; d < UI_DISPLAYS; d++)
            sent_valid[d] = 0;
    }
    for (unsigned d = 0; d < UI_DISPLAYS; d++) {
        ui_draw((int)d, &drawn, now);
        for (unsigned p = 0; p < FB_PAGES; p++) {
            if ((sent_valid[d] >> p & 1) && page_equal(sent[d][p], drawn.buf[p]))
                continue;
            if (panel_link_tx_free() < RVPANEL_MAX_WIRE)
                return;                    /* ring full: rest goes next frame */
            uint8_t pkt[2 + FB_W];
            pkt[0] = (uint8_t)d;
            pkt[1] = (uint8_t)p;
            for (unsigned i = 0; i < FB_W; i++)
                pkt[2 + i] = sent[d][p][i] = drawn.buf[p][i];
            if (panel_link_send(RVPANEL_PAGE, pkt, sizeof pkt) == 0)
                sent_valid[d] |= (uint8_t)(1u << p);
        }
    }
}
