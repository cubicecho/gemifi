/* host-sim: the real usb_midi.c with the USB endpoint layer stubbed:
 * round trip of the SysEx packing (CIN 4-7) and unpacking, and (with a
 * packet ring, MIDI_TX_PKTS > 0) the transmit queue: sends that don't wait,
 * a host that stops reading (no app has the port open) and comes back. */
#include <stdio.h>
#include <string.h>
#include "usb_midi.h"
#include "usbd.h"
#include "sys.h"
volatile gemifi_mailbox_t sim_mailbox;
#ifndef MIDI_TX_TIMEOUT_MS
#define MIDI_TX_TIMEOUT_MS 50               /* usb_midi.c's default */
#endif

/* The stubbed IN endpoint. pkts: everything the device armed; host: what
 * the host took. host_reads = 0: the host issues no IN tokens. */
static uint8_t pkts[8192]; static int pn; static int busy;
static uint8_t armed[64]; static int armed_n;
static uint8_t host[8192]; static int hn; static int zlps;
static int host_reads = 1, polls, writes, cancels;
static uint32_t now, tick_step = 1;
static uint8_t outpkt[64]; static int outlen = -1;
uint32_t sys_tick_ms(void) { return now += tick_step; }
void usbd_poll(void) {
    polls++;
    if (busy && host_reads) {
        if (!armed_n) zlps++;
        if (hn + armed_n <= (int)sizeof host) { memcpy(host + hn, armed, armed_n); hn += armed_n; }
        busy = 0;
    }
}
int usbd_configured(void) { return 1; }
int usbd_in_busy(void) { return busy; }
void usbd_in_write(const uint8_t *p, uint32_t n) {
    if (n > 64) printf("FAIL pkt>64\n");
    if (busy) printf("FAIL write while busy\n");
    memcpy(pkts + pn, p, n); pn += n; memcpy(armed, p, n); armed_n = (int)n; busy = 1; writes++;
}
void usbd_in_cancel(void) { busy = 0; cancels++; }
int usbd_out_len(void) { return outlen; }
const volatile uint8_t *usbd_out_data(void) { return outpkt; }
void usbd_out_release(void) { outlen = -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void roundtrip(void)
{
    for (int n = 2; n <= 300; n++) {
        uint8_t msg[300]; msg[0] = 0xF0; for (int i = 1; i < n - 1; i++) msg[i] = (uint8_t)(i * 7) & 0x7F; msg[n-1] = 0xF7;
        pn = 0; hn = 0; midi_tx_sysex(msg, n); midi_tx_flush(10);
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
    usbd_poll();                            /* the host takes the last one */
    printf("midi pack/unpack: %s\n", fails ? "FAIL" : "ok (n = 2..300)");
}

#if MIDI_TX_PKTS > 0
static void note(uint8_t k) { midi_tx_event(0x09, 0x90, k, 100); }

/* The host's notes since host[from], in order; -1 if anything else came. */
static int host_notes(int from, uint8_t *k, int max)
{
    int n = 0;
    for (int i = from; i + 4 <= hn; i += 4) {
        if (host[i] != 0x09 || host[i + 1] != 0x90 || n == max) return -1;
        k[n++] = host[i + 2];
    }
    return n;
}

static void queue(void)
{
    int f0 = fails;
    /* One packet in flight; MIDI_TX_PKTS more queue without a wait. */
    host_reads = 1; hn = 0; polls = 0;
    note(0); midi_tx_poll();
    CHECK(busy, "first packet not armed");
    int n = 1;
    for (int i = 0; i < MIDI_TX_PKTS * 16; i++) note((uint8_t)n++);
    CHECK(polls == 0, "queueing %d events waited (%d polls)", MIDI_TX_PKTS * 16, polls);
    /* The next one waits for the host to take a packet. */
    note((uint8_t)n++);
    CHECK(polls >= 1, "ring full but no wait");
    for (int i = 0; i < 40; i++) note((uint8_t)n++);
    CHECK(midi_tx_flush(50) == 0, "flush with the host reading failed");
    usbd_poll();
    uint8_t k[256];
    int got = host_notes(0, k, 256);
    CHECK(got == n, "host got %d of %d notes", got, n);
    for (int i = 0; i < got && i < n; i++)
        if (k[i] != i) { CHECK(0, "note %d out of order (%d)", i, k[i]); break; }
    printf("midi tx ring (%d packets): %s\n", MIDI_TX_PKTS, fails > f0 ? "FAIL" : "ok");
}

static void absent(void)
{
    int f0 = fails;
    /* The host stops reading: the packet in flight goes stale. */
    host_reads = 0; hn = 0; zlps = 0; cancels = 0;
    note(1); midi_tx_poll();
    CHECK(busy, "not armed");
    now += MIDI_TX_TIMEOUT_MS / 2;
    midi_tx_poll();
    CHECK(cancels == 0, "gave up after %d ms", MIDI_TX_TIMEOUT_MS / 2);
    now += MIDI_TX_TIMEOUT_MS;
    midi_tx_poll();
    CHECK(cancels == 1, "stale packet not cancelled");
    CHECK(busy && armed_n == 0, "no zero-length probe armed");

    /* Absent: sends neither wait nor queue. */
    polls = 0; writes = 0;
    uint32_t t0 = now;
    for (int i = 0; i < 2000; i++) note((uint8_t)(i & 0x7F));
    uint8_t sx[200]; sx[0] = 0xF0; memset(sx + 1, 0x11, sizeof sx - 2); sx[sizeof sx - 1] = 0xF7;
    midi_tx_sysex(sx, sizeof sx);
    CHECK(midi_tx_flush(200) == 0, "flush with nothing queued");
    for (int i = 0; i < 100; i++) midi_tx_poll();
    CHECK(polls == 0, "absent host: sends waited (%d polls)", polls);
    CHECK(now - t0 < 200, "absent host: %u ms passed", (unsigned)(now - t0));
    CHECK(writes == 0 && busy && armed_n == 0, "absent host: something replaced the probe");

    /* The host opens the port: it takes the probe, and gets nothing stale. */
    host_reads = 1; usbd_poll();
    CHECK(zlps == 1 && hn == 0, "probe not taken cleanly (zlps %d, %d bytes)", zlps, hn);
    /* The tail of a SysEx whose start was dropped stays dropped. */
    midi_tx_event(0x04, 0x11, 0x11, 0x11);
    midi_tx_event(0x07, 0x11, 0x11, 0xF7);
    midi_tx_poll();
    CHECK(!busy && writes == 0, "SysEx tail sent after the host came back");
    note(42); midi_tx_poll(); usbd_poll();
    uint8_t k[4];
    CHECK(host_notes(0, k, 4) == 1 && k[0] == 42, "host back: note not delivered");
    printf("midi tx host absent: %s\n", fails > f0 ? "FAIL" : "ok");
}

static void absent_burst(void)
{
    int f0 = fails;
    /* The host stops reading while a burst fills the ring: one bounded
     * wait (until the packet in flight is MIDI_TX_TIMEOUT_MS old), then
     * no more. */
    host_reads = 0; cancels = 0; polls = 0;
    note(1); midi_tx_poll();
    for (int i = 0; i < (MIDI_TX_PKTS + 2) * 16; i++) note(2);
    CHECK(cancels == 1, "burst: host not marked absent");
    CHECK(polls <= MIDI_TX_TIMEOUT_MS + 2, "burst: waited %d polls", polls);
    polls = 0;
    for (int i = 0; i < 500; i++) note(3);
    CHECK(polls == 0, "burst: waited again");
    host_reads = 1; usbd_poll();

    /* A flush that times out marks the host absent too. */
    host_reads = 0; cancels = 0;
    note(1); midi_tx_poll(); note(2);
    CHECK(midi_tx_flush(20) == -1, "flush with no host didn't fail");
    CHECK(cancels == 1, "flush timeout: not absent");
    polls = 0;
    note(3);
    CHECK(midi_tx_flush(200) == 0 && polls == 0, "flush after timeout waited");
    host_reads = 1; usbd_poll();
    hn = 0; note(9); CHECK(midi_tx_flush(20) == 0, "flush after recovery"); usbd_poll();
    uint8_t k[4];
    CHECK(host_notes(0, k, 4) == 1 && k[0] == 9, "recovery after flush timeout");
    printf("midi tx absent burst/flush: %s\n", fails > f0 ? "FAIL" : "ok");
}
#endif

int main(void) {
    roundtrip();
#if MIDI_TX_PKTS > 0
    queue();
    absent();
    absent_burst();
#endif
    return fails != 0;
}
