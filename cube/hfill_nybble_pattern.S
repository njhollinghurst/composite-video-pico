#include "pico/asm_helper.S"

pico_default_asm_setup

.syntax unified

	// void hfill_nybble_pattern(ptr, pattern, x0, x1);
	// ptr is assumed to be 4-byte aligned
	// pattern is a single 16-bit fill pattern
	// x0, x1 are start and end positions in *nybbles*

.section .time_critical_hfill_nybble_pattern
regular_func hfill_nybble_pattern
	push {r4,r5}
	lsls r4, r1, #16
	lsrs r5, r3, #3
	orrs r1, r4      // r1 = pattern (32-bit)
	lsrs r4, r2, #3
	lsls r5, #2
	lsls r4, #2
	adds r5, r0      // r5 = end word addres
	adds r0, r4      // r0 = start word address
	lsls r2, #29     // r2 = start nybble index << 29
	beq  2f
	mvns r4, r2      // r4 will now have all bits 29:0 set
	lsrs r2, #27
	cmp  r0, r5
	blo  1f
	lsls r3, #29
	lsrs r3, #27
	mov  r5, r4
	lsls r4, r2
	lsls r5, r3
	bics r4, r5
	ldr  r2, [r0]
	bics r2, r4
	ands r4, r1
	orrs r2, r4
	str  r2, [r0]
	pop {r4,r5}
	bx lr
1:
	lsls r4, r2
	ldr  r2, [r0]
	bics r2, r4
	ands r4, r1
	orrs r2, r4
	stmia r0!, {r2}
2:
	cmp r0, r5
	bhs 4f
3:
	stmia r0!, {r1}
	cmp  r0, r5
	bhs  4f
	stmia r0!, {r1}
	cmp  r0, r5
	blo  3b
4:
	lsls r3, #29
	beq 9f
	mvns r4, r3
	lsrs r3, #27
	lsls r4, r3
	ldr  r2, [r0]
	ands r2, r4
	bics r1, r4
	orrs r2, r1
	str  r2, [r0]
9:
	pop {r4,r5}
	bx lr
