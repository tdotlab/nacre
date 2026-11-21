@ Source of the test program in tests/nds_test_rom.h. Rebuild with the GNU Arm
@ toolchain: assemble each half (ARM9 with -march=armv5te, ARM7 with
@ -march=armv4t), objcopy -O binary, and paste the words into the header.

@ ---- ARM9, loaded and entered at 0x02000000 ----
    .arm
    .text
start:
    msr cpsr_c, #0x92           @ IRQ mode: the boot stacks point at WRAM the
    ldr sp, =0x027C3F00         @ ARM9 can't see, so set ours in DTCM, as every
    msr cpsr_c, #0x1F           @ game's startup code does
    ldr sp, =0x027C3E00
    mov r0, #0x04000000
    mov r1, #0x00010000         @ DISPCNT: display mode 1 (backdrop)
    str r1, [r0]
    add r4, r0, #0x1000
    str r1, [r4]                @ engine B too
    mov r2, #0x05000000
    mov r3, #0x1F               @ engine A backdrop red
    strh r3, [r2]
    add r5, r2, #0x400
    mov r3, #0x3E0              @ engine B backdrop green
    strh r3, [r5]
    ldr r6, =0x02100000
    mov r0, #100
    mov r1, #7
    swi 0x090000                @ Div
    str r0, [r6]
    str r1, [r6, #4]
    mov r0, #0x04000000
    add r7, r0, #0x180
wait_ipc:
    ldrh r1, [r7]               @ IPCSYNC input from the ARM7
    and r1, r1, #15
    cmp r1, #5
    bne wait_ipc
    str r1, [r6, #12]
    ldr r1, =0x027C3FFC         @ IRQ handler slot at the end of DTCM
    adr r2, handler
    str r2, [r1]
    mov r1, #8
    strh r1, [r0, #4]           @ DISPSTAT: VBlank IRQ enable
    mov r1, #1
    str r1, [r0, #0x210]        @ IE = VBlank
    str r1, [r0, #0x208]        @ IME
    mov r8, #3
vbl:
    swi 0x050000                @ VBlankIntrWait
    subs r8, r8, #1
    bne vbl
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]        @ IME off
    mov r1, #0x77
    str r1, [r6, #16]           @ done marker
halt:
    mcr p15, 0, r0, c7, c0, 4
    b halt
handler:
    mov r0, #0x04000000
    ldr r1, [r0, #0x214]
    str r1, [r0, #0x214]
    ldr r2, =0x027C3FF8
    ldr r3, [r2]
    orr r3, r3, r1
    str r3, [r2]
    ldr r2, =0x02100008
    ldr r3, [r2]
    add r3, r3, #1
    str r3, [r2]
    bx lr
    .ltorg

@ ---- ARM7, loaded and entered at 0x037F8000 ----
    .arm
    .text
    mov r0, #0x04000000
    mov r1, #0x500              @ IPCSYNC output 5
    add r0, r0, #0x180
    strh r1, [r0]
    mov r0, #0x04000000
    mov r1, #0x80
halt:
    strb r1, [r0, #0x301]       @ HALTCNT: halt
    b halt
