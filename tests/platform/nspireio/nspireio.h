#ifndef UA_TEST_NIO_H
#define UA_TEST_NIO_H
typedef struct { int unused; } nio_console;
enum { NIO_MAX_COLS = 40, NIO_MAX_ROWS = 30, NIO_COLOR_BLACK = 0,
       NIO_COLOR_WHITE = 1, TRUE = 1 };
int nio_init(nio_console *, int, int, int, int, int, int, int);
void nio_set_default(nio_console *);
void nio_fputs(const char *, nio_console *);
void nio_fputc(char, nio_console *);
void nio_free(nio_console *);
#endif
