/*
 * midi_esp.cpp: MIDI in/out (gm_hal.h) over the transport chosen by
 * GM_MIDI_TRANSPORT: USB MIDI (TinyUSB, S2/S3), raw MIDI bytes on UART0, or
 * BLE-MIDI (ESP32 / S3, the core's BLE library).
 *
 * hal_midi_send() queues whole messages in a ring; hal_midi_poll() hands
 * what the transport accepts to it and feeds received bytes to
 * gm_midi_rx_byte(). Both run in the main loop only.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#include "gm_hal.h"
#include "gm_controller.h"

#if GM_MIDI_TRANSPORT == GM_MIDI_USB
#include <USB.h>
#include <USBMIDI.h>
#elif GM_MIDI_TRANSPORT == GM_MIDI_BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
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

/* ======================================================================= */
#if GM_MIDI_TRANSPORT == GM_MIDI_USB

static USBMIDI usb_midi(GM_DEVICE_NAME);
static char usb_serial[25];

void gm_esp_midi_begin(void)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t uid[12];
    hal_uid(uid);
    for (int i = 0; i < 12; i++) {
        usb_serial[2 * i] = hex[uid[i] >> 4];
        usb_serial[2 * i + 1] = hex[uid[i] & 15];
    }
    USB.VID(GM_USB_VID);
    USB.PID(GM_USB_PID);
    USB.manufacturerName("gemifi");
    USB.productName(GM_DEVICE_NAME " (gemifi)");
    USB.serialNumber(usb_serial);
    usb_midi.begin();
    USB.begin();
}

static void tp_rx(void)
{
    uint8_t buf[64];
    uint32_t n;
    while (tud_midi_available() && (n = tud_midi_stream_read(buf, sizeof(buf))) > 0)
        for (uint32_t i = 0; i < n; i++)
            gm_midi_rx_byte(buf[i]);
}

static void tp_tx(void)
{
    uint32_t n;
    if (!tud_mounted()) {
        tx_tail = tx_head;                  /* nobody listening */
        return;
    }
    const uint8_t *p;
    while ((n = txq_peek(&p)) > 0) {
        uint32_t w = tud_midi_stream_write(0, p, n);
        tx_tail = (uint16_t)(tx_tail + w);
        if (w < n)
            break;                          /* endpoint FIFO full */
    }
}

static int tp_idle(void)
{
    return txq_used() == 0;
}

/* ======================================================================= */
#elif GM_MIDI_TRANSPORT == GM_MIDI_SERIAL

void gm_esp_midi_begin(void)
{
    Serial.setRxBufferSize(512);            /* a whole FW_DATA frame */
    Serial.begin(GM_MIDI_BAUD);
}

static void tp_rx(void)
{
    for (int k = 0; k < 512 && Serial.available() > 0; k++)
        gm_midi_rx_byte((uint8_t)Serial.read());
}

static void tp_tx(void)
{
    const uint8_t *p;
    uint32_t n = txq_peek(&p);
    if (n) {
        int room = Serial.availableForWrite();
        if (room > 0) {
            if (n > (uint32_t)room)
                n = (uint32_t)room;
            tx_tail = (uint16_t)(tx_tail + Serial.write(p, n));
        }
    }
}

static int tp_idle(void)
{
    if (txq_used())
        return 0;
    Serial.flush();                         /* UART FIFO drained */
    return 1;
}

/* ======================================================================= */
#elif GM_MIDI_TRANSPORT == GM_MIDI_BLE

/* BLE-MIDI 1.0: service 03B80E5A-..., one characteristic (notify for
 * device -> host, write-without-response for host -> device). Each packet:
 * header (0x80 | ts_hi), then per message a timestamp byte (0x80 | ts_lo)
 * and the message; a SysEx continues in the next packet after its header,
 * and its F7 gets a timestamp of its own. */
#define BLE_SVC "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
#define BLE_CHR "7772E5DB-3868-4112-A1A9-F2669D106BF3"

static BLEServer *ble_srv;
static BLECharacteristic *ble_chr;
static volatile uint8_t ble_conn;
static uint8_t rxq[RXQ_SIZE];
static volatile uint16_t rx_head, rx_tail;  /* BLE task writes, loop reads */

static void rxq_put(uint8_t b)
{
    if ((uint16_t)(rx_head - rx_tail) < RXQ_SIZE) {
        rxq[rx_head & (RXQ_SIZE - 1u)] = b;
        rx_head = (uint16_t)(rx_head + 1);
    }
}

class SrvCb : public BLEServerCallbacks {
    void onConnect(BLEServer *s) override
    {
        (void)s;
        ble_conn = 1;
    }
    void onDisconnect(BLEServer *s) override
    {
        ble_conn = 0;
        s->startAdvertising();
    }
};

class ChrCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override
    {
        String v = c->getValue();
        const uint8_t *p = (const uint8_t *)v.c_str();
        uint32_t n = v.length();
        if (n < 2 || !(p[0] & 0x80))
            return;
        for (uint32_t i = 1; i < n; i++) {
            if (p[i] & 0x80) {              /* timestamp, maybe a status */
                if (i + 1 < n && (p[i + 1] & 0x80))
                    rxq_put(p[++i]);
            } else {
                rxq_put(p[i]);              /* data / SysEx continuation */
            }
        }
    }
};

void gm_esp_midi_begin(void)
{
    BLEDevice::init(GM_DEVICE_NAME);
    BLEDevice::setMTU(185);
    ble_srv = BLEDevice::createServer();
    ble_srv->setCallbacks(new SrvCb());
    BLEService *svc = ble_srv->createService(BLE_SVC);
    ble_chr = svc->createCharacteristic(BLE_CHR,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE_NR |
        BLECharacteristic::PROPERTY_NOTIFY);
#if CONFIG_BLUEDROID_ENABLED
    ble_chr->addDescriptor(new BLE2902());
#endif
    ble_chr->setCallbacks(new ChrCb());
    svc->start();
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SVC);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();
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
static int ble_send_packet(void)
{
    uint8_t pkt[182];
    uint32_t cap = ble_srv->getPeerMTU(ble_srv->getConnId());
    cap = cap > 23 ? cap - 3 : 20;
    if (cap > sizeof(pkt))
        cap = sizeof(pkt);
    uint32_t ts = millis() & 0x1FFF;
    uint32_t n = 0;
    pkt[n++] = (uint8_t)(0x80 | (ts >> 7));
    while (txq_used()) {
        uint8_t b = txq[tx_tail & (TXQ_SIZE - 1u)];
        uint32_t len = 1, need;
        if (b & 0x80) {
            len = b == 0xF0 || b == 0xF7 ? 1 : msg_len(b);
            if (len > txq_used())
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
            pkt[n++] = txq[(tx_tail + i) & (TXQ_SIZE - 1u)];
        tx_tail = (uint16_t)(tx_tail + len);
    }
    if (n == 1)
        return 0;
    ble_chr->setValue(pkt, n);
    ble_chr->notify();
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
    if (!ble_conn) {
        tx_tail = tx_head;
        return;
    }
    for (int k = 0; k < 4 && ble_send_packet(); k++) {
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
    uint32_t t0 = millis();
    for (;;) {                              /* no rx here: the caller may be */
        tp_tx();                            /* handling a received frame     */
        if (tp_idle() || millis() - t0 >= timeout_ms)
            return;
        gm_esp_scan_service();              /* ESP8266: keep scanning */
        yield();
    }
}
