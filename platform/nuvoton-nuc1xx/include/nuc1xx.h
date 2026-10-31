/*
 * nuc1xx.h: minimal register definitions for the Nuvoton NUC100/NUC120
 * (Low Density, e.g. NUC120LC1BN), written by hand for gemifi.
 *
 * Every address and bit below was checked against the NuMicro NUC100/NUC120
 * Technical Reference Manual V2.02 (section numbers in comments). We use our
 * own small header instead of the vendor NUC100Series.h so the bootloader has
 * no CMSIS dependency and every register it touches is visible in one file.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_NUC1XX_H
#define GEMIFI_NUC1XX_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* Registers are reached through a per-block base pointer and a struct
 * offset, not as absolute addresses: GCC then loads each base once and uses
 * immediate offsets, which saves a literal-pool word per register access
 * (it matters for the 4 KB bootloader). RB(block, offset) is that access. */
typedef struct { volatile uint32_t w[256]; } nuc_regblock_t;
#define RB(base, off) (((nuc_regblock_t *)(base))->w[(off) / 4u])

/* ---- Memory map ------------------------------------------------------- */
#define FLASH_APROM_BASE     0x00000000u
#define FLASH_APROM_SIZE     0x00008000u   /* 32 KB on NUC120LC1BN */
#define FLASH_DATA_BASE      0x0001F000u   /* data flash, 4 KB      */
#define FLASH_DATA_SIZE      0x00001000u
/* Last data-flash page: the device ID on chips without a UID (hal.c). The
 * config journal (config_store.c) uses the pages before it. */
#define FLASH_DEVID_ADDR     (FLASH_DATA_BASE + FLASH_DATA_SIZE - FLASH_PAGE_SIZE)
#define FLASH_LDROM_BASE     0x00100000u
#define FLASH_LDROM_SIZE     0x00001000u
#define FLASH_CONFIG_BASE    0x00300000u
#define FLASH_PAGE_SIZE      512u
#define SRAM_BASE            0x20000000u
#define SRAM_SIZE            0x00001000u   /* 4 KB */

/* ---- GCR: system manager (TRM 5.2), base 0x5000_0000 ------------------ */
#define GCR_BASE             0x50000000u
#define GCR_RSTSRC           RB(GCR_BASE, 0x004)
#define GCR_IPRSTC1          RB(GCR_BASE, 0x008)
#define GCR_IPRSTC2          RB(GCR_BASE, 0x00C)
#define GCR_GPA_MFP          RB(GCR_BASE, 0x030)
#define GCR_GPB_MFP          RB(GCR_BASE, 0x034)
#define GCR_GPC_MFP          RB(GCR_BASE, 0x038)
#define GCR_REGWRPROT        RB(GCR_BASE, 0x100)

#define RSTSRC_POR           (1u << 0)
#define RSTSRC_RESET         (1u << 1)
#define RSTSRC_CPU           (1u << 7)

#define IPRSTC1_CHIP_RST     (1u << 0)     /* never used: reloads CBS -> BS */
#define IPRSTC1_CPU_RST      (1u << 1)     /* CPU + FMC only; keeps BS      */
#define IPRSTC2_USBD_RST     (1u << 27)

#define REGWRPROT_DIS        (1u << 0)     /* read: 1 = protection disabled */

/* ---- CLK: clock controller (TRM 5.3), base 0x5000_0200 ---------------- */
#define CLK_BASE             0x50000200u
#define CLK_PWRCON           RB(CLK_BASE, 0x00)
#define CLK_AHBCLK           RB(CLK_BASE, 0x04)
#define CLK_APBCLK           RB(CLK_BASE, 0x08)
#define CLK_CLKSTATUS        RB(CLK_BASE, 0x0C)
#define CLK_CLKSEL0          RB(CLK_BASE, 0x10)
#define CLK_CLKSEL1          RB(CLK_BASE, 0x14)
#define CLK_CLKDIV           RB(CLK_BASE, 0x18)
#define CLK_PLLCON           RB(CLK_BASE, 0x20)

#define PWRCON_XTL12M_EN     (1u << 0)
#define PWRCON_OSC22M_EN     (1u << 2)
#define AHBCLK_ISP_EN        (1u << 2)
#define APBCLK_USBD_EN       (1u << 27)
#define CLKSTATUS_XTL12M_STB (1u << 0)
#define CLKSTATUS_PLL_STB    (1u << 2)
#define CLKSTATUS_OSC22M_STB (1u << 4)
#define CLKSTATUS_CLK_SW_FAIL (1u << 7)
#define CLKSEL0_HCLK_S_MASK  0x7u
#define CLKSEL0_HCLK_S_HXT   0x0u
#define CLKSEL0_HCLK_S_PLL   0x2u
#define CLKSEL0_HCLK_S_HIRC  0x7u
#define PLLCON_PD            (1u << 16)
#define PLLCON_OE            (1u << 18)
/* FIN 12 MHz, FB_DV=46 (NF=48), IN_DV=1 (NR=3), OUT_DV=3 (NO=4): 48 MHz.
 * The TRM's own example value (TRM 5.3, PLLCON); also what stock firmware uses. */
#define PLLCON_48MHZ_FROM_12MHZ 0x0000C22Eu

/* ---- GPIO (TRM 5.5), base 0x5000_4000, 0x40 per port ------------------ */
#define GPIO_BASE            0x50004000u
#define GPIO_PORT(n)         (GPIO_BASE + 0x40u * (n))
#define GPIO_PMD(n)          RB(GPIO_BASE, 0x40u * (n) + 0x00)
#define GPIO_OFFD(n)         RB(GPIO_BASE, 0x40u * (n) + 0x04)
#define GPIO_DOUT(n)         RB(GPIO_BASE, 0x40u * (n) + 0x08)
#define GPIO_DMASK(n)        RB(GPIO_BASE, 0x40u * (n) + 0x0C)
#define GPIO_PIN(n)          RB(GPIO_BASE, 0x40u * (n) + 0x10)
#define GPIO_DBEN(n)         RB(GPIO_BASE, 0x40u * (n) + 0x14)
#define PORTA 0
#define PORTB 1
#define PORTC 2
#define PORTD 3
#define PMD_INPUT            0u
#define PMD_OUTPUT           1u
#define PMD_OPEN_DRAIN       2u
#define PMD_QUASI            3u

/* ---- FMC: flash memory controller (TRM 6.9/6.10), base 0x5000_C000 ---- */
#define FMC_BASE             0x5000C000u
#define FMC_ISPCON           RB(FMC_BASE, 0x00)
#define FMC_ISPADR           RB(FMC_BASE, 0x04)
#define FMC_ISPDAT           RB(FMC_BASE, 0x08)
#define FMC_ISPCMD           RB(FMC_BASE, 0x0C)
#define FMC_ISPTRG           RB(FMC_BASE, 0x10)
#define FMC_DFBADR           RB(FMC_BASE, 0x14)

#define ISPCON_ISPEN         (1u << 0)
#define ISPCON_BS            (1u << 1)     /* 1 = boot LDROM next CPU reset */
#define ISPCON_CFGUEN        (1u << 4)     /* gemifi NEVER sets this        */
#define ISPCON_LDUEN         (1u << 5)     /* gemifi NEVER sets this        */
#define ISPCON_ISPFF         (1u << 6)     /* fail flag, write 1 to clear   */
#define ISPCON_FORBIDDEN     (ISPCON_CFGUEN | ISPCON_LDUEN)

#define ISPCMD_READ          0x00u
#define ISPCMD_READ_UID      0x04u   /* not in TRM V2.02; from NUC100BSP fmc.h */
#define ISPCMD_PROGRAM       0x21u
#define ISPCMD_PAGE_ERASE    0x22u
#define ISPTRG_ISPGO         (1u << 0)

/* ---- USBD: USB device controller (TRM 5.4.5), base 0x4006_0000 -------- */
#define USBD_BASE            0x40060000u
#define USBD_INTEN           RB(USBD_BASE, 0x000)
#define USBD_INTSTS          RB(USBD_BASE, 0x004)
#define USBD_FADDR           RB(USBD_BASE, 0x008)
#define USBD_EPSTS           RB(USBD_BASE, 0x00C)
#define USBD_ATTR            RB(USBD_BASE, 0x010)
#define USBD_FLDET           RB(USBD_BASE, 0x014)
#define USBD_STBUFSEG        RB(USBD_BASE, 0x018)
/* Six simplex endpoint "slots", 0x10 apart starting at 0x020. */
#define USBD_BUFSEG(s)       RB(USBD_BASE, 0x020 + 0x10u * (s))
#define USBD_MXPLD(s)        RB(USBD_BASE, 0x024 + 0x10u * (s))
#define USBD_CFG(s)          RB(USBD_BASE, 0x028 + 0x10u * (s))
#define USBD_CFGP(s)         RB(USBD_BASE, 0x02C + 0x10u * (s))
#define USBD_DRVSE0          RB(USBD_BASE, 0x090)
#define USBD_SRAM            ((volatile uint8_t *)(USBD_BASE + 0x100)) /* 512 B */

#define USBD_INTEN_BUS_IE    (1u << 0)
#define USBD_INTEN_USB_IE    (1u << 1)
#define USBD_INTEN_FLDET_IE  (1u << 2)
#define USBD_INTSTS_BUS      (1u << 0)
#define USBD_INTSTS_USB      (1u << 1)
#define USBD_INTSTS_FLDET    (1u << 2)
#define USBD_INTSTS_WAKEUP   (1u << 3)
#define USBD_INTSTS_EPEVT(s) (1u << (16 + (s)))
#define USBD_INTSTS_SETUP    (1u << 31)
#define USBD_ATTR_USBRST     (1u << 0)
#define USBD_ATTR_SUSPEND    (1u << 1)
#define USBD_ATTR_RESUME     (1u << 2)
#define USBD_ATTR_PHY_EN     (1u << 4)
#define USBD_ATTR_USB_EN     (1u << 7)
#define USBD_ATTR_DPPU_EN    (1u << 8)
#define USBD_ATTR_PWRDN      (1u << 9)     /* 1 = PHY powered (low-active PD) */
#define USBD_ATTR_BYTEM      (1u << 10)    /* CPU accesses USB SRAM in bytes  */
/* 0x7D0: BYTEM | PWRDN | DPPU_EN | USB_EN | bit6 (reserved, reset 1) | PHY_EN.
 * Same value as the stock firmware and TinyUSB's dcd_nuc120. */
#define USBD_ATTR_INIT       0x7D0u
#define USBD_CFG_EPNUM(n)    ((uint32_t)(n) & 0xFu)
#define USBD_CFG_ISOCH       (1u << 4)
#define USBD_CFG_STATE_OUT   (1u << 5)
#define USBD_CFG_STATE_IN    (2u << 5)
#define USBD_CFG_DSQ_SYNC    (1u << 7)     /* next IN packet is DATA1 */
#define USBD_CFG_CSTALL      (1u << 9)     /* auto-clear STALL on SETUP */
#define USBD_CFGP_CLRRDY     (1u << 0)
#define USBD_CFGP_SSTALL     (1u << 1)
#define USBD_DRVSE0_DRVSE0   (1u << 0)

/* ---- Cortex-M0 SysTick ----------------------------------------------- */
#define SYST_CSR             RB(0xE000E000u, 0x10)
#define SYST_RVR             RB(0xE000E000u, 0x14)
#define SYST_CVR             RB(0xE000E000u, 0x18)
#define SYST_CSR_ENABLE      (1u << 0)
#define SYST_CSR_CLKSOURCE   (1u << 2)     /* 1 = core clock */
#define SYST_CSR_COUNTFLAG   (1u << 16)

/* The timer, ADC and NVIC sections below (added for midi-controller) match
 * the stock firmware's register writes (hw-map.md section 4) but have not run
 * on hardware yet. */

/* ---- Clock bits used by apps (TRM 5.3) -------------------------------- */
#define APBCLK_TMR0_EN       (1u << 2)
#define APBCLK_TMR1_EN       (1u << 3)
#define APBCLK_ADC_EN        (1u << 28)
#define CLKSEL1_ADC_S_MASK   (3u << 2)     /* 00 = 12 MHz crystal          */
#define CLKSEL1_TMR0_S_MASK  (7u << 8)     /* 000 = 12 MHz crystal         */
#define CLKSEL1_TMR1_S_MASK  (7u << 12)    /* 111 = internal 22 MHz RC     */
#define CLKDIV_ADC_N_SHIFT   16            /* ADC clock = src / (ADC_N+1)  */
#define CLKDIV_ADC_N_MASK    (0xFFu << 16)
#define IPRSTC2_TMR0_RST     (1u << 2)
#define IPRSTC2_ADC_RST      (1u << 28)

/* ---- Timer0 (TRM 5.11) ------------------------------------------------ */
#define TMR0_BASE            0x40010000u
#define TMR0_TCSR            RB(TMR0_BASE, 0x00)
#define TMR0_TCMPR           RB(TMR0_BASE, 0x04)
#define TMR0_TISR            RB(TMR0_BASE, 0x08)
#define TMR0_TDR             RB(TMR0_BASE, 0x0C)
#define TMR1_BASE            0x40010020u
#define TMR1_TCSR            RB(TMR1_BASE, 0x00)
#define TMR1_TCMPR           RB(TMR1_BASE, 0x04)
#define TMR1_TDR             RB(TMR1_BASE, 0x0C)
#define TCSR_TDR_EN          (1u << 16)    /* keep TDR updated             */
#define TCSR_PRESCALE(n)     ((uint32_t)(n) & 0xFFu)
#define TCSR_CRST            (1u << 26)
#define TCSR_MODE_PERIODIC   (1u << 27)    /* MODE[28:27] = 01             */
#define TCSR_IE              (1u << 29)
#define TCSR_CEN             (1u << 30)
#define TISR_TIF             (1u << 0)     /* write 1 to clear             */

/* ---- ADC (TRM 5.15) --------------------------------------------------- */
#define ADC_BASE             0x400E0000u
#define ADC_ADDR(ch)         RB(ADC_BASE, 0x00 + 4u * (ch))
#define ADC_ADCR             RB(ADC_BASE, 0x20)
#define ADC_ADCHER           RB(ADC_BASE, 0x24)
#define ADC_ADSR             RB(ADC_BASE, 0x30)
#define ADCR_ADEN            (1u << 0)
#define ADCR_ADST            (1u << 11)
#define ADSR_ADF             (1u << 0)     /* write 1 to clear             */
#define ADDR_RSLT_MASK       0xFFFu

/* ---- Cortex-M0 NVIC --------------------------------------------------- */
#define NVIC_ISER            RB(0xE000E000u, 0x100)
#define NVIC_ICER            RB(0xE000E000u, 0x180)
#define IRQ_TMR0             8
#define IRQ_USBD             23

static inline void irq_enable_all(void) { __asm__ volatile ("cpsie i" ::: "memory"); }
static inline void irq_disable_all(void) { __asm__ volatile ("cpsid i" ::: "memory"); }

#endif
