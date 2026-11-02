/*
 * fmc.h: flash memory controller ISP helpers (TRM section 6).
 *
 * None of these functions check addresses: callers do (see the bootloader's
 * target range check). They never set ISPCON.LDUEN or ISPCON.CFGUEN, so the
 * hardware itself refuses LDROM and CONFIG writes (ISPFF).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_FMC_H
#define GEMIFI_FMC_H

#include <stdint.h>

/* Enable ISP (ISPCON.ISPEN). Unlocks REGWRPROT. */
void fmc_open(void);

/* Each returns 0 on success, -1 if ISPFF was set (the flag is cleared). */
uint32_t fmc_read(uint32_t addr);
int fmc_erase_page(uint32_t addr);
int fmc_program(uint32_t addr, uint32_t word);

/* Read the 96-bit chip unique ID as three words. A word whose read is
 * refused is 0, and then it returns -1 (the NUC120BN refuses all three). */
int fmc_read_uid(uint32_t uid[3]);

#endif
