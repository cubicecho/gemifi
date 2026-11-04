/*
 * stm32f1.h: the STM32F10x registers gemifi uses (RM0008 rev 21, plus the
 * Cortex-M3 core registers from the ARMv7-M ARM). Written from the manuals,
 * not from ST's CMSIS headers. USB registers live in stm32f1_usb.h (the USB
 * driver's own header), not here.
 *
 * Pins are numbered port * 16 + bit (PA0 = 0, PB8 = 24, PC0 = 32, ...).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_STM32F1_H
#define GEMIFI_STM32F1_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define REG16(addr) (*(volatile uint16_t *)(addr))

/* ---- RCC (RM0008 7.3) ------------------------------------------------- */
#define RCC_BASE        0x40021000u
#define RCC_CR          REG32(RCC_BASE + 0x00u)
#define RCC_CFGR        REG32(RCC_BASE + 0x04u)
#define RCC_CIR         REG32(RCC_BASE + 0x08u)
#define RCC_APB2RSTR    REG32(RCC_BASE + 0x0Cu)
#define RCC_APB1RSTR    REG32(RCC_BASE + 0x10u)
#define RCC_AHBENR      REG32(RCC_BASE + 0x14u)
#define RCC_APB2ENR     REG32(RCC_BASE + 0x18u)
#define RCC_APB1ENR     REG32(RCC_BASE + 0x1Cu)
#define RCC_BDCR        REG32(RCC_BASE + 0x20u)
#define RCC_CSR         REG32(RCC_BASE + 0x24u)

#define RCC_CR_HSION    (1u << 0)
#define RCC_CR_HSIRDY   (1u << 1)
#define RCC_CR_HSEON    (1u << 16)
#define RCC_CR_HSERDY   (1u << 17)
#define RCC_CR_HSEBYP   (1u << 18)
#define RCC_CR_CSSON    (1u << 19)
#define RCC_CR_PLLON    (1u << 24)
#define RCC_CR_PLLRDY   (1u << 25)

#define RCC_CFGR_SW_MASK      (3u << 0)
#define RCC_CFGR_SW_HSI       (0u << 0)
#define RCC_CFGR_SW_HSE       (1u << 0)
#define RCC_CFGR_SW_PLL       (2u << 0)
#define RCC_CFGR_SWS_MASK     (3u << 2)
#define RCC_CFGR_SWS_HSI      (0u << 2)
#define RCC_CFGR_SWS_PLL      (2u << 2)
#define RCC_CFGR_HPRE_DIV1    (0u << 4)
#define RCC_CFGR_PPRE1_DIV2   (4u << 8)
#define RCC_CFGR_PPRE2_DIV1   (0u << 11)
#define RCC_CFGR_ADCPRE_DIV6  (2u << 14)
#define RCC_CFGR_PLLSRC_HSE   (1u << 16)   /* 0 = HSI / 2 */
#define RCC_CFGR_PLLXTPRE     (1u << 17)   /* HSE / 2 into the PLL */
#define RCC_CFGR_PLLMUL(n)    ((uint32_t)((n) - 2u) << 18)   /* n = 2..16 */
#define RCC_CFGR_USBPRE_DIV1  (1u << 22)   /* 0 = PLL / 1.5 */

/* AHBENR */
#define RCC_AHBENR_DMA1EN     (1u << 0)
#define RCC_AHBENR_FLITFEN    (1u << 4)
/* APB2ENR / APB2RSTR */
#define RCC_APB2_AFIO   (1u << 0)
#define RCC_APB2_IOPA   (1u << 2)          /* IOPx = 1 << (2 + port) */
#define RCC_APB2_IOP(port) (1u << (2u + (port)))
#define RCC_APB2_ADC1   (1u << 9)
#define RCC_APB2_ADC2   (1u << 10)
#define RCC_APB2_TIM1   (1u << 11)
#define RCC_APB2_USART1 (1u << 14)
/* APB1ENR / APB1RSTR */
#define RCC_APB1_TIM2   (1u << 0)
#define RCC_APB1_TIM3   (1u << 1)
#define RCC_APB1_TIM4   (1u << 2)
#define RCC_APB1_USART2 (1u << 17)
#define RCC_APB1_USB    (1u << 23)
#define RCC_APB1_PWR    (1u << 28)

/* CSR: LSI and the reset flags (cleared by RMVF) */
#define RCC_CSR_LSION    (1u << 0)
#define RCC_CSR_LSIRDY   (1u << 1)
#define RCC_CSR_RMVF     (1u << 24)
#define RCC_CSR_PINRSTF  (1u << 26)
#define RCC_CSR_PORRSTF  (1u << 27)
#define RCC_CSR_SFTRSTF  (1u << 28)
#define RCC_CSR_IWDGRSTF (1u << 29)
#define RCC_CSR_WWDGRSTF (1u << 30)
#define RCC_CSR_LPWRRSTF (1u << 31)

/* ---- FLASH interface (RM0008 3.3.3, PM0075) --------------------------- */
#define FLASH_R_BASE    0x40022000u
#define FLASH_ACR       REG32(FLASH_R_BASE + 0x00u)
#define FLASH_KEYR      REG32(FLASH_R_BASE + 0x04u)
#define FLASH_OPTKEYR   REG32(FLASH_R_BASE + 0x08u)
#define FLASH_SR        REG32(FLASH_R_BASE + 0x0Cu)
#define FLASH_CR        REG32(FLASH_R_BASE + 0x10u)
#define FLASH_AR        REG32(FLASH_R_BASE + 0x14u)
#define FLASH_OBR       REG32(FLASH_R_BASE + 0x1Cu)
#define FLASH_WRPR      REG32(FLASH_R_BASE + 0x20u)

#define FLASH_ACR_LATENCY_MASK 7u
#define FLASH_ACR_LATENCY(n)   ((uint32_t)(n))
#define FLASH_ACR_HLFCYA       (1u << 3)
#define FLASH_ACR_PRFTBE       (1u << 4)
#define FLASH_ACR_PRFTBS       (1u << 5)

#define FLASH_KEY1      0x45670123u
#define FLASH_KEY2      0xCDEF89ABu

#define FLASH_SR_BSY      (1u << 0)
#define FLASH_SR_PGERR    (1u << 2)
#define FLASH_SR_WRPRTERR (1u << 4)
#define FLASH_SR_EOP      (1u << 5)

#define FLASH_CR_PG     (1u << 0)
#define FLASH_CR_PER    (1u << 1)
#define FLASH_CR_MER    (1u << 2)
#define FLASH_CR_OPTPG  (1u << 4)
#define FLASH_CR_OPTER  (1u << 5)
#define FLASH_CR_STRT   (1u << 6)
#define FLASH_CR_LOCK   (1u << 7)

#define FLASH_OBR_OPTERR (1u << 0)
#define FLASH_OBR_RDPRT  (1u << 1)         /* read protection active */

/* System memory: flash size in KB (16 bit) and the 96-bit unique ID */
#define FLASH_SIZE_REG  REG16(0x1FFFF7E0u)
#define UID_BASE        0x1FFFF7E8u

/* ---- GPIO (RM0008 9.2) ------------------------------------------------ */
#define GPIO_BASE(port) (0x40010800u + 0x400u * (uint32_t)(port))
#define GPIO_CRL(port)  REG32(GPIO_BASE(port) + 0x00u)
#define GPIO_CRH(port)  REG32(GPIO_BASE(port) + 0x04u)
#define GPIO_IDR(port)  REG32(GPIO_BASE(port) + 0x08u)
#define GPIO_ODR(port)  REG32(GPIO_BASE(port) + 0x0Cu)
#define GPIO_BSRR(port) REG32(GPIO_BASE(port) + 0x10u)
#define GPIO_BRR(port)  REG32(GPIO_BASE(port) + 0x14u)
#define GPIO_LCKR(port) REG32(GPIO_BASE(port) + 0x18u)
#define PORTA 0
#define PORTB 1
#define PORTC 2
#define PORTD 3
#define PORTE 4
#define PORTF 5
#define PORTG 6
#define GPIO_PORTS 7

/* 4-bit CNF:MODE nibbles for CRL/CRH */
#define GPIO_CFG_ANALOG   0x0u
#define GPIO_CFG_FLOAT    0x4u             /* input, floating (reset state) */
#define GPIO_CFG_PULL     0x8u             /* input, pull-up/down by ODR */
#define GPIO_CFG_OUT_PP   0x1u             /* output push-pull, 10 MHz */
#define GPIO_CFG_OUT_OD   0x5u             /* output open-drain, 10 MHz */
#define GPIO_CFG_AF_PP    0x9u             /* alternate push-pull, 10 MHz */

/* ---- AFIO (RM0008 9.4) ------------------------------------------------ */
#define AFIO_BASE       0x40010000u
#define AFIO_EVCR       REG32(AFIO_BASE + 0x00u)
#define AFIO_MAPR       REG32(AFIO_BASE + 0x04u)
#define AFIO_MAPR_SWJ_MASK   (7u << 24)
#define AFIO_MAPR_SWJ_NOJTAG (2u << 24)    /* JTAG off (PA15, PB3, PB4 free), SWD on */

/* ---- ADC1 (RM0008 11.12) ---------------------------------------------- */
#define ADC1_BASE       0x40012400u
#define ADC1_SR         REG32(ADC1_BASE + 0x00u)
#define ADC1_CR1        REG32(ADC1_BASE + 0x04u)
#define ADC1_CR2        REG32(ADC1_BASE + 0x08u)
#define ADC1_SMPR1      REG32(ADC1_BASE + 0x0Cu)
#define ADC1_SMPR2      REG32(ADC1_BASE + 0x10u)
#define ADC1_SQR1       REG32(ADC1_BASE + 0x2Cu)
#define ADC1_SQR2       REG32(ADC1_BASE + 0x30u)
#define ADC1_SQR3       REG32(ADC1_BASE + 0x34u)
#define ADC1_DR         REG32(ADC1_BASE + 0x4Cu)
/* ADC2: the same registers 0x400 further on. hal_adc_read uses it, so the
 * hardware analog scan (adc_dma.c) can keep ADC1. */
#define ADC2_BASE       0x40012800u
#define ADC2_SR         REG32(ADC2_BASE + 0x00u)
#define ADC2_CR2        REG32(ADC2_BASE + 0x08u)
#define ADC2_SMPR1      REG32(ADC2_BASE + 0x0Cu)
#define ADC2_SMPR2      REG32(ADC2_BASE + 0x10u)
#define ADC2_SQR1       REG32(ADC2_BASE + 0x2Cu)
#define ADC2_SQR3       REG32(ADC2_BASE + 0x34u)
#define ADC2_DR         REG32(ADC2_BASE + 0x4Cu)
#define ADC_SR_EOC      (1u << 1)
#define ADC_CR2_ADON    (1u << 0)
#define ADC_CR2_CAL     (1u << 2)
#define ADC_CR2_RSTCAL  (1u << 3)
#define ADC_CR2_EXTSEL_SWSTART (7u << 17)
#define ADC_CR2_EXTSEL_TIM4_CC4 (5u << 17) /* regular group trigger (RM0008 11.12.3) */
#define ADC_CR2_DMA     (1u << 8)
#define ADC_CR1_SCAN    (1u << 8)
#define ADC_CR2_EXTTRIG (1u << 20)
#define ADC_CR2_SWSTART (1u << 22)

/* ---- IWDG (RM0008 19.4) ----------------------------------------------- */
#define IWDG_BASE       0x40003000u
#define IWDG_KR         REG32(IWDG_BASE + 0x00u)
#define IWDG_PR         REG32(IWDG_BASE + 0x04u)
#define IWDG_RLR        REG32(IWDG_BASE + 0x08u)
#define IWDG_SR         REG32(IWDG_BASE + 0x0Cu)
#define IWDG_KEY_RELOAD 0xAAAAu
#define IWDG_KEY_ACCESS 0x5555u
#define IWDG_KEY_START  0xCCCCu

/* ---- general-purpose timers TIM2..TIM4 (RM0008 15.4) ------------------ */
#define TIM2_BASE       0x40000000u
#define TIM3_BASE       0x40000400u
#define TIM4_BASE       0x40000800u
#define TIM_CR1(t)      REG32((t) + 0x00u)
#define TIM_DIER(t)     REG32((t) + 0x0Cu)
#define TIM_SR(t)       REG32((t) + 0x10u)
#define TIM_EGR(t)      REG32((t) + 0x14u)
#define TIM_CNT(t)      REG32((t) + 0x24u)
#define TIM_PSC(t)      REG32((t) + 0x28u)
#define TIM_ARR(t)      REG32((t) + 0x2Cu)
#define TIM_CR1_CEN     (1u << 0)
#define TIM_CR1_URS     (1u << 2)          /* only overflow sets UIF */
#define TIM_CR1_ARPE    (1u << 7)
#define TIM_DIER_UIE    (1u << 0)
#define TIM_SR_UIF      (1u << 0)
#define TIM_EGR_UG      (1u << 0)
/* TIM1 (advanced, on APB2: clocked at HCLK) and the registers the matrix
 * scan uses (RM0008 14.4). The layout matches TIM2..TIM4 for these. */
#define TIM1_BASE       0x40012C00u
#define TIM_CR2(t)      REG32((t) + 0x04u)
#define TIM_CCMR1(t)    REG32((t) + 0x18u)
#define TIM_CCMR2(t)    REG32((t) + 0x1Cu)
#define TIM_CCER(t)     REG32((t) + 0x20u)
#define TIM_RCR(t)      REG32((t) + 0x30u)
#define TIM_CCR1(t)     REG32((t) + 0x34u)
#define TIM_CCR2(t)     REG32((t) + 0x38u)
#define TIM_CCR3(t)     REG32((t) + 0x3Cu)
#define TIM_CCR4(t)     REG32((t) + 0x40u)
#define TIM_DIER_UDE    (1u << 8)          /* update -> DMA request */
#define TIM_DIER_CC1DE  (1u << 9)          /* compare n -> DMA request */
#define TIM_DIER_CC2DE  (1u << 10)
#define TIM_DIER_CC3DE  (1u << 11)
#define TIM_CCMR2_OC4M_PWM2 (7u << 12)     /* OC4REF rises at CNT = CCR4 */
#define TIM_CCER_CC4E   (1u << 12)

/* ---- debug MCU (RM0008 31.6) ------------------------------------------ */
/* DBGMCU_IDCODE: DEV_ID [11:0], REV_ID [31:16]. Note ST errata (ES0340
 * 2.3.x): on STM32F101/F102/F103 it reads 0 unless a debugger is attached;
 * clones may behave differently. */
#define DBGMCU_IDCODE   REG32(0xE0042000u)
#define DBGMCU_CR       REG32(0xE0042004u)

/* ---- Cortex-M3 core --------------------------------------------------- */
#define SYST_CSR        REG32(0xE000E010u)
#define SYST_RVR        REG32(0xE000E014u)
#define SYST_CVR        REG32(0xE000E018u)
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_TICKINT   (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2)       /* 1 = HCLK */
#define SYST_CSR_COUNTFLAG (1u << 16)

#define NVIC_ISER(n)    REG32(0xE000E100u + 4u * (n))
#define NVIC_ICER(n)    REG32(0xE000E180u + 4u * (n))
#define NVIC_ICPR(n)    REG32(0xE000E280u + 4u * (n))
#define NVIC_IPR8(irq)  (*(volatile uint8_t *)(0xE000E400u + (irq)))

#define SCB_CPUID       REG32(0xE000ED00u)
#define SCB_ICSR        REG32(0xE000ED04u)
#define SCB_ICSR_PENDSTCLR (1u << 25)
#define SCB_ICSR_PENDSVCLR (1u << 27)
#define SCB_VTOR        REG32(0xE000ED08u)
#define SCB_AIRCR       REG32(0xE000ED0Cu)
#define SCB_SCR         REG32(0xE000ED10u)
#define SCB_CCR         REG32(0xE000ED14u)
#define SCB_SHCSR       REG32(0xE000ED24u)
#define SCB_CFSR        REG32(0xE000ED28u)
#define SCB_AIRCR_VECTKEY     (0x05FAu << 16)
#define SCB_AIRCR_PRIGROUP    (7u << 8)
#define SCB_AIRCR_SYSRESETREQ (1u << 2)

#define CORE_DEMCR      REG32(0xE000EDFCu)
#define CORE_DEMCR_TRCENA (1u << 24)
#define DWT_CTRL        REG32(0xE0001000u)
#define DWT_CYCCNT      REG32(0xE0001004u)
#define DWT_CTRL_CYCCNTENA (1u << 0)

/* IRQ numbers (RM0008 table 63, connectivity-line-free F101/F103) */
#define IRQ_USB_HP_CAN1_TX  19
#define IRQ_USB_LP_CAN1_RX0 20
#define IRQ_DMA1_CH1        11
#define IRQ_DMA1_CH3        13
#define IRQ_TIM2            28
#define IRQ_TIM3            29
#define IRQ_TIM4            30
#define IRQ_USART1          37
#define IRQ_USBWAKEUP       42
#define VECTOR_IRQS_F1      60

/* ---- bit-band (ARMv7-M B3.1, RM0008 3.3.2) ---------------------------- */
/* One word per bit of the peripheral region 0x40000000-0x400FFFFF:
 * reading gives the bit, writing bit 0 sets/clears it atomically. */
#define BITBAND_PERIPH(addr, bit) \
    (*(volatile uint32_t *)(0x42000000u + (((uint32_t)(addr) - 0x40000000u) << 5) + ((uint32_t)(bit) << 2)))

static inline void irq_enable_all(void) { __asm__ volatile ("cpsie i" ::: "memory"); }
static inline void irq_disable_all(void) { __asm__ volatile ("cpsid i" ::: "memory"); }

/* Save PRIMASK and disable; restore with irq_restore(). */
static inline uint32_t irq_save(void)
{
    uint32_t pm;
    __asm__ volatile ("mrs %0, primask\n\tcpsid i" : "=r"(pm) :: "memory");
    return pm;
}

static inline void irq_restore(uint32_t pm)
{
    __asm__ volatile ("msr primask, %0" :: "r"(pm) : "memory");
}


/* ---- USART1 (5-pin DIN MIDI: TX PA9, RX PA10), interrupt-driven ---- */
#define USART1_BASE     0x40013800u
#define USART1_SR       REG32(USART1_BASE + 0x00u)
#define USART1_DR       REG32(USART1_BASE + 0x04u)
#define USART1_BRR      REG32(USART1_BASE + 0x08u)
#define USART1_CR1      REG32(USART1_BASE + 0x0Cu)
#define USART_SR_RXNE   (1u << 5)
#define USART_SR_TXE    (1u << 7)
#define USART_SR_ORE    (1u << 3)
#define USART_CR1_RE    (1u << 2)
#define USART_CR1_RXNEIE (1u << 5)
#define USART_CR1_TXEIE (1u << 7)

/* ---- USART2 (TX on PA2) and its transmit DMA, DMA1 channel 7 ---- */
#define USART2_BASE     0x40004400u
#define USART2_SR       REG32(USART2_BASE + 0x00u)
#define USART2_DR       REG32(USART2_BASE + 0x04u)
#define USART2_BRR      REG32(USART2_BASE + 0x08u)
#define USART2_CR1      REG32(USART2_BASE + 0x0Cu)
#define USART2_CR3      REG32(USART2_BASE + 0x14u)
#define USART_CR1_UE    (1u << 13)
#define USART_CR1_TE    (1u << 3)
#define USART_CR3_DMAT  (1u << 7)
#define DMA1_BASE       0x40020000u
#define DMA1_ISR        REG32(DMA1_BASE + 0x00u)
#define DMA1_IFCR       REG32(DMA1_BASE + 0x04u)
#define DMA1_CCR(ch)    REG32(DMA1_BASE + 0x08u + 20u * ((ch) - 1u))
#define DMA1_CNDTR(ch)  REG32(DMA1_BASE + 0x0Cu + 20u * ((ch) - 1u))
#define DMA1_CPAR(ch)   REG32(DMA1_BASE + 0x10u + 20u * ((ch) - 1u))
#define DMA1_CMAR(ch)   REG32(DMA1_BASE + 0x14u + 20u * ((ch) - 1u))
#define DMA_CCR_EN      (1u << 0)
#define DMA_CCR_DIR     (1u << 4)          /* memory -> peripheral */
#define DMA_CCR_MINC    (1u << 7)
#define DMA_CCR_TCIE    (1u << 1)
#define DMA_CCR_HTIE    (1u << 2)
#define DMA_CCR_TEIE    (1u << 3)
#define DMA_CCR_CIRC    (1u << 5)
#define DMA_CCR_PSIZE32 (2u << 8)
#define DMA_CCR_MSIZE16 (1u << 10)
#define DMA_CCR_MSIZE32 (2u << 10)
#define DMA_CCR_PL(n)   ((uint32_t)(n) << 12)   /* 0 low .. 3 very high */
/* DMA1_ISR / DMA1_IFCR: 4 bits per channel. Clear only TC/HT/TE: clearing
 * the global flag of an active channel can hide a transfer error (ES0340
 * 2.4.1, the high-density sheet; the DMA1 block is the same). */
#define DMA_ISR_TCIF(ch) (2u << (4u * ((ch) - 1u)))
#define DMA_ISR_HTIF(ch) (4u << (4u * ((ch) - 1u)))
#define DMA_ISR_TEIF(ch) (8u << (4u * ((ch) - 1u)))

#endif
