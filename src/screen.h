#ifndef UA_SCREEN_H
#define UA_SCREEN_H
#include <stdint.h>
#include "music.h"
#define SCREEN_W 320
#define SCREEN_H 240
/* Pure RGB565 rendering; hardware presentation stays in main.c. */
void screen_render(uint16_t *pixels, const struct music_view *view);
#endif
