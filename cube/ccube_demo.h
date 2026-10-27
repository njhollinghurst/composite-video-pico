/**
 * Copyright (c) 2026 Raspberry Pi Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Note that "full_frame_height" and "start_y" are in frame rows (for interlace)
// or should be doubled when progressive. To render a lower field, "start_y"
// should be an odd number. "stripe_rows" is the number of contiguous rows
// to render, in the current field only. "buffer" and "chroma_phase" pertain
// to the top line of the current stripe.

extern void ccube_demo_init(int width_in_nybbles, int full_frame_height, int pal);

extern void ccube_demo_render(void *buffer, int start_y, int stripe_rows, int chroma_phase);

extern void ccube_demo_update();
