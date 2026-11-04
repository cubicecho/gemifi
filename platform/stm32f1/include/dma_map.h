/*
 * dma_map.h: which timers and DMA1 channels the hardware scans and the scan
 * tick use (README.md "DMA channels").
 *
 * The builder picks them (tools/builder/gemifi_device.py allocate_dma, from
 * hardware_scan.*.timer) and writes them to gm_device_config.h. A build
 * without a device config (the probe) gets the Code 61's original map:
 * matrix on TIM1 (select = update, ch5; columns in = CC1, ch2; read = CC2,
 * ch3; precharge = CC3, ch6), analog on TIM4 (mux = CC2, ch4; ADC1 started
 * by CC4), scan tick on TIM2.
 *
 * Requests: 0 = the update event, n = compare n. A _CH is the request's
 * DMA1 channel (RM0008 table 78), 0 = not used. The _CH values must be bare
 * digits: they name the interrupt handlers (DMA1_IRQ_HANDLER).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_DMA_MAP_H
#define GEMIFI_DMA_MAP_H

#if __has_include("gm_device_config.h")
#include "gm_device_config.h"
#endif

#ifndef GM_HW_SCAN_TIM
#define GM_HW_SCAN_TIM 1
#define GM_HW_SCAN_SEL 0
#define GM_HW_SCAN_SEL_CH 5
#define GM_HW_SCAN_IN 1
#define GM_HW_SCAN_IN_CH 2
#define GM_HW_SCAN_RD 2
#define GM_HW_SCAN_RD_CH 3
#define GM_HW_SCAN_PRE 3
#define GM_HW_SCAN_PRE_CH 6
#endif

#ifndef GM_HW_ADC_TIM
#define GM_HW_ADC_TIM 4
#define GM_HW_ADC_MUX 2
#define GM_HW_ADC_MUX_CH 4
#endif

#ifndef GM_SCAN_TICK_TIM
#define GM_SCAN_TICK_TIM 2
#endif

#define DMA1_IRQ_HANDLER_(n) DMA1_Channel##n##_IRQHandler
#define DMA1_IRQ_HANDLER(n) DMA1_IRQ_HANDLER_(n)    /* DMA1_IRQ_HANDLER(3) = DMA1_Channel3_IRQHandler */
#define TIM_IRQ_HANDLER_(n) TIM##n##_IRQHandler
#define TIM_IRQ_HANDLER(n) TIM_IRQ_HANDLER_(n)

#endif
