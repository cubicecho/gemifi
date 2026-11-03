/*
 * flash.h: STM32F1 main-flash erase / program (RM0008 3.3.3, PM0075).
 *
 * Flash reads are plain memory reads. Erase and program stall the CPU when
 * it fetches from flash meanwhile (no RAM code needed). Pages are
 * BOARD_FLASH_PAGE bytes (1 KB on the Code 61's chip, see board header).
 *
 * None of these check addresses: callers do. Never point them at the
 * vendor bootloader (below 0x08002400).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_FLASH_H
#define GEMIFI_FLASH_H

#include <stdint.h>

void flash_unlock(void);
void flash_lock(void);

/* Each returns 0 on success, -1 on PGERR/WRPRTERR/timeout (flags cleared). */
int flash_erase_page(uint32_t addr);
int flash_program_half(uint32_t addr, uint16_t v);   /* addr even */
int flash_program_word(uint32_t addr, uint32_t w);   /* two halfwords, LE */

static inline uint32_t flash_read(uint32_t addr)
{
    return *(const volatile uint32_t *)addr;
}

/* Flash size register (KB), from system memory at 0x1FFFF7E0. */
uint32_t flash_size_kb(void);

#endif
