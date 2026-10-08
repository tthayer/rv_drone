/* Host test: UI presets round trip (save, reset, load, boot LAST) through an in-memory store. */
#include <stdio.h>
#include <string.h>
#include "engine.h"
#include "ui.h"
static char files[4][2][1100]; static const char *names[4]; static int lens[4]; static int nf;
static int rd(const char *n, char *b, int m){ for(int i=0;i<nf;i++) if(!strcmp(names[i],n)){ int l=lens[i]<m?lens[i]:m; memcpy(b,files[i][0],l); return l;} return -2; }
static int wr(const char *n, const char *b, int l){ int i=0; for(;i<nf;i++) if(!strcmp(names[i],n)) break; if(i==nf){names[nf]=strdup(n); nf++;} memcpy(files[i][0],b,l); lens[i]=l; return 0; }
static const ui_store_t st={rd,wr};
int main(void){ engine_init(48000); ui_init(); ui_set_store(&st);
 int fails=0; ui_enc(0, 4*7); ui_sw(0,1); ui_sw(0,0); ui_enc(0, -4*20); /* page FILTER: cutoff down */
 float a=engine_param(P_DETUNE), b=engine_param(P_CUTOFF);
 if(ui_preset_save(3)) fails++;
 engine_init(48000); ui_init(); if (engine_param(P_CUTOFF)==b) fails++;
 if(ui_preset_load(3)) fails++;
 float a2=engine_param(P_DETUNE), b2=engine_param(P_CUTOFF);
 printf("detune %.3f -> %.3f, cutoff %.2f -> %.2f\n",a,a2,b,b2);
 if (a2<a*0.999f||a2>a*1.001f||b2<b*0.99f||b2>b*1.01f) fails++;
 if(ui_preset_load(4)!=-2) fails++;
 engine_init(48000); ui_init(); ui_boot_preset(); if (engine_param(P_CUTOFF)<b*0.99f||engine_param(P_CUTOFF)>b*1.01f) fails++;
 /* v1 file (no "v2" header): OSCS stored as a position on the old 3..7 range */
 { const char *v1 = "# rv_drone preset (NAME=position 0..10000)\nOSCS=5000\n";
   wr("P09.TXT", v1, (int)strlen(v1));
   engine_init(48000); ui_init();
   if (ui_preset_load(9) || engine_param(P_OSCS) != 5.0f) { printf("v1 OSCS migration: got %.1f\n", engine_param(P_OSCS)); fails++; } }
 printf("%s\n%.*s", fails?"FAILED":"preset round trip ok (+ v1 migration)", 120, files[0][0]); return fails; }
