#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "nec_receive.h"


/*

OK		12
exit	16
UP		0f
DOWN	15
LEFT	11
RIGHT	13

menu	0e
cancel	14
*/
#define key_exit  0x16       // KEYCODE_B
#define key_ok    0x12        // KEYCODE_A

#define key_up    0x0f       // KEYCODE_ARROW_UP
#define key_down  0x15       // KEYCODE_ARROW_DOWN
#define key_left  0x11       // KEYCODE_ARROW_LEFT
#define key_right 0x13       // KEYCODE_ARROW_RIGHT

#define key_cancel 0x14       // KEYCODE_X

#define key_F2  0x1e       // KEYCODE_F2
#define key_F9  0x1f       // KEYCODE_F9
#define key_F11  0x20       // KEYCODE_F11
#define key_F12  0x0e       // KEYCODE_F12

/* 超时释放：最后一次收到 IR 帧后多久释放
 * NEC repeat 帧间隔约 110ms，设 200ms 可容纳 1 次丢帧 */
#define RELEASE_TIMEOUT_MS  200

/* PIO 检测到 NEC repeat code 时 push 到 FIFO 的特殊标记 */
#define NEC_REPEAT_MARKER   0xFFFFFFFF



static uint8_t ir_to_hid(uint8_t rx_data) {
  switch (rx_data) {
    case key_exit:   return HID_KEY_ESCAPE;
    case key_ok:     return HID_KEY_ENTER;
    case key_up:     return HID_KEY_ARROW_UP;
    case key_down:   return HID_KEY_ARROW_DOWN;
    case key_left:   return HID_KEY_ARROW_LEFT;
    case key_right:  return HID_KEY_ARROW_RIGHT;
    case key_cancel: return HID_KEY_X;
    case key_F2:     return HID_KEY_F2;
    case key_F9:     return HID_KEY_F9;
    case key_F11:    return HID_KEY_F11;
    case key_F12:    return HID_KEY_F12;
    default:         return 0;
  }
}

/* 发送一次 key-down + key-up tap，确保两个 HID 报告都被 USB 主机收到 */
static void send_hid_tap(uint8_t hid_key)
{
  if (!tud_hid_n_ready(0)) return;

  uint8_t kcode[6] = { hid_key, 0, 0, 0, 0, 0 };
  tud_hid_n_keyboard_report(0, 0, 0, kcode);       /* key-down */

  /* 等待 key-down 报告被主机取走，endpoint 重新就绪 */
  uint32_t t0 = board_millis();
  tud_task();
  while (!tud_hid_n_ready(0)) {
    tud_task();
    if (board_millis() - t0 > 50) break;            /* 超时保护 */
  }

  sleep_ms(80);
  
  tud_hid_n_keyboard_report(0, 0, 0, NULL);         /* key-up */
}


int main(void)
{
  stdio_init_all();
  board_init();
  tusb_init();

  /* LED 用于诊断：收到 repeat 帧时闪烁 */
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);

  PIO pio = pio0;
  uint rx_gpio = 27;
  int rx_sm = nec_rx_init (pio, rx_gpio);
  uint8_t rx_address, rx_data;

  uint8_t last_hid_key = 0;      /* 当前按住的 HID 键码 */
  bool active = false;            /* 是否有按键处于活动状态 */
  uint32_t last_ir_time = 0;     /* 上次收到任何 IR 帧的时间 */
  uint32_t keepCount = 0;

  while (1)
  {
    tud_task();
    uint32_t now = board_millis();

    /* === 1. 超时释放 === */
    if (active && (now - last_ir_time >= RELEASE_TIMEOUT_MS)) {
      active = false;
      last_hid_key = 0;
      keepCount = 0;
      gpio_put(PICO_DEFAULT_LED_PIN, 0);
    }

    /* === 2. 处理 PIO FIFO 红外帧 === */
    while (!pio_sm_is_rx_fifo_empty(pio, rx_sm)) {
      uint32_t rx_frame = pio_sm_get(pio, rx_sm);
      
      if (rx_frame == NEC_REPEAT_MARKER) {
        /* NEC repeat code：遥控器长按时每 ~110ms 收到一次 */
        
        if (active && last_hid_key ) {
          last_ir_time = now;
          if(keepCount>0){
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
            send_hid_tap(last_hid_key);
          }
        }
        keepCount=1;
      } else if (nec_decode_frame(rx_frame, &rx_address, &rx_data)) {
        /* 完整 NEC 帧（首次按键 或 部分遥控器长按也重发完整帧） */
        uint8_t hid_key = ir_to_hid(rx_data);
        if (hid_key) {
          last_hid_key = hid_key;
          last_ir_time = now;
          keepCount = 0;
          active = true;
          gpio_put(PICO_DEFAULT_LED_PIN, 1);
          send_hid_tap(hid_key);
        }
        /* 未映射的按键直接忽略，不影响当前活动状态 */
      }
      /* 无效帧直接忽略，不影响当前活动状态 */
    }
  }
  return 0;
}


uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
  (void) itf;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
  (void) itf;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) bufsize;
}