/* Tiny test helper: one header, no library. Each tests/<name>_test.c:
 *
 *   #include "t.h"
 *   static void test_foo(void) { T_EQ(1 + 1, 2); T_TRUE(p != NULL); }
 *   int main(void) { T_RUN(test_foo); return T_DONE(); }
 *
 * Checks keep going after a failure; T_DONE() prints a summary and returns
 * the process exit code (0 = all passed). Works natively and under qemu-arm
 * (newlib printf). */
#ifndef UA_T_H
#define UA_T_H

#include <stdio.h>
#include <string.h>

static int t_checks, t_fails;
static const char *t_name = "";

#define T_FAIL_(fmt, ...) \
    (t_fails++, printf("FAIL %s:%d %s: " fmt "\n", __FILE__, __LINE__, t_name, __VA_ARGS__))

#define T_TRUE(c) \
    do { t_checks++; if (!(c)) T_FAIL_("%s", #c); } while (0)

#define T_EQ(a, b) \
    do { long t_a_ = (long) (a), t_b_ = (long) (b); t_checks++; \
         if (t_a_ != t_b_) T_FAIL_("%s == %s: %ld != %ld (0x%lx != 0x%lx)", #a, #b, \
                                   t_a_, t_b_, (unsigned long) t_a_, (unsigned long) t_b_); \
    } while (0)

#define T_STR(a, b) \
    do { const char *t_a_ = (a), *t_b_ = (b); t_checks++; \
         if (!t_a_ || !t_b_ || strcmp(t_a_, t_b_) != 0) \
             T_FAIL_("%s: \"%s\" != \"%s\"", #a, t_a_ ? t_a_ : "(null)", t_b_ ? t_b_ : "(null)"); \
    } while (0)

#define T_MEM(a, b, n) \
    do { t_checks++; if (memcmp((a), (b), (n)) != 0) T_FAIL_("%s == %s (%u bytes)", #a, #b, (unsigned) (n)); } while (0)

#define T_RUN(fn) do { t_name = #fn; fn(); } while (0)

#define T_DONE() \
    (printf("%s: %d checks, %d failed\n", __FILE__, t_checks, t_fails), t_fails ? 1 : 0)

#endif
