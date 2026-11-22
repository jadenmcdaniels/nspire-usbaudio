/* Writes the test WAV: the built-in "Twinkle, Twinkle" tune (public domain,
 * synthesised by src/melody.c), 22.05 kHz mono 16-bit, the format
 * tools/make_wav.sh makes by default. Usage: mkwav out.wav.tns */
#include <stdio.h>
#include <stdlib.h>
#include "melody.h"
#include "wavbuild.h"

int main(int argc, char **argv)
{
    static int16_t lr[2 * 400000], mono[400000];
    struct melody m;
    unsigned n, i, len;
    uint8_t *out;
    FILE *f;
    if (argc != 2) {
        fprintf(stderr, "usage: %s out.wav.tns\n", argv[0]);
        return 2;
    }
    melody_init(&m, SONG_TWINKLE, 22050);
    n = melody_render(&m, lr, 400000);
    for (i = 0; i < n; i++)
        mono[i] = lr[2 * i];
    out = malloc(44 + 2 * n);
    len = wav_build(out, mono, n, 1, 16, 22050);
    f = fopen(argv[1], "wb");
    if (!f || fwrite(out, 1, len, f) != len) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    free(out);
    return 0;
}
