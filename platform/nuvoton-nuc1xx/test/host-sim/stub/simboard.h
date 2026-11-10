/* host-sim: stand-in board header (no registers); buttons come from sim_buttons. */
#include <stdint.h>
#include "board_layout.h"   /* BOARD_ID, BOARD_NAME: the default board's profile */
#define BTN_PITCH_DOWN 5
#define BTN_PITCH_UP 6
#define BTN_OCT_UP 4
#define BOARD_BOOT_COMBO ((1u << BTN_PITCH_DOWN) | (1u << BTN_PITCH_UP))
#define LED_OCT_DOWN 1
#define LED_OCT_UP 2
extern uint32_t sim_buttons;
static inline void board_gpio_init(void) {}
static inline uint32_t board_buttons(void) { return sim_buttons; }
static inline void board_led(uint32_t m, int on) { (void)m; (void)on; }
#define BOARD_BTN_FIRST 1
#define BOARD_BTN_LAST 7
