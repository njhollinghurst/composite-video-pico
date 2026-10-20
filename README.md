# composite-video-pico
Experiments to generate colour composite video output (NTSC/PAL) from Raspberry Pi Pico.

I've targeted simplicity over high quality or standards compliance, and with little regard for EMC. Some colour and pattern artifacts are to be expected, due to the lack of proper filtering, and liberties taken with timings.

This is a work in progress, largely undocumented and entirely without warranty. It may be broken from time to time or squashed/rebased without warning.

## Experiment 1: Minimal

Probably the simplest way to generate colour video. The following external hardware is used:

![Simple 4-bit resistor DAC](doc/rdac.png)

The entire frame, including sync signals and colour bursts, is "painted" into a 4-bit-per-pixel buffer, scanned out by PIO and... that's it.

This simplicity comes at a cost: Only progressive modes (up to 752x240p, 912x288p) are supported. The buffer is huge (115K for NTSC, 168K for PAL), as it must cover all sync pulses as well as the active region. To help with memory alignment and timing, the buffer is arranged as discrete scanlines; the roughly 1 &mu;s gaps between lines are filled with blanking value.

![Visualisation of progressive frame buffer](doc/minimal_viz.png)

Colours have to be encoded by software, so the sample-rate was chosen to simplify this (14.318 MHz or 17.735 MHz). This in turn requires a specific core clock rate: 157.5 MHz for NTSC or 150.75 MHz for PAL. (Higher rates may be possible, if you want to overclock, but I haven't tried any.)

NTSC is encoded pretty much by-the-book. For PAL, a bodge was needed: I've reduced the effective colour carrier frequency from 4433618.75 Hz to 4433593.75 Hz, a discrepancy of -5.6 ppm (on top of the Pico crystal's &pm; 30 ppm tolerance). This is essential to ensure that a single 312-line field can be repeated exactly.

## Experiment 2: Sync and burst in PIO

A modification to the above, using PIO to generate all sync pulses and colour bursts.

This is difficult for PIO due to its limited internal state; I had to give it a buffer of
short "commands" specifying the sync and burst structure. Each 32-bit word encodes a sync
pulse width, the presence and polarity of a colour burst, the burst-to-active delay,
and whether each scanline is an active one. Active lines raise an interrupt which triggers the active video SM.

This uses more PIO and DMA resources, but saves a significant amount of memory, since the
main buffer now only needs to hold active video (88K for 752x240p NTSC; 128K for 912x288p PAL).
The frame can be cropped smaller than this, saving even more memory.

Output quality is much the same as before, including the same colour and texture artifacts.

![Colour cube demo](doc/ccube_viz.png)

This time I added an animated demo, which visualises the use of a 6x6x6 "colour cube" palette. (It runs single-buffered, but is timed to avoid tearing.)

## Experiment 3: Rough interlace

First attempt at interlaced video! The sync-and-burst pattern is modified (to generate 4 fields for NTSC, 8 fields for PAL), while retaining a single-field buffer for the video.

Since fields alternate between upper and lower *and* the colour-encoding phase changes on a 4- or 8-field cycle, it is necessary for S/W to re-write the buffer for every field. The cube demo was hastily modified to render either an upper or a lower field. The cube now stop rotating periodically, so the full-resolution image can be examined more closely.

Although artifacts are significantly reduced compared to progressive, interlaced PAL still suffers from an unpleasant "dot-crawl" effect. That is because I'm still omitting the 25Hz precision offset.

### Things maybe to do:

 - Implement proper circular buffering à la scanvideo (maybe in broad strips for easier 3D rendering)
 - Fix up the half-lines (currently they are output as full lines, and in the PAL case impinge on vertical blanking)
 - Investigate if I can restore the PAL 25Hz precision offset, preferably without breaking palette lookups
 - Investigate efficient ways to use different horizontal sample rates, which some emulators might want
 - Investigate efficient ways to filter the chroma components to reduce artifacts
