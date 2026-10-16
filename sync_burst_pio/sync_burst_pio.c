/**
 * Copyright (c) 2026 Raspberry Pi Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/sem.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"

#ifndef PAL
#define PAL 0
#endif

#if PAL
#include "pal.pio.h"
#else
#include "ntsc.pio.h"
#endif

#include "ccube_demo.h"
#include "simple_bold_font_8x8_le1bit.h"


/***************************************************************************
 * Second example of composite video output from a Raspberry Pi Pico.
 * Outputs NTSC or PAL progressive video from a static 4-bit frame-buffer.
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

#if PAL
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
#define STANDARD_BURST_TO_ACTIVE      24
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
#define STANDARD_BURST_TO_ACTIVE      10
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
  int ay = 2 * ((MAX_ACTIVE_HEIGHT - active_height) >> 2);
  int ax = STANDARD_BURST_TO_ACTIVE + 2 * ((MAX_ACTIVE_WIDTH - active_width) >> 3);
  if (ay < 0) ay = 0;
  if (ax < 8) ax = 8;
  if (ax > 62) ax = 62;
  uint32_t *p = syncburst;

  for(int i = 0; i < TOTAL_ROWS; i++) {
    uint32_t w;
    if (i >= MAX_ACTIVE_HEIGHT && i < MAX_ACTIVE_HEIGHT + NUM_HALFLINES) {
#if PAL
      w = (i >= MAX_ACTIVE_HEIGHT + 2 && i < MAX_ACTIVE_HEIGHT + 5) ?
	CODE_HALF_LINE + CODE_BURST_NONE + BROAD_PULSE_WIDTH :
	CODE_HALF_LINE + CODE_BURST_NONE + EQ_PULSE_WIDTH;
      *p++ = w;
      w = (i >= MAX_ACTIVE_HEIGHT + 3 && i < MAX_ACTIVE_HEIGHT + 5) ?
	CODE_HALF_LINE + CODE_BURST_NONE + BROAD_PULSE_WIDTH :
	CODE_HALF_LINE + CODE_BURST_NONE + EQ_PULSE_WIDTH;
      *p++ = w;
#else
      w = (i >= MAX_ACTIVE_HEIGHT + 3 && i < MAX_ACTIVE_HEIGHT + 6) ?
	CODE_HALF_LINE + CODE_BURST_NONE + BROAD_PULSE_WIDTH :
	CODE_HALF_LINE + CODE_BURST_NONE + EQ_PULSE_WIDTH;
      *p++ = w;
      *p++ = w;
#endif
    }
    else {
#if PAL
      if (i >= MAX_ACTIVE_HEIGHT - 1 && i < MAX_ACTIVE_HEIGHT + 8)
	w = CODE_BURST_NONE + SYNC_PULSE_WIDTH;
      else
	w = (i & 2) ? CODE_BURST_NEG + SYNC_PULSE_WIDTH : CODE_BURST_POS + SYNC_PULSE_WIDTH;
#else
      w = (i & 1) ? CODE_BURST_NEG + SYNC_PULSE_WIDTH : CODE_BURST_POS + SYNC_PULSE_WIDTH;
#endif
      w += ax << 24;
      if (i < ay || i >= ay + active_height)
	w += CODE_INACTIVE;
      *p++ = w;
    }
  }
}

/*
 * The frame-buffer -- this time it only holds the active region
 */

#if PAL
#define ACTIVE_HEIGHT 288
#define ACTIVE_WIDTH  912
#else
#define ACTIVE_HEIGHT 240
#define ACTIVE_WIDTH  752
#endif

uint8_t buffer[ACTIVE_HEIGHT][ACTIVE_WIDTH / 2];

/*
 * Set up PIO and DMA to output the buffers continuously
 * with an interrupt and a semaphore to kick user code
 */

static int sync_chan = -1, video_chan = -1;
static semaphore_t sem;


void video_handler()
{
  if (video_chan >= 0) {
    dma_channel_acknowledge_irq1(video_chan);
    dma_channel_set_read_addr(video_chan, buffer, true);
    sem_release(&sem);
  }
}

void sync_handler()
{
  if (sync_chan >= 0) {
    dma_channel_acknowledge_irq0(sync_chan);
    dma_channel_set_read_addr(sync_chan, syncburst, true);
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
#if PAL
  pio_claim_free_sm_and_add_program(&pal_sync_program, &pio, &sync_sm, &offset);
  pioc = pal_sync_program_get_default_config(offset);
#else
  pio_claim_free_sm_and_add_program(&ntsc_sync_program, &pio, &sync_sm, &offset);
  pioc = ntsc_sync_program_get_default_config(offset);
#endif
  sm_config_set_out_shift(&pioc, true, true, 32); // only for autopull
  sm_config_set_fifo_join(&pioc, PIO_FIFO_JOIN_TX);
  sm_config_set_out_pins(&pioc, 12, 4);
  sm_config_set_set_pins(&pioc, 12, 4);
#if PAL
  pio_sm_init(pio, sync_sm, pal_sync_offset_Start, &pioc);
#else
  pio_sm_init(pio, sync_sm, ntsc_sync_offset_Start, &pioc);
#endif
  pio_sm_set_consecutive_pindirs(pio, sync_sm, 12, 4, true);
  sync_chan = dma_claim_unused_channel(true);
  dc = dma_channel_get_default_config(sync_chan);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(pio, sync_sm, true));
  irq_add_shared_handler(DMA_IRQ_0, sync_handler, 0);
  irq_set_enabled(DMA_IRQ_0, true);
  dma_channel_set_irq0_enabled(sync_chan, true);
  dma_channel_configure(sync_chan, &dc, &pio->txf[sync_sm], syncburst, sizeof(syncburst)/4, true);

  // Video state machine and its DMA
  video_sm = pio_claim_unused_sm(pio, true);
#if PAL
  offset = pio_add_program(pio, &scanlines_4bpp_div8p5_program);
  pioc = scanlines_4bpp_div8p5_program_get_default_config(offset);
#else
  offset = pio_add_program(pio, &scanlines_4bpp_div11_program);
  pioc = scanlines_4bpp_div11_program_get_default_config(offset);
#endif
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
  irq_add_shared_handler(DMA_IRQ_1, video_handler, 0);
  irq_set_enabled(DMA_IRQ_1, true);
  dma_channel_set_irq1_enabled(video_chan, true);
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
  memset(buffer, 0x44, sizeof(buffer));
}




/*
 * Draw text
 */

void draw_text(int x, int y, const char *str)
{
  for(int i = 0; i < 8; i++) {
    uint16_t *ptr = (uint16_t *)(get_line(y + i) + (x >> 1));
    for(const char *sp = str; *sp; sp++) {
      static const uint16_t explode_nybble[16] = {
        0x5555, 0x555e, 0x55e5, 0x55ee, 0x5e55, 0x5e5e, 0x5ee5, 0x5eee,
        0xe555, 0xe55e, 0xe5e5, 0xe5ee, 0xee55, 0xee5e, 0xeee5, 0xeeee
      };
      int ix = (*sp >= ' ' && *sp <= '~') ? *sp - ' ' : 95;
      ptr[0] = explode_nybble[simple_bold_font_8x8_le1bit[ix][i] & 0xf];
      ptr[1] = explode_nybble[simple_bold_font_8x8_le1bit[ix][i] >> 4];
      ptr += 2;
    }
  }
}

void main()
{
  unsigned int frame_count = 0;

  // PAL's 150.75 MHz requires a special clock setup with REFDIV=2
  // otherwise we simply set the core clock to 157.5 MHz (for NTSC).
  // Strictly speaking, both settings are a slight overclock.
  vreg_set_voltage(VREG_VOLTAGE_1_15);
#if !PAL
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
  ccube_demo_init(ACTIVE_WIDTH, ACTIVE_HEIGHT, PAL);

  // start output
  setup_pio_and_dma();

  while (true) {
    char str[80];
    sprintf(str, "%s composite video from Raspberry Pi Pico via 4-bit \"DAC\" %dx%dp %dfps", PAL ? "PAL" : "NTSC", ACTIVE_WIDTH, ACTIVE_HEIGHT, PAL ? 50 : 60);
    sem_acquire_blocking(&sem);
    gpio_put(PICO_DEFAULT_LED_PIN, (frame_count++ & 16) ? 0 : 1);
    ccube_demo_render(get_line(0));
    draw_text((ACTIVE_WIDTH - 73*8) / 2, ACTIVE_HEIGHT - 40, str);
    ccube_demo_update();
  }
}
