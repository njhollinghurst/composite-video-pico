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

#include "minimal.pio.h"
#include "raspberry_rle_data.h"

/***************************************************************************
 * Simplest example of composite video output from a Raspberry Pi Pico.
 * Outputs NTSC or PAL progressive video from a static 4-bit frame-buffer.
 * Colours are pre-encoded and written to the buffer as nybbles 0x3-0xF.
 *
 * Requires a 4-bit "resistor DAC" to be connected to GPIO pins 15:12.
 * It should have a peak-peak swing of about 1.1V into a 75 Ohm load.
 * A suggested set of resistor values is: 270R, 560R, 1.2K, 2.2K.
 *
 * For NTSC: We require the core clock frequency to be exactly 157.5 MHz.
 * Samples are sent at 11-cycle intervals (14.318 MHz), four times f_c.
 * The active video region has dimensions 752x240. The visible region
 * depends on the TV, but will usually include the central 640x200.
 *
 * For PAL: We require the core clock frequency to be exactly 150.75 MHz.
 * Samples are sent at alternate 8- and 9-cycle intervals (~17.735 MHz),
 * almost 4*f_c. (To avoid jitter, PIO fractional division is not used.)
 * The active video region has dimensions 912x288. The visible region
 * depends on the TV, but will usually include the central 760x256.
 *
 * For simplicity, all the sync, blanking and colour-burst signals are
 * pre-encoded in the same 4-bit frame-buffer that holds the image data.
 * But a linear buffer layout would be inconvenient: total DMA size would
 * be a non-multiple of 4 bytes, and some lines would be oddly aligned!
 * The buffer is therefore divided into rows, organized as a 2D array
 * of uint8_t [262][448] (115K) for NTSC; or [312][552] (169K) for PAL.
 ***************************************************************************/

#ifdef PAL
#define CORE_FREQ_KHZ             150750 // 150.75 MHz
#define CYCLES_PER_ROW              9648 // 15625 Hz
#define CYCLES_PER_SAMPLE            8.5 // 17.735294 MHz ~= 4*f_c
#define SAMPLE_RATE               (CORE_FREQ_KHZ*1000.0/CYCLES_PER_SAMPLE)
#define TOTAL_ROWS                   312
#define SAMPLES_PER_ROW             1104
#define SYNC_PULSE_WIDTH          ((int)(SAMPLE_RATE*4.7e-6))
#define EQ_PULSE_WIDTH            ((int)(SAMPLE_RATE*2.35e-6))
#define BROAD_PULSE_WIDTH         ((int)(SAMPLE_RATE*27.3e-6))
#define ACTIVE_START_ROW              24
#define ACTIVE_START_COL             192
#define ACTIVE_HEIGHT                288
#define ACTIVE_WIDTH                 912
#else
#define CORE_FREQ_KHZ             157500 // 157.5 MHz
#define CYCLES_PER_ROW             10010 // 15734.266 Hz
#define CYCLES_PER_SAMPLE             11 // 14.318182 MHz == 4*f_c
#define SAMPLE_RATE               (CORE_FREQ_KHZ*1000.0/CYCLES_PER_SAMPLE)
#define TOTAL_ROWS                   262
#define SAMPLES_PER_ROW              896
#define SYNC_PULSE_WIDTH          ((int)(SAMPLE_RATE*4.7e-6))
#define EQ_PULSE_WIDTH            ((int)(SAMPLE_RATE*2.31e-6))
#define BROAD_PULSE_WIDTH         ((int)(SAMPLE_RATE*27.1e-6))
#define ACTIVE_START_ROW              22
#define ACTIVE_START_COL             136
#define ACTIVE_HEIGHT                240
#define ACTIVE_WIDTH                 752
#endif


/*
 * The frame-buffer. All samples are nybbles in LE order.
 */

uint8_t buffer[TOTAL_ROWS][SAMPLES_PER_ROW / 2];


/*
 * Initialize the buffer to black and "paint" in the sync and burst pattern.
 */

static void zero_nybbles(uint8_t * buffer, unsigned int offset, unsigned int width)
{
  width += offset;
  if (offset & 1) {
    buffer[offset >> 1] &= 0x0f;
    offset++;
  }
  memset(buffer + (offset>>1), 0, (width - offset) >> 1);
  if (width & 1) {
    buffer[width >> 1] &= 0xf0;
  }
}

void init_buffer()
{
  /*
   * Vertical sync pattern is modified for progressive output
   * such that a single field/frame can be repeated exactly.
   * We note that this can introduce some colour artifacts...
   */
#ifdef PAL
  static const uint16_t vsync_pattern[7][2] = {
    { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
    { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
    { EQ_PULSE_WIDTH,    BROAD_PULSE_WIDTH },
    { BROAD_PULSE_WIDTH, BROAD_PULSE_WIDTH },
    { BROAD_PULSE_WIDTH, BROAD_PULSE_WIDTH },
    { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
    { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH }
  };
#else
  static const uint16_t vsync_pattern[9][2] = {
     { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
     { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
     { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
     { BROAD_PULSE_WIDTH, BROAD_PULSE_WIDTH },
     { BROAD_PULSE_WIDTH, BROAD_PULSE_WIDTH },
     { BROAD_PULSE_WIDTH, BROAD_PULSE_WIDTH },
     { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
     { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH },
     { EQ_PULSE_WIDTH,    EQ_PULSE_WIDTH }
  };
#endif

  memset(buffer, 0x44, sizeof(buffer));

  for(int i = 0; i < TOTAL_ROWS; i++) {
    int w = SYNC_PULSE_WIDTH;
    if (i < sizeof(vsync_pattern)/sizeof(vsync_pattern[0])) {
      zero_nybbles(buffer[i], CYCLES_PER_ROW / CYCLES_PER_SAMPLE / 2,
                   vsync_pattern[i][1]);
      w = vsync_pattern[i][0];
    }
    zero_nybbles(buffer[i], 0, w);

#ifdef PAL
    if (i >= 8 && i < 311) {
      /*
       * This is an approximation to the famous PAL "swinging burst", neglecting
       * the "precision offset", effectively rounding down f_c to 4433593.75 Hz
       * so that burst phase (relative to H0) will be in 4-row pattern (+,+,-,-).
       */
      static const uint8_t burst[2][24] = {
        { 0x44, 0x34, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22,
          0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x45, 0x44 },
        { 0x44, 0x54, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66,
          0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x43, 0x44 },
      };
      memcpy(buffer[i] + 48, burst[(i >> 1) & 1], 24);
    }
#else
    if (i >= 9) {
      /*
       * No such hack is required for NTSC, whose colour burst frequency is an
       * exact half-multiple of the line rate, so phase alternates on every line.
       */
      static const uint8_t burst[2][24] = {
        { 0x44, 0x34, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22,
          0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x45, 0x44, 0x44, 0x44 },
        { 0x44, 0x54, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66,
          0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x22, 0x66, 0x43, 0x44, 0x44, 0x44 },
      };
      memcpy(buffer[i] + 36, burst[i & 1], 24);
    }
#endif
  }
}


/*
 * Set up PIO and DMA to output the buffer continuously
 * with an interrupt and a semaphore to kick user code
 */

static int chan = -1;
static semaphore_t sem;

void handler()
{
  if (chan >= 0) {
    dma_channel_acknowledge_irq0(chan);
    dma_channel_set_read_addr(chan, buffer, true);
    sem_release(&sem);
  }
}

void setup_pio_and_dma()
{
  PIO pio;
  uint sm;
  uint offset;
  dma_channel_config dc;
  pio_sm_config pioc;

  // First set up and start the timer SM
  pio_claim_free_sm_and_add_program(&timer_program, &pio, &sm, &offset);
  pioc = timer_program_get_default_config(offset);
  sm_config_set_out_shift(&pioc, true, true, 32); // only for autopull
  pio_sm_init(pio, sm, offset, &pioc);
  pio_sm_put(pio, sm, CYCLES_PER_ROW - 3);
  pio_sm_set_enabled(pio, sm, true);

  // Now the output SM
  sm = pio_claim_unused_sm(pio, true);
#ifdef PAL
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
  pio_sm_init(pio, sm, offset, &pioc);
  pio_sm_set_consecutive_pindirs(pio, sm, 12, 4, true);
  pio_sm_put(pio, sm, SAMPLES_PER_ROW /2 - 1);

  // Configure GIOs -- need high drive strength for the resistor DAC
  gpio_set_function(12, pio_get_funcsel(pio));
  gpio_set_function(13, pio_get_funcsel(pio));
  gpio_set_function(14, pio_get_funcsel(pio));
  gpio_set_function(15, pio_get_funcsel(pio));
  gpio_set_drive_strength(12, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(13, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(14, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(15, GPIO_DRIVE_STRENGTH_12MA);

  // Now setup DMA -- we'll use the IRQ to restart it
  chan = dma_claim_unused_channel(true);
  dc = dma_channel_get_default_config(chan);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));
  irq_add_shared_handler(DMA_IRQ_0, handler, 0);
  irq_set_enabled(DMA_IRQ_0, true);
  dma_channel_set_irq0_enabled(chan, true);
  dma_channel_configure(chan, &dc, &pio->txf[sm], buffer, sizeof(buffer)/4, true);
  pio_sm_set_enabled(pio, sm, true);
  sem_init(&sem, 0, 1);
}


/*
 * Get a pointer to start of the ith active scanline.
 */

static inline uint8_t *get_line(int i) {
  return buffer[i+ ACTIVE_START_ROW] + ACTIVE_START_COL / 2;
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

  for(int i =  48; i < 56; i++) {
    *(uint32_t *)(get_line(i) + 48) = digits[(n / 1000) % 10][i & 7];
    *(uint32_t *)(get_line(i) + 52) = digits[(n / 100) % 10][i & 7];
    *(uint32_t *)(get_line(i) + 56) = digits[(n / 10) % 10][i & 7];
    *(uint32_t *)(get_line(i) + 60) = digits[n % 10][i & 7];
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

  // initialize frame buffer
  init_buffer();
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
