/*
 * hello.c — Minimal test binary for SN110 bFLT verification
 *
 * Does nothing except write "hello" to stdout and exit.
 * If this works on the device, the bFLT format and OABI syscalls are correct.
 */

/* OABI write syscall — inlined to minimize dependencies */
static int sys_write(int fd, const void *buf, int len) {
    register int r0 __asm__("r0") = fd;
    register const void *r1 __asm__("r1") = buf;
    register int r2 __asm__("r2") = len;
    __asm__ volatile(
        "swi #0x900004"   /* __NR_write = 4 */
        : "+r"(r0)
        : "r"(r1), "r"(r2)
        : "memory"
    );
    return r0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    sys_write(1, "sn110dmx: bFLT hello from open firmware!\n", 41);
    return 0;
}
