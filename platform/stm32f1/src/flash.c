/*
 * flash.c: main-flash unlock / page erase / halfword program (RM0008 3.3.3,
 * PM0075 2.3). Executing from flash meanwhile is fine: the CPU stalls on
 * the next flash fetch until the operation is done.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash.h"
#include "stm32f1.h"
#include "board.h"

void flash_unlock(void)
{
    /* PM0075 2.3.1: the HSI must be on for program/erase. */
    RCC_CR |= RCC_CR_HSION;
    while (!(RCC_CR & RCC_CR_HSIRDY)) {
    }
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

void flash_lock(void)
{
    FLASH_CR |= FLASH_CR_LOCK;
}

/* Wait for BSY = 0 (bounded: a page erase takes ~20-40 ms), then check and
 * clear the error flags. */
static int flash_wait(void)
{
    uint32_t i = 0;
    while (FLASH_SR & FLASH_SR_BSY) {
        IWDG_KR = IWDG_KEY_RELOAD;
        if (++i > 4000000u)
            return -1;
    }
    uint32_t sr = FLASH_SR;
    FLASH_SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;   /* w1c */
    return (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) ? -1 : 0;
}

int flash_erase_page(uint32_t addr)
{
    if (flash_wait() != 0 && (FLASH_SR & FLASH_SR_BSY))
        return -1;
    FLASH_CR = FLASH_CR_PER;
    FLASH_AR = addr;
    FLASH_CR = FLASH_CR_PER | FLASH_CR_STRT;
    __asm__ volatile ("dsb" ::: "memory");
    int rc = flash_wait();
    FLASH_CR = 0;
    /* Verify blank (catches a clone that silently ignores the erase). */
    if (rc == 0) {
        uint32_t page = addr & ~(uint32_t)(BOARD_FLASH_PAGE - 1u);
        for (uint32_t i = 0; i < BOARD_FLASH_PAGE; i += 4u)
            if (flash_read(page + i) != 0xFFFFFFFFu)
                return -1;
    }
    return rc;
}

int flash_program_half(uint32_t addr, uint16_t v)
{
    if (flash_wait() != 0 && (FLASH_SR & FLASH_SR_BSY))
        return -1;
    FLASH_CR = FLASH_CR_PG;
    *(volatile uint16_t *)addr = v;
    __asm__ volatile ("dsb" ::: "memory");
    int rc = flash_wait();
    FLASH_CR = 0;
    if (rc == 0 && *(const volatile uint16_t *)addr != v)
        rc = -1;
    return rc;
}

int flash_program_word(uint32_t addr, uint32_t w)
{
    if (flash_program_half(addr, (uint16_t)w) != 0)
        return -1;
    return flash_program_half(addr + 2u, (uint16_t)(w >> 16));
}

uint32_t flash_size_kb(void)
{
    return FLASH_SIZE_REG;
}
