/*
 * adc_dma.h: the analog inputs and pads read by TIM4 + ADC1 + DMA1, for
 * the core's analog plan (gm_controller.h gm_adc_plan_t, core/README.md
 * "Hardware scan").
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_STM32F1_ADC_DMA_H
#define GEMIFI_STM32F1_ADC_DMA_H

#include <stdint.h>
#include "gm_controller.h"

/* Start sweeping plan p; 1 = running (then call gm_adc_hw_started()). 0 =
 * this plan can't run here; nothing was changed. */
int adc_dma_start(const gm_adc_plan_t *p);
uint32_t adc_dma_sweep_us(void);
void adc_dma_stop(void);
int adc_dma_running(void);          /* hal_adc_read() refuses while it is */

/* For the scan report: the longest sweep interrupt (us), sweeps the
 * interrupt was too late for, restarts after the mux steps and the
 * conversions fell out of step, and transfer errors (the scan stops). */
extern volatile uint32_t adc_dma_isr_max_us, adc_dma_overruns, adc_dma_resyncs, adc_dma_errors;

#endif
