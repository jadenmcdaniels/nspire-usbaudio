#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "t.h"
#include "library.h"
#include "wavbuild.h"

static void write_file(const char *path, const void *data, unsigned len)
{
    FILE *f = fopen(path, "wb");
    T_TRUE(f != NULL);
    if (f) { T_EQ(fwrite(data, 1, len, f), len); fclose(f); }
}
static void test_library(void)
{
    char dir[] = "/tmp/music-library.XXXXXX", path[512];
    uint8_t wav[128], copy[128];
    int16_t pcm[] = {1, 2, 3, 4};
    unsigned n = wav_build(wav, pcm, 4, 1, 16, 22050);
    struct music_track track;
    struct wav_info info;
    const char *why;
    T_TRUE(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/music", dir); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/usbaudio", dir); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/music/Zebra.WAV.TNS", dir); write_file(path, wav, n);
    snprintf(path, sizeof path, "%s/music/Alpha.wav", dir); write_file(path, wav, n);
    snprintf(path, sizeof path, "%s/music/not-a-song.tns", dir); write_file(path, wav, n);
    snprintf(path, sizeof path, "%s/music/folder.wav.tns", dir); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/usbaudio/music.wav.tns", dir); write_file(path, wav, n);
    snprintf(path, sizeof path, "%s/usbaudio/music.title.txt.tns", dir);
    write_file(path, "Dreams and Nightmares\n", 21);
    T_EQ(library_scan(dir), 3);
    T_STR(library_title(0), "Alpha");
    T_STR(library_title(1), "Dreams and Nightmares");
    T_STR(library_title(2), "Zebra");
    T_TRUE(library_open(1, &track));
    T_EQ(track.read(track.ctx, 0, copy, n), n); T_MEM(copy, wav, n);
    T_EQ(wav_parse(track.read, track.ctx, track.size, &info, &why), 0);
    T_EQ(info.rate, 22050);
    T_TRUE(!library_open(3, &track));
    library_close();
    snprintf(path, sizeof path, "%s/music/Zebra.WAV.TNS", dir); unlink(path);
    snprintf(path, sizeof path, "%s/music/Alpha.wav", dir); unlink(path);
    snprintf(path, sizeof path, "%s/music/not-a-song.tns", dir); unlink(path);
    snprintf(path, sizeof path, "%s/music/folder.wav.tns", dir); rmdir(path);
    snprintf(path, sizeof path, "%s/usbaudio/music.wav.tns", dir); unlink(path);
    snprintf(path, sizeof path, "%s/usbaudio/music.title.txt.tns", dir); unlink(path);
    snprintf(path, sizeof path, "%s/music", dir); rmdir(path);
    snprintf(path, sizeof path, "%s/usbaudio", dir); rmdir(path);
    rmdir(dir);
    T_EQ(library_scan(dir), 0);
}
int main(void) { T_RUN(test_library); return T_DONE(); }
