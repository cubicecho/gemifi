# maudio_pads

An LED strip platform ([led_strip](../led_strip/README.md), a hub type on the [hub](../hub/README.md) base): the M-Audio Code pad LED board, a small controller that drives the pads' RGB LEDs and takes their colours over a serial line (Code 61: USART2 TX on PA2, 115200 baud 8N1).

```yaml
led_strip:
  - { id: pad_leds, platform: maudio_pads, tx_pin: PA2, rx_pin: PA3, count: 16, gamma: 2.2, white: [255, 200, 220] }   # dma: false sends from the interrupt ("DMA channels")
leds:
  - { id: pad_1_led, pin: { led_strip: pad_leds, number: 36 }, pin2: { led_strip: pad_leds, number: 37 }, pin3: { led_strip: pad_leds, number: 38 } }
pads:
  - { id: pad_1, pin: { cd74hc4051: pad_mux_b, number: 4 }, note: 36, led: pad_1_led }   # lit while held
```

<!-- keys -->

Each colour is a byte (0 off to 255 full), and an RGB LED on the board shows any colour, with the strip's `gamma:` and `white:` ([led_strip](../led_strip/README.md)). It was called `maudio_pad_leds:`, and `rgb_serial:` before that, as its own list.

The board's protocol, found in the stock firmware: one message per LED, `02, 0x80 + LED, R, G, B, 03` (each colour 00 off to FF full), and a lone `A5` keepalive at least every ~60 ms, without which the board turns its LEDs off. The board drops messages sent back to back, so the core sends one changed LED per 5 ms from the main loop (16 LEDs all changing take 80 ms) and the keepalive every 50 ms, with every write, the keepalive included, at least 5 ms after the last. The UART runs in the background (DMA on stm32f1, `hal_serial_write`).

At power-on the board isn't listening for the first ~220 ms, so the first colours are lost. Without `rx_pin`, every LED is sent once more 2 s after start; after that an LED goes out only when it changes. Don't resend LEDs on a timer: resending one every 50 ms left the Code 61's board deaf after a cold start.

With `rx_pin`, the core listens to the board, which answers each LED message with `0x40 + LED` (the keepalive gets no answer). An LED counts as shown only once answered. Without an answer in 20 ms it goes out again, after a pause that grows with each miss in a row (50 ms, 100 ms, ... up to 500 ms), so a board that is still starting gets little traffic. The first answer means the board is up, and every LED is sent again then instead of at 2 s. If nothing answers in the first 3 s, the hub works as without `rx_pin`.

## Files

- `maudio_pads.c`: the driver, and the weak `hal_serial_*` defaults.
- `maudio_pads.h`: its config and state.
- `__init__.py`: the YAML: validation and generated C.
- Its host test is `core/test/test_tm1628.c`, with the TM1628's.
