// Host test for debounce.h: cc -I.. test_debounce.c
#include <stdio.h>

#include "debounce.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    debounce_t d = {0};

    // A press must last DEBOUNCE_SAMPLES samples; the change is reported once, on the last one.
    for (int i = 0; i < DEBOUNCE_SAMPLES - 1; i++) CHECK(!debounce_step(&d, true));
    CHECK(!d.state);
    CHECK(debounce_step(&d, true) && d.state);
    CHECK(!debounce_step(&d, true));

    // Bounce: short glitches of the opposite level never change the state.
    for (int k = 0; k < 20; k++) {
        for (int i = 0; i < DEBOUNCE_SAMPLES - 1; i++) CHECK(!debounce_step(&d, false));
        CHECK(!debounce_step(&d, true));
        CHECK(d.state);
    }

    // Release after a long, stable low.
    for (int i = 0; i < DEBOUNCE_SAMPLES - 1; i++) CHECK(!debounce_step(&d, false));
    CHECK(debounce_step(&d, false) && !d.state);

    // Noisy press: bouncing 1,0,1,1,0,1,1,1,1,1,1 settles only after 5 straight highs.
    debounce_t e = {0};
    const int noisy[] = {1,0,1,1,0,1,1,1,1,1,1};
    int changed_at = -1;
    for (int i = 0; i < (int)(sizeof noisy / sizeof noisy[0]); i++)
        if (debounce_step(&e, noisy[i])) changed_at = i;
    CHECK(changed_at == 9 && e.state);

    // Bank: independent channels, mask of changes.
    debounce_bank_t b = {0};
    uint8_t ch = 0;
    for (int i = 0; i < DEBOUNCE_SAMPLES; i++) ch = debounce_bank_step(&b, 6, 0x05);   // ch 0 and 2
    CHECK(ch == 0x05 && b.ch[0].state && !b.ch[1].state && b.ch[2].state);
    for (int i = 0; i < DEBOUNCE_SAMPLES; i++) ch = debounce_bank_step(&b, 6, 0x04);   // ch 0 releases
    CHECK(ch == 0x01 && !b.ch[0].state && b.ch[2].state);
    // Idle (nothing connected, pull-ups hold it high = not pressed): no events ever.
    debounce_bank_t idle = {0};
    int events = 0;
    for (int i = 0; i < 100000; i++) events += debounce_bank_step(&idle, 6, 0) != 0;
    CHECK(events == 0);

    printf("debounce: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
