/*
 * scan_dma.h: the matrix scan in hardware (TIM1 + DMA1), for the core's
 * hardware-scan plan (gm_controller.h gm_scan_plan_t, core/README.md
 * "Hardware scan").
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_STM32F1_SCAN_DMA_H
#define GEMIFI_STM32F1_SCAN_DMA_H

#include <stdint.h>
#include "gm_controller.h"

/* Start scanning plan p; 1 = running (then call gm_scan_hw_started() with
 * scan_dma_frame_q4()). 0 = this plan can't run here; nothing was changed. */
int scan_dma_start(const gm_scan_plan_t *p);
uint32_t scan_dma_frame_q4(void);   /* frame period, 1/16 us */
uint32_t scan_dma_frame_ns(void);
void scan_dma_stop(void);

/* For the scan report: the longest frame-processing interrupt (us), how
 * often the processing fell behind the DMA, and transfer errors (the scan
 * stops on one). */
extern volatile uint32_t scan_dma_isr_max_us, scan_dma_overruns, scan_dma_errors;

#endif
