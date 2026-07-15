/**
 * Copyright (c) 2021 mjcross
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef NEC_RECEIVE_H
#define NEC_RECEIVE_H

#include "hardware/pio.h"
#include "pico/stdlib.h"

// public API
//
int nec_rx_init(PIO pio, uint pin);
bool nec_decode_frame(uint32_t frame, uint8_t *p_address, uint8_t *p_data);

#endif
