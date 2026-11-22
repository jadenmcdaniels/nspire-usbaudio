#ifndef UA_LIBRARY_H
#define UA_LIBRARY_H
#include "music.h"
/* Up to 64 WAVs from My Documents/music and the existing usbaudio folder. */
unsigned library_scan(const char *documents);
const char *library_title(unsigned index);
int library_open(unsigned index, struct music_track *track);
void library_close(void);
#endif
