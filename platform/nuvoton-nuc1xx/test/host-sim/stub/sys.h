/* host-test shadow of platform sys.h */
#ifndef GEMIFI_SYS_H
#define GEMIFI_SYS_H
#include <stdint.h>
#include "nuc1xx.h"
#define HCLK_HZ 48000000u
void sys_unlock(void);
void sys_clock_init(void);
void sys_tick_init(void);
uint32_t sys_tick_ms(void);
void sys_delay_ms(uint32_t ms);
typedef struct { uint32_t magic, magic_inv, boot_count, reason; } gemifi_mailbox_t;
extern volatile gemifi_mailbox_t sim_mailbox;
#define MAILBOX (&sim_mailbox)
#define MAILBOX_MAGIC_BOOT 0x544F4F42u
__attribute__((noreturn)) void sys_reset_to(int to_ldrom);
#endif
