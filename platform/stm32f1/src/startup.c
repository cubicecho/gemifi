/*
 * startup.c: Cortex-M3 vector table and reset handler for STM32F1 apps
 * linked at 0x08002400 (above the vendor bootloader).
 *
 * The table has 16 core + 60 IRQ entries = 0x130 bytes, so it ends exactly
 * where the .gfw header goes (app offset 0x130, ld/app.ld).
 *
 * Reset_Handler, in this order:
 *   0. interrupts masked (PRIMASK): the bootloader may leave some live,
 *   0b. board_gpio_init(): pins the board needs held from the start (the
 *      Code 61 switches itself off unless PC15 is driven high),
 *   1. SP = _estack (in case the bootloader jumped without loading MSP),
 *   2. recovery_check() (src/recovery.c): the button combo, before anything
 *      else can go wrong; it needs only a stack,
 *   3. SysTick off, every IRQ disabled and cleared, then VTOR = our table
 *      (the vendor bootloader may leave it at 0x08000000),
 *   4. .data copy, .bss zero, main().
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "stm32f1.h"
#include "recovery.h"
#include "board.h"

/* The bootloader (GEMIFI_BOOTLOADER) checks its own entry combo; only apps run
 * recovery_check() from the reset handler. */
#ifdef GEMIFI_BOOTLOADER
#define RECOVERY_CALL ""
#else
#define RECOVERY_CALL "bl recovery_check\n\t"
#endif

#define VECTOR_IRQS VECTOR_IRQS_F1

extern uint32_t _sidata[], _sdata[], _edata[], _sbss[], _ebss[], _estack[];
int main(void);

void Reset_Handler(void);
void Default_Handler(void);

void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void USB_HP_CAN1_TX_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel4_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel5_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel6_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel7_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM4_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USBWakeUp_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

typedef void (*vector_t)(void);

/* The IRQ range initialiser is overridden for the IRQs we name. */
#pragma GCC diagnostic ignored "-Woverride-init"

__attribute__((section(".vectors"), used))
const vector_t gemifi_vectors[16 + VECTOR_IRQS] = {
    (vector_t)(uintptr_t)_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    [16 + 0 ... 16 + VECTOR_IRQS - 1] = Default_Handler,
    [16 + IRQ_USB_HP_CAN1_TX] = USB_HP_CAN1_TX_IRQHandler,
    [16 + IRQ_USB_LP_CAN1_RX0] = USB_LP_CAN1_RX0_IRQHandler,
    [16 + IRQ_DMA1_CH1] = DMA1_Channel1_IRQHandler,
    [16 + IRQ_DMA1_CH2] = DMA1_Channel2_IRQHandler,
    [16 + IRQ_DMA1_CH3] = DMA1_Channel3_IRQHandler,
    [16 + IRQ_DMA1_CH4] = DMA1_Channel4_IRQHandler,
    [16 + IRQ_DMA1_CH5] = DMA1_Channel5_IRQHandler,
    [16 + IRQ_DMA1_CH6] = DMA1_Channel6_IRQHandler,
    [16 + IRQ_DMA1_CH7] = DMA1_Channel7_IRQHandler,
    [16 + IRQ_TIM2] = TIM2_IRQHandler,
    [16 + IRQ_TIM3] = TIM3_IRQHandler,
    [16 + IRQ_TIM4] = TIM4_IRQHandler,
    [16 + IRQ_USART1] = USART1_IRQHandler,
    [16 + IRQ_USART2] = USART2_IRQHandler,
    [16 + IRQ_USBWAKEUP] = USBWakeUp_IRQHandler,
};

_Static_assert(sizeof(gemifi_vectors) == 0x130, "vector table must be 0x130 bytes");

/* Called by name from Reset_Handler's asm: keep it global and "used" so
 * LTO neither drops nor renames it. */
__attribute__((noreturn, used, externally_visible)) void reset_c(void);
void reset_c(void)
{
    /* The vendor bootloader jumps here with whatever it was using still
     * running: SysTick and NVIC interrupts may be enabled or pending. The
     * stock app copes because its table has real handlers for them; ours
     * mostly point at Default_Handler. Stop SysTick, disable and clear
     * every IRQ and the pending SysTick/PendSV before our table goes live.
     * PRIMASK stays set (Reset_Handler); apps that want interrupts enable
     * them once their own handlers are set up (irq_enable_all()). */
    SYST_CSR = 0;
    for (uint32_t i = 0; i < (VECTOR_IRQS + 31u) / 32u; i++) {
        NVIC_ICER(i) = 0xFFFFFFFFu;
        NVIC_ICPR(i) = 0xFFFFFFFFu;
    }
    SCB_ICSR = SCB_ICSR_PENDSTCLR | SCB_ICSR_PENDSVCLR;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");

    SCB_VTOR = (uint32_t)(uintptr_t)gemifi_vectors;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");

    uint32_t *d, *s = _sidata;
    for (d = _sdata; d < _edata; )
        *d++ = *s++;
    for (d = _sbss; d < _ebss; d++)
        *d = 0;
    main();
    for (;;) {
    }
}

/* Called by name from Reset_Handler, before .data/.bss exist: the board
 * header keeps board_gpio_init() register-only. */
__attribute__((used, externally_visible)) void board_early_init(void);
void board_early_init(void)
{
    board_gpio_init();
}

/* Naked: no prologue touches the stack before SP is ours. */
__attribute__((naked, noreturn)) void Reset_Handler(void)
{
    __asm__ volatile (
        "cpsid i\n\t"
        "ldr r0, =_estack\n\t"
        "mov sp, r0\n\t"
        "bl board_early_init\n\t"
        RECOVERY_CALL
        "b reset_c\n\t"
        ".ltorg\n\t");
}

void Default_Handler(void)
{
    /* Reset rather than spin: a fault before the IWDG is started would
     * otherwise hang for good. Each reset runs recovery_check() again, so
     * the recovery combo still works from a crash loop. */
    __asm__ volatile ("dsb" ::: "memory");
    SCB_AIRCR = SCB_AIRCR_VECTKEY | (SCB_AIRCR & SCB_AIRCR_PRIGROUP) | SCB_AIRCR_SYSRESETREQ;
    __asm__ volatile ("dsb" ::: "memory");
    for (;;) {
    }
}
