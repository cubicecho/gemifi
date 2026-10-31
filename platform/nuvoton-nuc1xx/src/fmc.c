/*
 * fmc.c: ISP read / page erase / program / UID read.
 *
 * TRM 6.8 "ISP procedure": set ISPCMD, ISPADR (and ISPDAT), write ISPGO,
 * ISB, wait for ISPGO = 0 (the CPU is stalled meanwhile anyway), then check
 * and clear ISPCON.ISPFF.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fmc.h"
#include "nuc1xx.h"
#include "sys.h"

void fmc_open(void)
{
    sys_unlock();
    /* ISPEN only. LDUEN/CFGUEN are explicitly cleared, never set. */
    FMC_ISPCON = (FMC_ISPCON & ~(ISPCON_FORBIDDEN | ISPCON_ISPFF)) | ISPCON_ISPEN;
}

static int fmc_go(uint32_t cmd, uint32_t addr)
{
    FMC_ISPCMD = cmd;
    FMC_ISPADR = addr;
    FMC_ISPTRG = ISPTRG_ISPGO;
    __asm__ volatile ("isb" ::: "memory");
    while (FMC_ISPTRG & ISPTRG_ISPGO) {
    }
    uint32_t c = FMC_ISPCON;
    if (c & ISPCON_ISPFF) {
        /* write-1-to-clear; keep LDUEN/CFGUEN at 0 while doing so */
        FMC_ISPCON = (c & ~ISPCON_FORBIDDEN) | ISPCON_ISPFF;
        return -1;
    }
    return 0;
}

uint32_t fmc_read(uint32_t addr)
{
    fmc_go(ISPCMD_READ, addr);
    return FMC_ISPDAT;
}

int fmc_erase_page(uint32_t addr)
{
    return fmc_go(ISPCMD_PAGE_ERASE, addr);
}

int fmc_program(uint32_t addr, uint32_t word)
{
    FMC_ISPDAT = word;
    return fmc_go(ISPCMD_PROGRAM, addr);
}

int fmc_read_uid(uint32_t uid[3])
{
    int rc = 0;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t w = 0;
        if (fmc_go(ISPCMD_READ_UID, i * 4u) == 0)
            w = FMC_ISPDAT;
        else
            rc = -1;
        uid[i] = w;
    }
    return rc;
}
