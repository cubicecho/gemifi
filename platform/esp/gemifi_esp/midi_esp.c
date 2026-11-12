/*
 * midi_esp.c: MIDI in/out (gm_hal.h) over the transport chosen by
 * GM_MIDI_TRANSPORT: USB MIDI (TinyUSB, S2/S3), raw MIDI bytes on UART0, or
 * BLE-MIDI (NimBLE, ESP32 / S3).
 *
 * hal_midi_send() queues whole messages in a ring; hal_midi_poll() hands
 * what the transport accepts to it and feeds what it receives to the core:
 * USB-MIDI event packets to gm_midi_rx_usb() (so the cable is kept), serial
 * and BLE bytes to gm_midi_rx_byte(). Both run in the main loop only.
 *
 * USB: our own descriptors (one MIDI interface, cable 0 "<name>"); with a
 * DAW port (GM_USB_DAW_PORT, core/README.md "DAW feedback") a second cable
 * "<name> DAW" on the same endpoint pair, with a TX ring of its own.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "gm_esp.h"
#include "gm_hal.h"
#include "gm_controller.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef GM_USB_DAW_PORT
#define GM_USB_DAW_PORT 0
#endif

#if GM_MIDI_TRANSPORT == GM_MIDI_USB
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#elif GM_MIDI_TRANSPORT == GM_MIDI_SERIAL
#include "driver/uart.h"
#include "soc/uart_pins.h"
#elif GM_MIDI_TRANSPORT == GM_MIDI_BLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#define TXQ_SIZE 1024u                      /* power of 2 */
#define RXQ_SIZE 512u                       /* BLE only: callback -> loop */

static uint8_t txq[TXQ_SIZE];
static uint16_t tx_head, tx_tail;           /* head: write, tail: read */

static uint32_t txq_used(void)
{
    return (uint16_t)(tx_head - tx_tail);
}

/* Contiguous queued bytes at the tail. */
static __attribute__((unused)) uint32_t txq_peek(const uint8_t **p)
{
    uint32_t at = tx_tail & (TXQ_SIZE - 1u);
    uint32_t n = txq_used();
    *p = &txq[at];
    return n < TXQ_SIZE - at ? n : TXQ_SIZE - at;
}

void hal_midi_send(const uint8_t *msg, uint32_t n)
{
    if (n > TXQ_SIZE - txq_used())
        return;                             /* no room: drop the message */
    for (uint32_t i = 0; i < n; i++)
        txq[(tx_head + i) & (TXQ_SIZE - 1u)] = msg[i];
    tx_head = (uint16_t)(tx_head + n);
}

#if GM_MIDI_TRANSPORT == GM_MIDI_USB && GM_USB_DAW_PORT
#define DAWQ_SIZE 256u                      /* power of 2 */
static uint8_t dawq[DAWQ_SIZE];
static uint16_t daw_head, daw_tail;

void hal_midi_send_cable(uint8_t cable, const uint8_t *msg, uint32_t n)
{
    if (cable == 0) {
        hal_midi_send(msg, n);
        return;
    }
    if (n > DAWQ_SIZE - (uint16_t)(daw_head - daw_tail))
        return;
    for (uint32_t i = 0; i < n; i++)
        dawq[(daw_head + i) & (DAWQ_SIZE - 1u)] = msg[i];
    daw_head = (uint16_t)(daw_head + n);
}
#else
/* One port: everything goes out on it. */
void hal_midi_send_cable(uint8_t cable, const uint8_t *msg, uint32_t n)
{
    (void)cable;
    hal_midi_send(msg, n);
}
#endif

/* Without the core (firmware/esp/hello): USB SysEx bytes to gm_midi_rx_byte,
 * as before there were cables. The core's gm_midi_rx_usb replaces it. */
__attribute__((weak)) void gm_midi_rx_usb(const uint8_t ev[4])
{
    uint32_t cin = ev[0] & 0x0Fu;
    if (cin < 4 || cin > 7)
        return;
    uint32_t k = (cin == 5) ? 1 : (cin == 6) ? 2 : 3;
    for (uint32_t i = 0; i < k; i++)
        gm_midi_rx_byte(ev[1 + i]);
}

/* ======================================================================= */
#if GM_MIDI_TRANSPORT == GM_MIDI_USB

#define N_CABLES (GM_USB_DAW_PORT ? 2 : 1)
#define EP_OUT 0x01
#define EP_IN 0x81
#define EP_SIZE 64
enum { STR_LANG, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_PORT, STR_DAW, STR_COUNT };

static const tusb_desc_device_t dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,                      /* per interface */
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = GM_USB_VID,
    .idProduct = GM_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

/* USB-MIDI 1.0: the audio control and MIDI streaming interfaces, a jack
 * pair per cable, one bulk endpoint each way. */
#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_HEAD_LEN + N_CABLES * TUD_MIDI_DESC_JACK_LEN + \
                 2 * TUD_MIDI_DESC_EP_LEN(N_CABLES))
static const uint8_t cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CFG_LEN, 0x00, 100),
    TUD_MIDI_DESC_HEAD(0, STR_PORT, N_CABLES),
    TUD_MIDI_DESC_JACK_DESC(1, STR_PORT),
#if GM_USB_DAW_PORT
    TUD_MIDI_DESC_JACK_DESC(2, STR_DAW),
#endif
    TUD_MIDI_DESC_EP(EP_OUT, EP_SIZE, N_CABLES),
    TUD_MIDI_JACKID_IN_EMB(1),
#if GM_USB_DAW_PORT
    TUD_MIDI_JACKID_IN_EMB(2),
#endif
    TUD_MIDI_DESC_EP(EP_IN, EP_SIZE, N_CABLES),
    TUD_MIDI_JACKID_OUT_EMB(1),
#if GM_USB_DAW_PORT
    TUD_MIDI_JACKID_OUT_EMB(2),
#endif
};
_Static_assert(sizeof cfg_desc == CFG_LEN, "USB configuration descriptor length");

#ifndef GM_DAW_PORT_NAME
#define GM_DAW_PORT_NAME GM_DEVICE_NAME " DAW"
#endif
static char usb_serial[25];
static const char *usb_strings[STR_COUNT] = {
    [STR_LANG] = (const char[]){ 0x09, 0x04 },  /* English (US) */
    [STR_MANUF] = "gemifi",
    [STR_PRODUCT] = "gemifi (" GM_DEVICE_NAME ")",
    [STR_SERIAL] = usb_serial,
    [STR_PORT] = GM_DEVICE_NAME,
    [STR_DAW] = GM_DAW_PORT_NAME,
};

void gm_esp_midi_begin(void)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t uid[12];
    hal_uid(uid);
    for (int i = 0; i < 12; i++) {
        usb_serial[2 * i] = hex[uid[i] >> 4];
        usb_serial[2 * i + 1] = hex[uid[i] & 15];
    }
    tinyusb_config_t c = TINYUSB_DEFAULT_CONFIG();
    c.descriptor.device = &dev_desc;
    c.descriptor.string = usb_strings;
    c.descriptor.string_count = STR_COUNT;
    c.descriptor.full_speed_config = cfg_desc;
    tinyusb_driver_install(&c);
}

static void tp_rx(void)
{
    uint8_t ev[4];
    for (int k = 0; k < 64 && tud_midi_available() && tud_midi_packet_read(ev); k++)
        gm_midi_rx_usb(ev);
}

#if GM_USB_DAW_PORT
/* Cable 1's ring, to TinyUSB. It is only entered when cable 0's ring is
 * empty and left when it is empty itself, so TinyUSB's stream writer (one
 * per interface) never sees two cables' messages interleaved mid-message. */
static uint8_t tx_cable;

static int daw_tx(void)
{
    while (daw_tail != daw_head) {
        uint32_t at = daw_tail & (DAWQ_SIZE - 1u);
        uint32_t n = (uint16_t)(daw_head - daw_tail);
        if (n > DAWQ_SIZE - at)
            n = DAWQ_SIZE - at;
        uint32_t w = tud_midi_stream_write(1, &dawq[at], n);
        daw_tail = (uint16_t)(daw_tail + w);
        if (w < n)
            return 0;                       /* FIFO full: stay on cable 1 */
    }
    return 1;
}
#endif

static void tp_tx(void)
{
    uint32_t n;
    if (!tud_mounted()) {
        tx_tail = tx_head;                  /* nobody listening */
#if GM_USB_DAW_PORT
        daw_tail = daw_head;
#endif
        return;
    }
#if GM_USB_DAW_PORT
    if (tx_cable == 1) {
        if (!daw_tx())
            return;
        tx_cable = 0;
    }
#endif
    const uint8_t *p;
    while ((n = txq_peek(&p)) > 0) {
        uint32_t w = tud_midi_stream_write(0, p, n);
        tx_tail = (uint16_t)(tx_tail + w);
        if (w < n)
            return;                         /* endpoint FIFO full */
    }
#if GM_USB_DAW_PORT
    if (daw_tail != daw_head) {
        tx_cable = 1;
        if (daw_tx())
            tx_cable = 0;
    }
#endif
}

static int tp_idle(void)
{
#if GM_USB_DAW_PORT
    if (daw_tail != daw_head)
        return 0;
#endif
    return txq_used() == 0;
}

/* ======================================================================= */
#elif GM_MIDI_TRANSPORT == GM_MIDI_SERIAL

/* UART0 (the board's USB-UART), its default pins. The RX buffer holds a
 * whole FW_DATA frame; sdkconfig.serial keeps the console and logs off it. */
#define UART UART_NUM_0

void gm_esp_midi_begin(void)
{
    const uart_config_t c = {
        .baud_rate = GM_MIDI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART, 1024, 1024, 0, NULL, 0);
    uart_param_config(UART, &c);
    /* the board's USB-UART pins: the ROM sets them up, but with no console
     * (sdkconfig.serial) nothing after it does */
    uart_set_pin(UART, U0TXD_GPIO_NUM, U0RXD_GPIO_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static void tp_rx(void)
{
    uint8_t b[64];
    int n;
    for (int k = 0; k < 8 && (n = uart_read_bytes(UART, b, sizeof b, 0)) > 0; k++)
        for (int i = 0; i < n; i++)
            gm_midi_rx_byte(b[i]);
}

static void tp_tx(void)
{
    const uint8_t *p;
    uint32_t n = txq_peek(&p);
    size_t room = 0;
    if (n && uart_get_tx_buffer_free_size(UART, &room) == ESP_OK && room > 0) {
        if (n > room)
            n = (uint32_t)room;
        int w = uart_write_bytes(UART, p, n);
        if (w > 0)
            tx_tail = (uint16_t)(tx_tail + (uint32_t)w);
    }
}

static int tp_idle(void)
{
    return txq_used() == 0 && uart_wait_tx_done(UART, 0) == ESP_OK;   /* the FIFO drained */
}

/* ======================================================================= */
#elif GM_MIDI_TRANSPORT == GM_MIDI_BLE

/* BLE-MIDI 1.0: service 03B80E5A-..., one characteristic (notify for
 * device -> host, write-without-response for host -> device, read gives
 * nothing). Each packet: header (0x80 | ts_hi), then per message a
 * timestamp byte (0x80 | ts_lo) and the message; a SysEx continues in the
 * next packet after its header, and its F7 gets a timestamp of its own.
 * NimBLE's host runs in its own task; the write callback queues the bytes
 * for the main loop. UUIDs are little-endian here. */
static const ble_uuid128_t ble_svc_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t ble_chr_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static uint16_t ble_chr_handle;
static volatile uint16_t ble_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t ble_addr_type;
static uint8_t rxq[RXQ_SIZE];
static volatile uint16_t rx_head, rx_tail;  /* BLE task writes, loop reads */

static void rxq_put(uint8_t b)
{
    if ((uint16_t)(rx_head - rx_tail) < RXQ_SIZE) {
        rxq[rx_head & (RXQ_SIZE - 1u)] = b;
        rx_head = (uint16_t)(rx_head + 1);
    }
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return 0;                           /* an empty value */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    uint8_t p[256];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, p, sizeof p, &n) != 0 || n < 2 || !(p[0] & 0x80))
        return 0;
    for (uint32_t i = 1; i < n; i++) {
        if (p[i] & 0x80) {                  /* timestamp, maybe a status */
            if (i + 1 < n && (p[i + 1] & 0x80))
                rxq_put(p[++i]);
        } else {
            rxq_put(p[i]);                  /* data / SysEx continuation */
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def ble_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &ble_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &ble_chr_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &ble_chr_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &ble_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields r = { 0 };    /* the name in the scan response: no room left above */
    r.name = (const uint8_t *)GM_DEVICE_NAME;
    r.name_len = (uint8_t)strlen(GM_DEVICE_NAME);
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params a = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &a, gap_event, NULL);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0)
            ble_conn = ev->connect.conn_handle;
        else
            advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_conn = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (ble_conn == BLE_HS_CONN_HANDLE_NONE)
            advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void ble_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &ble_addr_type);
    advertise();
}

static void ble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();                      /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void gm_esp_midi_begin(void)
{
    if (nimble_port_init() != ESP_OK)
        return;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(ble_svcs);
    ble_gatts_add_svcs(ble_svcs);
    ble_svc_gap_device_name_set(GM_DEVICE_NAME);
    ble_att_set_preferred_mtu(185);
    nimble_port_freertos_init(ble_host_task);
}

static uint32_t msg_len(uint8_t st)
{
    if (st < 0xF0)
        return (st & 0xE0) == 0xC0 ? 2 : 3;     /* Cn, Dn: 2 bytes */
    if (st == 0xF1 || st == 0xF3)
        return 2;
    if (st == 0xF2)
        return 3;
    return 1;
}

/* One notification from the queue; returns 0 when nothing was sent. */
static int ble_send_packet(uint16_t conn)
{
    uint8_t pkt[182];
    uint32_t cap = ble_att_mtu(conn);
    cap = cap > 23 ? cap - 3 : 20;
    if (cap > sizeof(pkt))
        cap = sizeof(pkt);
    uint32_t ts = hal_millis() & 0x1FFF;
    uint32_t n = 0;
    pkt[n++] = (uint8_t)(0x80 | (ts >> 7));
    uint16_t tail = tx_tail;
    while ((uint16_t)(tx_head - tail)) {
        uint32_t avail = (uint16_t)(tx_head - tail);
        uint8_t b = txq[tail & (TXQ_SIZE - 1u)];
        uint32_t len = 1, need;
        if (b & 0x80) {
            len = b == 0xF0 || b == 0xF7 ? 1 : msg_len(b);
            if (len > avail)
                break;
            need = 1 + len;                 /* timestamp + message */
        } else {
            need = 1;                       /* SysEx data byte */
        }
        if (n + need > cap)
            break;
        if (b & 0x80)
            pkt[n++] = (uint8_t)(0x80 | (ts & 0x7F));
        for (uint32_t i = 0; i < len; i++)
            pkt[n++] = txq[(tail + i) & (TXQ_SIZE - 1u)];
        tail = (uint16_t)(tail + len);
    }
    if (n == 1)
        return 0;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, (uint16_t)n);
    if (!om || ble_gatts_notify_custom(conn, ble_chr_handle, om) != 0)
        return 0;                           /* no buffers now: the bytes stay queued */
    tx_tail = tail;
    return 1;
}

static void tp_rx(void)
{
    while (rx_tail != rx_head) {
        uint8_t b = rxq[rx_tail & (RXQ_SIZE - 1u)];
        rx_tail = (uint16_t)(rx_tail + 1);
        gm_midi_rx_byte(b);
    }
}

static void tp_tx(void)
{
    uint16_t conn = ble_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        tx_tail = tx_head;
        return;
    }
    for (int k = 0; k < 4 && ble_send_packet(conn); k++) {
    }
}

static int tp_idle(void)
{
    return txq_used() == 0;
}

#endif

/* ======================================================================= */

void hal_midi_poll(void)
{
    tp_rx();
    tp_tx();
}

void hal_midi_flush(uint32_t timeout_ms)
{
    uint32_t t0 = hal_millis();
    for (;;) {                              /* no rx here: the caller may be */
        tp_tx();                            /* handling a received frame     */
        if (tp_idle() || hal_millis() - t0 >= timeout_ms)
            return;
        vTaskDelay(1);                      /* let the USB / BLE task send */
    }
}
