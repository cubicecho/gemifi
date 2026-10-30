/*
 * usbd.c: polled USB device core for the NUC1xx USBD. See usbd.h.
 *
 * Register-level references: TRM V2.02 section 5.4 (USB device controller),
 * the stock firmware's USB init (analysis/excerpts-hw.txt) and the flow of
 * TinyUSB's dcd_nuc120.c / Nuvoton's BSP usbd.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "usbd.h"
#include "nuc1xx.h"
#include "sys.h"

#define SLOT_CTRL_IN  0
#define SLOT_CTRL_OUT 1
#define SLOT_DATA_IN  2
#define SLOT_DATA_OUT 3
#define NUM_SLOTS     6

#define BUF_SETUP     0x00
#define BUF_CTRL_IN   0x08
#define BUF_CTRL_OUT  0x48
#define BUF_DATA_IN   0x88
#define BUF_DATA_OUT  0xC8

#define EP0_SIZE  64
#define DATA_SIZE 64
#define STR_MAX   40

static const usbd_desc_t *desc;
static const uint8_t *ctl_ptr;      /* NULL: no IN data stage pending */
static uint16_t ctl_left;
static uint8_t ctl_zlp;             /* reply shorter than wLength */
static uint8_t pending_addr;
static uint8_t config;
static uint8_t in_busy;
static int16_t out_len;             /* set to -1 by bus_reset() */
static uint8_t ctl_buf[2 + 2 * STR_MAX];

static const uint8_t zeros[2];
static const uint8_t langid[4] = { 4, 3, 0x09, 0x04 };   /* en-US */

static void sram_write(uint32_t off, const uint8_t *p, uint32_t n)
{
    /* ATTR.BYTEM = 1: the CPU must access USB SRAM with byte accesses
     * (TRM 5.4.6, USB_ATTR[10]). */
    volatile uint8_t *d = USBD_SRAM + off;
    while (n--)
        *d++ = *p++;
}

static void bus_reset(void)
{
    USBD_FADDR = 0;
    pending_addr = 0;
    config = 0;
    ctl_ptr = 0;
    in_busy = 0;
    out_len = -1;

    USBD_STBUFSEG = BUF_SETUP;
    for (uint32_t s = 0; s < NUM_SLOTS; s++) {
        USBD_CFG(s) = 0;                /* disabled */
        USBD_CFGP(s) = USBD_CFGP_CLRRDY; /* also clears SSTALL */
    }
    /* EP0 = two slots, both endpoint number 0. CSTALL: the hardware drops a
     * STALL by itself when the next SETUP arrives (TRM 5.4.6 USB_CFGx[9]). */
    USBD_BUFSEG(SLOT_CTRL_IN) = BUF_CTRL_IN;
    USBD_CFG(SLOT_CTRL_IN) = USBD_CFG_CSTALL | USBD_CFG_STATE_IN | USBD_CFG_EPNUM(0);
    USBD_BUFSEG(SLOT_CTRL_OUT) = BUF_CTRL_OUT;
    USBD_CFG(SLOT_CTRL_OUT) = USBD_CFG_CSTALL | USBD_CFG_STATE_OUT | USBD_CFG_EPNUM(0);
    USBD_BUFSEG(SLOT_DATA_IN) = BUF_DATA_IN;
    USBD_BUFSEG(SLOT_DATA_OUT) = BUF_DATA_OUT;
}

void usbd_init(const usbd_desc_t *d)
{
    desc = d;
    CLK_APBCLK |= APBCLK_USBD_EN;
    /* Pulse the block reset: CPU_RST does not reset the USBD, so an earlier
     * stage may have left it enumerated (bootloader-design.md 3.3). */
    GCR_IPRSTC2 |= IPRSTC2_USBD_RST;
    sys_delay_ms(1);
    GCR_IPRSTC2 &= ~IPRSTC2_USBD_RST;

    USBD_DRVSE0 = USBD_DRVSE0_DRVSE0;   /* detached while we set up */
    USBD_ATTR = USBD_ATTR_INIT;
    bus_reset();
    /* Interrupt enables also gate the status flags we poll; the NVIC line
     * stays disabled. */
    USBD_INTEN = USBD_INTEN_BUS_IE | USBD_INTEN_USB_IE;
    USBD_INTSTS = 0xFFFFFFFFu;
    /* Hold SE0 ~10 ms so the host notices the disconnect. */
    sys_delay_ms(10);
    USBD_DRVSE0 = 0;
}

void usbd_disconnect(void)
{
    USBD_DRVSE0 = USBD_DRVSE0_DRVSE0;
    config = 0;
}

int usbd_configured(void)
{
    return config != 0;
}

static void ctrl_send(void)
{
    uint32_t n = ctl_left < EP0_SIZE ? ctl_left : EP0_SIZE;
    sram_write(BUF_CTRL_IN, ctl_ptr, n);
    USBD_MXPLD(SLOT_CTRL_IN) = n;       /* arms the IN slot */
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
    USBD_CFGP(SLOT_DATA_IN) = USBD_CFGP_CLRRDY;
    USBD_CFG(SLOT_DATA_IN) = USBD_CFG_STATE_IN | USBD_CFG_EPNUM(1);
    USBD_CFGP(SLOT_DATA_OUT) = USBD_CFGP_CLRRDY;
    USBD_CFG(SLOT_DATA_OUT) = USBD_CFG_STATE_OUT | USBD_CFG_EPNUM(1);
    in_busy = 0;
    out_len = -1;
    USBD_MXPLD(SLOT_DATA_OUT) = DATA_SIZE;
}

static void handle_setup(void)
{
    uint8_t s[8];
    for (uint32_t i = 0; i < 8; i++)
        s[i] = USBD_SRAM[BUF_SETUP + i];

    /* Drop whatever the last control transfer had queued (TRM 5.4.6
     * USB_CFGPx.CLRRDY); writing 0 to SSTALL also lifts a stall. */
    USBD_CFGP(SLOT_CTRL_IN) = USBD_CFGP_CLRRDY;
    USBD_CFGP(SLOT_CTRL_OUT) = USBD_CFGP_CLRRDY;
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
            uint32_t slot = (s[4] & 0x80) ? SLOT_DATA_IN : SLOT_DATA_OUT;
            USBD_CFG(slot) &= ~USBD_CFG_DSQ_SYNC;
            USBD_CFGP(slot) &= ~USBD_CFGP_SSTALL;
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
        data = &config;
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

    /* IN data stage, starting with DATA1. */
    if (len > wlength)
        len = wlength;
    ctl_zlp = len < wlength;
    ctl_left = (uint16_t)len;
    ctl_ptr = data;
    USBD_CFG(SLOT_CTRL_IN) |= USBD_CFG_DSQ_SYNC;
    ctrl_send();
    /* Arm EP0 OUT for the zero-length status stage (DATA1). */
    USBD_CFG(SLOT_CTRL_OUT) |= USBD_CFG_DSQ_SYNC;
    USBD_MXPLD(SLOT_CTRL_OUT) = 0;
    return;

status:
    /* No data stage: zero-length IN status packet, DATA1. */
    USBD_CFG(SLOT_CTRL_IN) |= USBD_CFG_DSQ_SYNC;
    USBD_MXPLD(SLOT_CTRL_IN) = 0;
    return;

stall:
    USBD_CFGP(SLOT_CTRL_IN) = USBD_CFGP_SSTALL;
    USBD_CFGP(SLOT_CTRL_OUT) = USBD_CFGP_SSTALL;
}

void usbd_poll(void)
{
    uint32_t st = USBD_INTSTS;

    /* FLDET (VBUS) and suspend/resume need no action: the device is bus
     * powered, and we never turn the PHY or controller off on suspend. */
    if (st & USBD_INTSTS_BUS) {
        USBD_INTSTS = USBD_INTSTS_BUS;
        uint32_t a = USBD_ATTR;
        if (a & USBD_ATTR_USBRST) {
            USBD_ATTR |= USBD_ATTR_USB_EN | USBD_ATTR_PHY_EN;
            bus_reset();
        }
    }

    if (st & USBD_INTSTS_USB) {
        /* Endpoint events before SETUP: a stale IN ACK from an aborted
         * transfer must not run after the new SETUP has been handled, and
         * SET_ADDRESS must take effect before the next request. */
        if (st & USBD_INTSTS_EPEVT(SLOT_CTRL_IN)) {
            USBD_INTSTS = USBD_INTSTS_EPEVT(SLOT_CTRL_IN);
            if (pending_addr) {
                USBD_FADDR = pending_addr;
                pending_addr = 0;
            }
            if (ctl_ptr)
                ctrl_send();
        }
        if (st & USBD_INTSTS_EPEVT(SLOT_CTRL_OUT))
            USBD_INTSTS = USBD_INTSTS_EPEVT(SLOT_CTRL_OUT);
        if (st & USBD_INTSTS_EPEVT(SLOT_DATA_IN)) {
            USBD_INTSTS = USBD_INTSTS_EPEVT(SLOT_DATA_IN);
            in_busy = 0;
        }
        if (st & USBD_INTSTS_EPEVT(SLOT_DATA_OUT)) {
            USBD_INTSTS = USBD_INTSTS_EPEVT(SLOT_DATA_OUT);
            out_len = (int16_t)(USBD_MXPLD(SLOT_DATA_OUT) & 0x1FF);
        }
        if (st & USBD_INTSTS_SETUP) {
            USBD_INTSTS = USBD_INTSTS_SETUP;
            handle_setup();
        }
        USBD_INTSTS = USBD_INTSTS_USB;
    }
}

int usbd_in_busy(void)
{
    return in_busy;
}

void usbd_in_write(const uint8_t *p, uint32_t n)
{
    sram_write(BUF_DATA_IN, p, n);
    in_busy = 1;
    USBD_MXPLD(SLOT_DATA_IN) = n;
}

void usbd_in_cancel(void)
{
    USBD_CFGP(SLOT_DATA_IN) = USBD_CFGP_CLRRDY;
    in_busy = 0;
}

int usbd_out_len(void)
{
    return out_len;
}

const volatile uint8_t *usbd_out_data(void)
{
    return USBD_SRAM + BUF_DATA_OUT;
}

void usbd_out_release(void)
{
    out_len = -1;
    if (config)
        USBD_MXPLD(SLOT_DATA_OUT) = DATA_SIZE;
}
