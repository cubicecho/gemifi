/*
 * stm32f1_usb.h: registers used by usbd.c. USB full-speed device (RM0008
 * chapter 23) plus the few RCC/GPIO bits it needs for the clock, reset and
 * the SE0 disconnect. Kept apart from stm32f1.h; every name is USB_-prefixed
 * and #ifndef-guarded so both headers can be included together.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_STM32F1_USB_H
#define GEMIFI_STM32F1_USB_H

#include <stdint.h>

#define USB_MMIO32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define USB_REG16(a)  USB_MMIO32(a)     /* 16-bit regs at 32-bit stride */

/* ---- USB peripheral (RM0008 23.5) ------------------------------------- */
#ifndef USB_BASE
#define USB_BASE        0x40005C00u
#endif
#ifndef USB_PMA_BASE
#define USB_PMA_BASE    0x40006000u
#endif
#define USB_PMA_SIZE    512u

#define USB_EPR(n)      USB_REG16(USB_BASE + 4u * (n))
#define USB_CNTR        USB_REG16(USB_BASE + 0x40u)
#define USB_ISTR        USB_REG16(USB_BASE + 0x44u)
#define USB_FNR         USB_REG16(USB_BASE + 0x48u)
#define USB_DADDR       USB_REG16(USB_BASE + 0x4Cu)
#define USB_BTABLE      USB_REG16(USB_BASE + 0x50u)

/* EPnR (23.5.2). rc_w0: CTR_RX, CTR_TX. t (write 1 toggles): DTOG_RX,
 * STAT_RX, DTOG_TX, STAT_TX. r: SETUP. rw: EP_TYPE, EP_KIND, EA. */
#define USB_EP_CTR_RX   0x8000u
#define USB_EP_DTOG_RX  0x4000u
#define USB_EP_STAT_RX  0x3000u
#define USB_EP_SETUP    0x0800u
#define USB_EP_TYPE     0x0600u
#define USB_EP_KIND     0x0100u
#define USB_EP_CTR_TX   0x0080u
#define USB_EP_DTOG_TX  0x0040u
#define USB_EP_STAT_TX  0x0030u
#define USB_EP_EA       0x000Fu
#define USB_EP_RW       (USB_EP_SETUP | USB_EP_TYPE | USB_EP_KIND | USB_EP_EA)
#define USB_EP_TOGGLES  (USB_EP_DTOG_RX | USB_EP_STAT_RX | USB_EP_DTOG_TX | USB_EP_STAT_TX)

/* All EPnR writes go through this (a host test can model the bit types). */
#ifndef USB_EPR_WRITE
#define USB_EPR_WRITE(n, v) (USB_EPR(n) = (v))
#endif

#define USB_EP_BULK     0x0000u
#define USB_EP_CONTROL  0x0200u
#define USB_EP_ISO      0x0400u
#define USB_EP_INTR     0x0600u

/* STAT values, unshifted */
#define USB_STAT_DISABLED 0u
#define USB_STAT_STALL    1u
#define USB_STAT_NAK      2u
#define USB_STAT_VALID    3u
#define USB_RX_STAT(s)  ((uint32_t)(s) << 12)
#define USB_TX_STAT(s)  ((uint32_t)(s) << 4)

/* CNTR (23.5.1) */
#define USB_CNTR_CTRM   0x8000u
#define USB_CNTR_RESETM 0x0400u
#define USB_CNTR_FSUSP  0x0008u
#define USB_CNTR_LPMODE 0x0004u
#define USB_CNTR_PDWN   0x0002u
#define USB_CNTR_FRES   0x0001u

/* ISTR (23.5.1). Flags are rc_w0; CTR, DIR, EP_ID are read-only. */
#define USB_ISTR_CTR    0x8000u
#define USB_ISTR_PMAOVR 0x4000u
#define USB_ISTR_ERR    0x2000u
#define USB_ISTR_WKUP   0x1000u
#define USB_ISTR_SUSP   0x0800u
#define USB_ISTR_RESET  0x0400u
#define USB_ISTR_SOF    0x0200u
#define USB_ISTR_ESOF   0x0100u
#define USB_ISTR_FLAGS  0x7F00u

#define USB_DADDR_EF    0x0080u

/* Packet memory: byte offset 'off' (even) in the USB's local address space
 * is one 16-bit word at CPU address PMA_BASE + 2 * off (23.5.3). */
#define USB_PMA16(off)  (*(volatile uint16_t *)(uintptr_t)(USB_PMA_BASE + 2u * (off)))

/* Buffer descriptor table entry for EPnR n, BTABLE = 0 */
#define USB_ADDR_TX(n)  USB_PMA16(8u * (n) + 0u)
#define USB_COUNT_TX(n) USB_PMA16(8u * (n) + 2u)
#define USB_ADDR_RX(n)  USB_PMA16(8u * (n) + 4u)
#define USB_COUNT_RX(n) USB_PMA16(8u * (n) + 6u)
#define USB_COUNT_MASK  0x03FFu
/* COUNTn_RX allocation: BL_SIZE = 1 (32-byte blocks), NUM_BLOCK = n/32 - 1 */
#define USB_RX_BLOCKS32(bytes) (0x8000u | ((((bytes) / 32u) - 1u) << 10))

/* ---- RCC (RM0008 7.3) ------------------------------------------------- */
#ifndef USB_RCC_BASE
#define USB_RCC_BASE    0x40021000u
#endif
#define USB_RCC_APB1RSTR USB_MMIO32((USB_RCC_BASE + 0x10u))
#define USB_RCC_APB2ENR  USB_MMIO32((USB_RCC_BASE + 0x18u))
#define USB_RCC_APB1ENR  USB_MMIO32((USB_RCC_BASE + 0x1Cu))
#define USB_RCC_APB1_USB (1u << 23)                 /* USBEN / USBRST */
#define USB_RCC_APB2_IOP(port) (1u << (2u + (port))) /* IOPAEN = bit 2 */

/* ---- GPIO (RM0008 9.2) ------------------------------------------------ */
#ifndef USB_GPIOA_BASE
#define USB_GPIOA_BASE  0x40010800u
#endif
#define USB_GPIO_BASE(port) (USB_GPIOA_BASE + 0x400u * (port))   /* 0 = GPIOA */
#define USB_GPIO_CRL(port)  USB_MMIO32((USB_GPIO_BASE(port) + 0x00u))
#define USB_GPIO_CRH(port)  USB_MMIO32((USB_GPIO_BASE(port) + 0x04u))
#define USB_GPIO_BSRR(port) USB_MMIO32((USB_GPIO_BASE(port) + 0x10u))
#define USB_GPIO_BRR(port)  USB_MMIO32((USB_GPIO_BASE(port) + 0x14u))
/* CNF[1:0] MODE[1:0] nibbles */
#define USB_GPIO_IN_FLOAT 0x4u
#define USB_GPIO_OUT_PP   0x2u                       /* push-pull, 2 MHz */

#define USB_PIN_DM      11u                          /* PA11 */
#define USB_PIN_DP      12u                          /* PA12 */

#endif
