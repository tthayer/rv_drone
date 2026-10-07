// rv_drone host emulator: the Nano's engine + UI on macOS with SDL2.
//
//   rv_drone_emu                      window + audio
//     mouse wheel over a display half = that encoder, click = its switch
//     z s x d c v g b h n j m ,       = notes C..C (piano layout), up/down = octave
//     space = all notes off, esc = quit
//     presets: page 5 (enc 1 push to reach it); files in build/emu/presets
//   rv_drone_emu --wav out.wav [--seconds N] [--notes 38,45,...] [--set ID=VAL ...]
//     offline render to a 16-bit stereo WAV, then level stats and render speed.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL.h>

#include "engine.h"
#include "fb.h"
#include "params.h"
#include "ui.h"

#define SR      48000
#define SCALE   4
#define GAP     12

static SDL_AudioDeviceID dev;
static volatile double load_ema;

static void audio_cb(void *u, Uint8 *stream, int bytes) {
    (void)u;
    float *out = (float *)stream;
    int frames = bytes / (int)(2 * sizeof(float));
    Uint64 t0 = SDL_GetPerformanceCounter();
    float l[256], r[256];
    while (frames > 0) {
        int n = frames < 256 ? frames : 256;
        engine_render(l, r, n);
        for (int i = 0; i < n; i++) { out[2 * i] = l[i]; out[2 * i + 1] = r[i]; }
        out += 2 * n;
        frames -= n;
    }
    double dt = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
    double budget = (double)bytes / (2 * sizeof(float)) / SR;
    load_ema = 0.95 * load_ema + 0.05 * (dt / budget);
}

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4, riff = 36 + data, fmt = 16, rate = SR, brate = SR * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static int offline(int argc, char **argv) {
    const char *path = NULL, *notes = NULL;
    double secs = 20;
    engine_init(SR);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wav") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--notes") && i + 1 < argc) notes = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            char name[32]; float v;
            if (sscanf(argv[++i], "%31[^=]=%f", name, &v) == 2) {
                for (int p = 0; p < P_COUNT; p++)
                    if (!strcasecmp(param_desc(p)->name, name)) engine_set_param(p, v);
            }
        }
    }
    if (notes) {
        engine_all_off();
        engine_note_on(0, 0);                       // no-op, keeps held at 0
        char buf[256]; snprintf(buf, sizeof buf, "%s", notes);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) engine_note_on(atoi(t), 100);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) engine_note_off(atoi(t));
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    uint32_t total = (uint32_t)(secs * SR);
    wav_header(f, total);
    double peak[2] = {0}, sum2[2] = {0}, dc[2] = {0};
    long nonfinite = 0;
    Uint64 t0 = SDL_GetPerformanceCounter();
    double render_s = 0;
    float l[64], r[64];
    for (uint32_t done = 0; done < total; done += 64) {
        Uint64 a = SDL_GetPerformanceCounter();
        engine_render(l, r, 64);
        render_s += (double)(SDL_GetPerformanceCounter() - a) / (double)SDL_GetPerformanceFrequency();
        for (int i = 0; i < 64 && done + i < total; i++) {
            float s[2] = { l[i], r[i] };
            int16_t pcm[2];
            for (int c = 0; c < 2; c++) {
                if (!isfinite(s[c])) { nonfinite++; s[c] = 0; }
                double a2 = fabs(s[c]);
                if (a2 > peak[c]) peak[c] = a2;
                sum2[c] += s[c] * s[c];
                dc[c] += s[c];
                float x = s[c] > 1 ? 1 : s[c] < -1 ? -1 : s[c];
                pcm[c] = (int16_t)lrintf(x * 32767.0f);
            }
            fwrite(pcm, 2, 2, f);
        }
    }
    (void)t0;
    fclose(f);
    for (int c = 0; c < 2; c++)
        printf("%s: peak %.3f (%.1f dBFS)  rms %.4f (%.1f dBFS)  dc %+.5f\n", c ? "R" : "L",
               peak[c], 20 * log10(peak[c] + 1e-12), sqrt(sum2[c] / total),
               20 * log10(sqrt(sum2[c] / total) + 1e-12), dc[c] / total);
    printf("non-finite samples %ld; host render %.1f us/block of 64 (%.2f%% of real time)\n",
           nonfinite, render_s / (total / 64.0) * 1e6, 100 * render_s / secs);
    return nonfinite != 0;
}

// Preset store: plain files in build/emu/presets (the Nano uses /presets on SD).
#define EMU_PRESET_DIR "build/emu/presets"
static int emu_read(const char *name, char *buf, int max) {
    char p[256];
    snprintf(p, sizeof p, EMU_PRESET_DIR "/%s", name);
    FILE *f = fopen(p, "rb");
    if (!f) return -2;
    int n = (int)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}
static int emu_write(const char *name, const char *buf, int len) {
    char p[256];
    mkdir("build", 0755); mkdir("build/emu", 0755); mkdir(EMU_PRESET_DIR, 0755);
    snprintf(p, sizeof p, EMU_PRESET_DIR "/%s", name);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    int ok = fwrite(buf, 1, (size_t)len, f) == (size_t)len;
    return fclose(f) == 0 && ok ? 0 : -1;
}
static const ui_store_t emu_store = { emu_read, emu_write };

static const SDL_Keycode piano[] = { SDLK_z, SDLK_s, SDLK_x, SDLK_d, SDLK_c, SDLK_v, SDLK_g,
                                     SDLK_b, SDLK_h, SDLK_n, SDLK_j, SDLK_m, SDLK_COMMA };

static int half_at(int x, int y) {
    if (y < 0 || y >= 64 * SCALE) return -1;
    int d = x / (128 * SCALE + GAP);
    int xi = x - d * (128 * SCALE + GAP);
    if (d < 0 || d > 2 || xi >= 128 * SCALE) return -1;
    return 2 * d + (xi >= 64 * SCALE);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--wav")) {
            SDL_Init(0);
            return offline(argc, argv);
        }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    engine_init(SR);
    ui_init();
    ui_set_store(&emu_store);
    ui_boot_preset();

    SDL_AudioSpec want = { .freq = SR, .format = AUDIO_F32SYS, .channels = 2, .samples = 256,
                           .callback = audio_cb }, have;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return 1; }
    printf("audio: %d Hz, %d frames/callback\n", have.freq, have.samples);
    SDL_PauseAudioDevice(dev, 0);

    int W = 3 * 128 * SCALE + 2 * GAP, H = 64 * SCALE;
    SDL_Window *win = SDL_CreateWindow("rv_drone emu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       W, H, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    fb_t fb;
    int octave = 48, held_half = -1;
    for (int quit = 0; !quit;) {
        SDL_Event e;
        uint64_t now = SDL_GetTicks64();
        while (SDL_PollEvent(&e)) {
            SDL_LockAudioDevice(dev);
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
            case SDL_MOUSEWHEEL: {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                int h = half_at(mx, my);
                int d = e.wheel.y;
                if (h >= 0 && d) ui_enc(h, d * UI_COUNTS_PER_DETENT);   // one detent per wheel step
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                held_half = half_at(e.button.x, e.button.y);
                if (held_half >= 0) ui_sw(held_half, 1);
                break;
            case SDL_MOUSEBUTTONUP:
                if (held_half >= 0) ui_sw(held_half, 0);
                held_half = -1;
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                if (e.key.repeat) break;
                SDL_Keycode k = e.key.keysym.sym;
                int down = e.type == SDL_KEYDOWN;
                if (k == SDLK_ESCAPE) quit = 1;
                if (down && k == SDLK_UP) octave += 12;
                if (down && k == SDLK_DOWN) octave -= 12;
                if (down && k == SDLK_SPACE) { uint8_t m[3] = { 0xB0, 123, 0 }; ui_midi(m, 3, now); }
                for (int i = 0; i < 13; i++)
                    if (k == piano[i]) {
                        uint8_t m[3] = { (uint8_t)(down ? 0x90 : 0x80), (uint8_t)(octave + i), 100 };
                        ui_midi(m, 3, now);
                    }
                break;
            }
            }
            SDL_UnlockAudioDevice(dev);
        }
        SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
        SDL_RenderClear(ren);
        for (int d = 0; d < 3; d++) {
            SDL_LockAudioDevice(dev);
            ui_set_load((int)(load_ema * 100 + 0.5));
            ui_draw(d, &fb, now);
            SDL_UnlockAudioDevice(dev);
            int x0 = d * (128 * SCALE + GAP);
            SDL_SetRenderDrawColor(ren, 30, 30, 40, 255);
            SDL_Rect bg = { x0, 0, 128 * SCALE, 64 * SCALE };
            SDL_RenderFillRect(ren, &bg);
            SDL_SetRenderDrawColor(ren, 190, 225, 255, 255);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 128; x++)
                    if (fb.buf[y / 8][x] >> (y % 8) & 1) {
                        SDL_Rect px = { x0 + x * SCALE, y * SCALE, SCALE - 1, SCALE - 1 };
                        SDL_RenderFillRect(ren, &px);
                    }
        }
        SDL_RenderPresent(ren);
    }
    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
