/* Host test stand-in for flash.h: the whole 128 KB of simulated F1 flash
 * from 0x08000000 (boot_sim.c). */
#ifndef FLASH_H
#define FLASH_H
#include <stdint.h>

#define SIM_FLASH_BASE 0x08000000u
#define SIM_FLASH_SIZE 0x20000u
extern uint8_t sim_flash[SIM_FLASH_SIZE];

void flash_unlock(void);
void flash_lock(void);
int flash_erase_page(uint32_t addr);
int flash_program_half(uint32_t addr, uint16_t v);
int flash_program_word(uint32_t addr, uint32_t w);
uint32_t flash_size_kb(void);

static inline uint32_t flash_read(uint32_t addr)
{
    const uint8_t *p = sim_flash + (addr - SIM_FLASH_BASE);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
#endif
