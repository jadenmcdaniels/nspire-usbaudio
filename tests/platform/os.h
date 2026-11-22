/* Only the Ndless surface used by main.c; never on a calculator include path. */
#ifndef UA_TEST_OS_H
#define UA_TEST_OS_H
#include <stdint.h>
extern int is_cx2;
#define nl_hassyscall(fn) 1
const char *get_documents_dir(void);
unsigned hwtype(void);
unsigned nl_hwsubtype(void);
unsigned nl_ndless_rev(void);
enum { KEY_NSPIRE_ESC, KEY_NSPIRE_ENTER, KEY_NSPIRE_P, KEY_NSPIRE_TAB,
       KEY_NSPIRE_PLUS, KEY_NSPIRE_UP, KEY_NSPIRE_MINUS, KEY_NSPIRE_DOWN,
       KEY_NSPIRE_RIGHT, KEY_NSPIRE_LEFT, KEY_NSPIRE_R, KEY_NSPIRE_DEL };
enum { SCR_TYPE_INVALID, SCR_320x240_565, SCR_240x320_565 };
int lcd_type(void);
void lcd_init(int mode);
void lcd_blit(const void *pixels, int mode);
int isKeyPressed(int key);
int any_key_pressed(void);
#endif
