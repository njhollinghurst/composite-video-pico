/**
 * Copyright (c) 2026 Raspberry Pi Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/sem.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"

#include "ntsc.pio.h"
#include "raspberry_rle_data.h"

/***************************************************************************
 * Second example of composite video output from a Raspberry Pi Pico.
 * Outputs NTSC (** XXX ONLY, for now **) progressive video from a static 4-bit frame-buffer.
 * Colours are pre-encoded and written to the buffer as nybbles 0x3-0xF.
 *
 * Requires a 4-bit "resistor DAC" to be connected to GPIO pins 15:12.
 * It should have a peak-peak swing of about 1.1V into a 75 Ohm load.
 * A suggested set of resistor values is: 270R, 560R, 1.2K, 2.2K.
 *
 * For NTSC: We require the core clock frequency to be exactly 157.5 MHz.
 * Samples are sent at 11-cycle intervals (14.318 MHz), four times f_c.
 * The active video buffer may be any size up to a maximum 752x240.
 * The visible region depends on the TV; usually at least 640x200.
 *
 * For PAL: We require the core clock frequency to be exactly 150.75 MHz.
 * Samples are sent at alternate 8- and 9-cycle intervals (~17.735 MHz),
 * almost 4*f_c. (To avoid jitter, PIO fractional division is not used.)
 * The active video buffer may be any size up to a maximum 912x288.
 * The visible region depends on the TV; usually at least 760x256.
 *
 * This version uses a dedicated PIO state machine (and a small buffer
 * of "commands" to help it) to generate sync and colour-burst signals,
 * so the main buffer only needs to hold video data. The buffer has 4 bits
 * per pixel, but this approach could be generalized to 8bpp or even 1bpp.
 ***************************************************************************/

#ifdef PAL
#define CORE_FREQ_KHZ             150750 // 150.75 MHz
#define CYCLES_PER_ROW              9648 // 15625 Hz
#define CYCLES_PER_SAMPLE            8.5 // 17.735294 MHz ~= 4*f_c
#define SAMPLE_RATE               (CORE_FREQ_KHZ*1000.0/CYCLES_PER_SAMPLE)
#define NUM_HALFLINES                  7
#define TOTAL_ROWS                   312
#define SAMPLES_PER_ROW             1104
#define SYNC_PULSE_WIDTH          ((int)(CORE_FREQ_KHZ*500.0*4.7e-6)-1)
#define EQ_PULSE_WIDTH            ((int)(CORE_FREQ_KHZ*500.0*2.35e-6)-1)
#define BROAD_PULSE_WIDTH         ((int)(CORE_FREQ_KHZ*500.0*27.3e-6)-1)
#define NOMINAL_ACTIVE_START_ROW       4
#define NOMINAL_ACTIVE_START_COL     192
#define MAX_ACTIVE_HEIGHT            288
#define MAX_ACTIVE_WIDTH             912
#else
#define CORE_FREQ_KHZ             157500 // 157.5 MHz
#define CYCLES_PER_ROW             10010 // 15734.266 Hz
#define CYCLES_PER_SAMPLE             11 // 14.318182 MHz == 4*f_c
#define SAMPLE_RATE               (CORE_FREQ_KHZ*1000.0/CYCLES_PER_SAMPLE)
#define NUM_HALFLINES                  9
#define TOTAL_ROWS                   262
#define SAMPLES_PER_ROW              896
#define SYNC_PULSE_WIDTH          ((int)(CORE_FREQ_KHZ*500.0*4.7e-6)-1)
#define EQ_PULSE_WIDTH            ((int)(CORE_FREQ_KHZ*500.0*2.31e-6)-1)
#define BROAD_PULSE_WIDTH         ((int)(CORE_FREQ_KHZ*500.0*27.1e-6)-1)
#define NOMINAL_ACTIVE_START_ROW       4
#define NOMINAL_ACTIVE_START_COL     136
#define MAX_ACTIVE_HEIGHT            240
#define MAX_ACTIVE_WIDTH             752
#endif

/*
 * The buffer containing sync-and-burst commands, and code to initialize it.
 * We now put active video near the top, so that video DMA will run out
 * before sync DMA, but will not need refilling until sync is refilled.
 * XXX Only supporting NTSC progressive for now!
 */

uint32_t syncburst[TOTAL_ROWS + NUM_HALFLINES];

#define CODE_INACTIVE   0x80000000
#define CODE_HALF_LINE  0xc0000000
#define CODE_BURST_NONE 0x00924000
#define CODE_BURST_POS  0x00ad6000
#define CODE_BURST_NEG  0x00772000

static void init_syncburst(unsigned int active_width, unsigned int active_height)
{
  /* NTSC ONLY; PAL TODO */

  int ay = NOMINAL_ACTIVE_START_ROW + 2 * ((MAX_ACTIVE_HEIGHT - active_height) >> 2);
  int ax = (NOMINAL_ACTIVE_START_COL >> 1) + 2 * ((MAX_ACTIVE_WIDTH - active_width) >> 3) - 58;
  if (ay < 0) ay = 0;
  if (ax < 8) ax = 8;
  if (ax > 62) ax = 62;
  uint32_t *p = syncburst;

  for(int i = 0; i < TOTAL_ROWS; i++) {
    uint32_t w;
    if (i >= NOMINAL_ACTIVE_START_ROW + MAX_ACTIVE_HEIGHT &&
	i < NOMINAL_ACTIVE_START_ROW + MAX_ACTIVE_HEIGHT + NUM_HALFLINES) {
      w = (i >= NOMINAL_ACTIVE_START_ROW + MAX_ACTIVE_HEIGHT + 3 &&
	   i <  NOMINAL_ACTIVE_START_ROW + MAX_ACTIVE_HEIGHT + 6) ?
	CODE_HALF_LINE + CODE_BURST_NONE + BROAD_PULSE_WIDTH :
	CODE_HALF_LINE + CODE_BURST_NONE + EQ_PULSE_WIDTH;
      *p++ = w;
      *p++ = w;
    }
    else {
      w = (i & 1) ? CODE_BURST_NEG + SYNC_PULSE_WIDTH : CODE_BURST_POS + SYNC_PULSE_WIDTH;
      w += ax << 24;
      if (i < ay || i >= ay + active_height)
	w += CODE_INACTIVE;
      *p++ = w;
    }
  }
}

/*
 * The frame-buffer -- this time only 64K
 */

#define ACTIVE_HEIGHT 200
#define ACTIVE_WIDTH  640

uint8_t buffer[ACTIVE_HEIGHT][ACTIVE_WIDTH / 2];

/*
 * Set up PIO and DMA to output the buffers continuously
 * with an interrupt and a semaphore to kick user code
 */

static int sync_chan = -1, video_chan = -1;
static semaphore_t sem;

void handler()
{
  if (sync_chan >= 0) {
    dma_channel_acknowledge_irq0(sync_chan);
    dma_channel_set_read_addr(sync_chan, syncburst, true);
    dma_channel_set_read_addr(video_chan, buffer, true);
    sem_release(&sem);
  }
}

void setup_pio_and_dma()
{
  PIO pio;
  uint timer_sm, sync_sm, video_sm;
  uint offset;
  dma_channel_config dc;
  pio_sm_config pioc;

  // Sync state machine and its DMA
  pio_claim_free_sm_and_add_program(&ntsc_sync_program, &pio, &sync_sm, &offset);
  pioc = ntsc_sync_program_get_default_config(offset);
  sm_config_set_out_shift(&pioc, true, true, 32); // only for autopull
  sm_config_set_fifo_join(&pioc, PIO_FIFO_JOIN_TX);
  sm_config_set_out_pins(&pioc, 12, 4);
  sm_config_set_set_pins(&pioc, 12, 4);
  pio_sm_init(pio, sync_sm, ntsc_sync_offset_Start, &pioc);
  pio_sm_set_consecutive_pindirs(pio, sync_sm, 12, 4, true);
  sync_chan = dma_claim_unused_channel(true);
  dc = dma_channel_get_default_config(sync_chan);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(pio, sync_sm, true));
  irq_add_shared_handler(DMA_IRQ_0, handler, 0);
  irq_set_enabled(DMA_IRQ_0, true);
  dma_channel_set_irq0_enabled(sync_chan, true);
  dma_channel_configure(sync_chan, &dc, &pio->txf[sync_sm], syncburst, sizeof(syncburst)/4, true);

  // Video state machine and its DMA
  video_sm = pio_claim_unused_sm(pio, true);
  offset = pio_add_program(pio, &ntsc_4bpp_program);
  pioc = ntsc_4bpp_program_get_default_config(offset);
  sm_config_set_out_shift(&pioc, true, true, 32);
  sm_config_set_fifo_join(&pioc, PIO_FIFO_JOIN_TX);
  sm_config_set_out_pins(&pioc, 12, 4);
  sm_config_set_set_pins(&pioc, 12, 4);
  pio_sm_init(pio, video_sm, offset, &pioc);
  pio_sm_put(pio, video_sm, ACTIVE_WIDTH/2 - 1);
  pio_sm_set_enabled(pio, video_sm, true); // it will wait for an interrupt
  video_chan = dma_claim_unused_channel(true);
  dc = dma_channel_get_default_config(video_chan);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(pio, video_sm, true));
  dma_channel_configure(video_chan, &dc, &pio->txf[video_sm], buffer, sizeof(buffer)/4, true);
  pio_sm_set_enabled(pio, sync_sm, true); // it will wait for an interrupt

  // Configure GIOs -- need high drive strength for the resistor DAC
  gpio_set_function(12, pio_get_funcsel(pio));
  gpio_set_function(13, pio_get_funcsel(pio));
  gpio_set_function(14, pio_get_funcsel(pio));
  gpio_set_function(15, pio_get_funcsel(pio));
  gpio_set_drive_strength(12, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(13, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(14, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(15, GPIO_DRIVE_STRENGTH_12MA);
  sem_init(&sem, 0, 1);

  // Timer SM, will start the others
  timer_sm = pio_claim_unused_sm(pio, true);
  offset = pio_add_program(pio, &timer_program);
  pioc = timer_program_get_default_config(offset);
  sm_config_set_out_shift(&pioc, true, true, 32); // only for autopull
  pio_sm_init(pio, timer_sm, offset, &pioc);
  pio_sm_set_consecutive_pindirs(pio, timer_sm, 12, 4, true);
  pio_sm_put(pio, timer_sm, CYCLES_PER_ROW/2 - 3);
  pio_sm_set_enabled(pio, timer_sm, true);
}


/*
 * Get a pointer to start of the ith active scanline.
 */

static inline uint8_t *get_line(int i) {
  return buffer[i];
}

static void init_buffer() {
  memset(buffer, 0x40, sizeof(buffer));
}


/*
 * Convert (R,G,B) to an array of four unsigned shorts.
 * The index should be the scanline number modulo 4 (modulo 2 for NTSC).
 * Each byte-pair yields for four samples, assuming 2-byte alignment.
 */

#define CLIP(v) (((v)<3)?3:((v)>15)?15:(v))

void mk_colour(uint8_t r, uint8_t g, uint8_t b, uint16_t pattern[4])
{
#ifdef PAL

  // Convert to YUV. We want to scale U, V so that the 4.433 MHz component
  // of our "steppy" U and V waveforms will have the same power as a pure
  // sine wave at the specified level; which requires a boost of ~10%.
  int y = (77*r + 150*g + 29*b) >> 8;
  int u = (138*(b-y) + 128) >> 8;
  int v = (247*(r-y) + 128) >> 8;

  // Generate the 4x4 pixel pattern; all rows are permutations of the
  // top one (except for dithering). Black level is 4, white level 14.
  pattern[0] = CLIP((0x440 + 10 * (y - u)) >> 8) +
          16 * CLIP((0x4c0 + 10 * (y + v)) >> 8) +
         256 * CLIP((0x440 + 10 * (y + u)) >> 8) +
        4096 * CLIP((0x4c0 + 10 * (y - v)) >> 8);
  pattern[1] = CLIP((0x4c0 + 10 * (y + v)) >> 8) +
          16 * CLIP((0x440 + 10 * (y - u)) >> 8) +
         256 * CLIP((0x4c0 + 10 * (y - v)) >> 8) +
        4096 * CLIP((0x440 + 10 * (y + u)) >> 8);
  pattern[2] = (pattern[0] >> 8) | (pattern[0] << 8);
  pattern[3] = (pattern[1] >> 8) | (pattern[1] << 8);

#else

  // Convert to YUV. Although NTSC specifies I and Q, the underlying U, V
  // are defined much as in PAL (if we ignore chroma bandwidth limiting).
  // Due to a sampling phase difference we'll scale them by 1.1 / sqrt(2).
  int y = (77*r + 150*g + 29*b) >> 8;
  int u = (98*(b-y) + 128) >> 8;
  int v = (175*(r-y) + 128) >> 8;

  // Generate the 4x2 pixel pattern. For NTSC-M, black level is 5.
  pattern[0] = CLIP((0x510 + 9 * (y - u - v)) >> 8) +
          16 * CLIP((0x590 + 9 * (y - u + v)) >> 8) +
         256 * CLIP((0x510 + 9 * (y + u + v)) >> 8) +
        4096 * CLIP((0x590 + 9 * (y + u - v)) >> 8);
  pattern[1] = (pattern[0] >> 8) | (pattern[0] << 8);
  pattern[2] = pattern[0];
  pattern[3] = pattern[1];

#endif
}

#undef CLIP


/*
 * Draw colour bars
 */

void draw_colour_bars()
{
  for(int i = 0; i < ACTIVE_HEIGHT; i++) {
    uint16_t *ptr = (uint16_t *)get_line(i);
    int x = 0;
    uint16_t val = 0xeeee;
    for(int b = 0; b < 8; b++) {
      uint16_t pattern[4];
      if (b < 7)
        mk_colour((b & 2) ? 0 : 255, (b & 4) ? 0 : 255, (b & 1) ? 0 : 255, pattern);
      else
        mk_colour(i*256/ACTIVE_HEIGHT, i*256/ACTIVE_HEIGHT, i*256/ACTIVE_HEIGHT, pattern);

      // smooth the boundaries to reduce colour artifacts (ugh)
      *ptr++ =
        ((3 * (val & 0xf) + (pattern[i & 3] & 0xf)) >> 2) +
        ((((val & 0xf0) + 3 * (pattern[i & 3] & 0xf0)) >> 2) & 0x00f0) +
        (pattern[i & 3] & 0xff00);
      val = pattern[i & 3];
      for(x++; x < ACTIVE_WIDTH * (b + 1) / 32; x++) {
        *ptr++ = val;
      }
    }
  }
}

void draw_raspberry()
{
  const unsigned char *ptr = raspberry_rle_data;
  uint16_t palette[3][4];

  mk_colour(0, 0, 0, palette[0]);
  mk_colour(255, 0, 64, palette[1]);
  mk_colour(0, 200, 0, palette[2]);

  for(int i = 0; i < RASPBERRY_HEIGHT; i++) {
    int c, x0 = 0, x1 = 0;
    unsigned char *scanline =
      get_line((ACTIVE_HEIGHT - RASPBERRY_HEIGHT) / 2 + i) +
      (ACTIVE_WIDTH - RASPBERRY_WIDTH) / 4;
    while (x0 < RASPBERRY_WIDTH) {
      c = *ptr++;
      x1 = x0 + (c & 0x3f);
      if (x1 == x0) x1 = RASPBERRY_WIDTH;
      c >>= 6;
      if (c < 3) {
	if (x0 & 1) {
	  scanline[x0>>1] = (scanline[x0>>1] & 0xf) | ((palette[c][i & 3] >> (4 * (x0 & 3))) << 4);
	  x0++;
	}
	for (;x0 < x1; x0 += 2) {
	  scanline[x0>>1] = palette[c][i & 3] >> (4 * (x0 & 2));
	}
      }
      x0 = x1;
    }
  }
}

/*
 * Draw counter
 */

void draw_counter(unsigned int n)
{
  static const uint32_t digits[10][8] = {
    {
      0x59eeee95,
      0x9ee55ee9,
      0xaee55eea,
      0xbee55eeb,
      0xaee55eea,
      0x9ee55ee9,
      0x59eeee95,
      0x55555555,
    },
    {
      0x559ee955,
      0x559eee95,
      0x559ee955,
      0x559ee955,
      0x559ee955,
      0x559ee955,
      0x559ee955,
      0x55555555,
    },
    {
      0x59eeee95,
      0xbee55ee9,
      0xbee55555,
      0x59eee955,
      0x5559ee95,
      0x95555ee9,
      0x9eeeeeeb,
      0x55555555,
    },
    {
      0x59eeee95,
      0xbee55eeb,
      0xbee55555,
      0x59eee955,
      0xbee55555,
      0xbee55eeb,
      0x59eeee95,
      0x55555555,
    },
    {
      0x59ee9555,
      0x59eee955,
      0x59eeee95,
      0x59ee5ee9,
      0x9eeeeeee,
      0x59ee9555,
      0x59ee9555,
      0x55555555,
    },
    {
      0x9eeeeee9,
      0x55559ee9,
      0x59eeeee9,
      0xbee95555,
      0xbee95555,
      0xbee55eeb,
      0x59eeee95,
      0x55555555,
    },
    {
      0x59eeee95,
      0x9ee55eeb,
      0x55555eeb,
      0x59eeeee9,
      0xbee55eeb,
      0xbee55eeb,
      0x59eeee95,
      0x55555555,
    },
    {
      0x9eeeeee9,
      0x9ee55555,
      0x5ee95555,
      0x59ee5555,
      0x55ee9555,
      0x559ee555,
      0x555ee955,
      0x55555555,
    },
    {
      0x59eeee95,
      0xbee55eeb,
      0xbee55eeb,
      0x59eeee95,
      0xbee55eeb,
      0xbee55eeb,
      0x59eeee95,
      0x55555555,
    },
    {
      0x59eeee95,
      0xbee55eeb,
      0xbee55eeb,
      0x9eeeee95,
      0xbee95555,
      0xbee55ee9,
      0x59eeee95,
      0x55555555,
    }
  };

  for(int i =  16; i < 24; i++) {
    *(uint32_t *)(get_line(i) + 16) = digits[(n / 1000) % 10][i & 7];
    *(uint32_t *)(get_line(i) + 20) = digits[(n / 100) % 10][i & 7];
    *(uint32_t *)(get_line(i) + 24) = digits[(n / 10) % 10][i & 7];
    *(uint32_t *)(get_line(i) + 28) = digits[n % 10][i & 7];
  }
}

void main()
{
  unsigned int frame_count = 0;

  // PAL's 150.75 MHz requires a special clock setup with REFDIV=2
  // otherwise we simply set the core clock to 157.5 MHz (for NTSC).
  // Strictly speaking, both settings are a slight overclock.
  vreg_set_voltage(VREG_VOLTAGE_1_15);
#ifndef PAL
  set_sys_clock_khz(157500, true);
#endif

  // Initialize LED pin and change 3.3V SMPS mode to PWM
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);
  gpio_init(23);
  gpio_set_dir(23, GPIO_OUT);
  gpio_put(23, 1);

  // initialize frame buffer and sync command buffer
  init_buffer();
  init_syncburst(ACTIVE_WIDTH, ACTIVE_HEIGHT);
  draw_colour_bars();
  draw_raspberry();

  // start output
  setup_pio_and_dma();

  while (true) {
    sem_acquire_blocking(&sem);
    gpio_put(PICO_DEFAULT_LED_PIN, (frame_count & 16) ? 0 : 1);
    /*** Your demo here ***/
    draw_counter(++frame_count);
  }
}
