/*
 * usbd.c: polled USB device core for the STM32F1 USB full-speed device
 * peripheral (also GD32F103). See usbd.h.
 *
 * Register-level reference: RM0008 chapter 23. Behaviour matches the
 * NUC1xx driver (platform/nuvoton-nuc1xx/src/usbd.c) so usb_midi.c can be
 * shared unchanged.
 *
 * SPDX-License-Identifier: MIT
 */
#include "usbd.h"
#include "stm32f1_usb.h"
#include "sys.h"
#ifdef BOARD_HEADER
#include BOARD_HEADER
#endif

#define EP_CTRL 0                   /* EPnR slot / endpoint number 0 */
#define EP_DATA 1                   /* EPnR slot 1: both 0x81 and 0x01 */

/* PMA byte offsets (local addressing), BTABLE = 0. */
#define BUF_CTRL_TX 0x040
#define BUF_CTRL_RX 0x080
#define BUF_DATA_TX 0x0C0
#define BUF_DATA_RX 0x100           /* 0x140 of 512 B used */

#define EP0_SIZE  64
#define DATA_SIZE 64
#define STR_MAX   40

static const usbd_desc_t *desc;
static const uint8_t *ctl_ptr;      /* NULL: no IN data stage pending */
static uint16_t ctl_left;
static uint8_t ctl_zlp;             /* reply shorter than wLength */
static uint8_t pending_addr;
static volatile uint8_t config;     /* volatile: the USB interrupt may set these */
static volatile uint8_t in_busy;
static volatile int16_t out_len;    /* set to -1 by bus_reset() */
static volatile uint8_t irq_on;     /* usbd_irq_enable(): served from the interrupt */

#define USB_IRQ_BIT (1u << (IRQ_USB_LP_CAN1_RX0 % 32u))

/* Main-loop calls that touch EPnR mask the USB interrupt meanwhile: EPnR
 * writes are read-modify-write, and the interrupt writes the same registers. */
static uint32_t usb_lock(void)
{
    if (!irq_on)
        return 0;
    NVIC_ICER(IRQ_USB_LP_CAN1_RX0 / 32u) = USB_IRQ_BIT;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    return 1;
}

static void usb_unlock(uint32_t locked)
{
    if (locked)
        NVIC_ISER(IRQ_USB_LP_CAN1_RX0 / 32u) = USB_IRQ_BIT;
}
static uint8_t out_buf[DATA_SIZE];  /* PMA is not byte-addressable: copy */
static uint8_t ctl_buf[2 + 2 * STR_MAX];

static const uint8_t zeros[2];
static const uint8_t langid[4] = { 4, 3, 0x09, 0x04 };   /* en-US */

/* ---- EPnR access (RM0008 23.5.2) ---------------------------------------
 * Toggle bits flip when written 1, CTR bits clear when written 0. Every
 * write keeps the rw fields, writes 1 to CTR bits it must not clear (so an
 * event arriving between read and write is not lost) and writes
 * (current ^ wanted) to the toggle bits it changes, 0 to the rest. */

/* Set the toggle bits in 'mask' to 'val'. */
static void ep_set(uint32_t n, uint32_t mask, uint32_t val)
{
    uint32_t v = USB_EPR(n);
    USB_EPR_WRITE(n, (v & USB_EP_RW) | USB_EP_CTR_RX | USB_EP_CTR_TX |
                     ((v ^ val) & mask));
}

static void ep_clear_ctr(uint32_t n, uint32_t ctr)
{
    uint32_t v = USB_EPR(n);
    USB_EPR_WRITE(n, (v & USB_EP_RW) | ((USB_EP_CTR_RX | USB_EP_CTR_TX) & ~ctr));
}

/* Type and address, both CTR cleared, both DTOG = 0, given STATs. */
static void ep_open(uint32_t n, uint32_t type, uint32_t rx, uint32_t tx)
{
    uint32_t v = USB_EPR(n);
    USB_EPR_WRITE(n, type | n | ((v ^ (USB_RX_STAT(rx) | USB_TX_STAT(tx))) &
                                 USB_EP_TOGGLES));
}

#define SET_TX(n, s) ep_set((n), USB_EP_STAT_TX, USB_TX_STAT(s))
#define SET_RX(n, s) ep_set((n), USB_EP_STAT_RX, USB_RX_STAT(s))

/* ---- packet memory ---------------------------------------------------- */

static void pma_write(uint32_t off, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 2) {
        uint32_t w = p[i];
        if (i + 1 < n)
            w |= (uint32_t)p[i + 1] << 8;
        USB_PMA16(off + i) = (uint16_t)w;
    }
}

static void pma_read(uint32_t off, uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 2) {
        uint32_t w = USB_PMA16(off + i);
        p[i] = (uint8_t)w;
        if (i + 1 < n)
            p[i + 1] = (uint8_t)(w >> 8);
    }
}

/* ---- pins ------------------------------------------------------------- */

static void pin_mode(uint32_t port, uint32_t bit, uint32_t mode)
{
    volatile uint32_t *cr = bit < 8 ? &USB_GPIO_CRL(port) : &USB_GPIO_CRH(port);
    uint32_t sh = (bit & 7) * 4;
    *cr = (*cr & ~(0xFu << sh)) | (mode << sh);
}

#ifdef BOARD_USB_PULLUP_PIN
#ifdef BOARD_USB_PULLUP_ACTIVE_LOW
#define PULLUP_ON_LEVEL 0
#else
#define PULLUP_ON_LEVEL 1
#endif
static void pullup(int on)
{
    uint32_t port = (BOARD_USB_PULLUP_PIN) / 16, bit = (BOARD_USB_PULLUP_PIN) % 16;
    USB_RCC_APB2ENR |= USB_RCC_APB2_IOP(port);
    if ((on != 0) == PULLUP_ON_LEVEL)
        USB_GPIO_BSRR(port) = 1u << bit;
    else
        USB_GPIO_BRR(port) = 1u << bit;
    pin_mode(port, bit, USB_GPIO_OUT_PP);
}
#else
static void pullup(int on) { (void)on; }
#endif

/* USB block held in reset with its clock off, so the pins are plain GPIO;
 * then PA11/PA12 driven low (SE0) and the pull-up GPIO, if any, off. */
static void detach(void)
{
    USB_RCC_APB1RSTR |= USB_RCC_APB1_USB;
    USB_RCC_APB1ENR &= ~USB_RCC_APB1_USB;
    pullup(0);
    USB_RCC_APB2ENR |= USB_RCC_APB2_IOP(0);
    USB_GPIO_BRR(0) = (1u << USB_PIN_DM) | (1u << USB_PIN_DP);
    pin_mode(0, USB_PIN_DM, USB_GPIO_OUT_PP);
    pin_mode(0, USB_PIN_DP, USB_GPIO_OUT_PP);
    config = 0;
}

/* ---- device core ------------------------------------------------------ */

static void bus_reset(void)
{
    pending_addr = 0;
    config = 0;
    ctl_ptr = 0;
    in_busy = 0;
    out_len = -1;

    USB_BTABLE = 0;
    USB_ADDR_TX(EP_CTRL) = BUF_CTRL_TX;
    USB_COUNT_TX(EP_CTRL) = 0;
    USB_ADDR_RX(EP_CTRL) = BUF_CTRL_RX;
    USB_COUNT_RX(EP_CTRL) = USB_RX_BLOCKS32(EP0_SIZE);
    USB_ADDR_TX(EP_DATA) = BUF_DATA_TX;
    USB_COUNT_TX(EP_DATA) = 0;
    USB_ADDR_RX(EP_DATA) = BUF_DATA_RX;
    USB_COUNT_RX(EP_DATA) = USB_RX_BLOCKS32(DATA_SIZE);

    ep_open(EP_CTRL, USB_EP_CONTROL, USB_STAT_VALID, USB_STAT_NAK);
    ep_open(EP_DATA, USB_EP_BULK, USB_STAT_DISABLED, USB_STAT_DISABLED);
    USB_DADDR = USB_DADDR_EF;       /* address 0 */
}

void usbd_init(const usbd_desc_t *d)
{
    desc = d;
    /* An earlier stage (the vendor bootloader) may have left USB enumerated:
     * reset the block and hold SE0 ~10 ms so the host sees a disconnect. */
    detach();
    sys_delay_ms(10);
    pin_mode(0, USB_PIN_DM, USB_GPIO_IN_FLOAT);
    pin_mode(0, USB_PIN_DP, USB_GPIO_IN_FLOAT);

    /* USBCLK = PLL / 1.5 (RCC_CFGR.USBPRE = 0) is set up by sys.c. */
    USB_RCC_APB1ENR |= USB_RCC_APB1_USB;
    USB_RCC_APB1RSTR &= ~USB_RCC_APB1_USB;
    /* Power up the transceiver, wait tSTARTUP (1 us), release FRES
     * (RM0008 23.4.2). Interrupt masks stay 0: ISTR flags latch anyway. */
    USB_CNTR = USB_CNTR_FRES;
    sys_delay_ms(1);
    USB_CNTR = 0;
    USB_ISTR = 0;
    bus_reset();
    pullup(1);
}

void usbd_disconnect(void)
{
    if (irq_on) {
        irq_on = 0;
        NVIC_ICER(IRQ_USB_LP_CAN1_RX0 / 32u) = USB_IRQ_BIT;
        USB_CNTR &= (uint16_t)~(USB_CNTR_CTRM | USB_CNTR_RESETM);
    }
    detach();
}

int usbd_configured(void)
{
    return config != 0;
}

static void ctrl_send(void)
{
    uint32_t n = ctl_left < EP0_SIZE ? ctl_left : EP0_SIZE;
    pma_write(BUF_CTRL_TX, ctl_ptr, n);
    USB_COUNT_TX(EP_CTRL) = (uint16_t)n;
    SET_TX(EP_CTRL, USB_STAT_VALID);
    ctl_ptr += n;
    ctl_left -= n;
    if (n < EP0_SIZE || (ctl_left == 0 && !ctl_zlp))
        ctl_ptr = 0;                    /* last packet queued */
}

static const uint8_t *get_descriptor(uint32_t type, uint32_t idx, uint32_t *len)
{
    if (type == 1) {
        *len = 18;
        return desc->device;
    }
    if (type == 2) {
        *len = desc->config_len;
        return desc->config;
    }
    if (type == 3) {
        if (idx == 0) {
            *len = sizeof(langid);
            return langid;
        }
        if (idx > desc->num_strings)
            return 0;
        const char *s = desc->strings[idx - 1];
        uint32_t n = 2;
        while (*s && n < sizeof(ctl_buf)) {
            ctl_buf[n++] = (uint8_t)*s++;
            ctl_buf[n++] = 0;
        }
        ctl_buf[0] = (uint8_t)n;
        ctl_buf[1] = 3;
        *len = n;
        return ctl_buf;
    }
    return 0;   /* device qualifier etc.: STALL (full-speed only device) */
}

static void config_data_eps(void)
{
    in_busy = 0;
    out_len = -1;
    USB_COUNT_RX(EP_DATA) = USB_RX_BLOCKS32(DATA_SIZE);
    ep_open(EP_DATA, USB_EP_BULK, USB_STAT_VALID, USB_STAT_NAK);
}

static void handle_setup(void)
{
    uint8_t s[8];
    pma_read(BUF_CTRL_RX, s, 8);
    ep_clear_ctr(EP_CTRL, USB_EP_CTR_RX);

    /* Drop whatever the last control transfer had queued. */
    SET_TX(EP_CTRL, USB_STAT_NAK);
    ctl_ptr = 0;

    uint32_t req = s[1];
    uint32_t wvalue = s[2] | ((uint32_t)s[3] << 8);
    uint32_t wlength = s[6] | ((uint32_t)s[7] << 8);
    const uint8_t *data = 0;
    uint32_t len = 0;

    if ((s[0] & 0x60) != 0)             /* class/vendor: none supported */
        goto stall;

    switch (req) {
    case 0x00:                          /* GET_STATUS: all zero */
        data = zeros;
        len = 2;
        break;
    case 0x01:                          /* CLEAR_FEATURE */
        if ((s[0] & 0x1F) == 2) {       /* ENDPOINT_HALT: reset data toggle */
            uint32_t v = USB_EPR(EP_DATA);
            if (s[4] & 0x80) {
                uint32_t st = (v & USB_EP_STAT_TX) == USB_TX_STAT(USB_STAT_STALL)
                                  ? USB_TX_STAT(USB_STAT_NAK) : (v & USB_EP_STAT_TX);
                ep_set(EP_DATA, USB_EP_DTOG_TX | USB_EP_STAT_TX, st);
            } else {
                uint32_t st = (v & USB_EP_STAT_RX) == USB_RX_STAT(USB_STAT_STALL)
                                  ? USB_RX_STAT(USB_STAT_VALID) : (v & USB_EP_STAT_RX);
                ep_set(EP_DATA, USB_EP_DTOG_RX | USB_EP_STAT_RX, st);
            }
        }
        goto status;
    case 0x03:                          /* SET_FEATURE: accept, no effect */
        goto status;
    case 0x05:                          /* SET_ADDRESS: after status stage */
        pending_addr = wvalue & 0x7F;
        goto status;
    case 0x06:                          /* GET_DESCRIPTOR */
        data = get_descriptor(wvalue >> 8, wvalue & 0xFF, &len);
        if (!data)
            goto stall;
        break;
    case 0x08:                          /* GET_CONFIGURATION */
        ctl_buf[0] = config;
        data = ctl_buf;
        len = 1;
        break;
    case 0x09:                          /* SET_CONFIGURATION */
        if (wvalue > 1)
            goto stall;
        config = (uint8_t)wvalue;
        if (config)
            config_data_eps();
        goto status;
    case 0x0A:                          /* GET_INTERFACE: alt 0 */
        data = zeros;
        len = 1;
        break;
    case 0x0B:                          /* SET_INTERFACE: alt 0 only */
        if (wvalue != 0)
            goto stall;
        goto status;
    default:
        goto stall;
    }

    /* IN data stage starting with DATA1; EP0 OUT armed for the
     * zero-length status stage (DATA1). The hardware already sets both
     * toggles this way on SETUP; doing it again costs nothing. */
    if (len > wlength)
        len = wlength;
    ctl_zlp = len < wlength;
    ctl_left = (uint16_t)len;
    ctl_ptr = data;
    ep_set(EP_CTRL, USB_EP_DTOG_TX | USB_EP_DTOG_RX | USB_EP_STAT_RX,
           USB_EP_DTOG_TX | USB_EP_DTOG_RX | USB_RX_STAT(USB_STAT_VALID));
    ctrl_send();
    return;

status:
    /* No data stage: zero-length IN status packet, DATA1. */
    USB_COUNT_TX(EP_CTRL) = 0;
    ep_set(EP_CTRL, USB_EP_DTOG_TX | USB_EP_STAT_TX | USB_EP_STAT_RX,
           USB_EP_DTOG_TX | USB_TX_STAT(USB_STAT_VALID) | USB_RX_STAT(USB_STAT_VALID));
    return;

stall:
    /* A control endpoint still takes the next SETUP while stalled. */
    ep_set(EP_CTRL, USB_EP_STAT_TX | USB_EP_STAT_RX,
           USB_TX_STAT(USB_STAT_STALL) | USB_RX_STAT(USB_STAT_STALL));
}

static void poll_core(void);

void usbd_poll(void)
{
    if (!irq_on)
        poll_core();
}

/* Correct transfer (CTR) and bus reset raise USB_LP; one event per call, and
 * the interrupt stays pending while another is waiting. */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    poll_core();
}

void usbd_irq_enable(uint8_t prio)
{
    NVIC_IPR8(IRQ_USB_LP_CAN1_RX0) = prio;
    irq_on = 1;
    USB_CNTR |= USB_CNTR_CTRM | USB_CNTR_RESETM;
    NVIC_ISER(IRQ_USB_LP_CAN1_RX0 / 32u) = USB_IRQ_BIT;
}

static void poll_core(void)
{
    uint32_t st = USB_ISTR;

    if (st & USB_ISTR_RESET) {
        USB_ISTR = (uint16_t)~USB_ISTR_RESET;
        bus_reset();
        return;
    }
    /* Suspend/resume, SOF, errors: nothing to do. We never set FSUSP or
     * LP_MODE; the device is bus powered and keeps running. Clear only the
     * flags seen, so one raised meanwhile is not lost. */
    if (st & (USB_ISTR_FLAGS & ~USB_ISTR_RESET))
        USB_ISTR = (uint16_t)~(st & USB_ISTR_FLAGS);

    /* EP0: IN completion before SETUP, so a stale IN ACK from an aborted
     * transfer runs before the new SETUP is handled, and SET_ADDRESS takes
     * effect before the next request. */
    uint32_t v = USB_EPR(EP_CTRL);
    if (v & USB_EP_CTR_TX) {
        ep_clear_ctr(EP_CTRL, USB_EP_CTR_TX);
        if (pending_addr) {
            USB_DADDR = USB_DADDR_EF | pending_addr;
            pending_addr = 0;
        }
        if (ctl_ptr)
            ctrl_send();
    }
    if (v & USB_EP_CTR_RX) {
        if (v & USB_EP_SETUP) {             /* valid while CTR_RX is set */
            handle_setup();
        } else {
            /* status OUT (or an OUT we don't expect): accept, re-arm */
            ep_clear_ctr(EP_CTRL, USB_EP_CTR_RX);
            ctl_ptr = 0;
            SET_RX(EP_CTRL, USB_STAT_VALID);
        }
    }

    v = USB_EPR(EP_DATA);
    if (v & USB_EP_CTR_TX) {
        ep_clear_ctr(EP_DATA, USB_EP_CTR_TX);
        in_busy = 0;
    }
    if (v & USB_EP_CTR_RX) {
        /* The hardware set STAT_RX to NAK: the endpoint NAKs until
         * usbd_out_release(). */
        uint32_t n = USB_COUNT_RX(EP_DATA) & USB_COUNT_MASK;
        if (n > DATA_SIZE)
            n = DATA_SIZE;
        pma_read(BUF_DATA_RX, out_buf, n);
        ep_clear_ctr(EP_DATA, USB_EP_CTR_RX);
        out_len = (int16_t)n;
    }
}

int usbd_in_busy(void)
{
    return in_busy;
}

void usbd_in_write(const uint8_t *p, uint32_t n)
{
    uint32_t l = usb_lock();
    pma_write(BUF_DATA_TX, p, n);
    USB_COUNT_TX(EP_DATA) = (uint16_t)n;
    in_busy = 1;
    SET_TX(EP_DATA, USB_STAT_VALID);
    usb_unlock(l);
}

void usbd_in_cancel(void)
{
    uint32_t l = usb_lock();
    SET_TX(EP_DATA, USB_STAT_NAK);
    /* Drop a completion that raced the cancel so it can't mark the next
     * packet as sent. */
    ep_clear_ctr(EP_DATA, USB_EP_CTR_TX);
    in_busy = 0;
    usb_unlock(l);
}

int usbd_out_len(void)
{
    return out_len;
}

const volatile uint8_t *usbd_out_data(void)
{
    return out_buf;
}

void usbd_out_release(void)
{
    uint32_t l = usb_lock();
    out_len = -1;
    if (config)
        SET_RX(EP_DATA, USB_STAT_VALID);
    usb_unlock(l);
}
