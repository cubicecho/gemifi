/* host-sim: round-trip test of the real usb_midi.c SysEx packing (CIN 4-7) and
 * unpacking, with the USB endpoint layer stubbed. */
#include <stdio.h>
#include <string.h>
#include "usb_midi.h"
#include "usbd.h"
#include "sys.h"
volatile gemifi_mailbox_t sim_mailbox;
static uint8_t pkts[8192]; static int pn; static int busy;
static uint8_t outpkt[64]; static int outlen = -1;
uint32_t sys_tick_ms(void) { static uint32_t t; return t++; }
void usbd_poll(void) { busy = 0; }
int usbd_configured(void) { return 1; }
int usbd_in_busy(void) { return busy; }
void usbd_in_write(const uint8_t *p, uint32_t n) { if (n > 64) printf("FAIL pkt>64\n"); memcpy(pkts + pn, p, n); pn += n; busy = 1; }
void usbd_in_cancel(void) { busy = 0; }
int usbd_out_len(void) { return outlen; }
const volatile uint8_t *usbd_out_data(void) { return outpkt; }
void usbd_out_release(void) { outlen = -1; }
int main(void) {
    int fails = 0;
    for (int n = 2; n <= 300; n++) {
        uint8_t msg[300]; msg[0] = 0xF0; for (int i = 1; i < n - 1; i++) msg[i] = (uint8_t)(i * 7) & 0x7F; msg[n-1] = 0xF7;
        pn = 0; midi_tx_sysex(msg, n); midi_tx_flush(10);
        /* unpack via midi_rx_poll in 64-byte packets */
        uint8_t body[400]; gsx_rx_t rx = { body, sizeof body, 0, 0 };
        int got = 0;
        for (int off = 0; off < pn; off += 64) {
            int k = pn - off < 64 ? pn - off : 64;
            memcpy(outpkt, pkts + off, k); outlen = k;
            while (outlen >= 0) if (midi_rx_poll(&rx)) { got = 1; if (rx.n != n - 2 || memcmp(body, msg + 1, n - 2)) { printf("FAIL roundtrip n=%d\n", n); fails++; } gsx_rx_reset(&rx); }
        }
        if (!got) { printf("FAIL no frame n=%d\n", n); fails++; }
        /* check CIN usage */
        for (int i = 0; i < pn; i += 4) {
            int last = i + 4 >= pn, cin = pkts[i];
            if (!last && cin != 4) { printf("FAIL cin n=%d\n", n); fails++; break; }
            if (last && cin != 4 + ((n - 1) % 3 + 1)) { printf("FAIL last cin n=%d cin=%d\n", n, cin); fails++; }
        }
    }
    printf("midi pack/unpack: %s\n", fails ? "FAIL" : "ok (n = 2..300)");
    return fails != 0;
}
