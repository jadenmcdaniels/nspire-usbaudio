#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "library.h"

struct library_entry { char path[512], title[128]; };
static struct library_entry tracks[64];
static unsigned count;
static FILE *file;

void library_close(void) { if (file) fclose(file); file = NULL; }
static unsigned read_file(void *ctx, uint32_t off, void *buf, unsigned n)
{
    (void)ctx;
    if (!file || off > 0x7FFFFFFFu || fseek(file, (long)off, SEEK_SET)) return 0;
    return (unsigned)fread(buf, 1, n, file);
}
static void scan(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while (count < 64 && (e = readdir(d)) != NULL) {
        struct stat st;
        size_t n = strlen(e->d_name), title_len;
        int len;
        if (n > 8 && !strcasecmp(e->d_name + n - 8, ".wav.tns")) title_len = n - 8;
        else if (n > 4 && !strcasecmp(e->d_name + n - 4, ".wav")) title_len = n - 4;
        else continue;
        len = snprintf(tracks[count].path, sizeof tracks[count].path, "%s/%s", dir, e->d_name);
        if (len < 0 || (unsigned)len >= sizeof tracks[count].path) continue;
        if (stat(tracks[count].path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        if (title_len >= sizeof tracks[count].title) title_len = sizeof tracks[count].title - 1;
        memcpy(tracks[count].title, e->d_name, title_len);
        tracks[count].title[title_len] = 0;
        if (!strcasecmp(e->d_name, "music.wav.tns")) {
            char title_path[512];
            FILE *title;
            len = snprintf(title_path, sizeof title_path, "%s/music.title.txt.tns", dir);
            if (len > 0 && (unsigned)len < sizeof title_path && (title = fopen(title_path, "r"))) {
                char name[128];
                if (fgets(name, sizeof name, title)) {
                    name[strcspn(name, "\r\n")] = 0;
                    if (name[0]) strcpy(tracks[count].title, name);
                }
                fclose(title);
            }
        }
        count++;
    }
    closedir(d);
}
static int compare(const void *a, const void *b)
{
    const struct library_entry *aa = a, *bb = b;
    int order = strcasecmp(aa->title, bb->title);
    return order ? order : strcmp(aa->path, bb->path);
}
unsigned library_scan(const char *documents)
{
    char dir[512];
    unsigned i;
    static const char *const folders[] = {"music", "usbaudio"};
    library_close(); count = 0;
    for (i = 0; i < 2; i++) {
        int n = snprintf(dir, sizeof dir, "%s%s%s", documents,
                         documents[0] && documents[strlen(documents)-1] == '/' ? "" : "/", folders[i]);
        if (n > 0 && (unsigned)n < sizeof dir) scan(dir);
    }
    qsort(tracks, count, sizeof tracks[0], compare);
    return count;
}
const char *library_title(unsigned i) { return i < count ? tracks[i].title : ""; }
int library_open(unsigned i, struct music_track *t)
{
    long size;
    library_close();
    if (i >= count || !(file = fopen(tracks[i].path, "rb"))) return 0;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0) { library_close(); return 0; }
    t->read = read_file; t->ctx = NULL; t->size = (uint32_t)size;
    return 1;
}
