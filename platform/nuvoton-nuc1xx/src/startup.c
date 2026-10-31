/*
 * startup.c: Cortex-M0 vector table and reset handler for the NUC1xx.
 *
 * The reset handler copies .data and zeroes .bss before main(), so C code
 * may use initialised statics. It does not touch the 16-byte SRAM mailbox
 * at 0x2000_0FF0: the linker scripts end RAM (and the initial stack) there.
 *
 * VECTOR_IRQS: number of device IRQ slots in the table. Apps use the full
 * 32 (the table then ends exactly at 0x0C0, where the .gfw header goes);
 * the polled bootloader uses 0, which also drops the SVC/PendSV/SysTick
 * slots (176 bytes saved in the 4 KB LDROM).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#ifndef VECTOR_IRQS
#define VECTOR_IRQS 32
#endif

extern uint32_t _sidata[], _sdata[], _edata[], _sbss[], _ebss[], _estack[];
int main(void);

void Reset_Handler(void);
void Default_Handler(void);

void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
#if VECTOR_IRQS
void TMR0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USBD_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
#endif

typedef void (*vector_t)(void);

/* The IRQ range initialiser is overridden for the IRQs we name. */
#pragma GCC diagnostic ignored "-Woverride-init"

__attribute__((section(".vectors"), used))
#if VECTOR_IRQS
const vector_t gemifi_vectors[16 + VECTOR_IRQS] = {
#else
/* Polled bootloader: no interrupts are ever enabled and no SVC is issued,
 * so only NMI and HardFault can be taken; the table stops there (16 B). */
const vector_t gemifi_vectors[4] = {
#endif
    (vector_t)(uintptr_t)_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
#if VECTOR_IRQS
    0, 0, 0, 0, 0, 0, 0,
    SVC_Handler,
    0, 0,
    PendSV_Handler,
    SysTick_Handler,
    /* NUC100/120 IRQ numbers (TRM 5.2.7 / NVIC): 8 = TMR0, 23 = USBD */
    [16 + 0 ... 16 + VECTOR_IRQS - 1] = Default_Handler,
    [16 + 8] = TMR0_IRQHandler,
    [16 + 23] = USBD_IRQHandler,
#endif
};

void Reset_Handler(void)
{
    uint32_t *d;
#ifndef STARTUP_NO_DATA
    /* STARTUP_NO_DATA: the 4 KB bootloader has no .data (its linker script
     * asserts that), so it skips the copy loop. */
    uint32_t *s = _sidata;
    for (d = _sdata; d < _edata; )
        *d++ = *s++;
#endif
    for (d = _sbss; d < _ebss; d++)
        *d = 0;
    main();
    for (;;) {
    }
}

void Default_Handler(void)
{
    for (;;) {
    }
}
