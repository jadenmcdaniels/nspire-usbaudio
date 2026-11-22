#ifndef UA_PLATFORM_FAKE_H
#define UA_PLATFORM_FAKE_H
#include <stdint.h>
uint32_t ua_test_read(uint32_t at);
void ua_test_write(uint32_t at, uint32_t value);
uint32_t ua_test_irq_unmask(void);
void ua_test_irq_restore(uint32_t value);
void (*ua_test_bind_sleep(uint32_t at))(unsigned);
#endif
