/* Host test stand-in for flash.h: two pages of simulated F1 flash, with a
 * power cut after a set number of halfword programs (cs_test.c). */
#ifndef FLASH_H
#define FLASH_H
#include <stdint.h>
#include "board.h"

extern uint8_t sim_flash[BOARD_CONFIG_PAGES * BOARD_FLASH_PAGE];

void flash_unlock(void);
void flash_lock(void);
int flash_erase_page(uint32_t addr);
int flash_program_half(uint32_t addr, uint16_t v);
int flash_program_word(uint32_t addr, uint32_t w);

static inline uint32_t flash_read(uint32_t addr)
{
    const uint8_t *p = sim_flash + (addr - BOARD_CONFIG_BASE);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
#endif
