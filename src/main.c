#include "bsp/board.h"
#include "hardware/flash.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "nec_receive.h"
#include "pico/platform.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

// Apple Remote bytes and normalized command values. The command byte's LSB is
// an odd parity bit over the normalized command and remote ID.
#define APPLE_CUSTOM_NORMAL_0 0xee
#define APPLE_CUSTOM_PAIRING_0 0xe0
#define APPLE_CUSTOM_1 0x87
#define APPLE_PAIRING_COMMAND_RAW 0x02

#define APPLE_COMMAND_MENU 0x01
#define APPLE_COMMAND_CENTER_PLAY_TAIL 0x02
#define APPLE_COMMAND_RIGHT 0x03
#define APPLE_COMMAND_LEFT 0x04
#define APPLE_COMMAND_UP 0x05
#define APPLE_COMMAND_DOWN 0x06
#define APPLE_COMMAND_CENTER_PREFIX 0x2e
#define APPLE_COMMAND_PLAY_PAUSE_PREFIX 0x2f

// Release after this much time without an IR frame. NEC repeat frames arrive
// about every 110 ms, so 200 ms tolerates one missed repeat frame.
#define RELEASE_TIMEOUT_MS 200

// Marker pushed by the PIO program when it detects an NEC repeat code.
#define NEC_REPEAT_MARKER 0xffffffffu

#define HID_REPORT_TIMEOUT_MS 50
#define HID_TAP_MS 80
#define IR_RX_GPIO 27

#define PAIRING_MATCH_COUNT 5
#define PAIRING_WINDOW_MS 1000
#define BOOTSEL_POLL_MS 50
#define BOOTSEL_DEBOUNCE_MS 100

#ifndef DOCKIR_FLASH_SIZE_BYTES
#define DOCKIR_FLASH_SIZE_BYTES (512u * 1024u)
#endif

#if DOCKIR_FLASH_SIZE_BYTES < FLASH_SECTOR_SIZE
#error "DOCKIR_FLASH_SIZE_BYTES must be at least one flash sector"
#endif

#if DOCKIR_FLASH_SIZE_BYTES > PICO_FLASH_SIZE_BYTES
#error "DOCKIR_FLASH_SIZE_BYTES cannot exceed PICO_FLASH_SIZE_BYTES"
#endif

#define PAIRING_FLASH_OFFSET (DOCKIR_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define PAIRING_FLASH_MAGIC 0x52494341u
#define PAIRING_FLASH_VERSION 1u
#define PAIRING_FLASH_CHECK_XOR 0xa51fc37du

#define LOGF(...)                                                              \
  do {                                                                         \
    printf("[%lu ms] ", (unsigned long)board_millis());                        \
    printf(__VA_ARGS__);                                                       \
    printf("\r\n");                                                            \
  } while (0)

typedef enum {
  APPLE_FRAME_INVALID,
  APPLE_FRAME_BUTTON,
  APPLE_FRAME_CENTER_PLAY_TAIL,
  APPLE_FRAME_PAIRING,
} apple_frame_type_t;

typedef enum {
  APPLE_DECODE_OK,
  APPLE_DECODE_BAD_CUSTOM1,
  APPLE_DECODE_BAD_CUSTOM0,
  APPLE_DECODE_BAD_PAIRING_COMMAND,
  APPLE_DECODE_BAD_PARITY,
  APPLE_DECODE_UNKNOWN_COMMAND,
} apple_decode_status_t;

typedef struct {
  uint8_t custom0;
  uint8_t custom1;
  uint8_t command_raw;
  uint8_t remote_id;
} apple_payload_t;

typedef struct {
  apple_frame_type_t type;
  uint8_t command;
  uint8_t remote_id;
  uint8_t hid_key;
} apple_frame_t;

typedef struct {
  uint32_t magic;
  uint8_t version;
  uint8_t remote_id;
  uint16_t reserved;
  uint32_t checksum;
} pairing_flash_record_t;

typedef struct {
  bool paired;
  uint8_t remote_id;
} pairing_config_t;

typedef struct {
  uint8_t remote_id;
  uint8_t count;
  uint32_t first_ms;
} pairing_detector_t;

_Static_assert(sizeof(apple_payload_t) == sizeof(uint32_t),
               "Apple payload must stay four bytes");

static apple_payload_t apple_payload_from_raw(uint32_t raw_frame) {
  union {
    uint32_t raw;
    apple_payload_t payload;
  } frame;

  frame.raw = raw_frame;
  return frame.payload;
}

static char const *apple_command_name(uint8_t command) {
  switch (command) {
  case APPLE_COMMAND_MENU:
    return "Menu";
  case APPLE_COMMAND_CENTER_PLAY_TAIL:
    return "Center/Play tail";
  case APPLE_COMMAND_RIGHT:
    return "Right";
  case APPLE_COMMAND_LEFT:
    return "Left";
  case APPLE_COMMAND_UP:
    return "Up";
  case APPLE_COMMAND_DOWN:
    return "Down";
  case APPLE_COMMAND_CENTER_PREFIX:
    return "Center";
  case APPLE_COMMAND_PLAY_PAUSE_PREFIX:
    return "Play/Pause";
  default:
    return "Unknown";
  }
}

static char const *hid_key_name(uint8_t hid_key) {
  switch (hid_key) {
  case HID_KEY_ARROW_UP:
    return "ArrowUp";
  case HID_KEY_ARROW_DOWN:
    return "ArrowDown";
  case HID_KEY_ARROW_LEFT:
    return "ArrowLeft";
  case HID_KEY_ARROW_RIGHT:
    return "ArrowRight";
  case HID_KEY_ENTER:
    return "Enter";
  case HID_KEY_F12:
    return "F12";
  case HID_KEY_ESCAPE:
    return "Escape";
  default:
    return "None";
  }
}

static char const *decode_status_name(apple_decode_status_t status) {
  switch (status) {
  case APPLE_DECODE_OK:
    return "ok";
  case APPLE_DECODE_BAD_CUSTOM1:
    return "bad-custom1";
  case APPLE_DECODE_BAD_CUSTOM0:
    return "bad-custom0";
  case APPLE_DECODE_BAD_PAIRING_COMMAND:
    return "bad-pairing-command";
  case APPLE_DECODE_BAD_PARITY:
    return "bad-parity";
  case APPLE_DECODE_UNKNOWN_COMMAND:
    return "unknown-command";
  default:
    return "unknown-status";
  }
}

static uint8_t apple_command_to_hid(uint8_t command) {
  switch (command) {
  case APPLE_COMMAND_UP:
    return HID_KEY_ARROW_UP;
  case APPLE_COMMAND_DOWN:
    return HID_KEY_ARROW_DOWN;
  case APPLE_COMMAND_LEFT:
    return HID_KEY_ARROW_LEFT;
  case APPLE_COMMAND_RIGHT:
    return HID_KEY_ARROW_RIGHT;
  case APPLE_COMMAND_CENTER_PREFIX:
    return HID_KEY_ENTER;
  case APPLE_COMMAND_MENU:
    return HID_KEY_F12;
  case APPLE_COMMAND_PLAY_PAUSE_PREFIX:
    return HID_KEY_ESCAPE;
  default:
    return 0;
  }
}

static uint8_t popcount8(uint8_t value) {
  uint8_t count = 0;
  while (value) {
    count += value & 1u;
    value >>= 1u;
  }
  return count;
}

static bool apple_command_parity_ok(uint8_t command_raw, uint8_t remote_id) {
  uint8_t command = command_raw >> 1u;
  uint8_t parity = command_raw & 1u;
  uint32_t bits =
      (uint32_t)popcount8(command) + popcount8(remote_id) + parity;
  return (bits & 1u) == 1u;
}

static apple_decode_status_t decode_apple_frame(uint32_t raw_frame,
                                                apple_frame_t *frame) {
  apple_payload_t payload = apple_payload_from_raw(raw_frame);

  frame->type = APPLE_FRAME_INVALID;
  frame->command = 0;
  frame->remote_id = payload.remote_id;
  frame->hid_key = 0;

  if (payload.custom1 != APPLE_CUSTOM_1) {
    return APPLE_DECODE_BAD_CUSTOM1;
  }

  if (payload.custom0 == APPLE_CUSTOM_PAIRING_0) {
    if (payload.command_raw != APPLE_PAIRING_COMMAND_RAW) {
      return APPLE_DECODE_BAD_PAIRING_COMMAND;
    }
    frame->type = APPLE_FRAME_PAIRING;
    return APPLE_DECODE_OK;
  }

  if (payload.custom0 != APPLE_CUSTOM_NORMAL_0) {
    return APPLE_DECODE_BAD_CUSTOM0;
  }

  if (!apple_command_parity_ok(payload.command_raw, payload.remote_id)) {
    return APPLE_DECODE_BAD_PARITY;
  }

  frame->command = payload.command_raw >> 1u;

  if (frame->command == APPLE_COMMAND_CENTER_PLAY_TAIL) {
    frame->type = APPLE_FRAME_CENTER_PLAY_TAIL;
    return APPLE_DECODE_OK;
  }

  frame->hid_key = apple_command_to_hid(frame->command);
  if (!frame->hid_key) {
    return APPLE_DECODE_UNKNOWN_COMMAND;
  }

  frame->type = APPLE_FRAME_BUTTON;
  return APPLE_DECODE_OK;
}

static bool remote_id_allowed(pairing_config_t const *config,
                              uint8_t remote_id) {
  return !config->paired || config->remote_id == remote_id;
}

static uint32_t pairing_checksum(uint8_t remote_id) {
  return PAIRING_FLASH_MAGIC ^ PAIRING_FLASH_CHECK_XOR ^
         ((uint32_t)PAIRING_FLASH_VERSION << 24u) ^ remote_id;
}

static bool pairing_record_valid(pairing_flash_record_t const *record) {
  return record->magic == PAIRING_FLASH_MAGIC &&
         record->version == PAIRING_FLASH_VERSION &&
         record->checksum == pairing_checksum(record->remote_id);
}

static bool load_pairing_config(pairing_config_t *config) {
  pairing_flash_record_t const *record =
      (pairing_flash_record_t const *)(XIP_BASE + PAIRING_FLASH_OFFSET);

  if (!pairing_record_valid(record)) {
    config->paired = false;
    config->remote_id = 0;
    return false;
  }

  config->paired = true;
  config->remote_id = record->remote_id;
  return true;
}

static void store_pairing_config(uint8_t remote_id) {
  uint8_t page[FLASH_PAGE_SIZE];
  pairing_flash_record_t record = {
      .magic = PAIRING_FLASH_MAGIC,
      .version = PAIRING_FLASH_VERSION,
      .remote_id = remote_id,
      .reserved = 0xffff,
      .checksum = pairing_checksum(remote_id),
  };

  memset(page, 0xff, sizeof(page));
  memcpy(page, &record, sizeof(record));

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(PAIRING_FLASH_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(PAIRING_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
  restore_interrupts(ints);
}

static void clear_pairing_config(void) {
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(PAIRING_FLASH_OFFSET, FLASH_SECTOR_SIZE);
  restore_interrupts(ints);
}

static bool pairing_request_confirmed(pairing_detector_t *detector,
                                      uint8_t remote_id, uint32_t now_ms,
                                      uint8_t *match_count) {
  if (detector->count == 0 || detector->remote_id != remote_id ||
      now_ms - detector->first_ms > PAIRING_WINDOW_MS) {
    detector->remote_id = remote_id;
    detector->count = 1;
    detector->first_ms = now_ms;
    *match_count = detector->count;
    return false;
  }

  detector->count++;
  *match_count = detector->count;
  if (detector->count < PAIRING_MATCH_COUNT) {
    return false;
  }

  detector->count = 0;
  return true;
}

static void service_usb_for_ms(uint32_t delay_ms) {
  uint32_t start_ms = board_millis();
  while (board_millis() - start_ms < delay_ms) {
    tud_task();
    sleep_ms(1);
  }
}

static void blink_led(uint count, uint32_t on_ms, uint32_t off_ms) {
  for (uint i = 0; i < count; i++) {
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    service_usb_for_ms(on_ms);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
    service_usb_for_ms(off_ms);
  }
}

static void alert_paired(void) { blink_led(3, 80, 80); }

static void alert_unpaired(void) { blink_led(2, 250, 120); }

static bool __no_inline_not_in_flash_func(get_bootsel_button)(void) {
  const uint cs_pin_index = 1;

  uint32_t flags = save_and_disable_interrupts();

  hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                  GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

  for (volatile uint i = 0; i < 1000; ++i) {
    tight_loop_contents();
  }

  bool button_pressed = !(sio_hw->gpio_hi_in & (1u << cs_pin_index));

  hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                  GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

  restore_interrupts(flags);

  return button_pressed;
}

// Send one key-down/key-up tap and give the USB host time to receive both HID
// reports.
static bool send_hid_tap(uint8_t hid_key) {
  if (!tud_hid_n_ready(0)) {
    return false;
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

  service_usb_for_ms(HID_TAP_MS);
  tud_hid_n_keyboard_report(0, 0, 0, NULL);
  return true;
}

int main(void) {
  stdio_init_all();
  board_init();
  tusb_init();

  // The LED is diagnostic: it lights on a decoded key and toggles on repeats.
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);

  LOGF("DockIR Apple Remote firmware boot");
  LOGF("stdio=uart tx=GP%u rx=GP%u baud=%u usb_cdc=disabled ir_gpio=GP%u",
       DOCKIR_LOG_UART_TX_PIN, DOCKIR_LOG_UART_RX_PIN, DOCKIR_LOG_UART_BAUD,
       IR_RX_GPIO);
  LOGF("logical_flash=%lu bytes pairing_offset=0x%05lx pairing_sector=%u",
       (unsigned long)DOCKIR_FLASH_SIZE_BYTES,
       (unsigned long)PAIRING_FLASH_OFFSET, FLASH_SECTOR_SIZE);

  PIO pio = pio0;
  int rx_sm_result = nec_rx_init(pio, IR_RX_GPIO);
  if (rx_sm_result < 0) {
    LOGF("fatal: failed to initialise NEC timing receiver");
    panic("Failed to initialise NEC receiver");
  }
  uint rx_sm = (uint)rx_sm_result;
  LOGF("ir_receiver=ready pio=%u sm=%u", pio_get_index(pio), rx_sm);

  pairing_config_t pairing_config;
  load_pairing_config(&pairing_config);
  if (pairing_config.paired) {
    LOGF("pairing=loaded remote_id=0x%02x", pairing_config.remote_id);
  } else {
    LOGF("pairing=none accepting=any-apple-remote");
  }
  pairing_detector_t pairing_detector = {0};
  uint8_t last_hid_key = 0;
  bool active = false;
  uint32_t last_ir_time = 0;
  bool repeat_seen = false;
  uint32_t last_bootsel_poll = 0;
  uint32_t bootsel_pressed_since = 0;
  bool bootsel_was_pressed = false;
  bool bootsel_action_latched = false;

  while (true) {
    tud_task();
    uint32_t now = board_millis();

    if (now - last_bootsel_poll >= BOOTSEL_POLL_MS) {
      last_bootsel_poll = now;
      bool bootsel_pressed = get_bootsel_button();

      if (bootsel_pressed) {
        if (!bootsel_was_pressed) {
          bootsel_pressed_since = now;
          bootsel_was_pressed = true;
        } else if (!bootsel_action_latched && pairing_config.paired &&
                   now - bootsel_pressed_since >= BOOTSEL_DEBOUNCE_MS) {
          LOGF("bootsel=pressed unpair remote_id=0x%02x flash_erase_offset=0x%05lx",
               pairing_config.remote_id, (unsigned long)PAIRING_FLASH_OFFSET);
          clear_pairing_config();
          pairing_config.paired = false;
          pairing_config.remote_id = 0;
          pairing_detector.count = 0;
          active = false;
          last_hid_key = 0;
          repeat_seen = false;
          bootsel_action_latched = true;
          LOGF("pairing=cleared accepting=any-apple-remote");
          alert_unpaired();
        }
      } else {
        bootsel_was_pressed = false;
        bootsel_action_latched = false;
      }
    }

    // Release the active key state after the repeat timeout.
    if (active && (now - last_ir_time >= RELEASE_TIMEOUT_MS)) {
      LOGF("release hid=%s timeout_ms=%u", hid_key_name(last_hid_key),
           RELEASE_TIMEOUT_MS);
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
            LOGF("repeat hid=%s", hid_key_name(last_hid_key));
            if (!send_hid_tap(last_hid_key)) {
              LOGF("hid=not-ready dropped_repeat=%s",
                   hid_key_name(last_hid_key));
            }
          } else {
            LOGF("repeat_marker hid=%s primed", hid_key_name(last_hid_key));
          }
        } else {
          LOGF("repeat_marker ignored active=%u hid=%s", active ? 1u : 0u,
               hid_key_name(last_hid_key));
        }
        repeat_seen = true;
      } else {
        apple_payload_t payload = apple_payload_from_raw(rx_frame);
        apple_frame_t frame;
        apple_decode_status_t decode_status = decode_apple_frame(rx_frame, &frame);
        if (decode_status != APPLE_DECODE_OK) {
          LOGF("frame raw=0x%08lx bytes=%02x %02x %02x %02x ignored=%s",
               (unsigned long)rx_frame, payload.custom0, payload.custom1,
               payload.command_raw, payload.remote_id,
               decode_status_name(decode_status));
          continue;
        }

        if (frame.type == APPLE_FRAME_PAIRING) {
          bool can_pair = !pairing_config.paired ||
                          pairing_config.remote_id == frame.remote_id;
          if (!can_pair) {
            LOGF("pairing_request remote_id=0x%02x ignored=already-paired paired_id=0x%02x",
                 frame.remote_id, pairing_config.remote_id);
            continue;
          }

          uint8_t pairing_count = 0;
          bool confirmed = pairing_request_confirmed(
              &pairing_detector, frame.remote_id, now, &pairing_count);
          LOGF("pairing_request raw=0x%08lx bytes=%02x %02x %02x %02x remote_id=0x%02x count=%u/%u",
               (unsigned long)rx_frame, payload.custom0, payload.custom1,
               payload.command_raw, payload.remote_id, frame.remote_id,
               pairing_count, PAIRING_MATCH_COUNT);

          if (confirmed) {
            if (!pairing_config.paired) {
              LOGF("pairing=confirmed remote_id=0x%02x flash_write_offset=0x%05lx",
                   frame.remote_id, (unsigned long)PAIRING_FLASH_OFFSET);
              store_pairing_config(frame.remote_id);
              pairing_config.paired = true;
              pairing_config.remote_id = frame.remote_id;
              LOGF("pairing=stored remote_id=0x%02x", frame.remote_id);
            } else {
              LOGF("pairing=confirmed remote_id=0x%02x flash_write=skipped",
                   frame.remote_id);
            }
            active = false;
            last_hid_key = 0;
            repeat_seen = false;
            alert_paired();
          }
          continue;
        }

        if (!remote_id_allowed(&pairing_config, frame.remote_id)) {
          LOGF("frame raw=0x%08lx bytes=%02x %02x %02x %02x command=0x%02x remote_id=0x%02x ignored=wrong-remote paired_id=0x%02x",
               (unsigned long)rx_frame, payload.custom0, payload.custom1,
               payload.command_raw, payload.remote_id, frame.command,
               frame.remote_id, pairing_config.remote_id);
          continue;
        }

        if (frame.type == APPLE_FRAME_CENTER_PLAY_TAIL) {
          LOGF("tail raw=0x%08lx bytes=%02x %02x %02x %02x remote_id=0x%02x active_hid=%s",
               (unsigned long)rx_frame, payload.custom0, payload.custom1,
               payload.command_raw, payload.remote_id, frame.remote_id,
               hid_key_name(last_hid_key));
          if (active && (last_hid_key == HID_KEY_ENTER ||
                         last_hid_key == HID_KEY_ESCAPE)) {
            last_ir_time = now;
            repeat_seen = false;
          }
          continue;
        }

        if (frame.type == APPLE_FRAME_BUTTON) {
          LOGF("button raw=0x%08lx bytes=%02x %02x %02x %02x command=0x%02x name=%s remote_id=0x%02x hid=%s paired=%u",
               (unsigned long)rx_frame, payload.custom0, payload.custom1,
               payload.command_raw, payload.remote_id, frame.command,
               apple_command_name(frame.command), frame.remote_id,
               hid_key_name(frame.hid_key), pairing_config.paired ? 1u : 0u);
          last_hid_key = frame.hid_key;
          last_ir_time = now;
          repeat_seen = false;
          active = true;
          gpio_put(PICO_DEFAULT_LED_PIN, 1);
          if (!send_hid_tap(frame.hid_key)) {
            LOGF("hid=not-ready dropped_button=%s",
                 hid_key_name(frame.hid_key));
          }
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
