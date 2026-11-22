/* Render the real calculator renderer, not an illustrative mockup. */
#include <stdio.h>
#include <stdlib.h>
#include "screen.h"
int main(int argc, char **argv)
{
    static uint16_t pixels[SCREEN_W*SCREEN_H];
    struct music_view v = {0};
    unsigned i;
    FILE *out;
    if (argc != 2) return 2;
    v.title = "Dreams and Nightmares - Meek Mill";
    v.status = "Audio device detected";
    v.detail = "Waiting for the calculator's USB host";
    v.count = 1; v.volume=50; v.duration=230;
    if (getenv("PLAYING")) {
        v.status="Playing"; v.detail="R repeat    Del restart    Esc exit";
        v.playing=v.connected=1; v.elapsed=83;
    }
    screen_render(pixels,&v);
    out=fopen(argv[1],"wb");
    if (!out) return 1;
    fprintf(out,"P6\n%d %d\n255\n",SCREEN_W,SCREEN_H);
    for (i=0;i<SCREEN_W*SCREEN_H;i++) {
        unsigned p=pixels[i];
        unsigned char rgb[3]={(unsigned char)((p>>11)*255/31),
            (unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)};
        if (fwrite(rgb,1,3,out)!=3) { fclose(out); return 1; }
    }
    return fclose(out)!=0;
}
