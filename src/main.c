#include "nec_receive.h"
#include "pico/stdlib.h"
#include <stdio.h>

// Marker pushed by the PIO program when it detects an NEC repeat code. NEC
// repeat frames do not carry a payload, so the logger reports them separately.
#define NEC_REPEAT_MARKER 0xffffffffu

#define APPLE_CUSTOM0 0xeeu
#define APPLE_CUSTOM1 0x87u
#define APPLE_CENTER_PREFIX 0x2eu
#define APPLE_PLAY_PREFIX 0x2fu
#define APPLE_SELECT_TAIL 0x02u
#define APPLE_SEQUENCE_TIMEOUT_MS 250u
#define IR_RX_GPIO 27

typedef struct {
  uint8_t custom0;
  uint8_t custom1;
  uint8_t command;
  uint8_t remote_id;
} apple_ir_frame_t;

static apple_ir_frame_t apple_frame_from_raw(uint32_t raw) {
  union {
    uint32_t raw;
    uint8_t byte[4];
  } frame = {.raw = raw};

  return (apple_ir_frame_t){
      .custom0 = frame.byte[0],
      .custom1 = frame.byte[1],
      .command = frame.byte[2],
      .remote_id = frame.byte[3],
  };
}

static bool apple_frame_has_custom_code(apple_ir_frame_t frame) {
  return frame.custom0 == APPLE_CUSTOM0 && frame.custom1 == APPLE_CUSTOM1;
}

static void byte_to_bits(uint8_t value, char bits[9]) {
  for (uint8_t i = 0; i < 8; i++) {
    bits[i] = (value & (uint8_t)(1u << (7u - i))) ? '1' : '0';
  }
  bits[8] = '\0';
}

static uint8_t apple_logical_command(uint8_t raw_command) {
  return raw_command >> 1u;
}

static uint8_t bit_count8(uint8_t value) {
  uint8_t count = 0;
  while (value != 0) {
    count += value & 1u;
    value >>= 1u;
  }
  return count;
}

static bool apple_command_parity_ok(apple_ir_frame_t frame) {
  uint8_t logical_command = apple_logical_command(frame.command);
  uint8_t expected_parity =
      (uint8_t)((bit_count8(logical_command) + bit_count8(frame.remote_id) +
                 1u) &
                1u);
  return (frame.command & 1u) == expected_parity;
}

static const char *apple_command_name(uint8_t logical_command) {
  switch (logical_command) {
  case 0x01:
    return "Menu";
  case APPLE_SELECT_TAIL:
    return "Center/Play tail";
  case 0x03:
    return "Right / Next";
  case 0x04:
    return "Left / Previous";
  case 0x05:
    return "Up / Volume Up";
  case 0x06:
    return "Down / Volume Down";
  case APPLE_CENTER_PREFIX:
    return "Center prefix";
  case APPLE_PLAY_PREFIX:
    return "Play/Pause prefix";
  default:
    return "unknown";
  }
}

static bool apple_command_is_prefix(uint8_t logical_command) {
  return logical_command == APPLE_CENTER_PREFIX ||
         logical_command == APPLE_PLAY_PREFIX;
}

static const char *apple_sequence_name(uint8_t logical_prefix_command) {
  if (logical_prefix_command == APPLE_CENTER_PREFIX) {
    return "Center";
  }
  if (logical_prefix_command == APPLE_PLAY_PREFIX) {
    return "Play/Pause";
  }
  return "none";
}

int main(void) {
  stdio_init_all();

  sleep_ms(100);
  printf("\nDockIR IR frame reader\n");
  printf("IR input: GP%u\n", IR_RX_GPIO);
  printf("Apple expected bytes: %02x %02x <command> <remote_id>\n",
         APPLE_CUSTOM0, APPLE_CUSTOM1);
  fflush(stdout);

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
  uint32_t frame_count = 0;
  uint32_t last_frame = 0;
  uint8_t pending_apple_prefix = 0;
  uint32_t pending_apple_prefix_time = 0;

  while (true) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Process all queued IR frames from the PIO RX FIFO.
    while (!pio_sm_is_rx_fifo_empty(pio, rx_sm)) {
      uint32_t rx_frame = pio_sm_get(pio, rx_sm);

      if (rx_frame == NEC_REPEAT_MARKER) {
        gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        printf("[%lu ms] repeat frame last_raw=0x%08lx\n", now, last_frame);
        fflush(stdout);
        continue;
      }

      frame_count++;
      last_frame = rx_frame;
      gpio_put(PICO_DEFAULT_LED_PIN, 1);

      apple_ir_frame_t apple = apple_frame_from_raw(rx_frame);
      char custom0_bits[9];
      char custom1_bits[9];
      char command_bits[9];
      char remote_id_bits[9];
      byte_to_bits(apple.custom0, custom0_bits);
      byte_to_bits(apple.custom1, custom1_bits);
      byte_to_bits(apple.command, command_bits);
      byte_to_bits(apple.remote_id, remote_id_bits);

      printf("[%lu ms] #%lu raw=0x%08lx bytes=%02x %02x %02x %02x "
             "bits=%s %s %s %s",
             now, frame_count, rx_frame, apple.custom0, apple.custom1,
             apple.command, apple.remote_id, custom0_bits, custom1_bits,
             command_bits, remote_id_bits);

      if (apple_frame_has_custom_code(apple)) {
        uint8_t logical_command = apple_logical_command(apple.command);
        const char *sequence = "none";
        if (logical_command == APPLE_SELECT_TAIL &&
            pending_apple_prefix != 0 &&
            now - pending_apple_prefix_time <= APPLE_SEQUENCE_TIMEOUT_MS) {
          sequence = apple_sequence_name(pending_apple_prefix);
        }
        if (apple_command_is_prefix(logical_command)) {
          pending_apple_prefix = logical_command;
          pending_apple_prefix_time = now;
        } else if (logical_command != APPLE_SELECT_TAIL) {
          pending_apple_prefix = 0;
        }

        printf(" apple=yes custom=ok command_raw=0x%02x command=0x%02x (%s) "
               "remote_id=0x%02x parity=%s sequence=%s\n",
               apple.command, logical_command,
               apple_command_name(logical_command), apple.remote_id,
               apple_command_parity_ok(apple) ? "ok" : "bad", sequence);
      } else if (nec_decode_frame(rx_frame, &rx_address, &rx_data)) {
        printf(" apple=no nec=valid address=0x%02x data=0x%02x\n",
               rx_address, rx_data);
        pending_apple_prefix = 0;
      } else {
        printf(" apple=no nec=invalid\n");
        pending_apple_prefix = 0;
      }

      fflush(stdout);
    }
  }

  return 0;
}
