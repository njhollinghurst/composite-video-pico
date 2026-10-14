# composite-video-pico
Experiments to generate colour composite video output (NTSC/PAL) from Raspberry Pi Pico.

I am targeting simplicity over high quality or standards compliance. Some colour and pattern artifacts are to be expected, due to the lack of proper filtering, and liberties taken with timings.

The following external hardware is used:
![Simple 4-bit resistor DAC](doc/rdac.png)

Some programs will require very particular core clock frequencies (e.g. 157.5 MHz for NTSC, or 150.75 MHz for PAL) and will not work at other rates.

This is a work in progress, largely undocumented and entirely without warranty. It may be broken from time to time or squashed/rebased without warning.
