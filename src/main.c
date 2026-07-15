#include "bsp/board.h"
#include "nec_receive.h"
#include "pico/stdlib.h"
#include "tusb.h"

// IR command codes for the target remote.
#define IR_KEY_EXIT 0x16
#define IR_KEY_OK 0x12
#define IR_KEY_UP 0x0f
#define IR_KEY_DOWN 0x15
#define IR_KEY_LEFT 0x11
#define IR_KEY_RIGHT 0x13
#define IR_KEY_CANCEL 0x14
#define IR_KEY_F2 0x1e
#define IR_KEY_F9 0x1f
#define IR_KEY_F11 0x20
#define IR_KEY_F12 0x0e

// Release after this much time without an IR frame. NEC repeat frames arrive
// about every 110 ms, so 200 ms tolerates one missed repeat frame.
#define RELEASE_TIMEOUT_MS 200

// Marker pushed by the PIO program when it detects an NEC repeat code.
#define NEC_REPEAT_MARKER 0xffffffffu

#define HID_REPORT_TIMEOUT_MS 50
#define HID_TAP_MS 80
#define IR_RX_GPIO 27

static uint8_t ir_to_hid(uint8_t rx_data) {
  switch (rx_data) {
  case IR_KEY_EXIT:
    return HID_KEY_ESCAPE;
  case IR_KEY_OK:
    return HID_KEY_ENTER;
  case IR_KEY_UP:
    return HID_KEY_ARROW_UP;
  case IR_KEY_DOWN:
    return HID_KEY_ARROW_DOWN;
  case IR_KEY_LEFT:
    return HID_KEY_ARROW_LEFT;
  case IR_KEY_RIGHT:
    return HID_KEY_ARROW_RIGHT;
  case IR_KEY_CANCEL:
    return HID_KEY_X;
  case IR_KEY_F2:
    return HID_KEY_F2;
  case IR_KEY_F9:
    return HID_KEY_F9;
  case IR_KEY_F11:
    return HID_KEY_F11;
  case IR_KEY_F12:
    return HID_KEY_F12;
  default:
    return 0;
  }
}

// Send one key-down/key-up tap and give the USB host time to receive both HID
// reports.
static void send_hid_tap(uint8_t hid_key) {
  if (!tud_hid_n_ready(0)) {
    return;
  }

  uint8_t kcode[6] = {hid_key, 0, 0, 0, 0, 0};
  tud_hid_n_keyboard_report(0, 0, 0, kcode);

  uint32_t start_ms = board_millis();
  tud_task();
  while (!tud_hid_n_ready(0)) {
    tud_task();
    if (board_millis() - start_ms > HID_REPORT_TIMEOUT_MS) {
      break;
    }
  }

  sleep_ms(HID_TAP_MS);
  tud_hid_n_keyboard_report(0, 0, 0, NULL);
}

int main(void) {
  stdio_init_all();
  board_init();
  tusb_init();

  // The LED is diagnostic: it lights on a decoded key and toggles on repeats.
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);

  PIO pio = pio0;
  int rx_sm_result = nec_rx_init(pio, IR_RX_GPIO);
  if (rx_sm_result < 0) {
    panic("Failed to initialise NEC receiver");
  }
  uint rx_sm = (uint)rx_sm_result;
  uint8_t rx_address;
  uint8_t rx_data;

  uint8_t last_hid_key = 0;
  bool active = false;
  uint32_t last_ir_time = 0;
  bool repeat_seen = false;

  while (true) {
    tud_task();
    uint32_t now = board_millis();

    // Release the active key state after the repeat timeout.
    if (active && (now - last_ir_time >= RELEASE_TIMEOUT_MS)) {
      active = false;
      last_hid_key = 0;
      repeat_seen = false;
      gpio_put(PICO_DEFAULT_LED_PIN, 0);
    }

    // Process all queued IR frames from the PIO RX FIFO.
    while (!pio_sm_is_rx_fifo_empty(pio, rx_sm)) {
      uint32_t rx_frame = pio_sm_get(pio, rx_sm);

      if (rx_frame == NEC_REPEAT_MARKER) {
        if (active && last_hid_key) {
          last_ir_time = now;
          if (repeat_seen) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
            send_hid_tap(last_hid_key);
          }
        }
        repeat_seen = true;
      } else if (nec_decode_frame(rx_frame, &rx_address, &rx_data)) {
        uint8_t hid_key = ir_to_hid(rx_data);
        if (hid_key) {
          last_hid_key = hid_key;
          last_ir_time = now;
          repeat_seen = false;
          active = true;
          gpio_put(PICO_DEFAULT_LED_PIN, 1);
          send_hid_tap(hid_key);
        }
      }
    }
  }

  return 0;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;

  return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
}
