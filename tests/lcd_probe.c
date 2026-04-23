/*
 * lcd_probe — /dev/lcd0 interface discovery tool for SN110
 *
 * Systematically tests write protocols to discover how the LCD driver
 * works. Output goes to stdout (telnet terminal). Watch the LCD between
 * steps — each test sleeps for 2 seconds so you can observe the result.
 *
 * Usage:
 *   lcd_probe all        Run all tests in sequence (recommended first run)
 *   lcd_probe text       Test 1: plain 16-char and 64-char ASCII writes
 *   lcd_probe newline    Test 2: \n and \r\n as line separators
 *   lcd_probe formfeed   Test 3: \f (form feed) as clear-screen
 *   lcd_probe ansi       Test 4: ANSI ESC[H cursor positioning
 *   lcd_probe fe         Test 5: \xfe prefix HD44780 command escape
 *   lcd_probe chars      Test 6: write raw bytes 0x00-0x07 (custom char slots)
 *   lcd_probe ioctl      Test 7: ioctl _IOW('l',7,48) discovered from lxnetdmx
 *   lcd_probe read       Test 8: try read() to see if driver returns info
 *   lcd_probe partial    Test 9: targeted partial write experiments
 *   lcd_probe nulltest   Test 10: NUL/special byte transparency probe
 *   lcd_probe counter    Test 11: close/open counter reset + ioctl NUL behavior
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define LCD_DEV "/dev/lcd0"

/*
 * LCD ioctl encoding (same scheme as DMX driver):
 *   _IOC(dir, type, nr, size)
 *   dir  = bits 30-31  (1 = _IOC_WRITE)
 *   size = bits 16-29
 *   type = bits 8-15
 *   nr   = bits 0-7
 *
 * Discovered from lxnetdmx binary: _IOW('l', 7, 48) = 0x40306C07
 */
#define _IOC_WRITE  1
#define LCD_IOC(nr, size) \
    ((_IOC_WRITE << 30) | ((size) << 16) | ('l' << 8) | (nr))

/* ===== Helpers ===== */

static int g_fd = -1;

static void pause_observe(void)
{
    /* 2 seconds: enough time to glance at the LCD */
    usleep(2000000);
}

static void wlcd(const void *buf, int len)
{
    int n = write(g_fd, buf, len);
    printf("  write(%d bytes) -> %d\n", len, n);
}

/* ===== Test 1: plain ASCII ===== */

static void test_text(void)
{
    printf("\n[Test 1a] Plain 16-char write\n");
    printf("  Expecting: first 16 chars appear on line 1\n");
    wlcd("TEST1-PLAIN-16  ", 16);
    pause_observe();

    printf("\n[Test 1b] 64-char block (4 x 16)\n");
    printf("  Expecting: all 4 lines filled if driver accepts a full screen buffer\n");
    wlcd(
        "LINE1-BLOCK-16  "
        "LINE2-BLOCK-16  "
        "LINE3-BLOCK-16  "
        "LINE4-BLOCK-16  ",
        64);
    pause_observe();

    printf("\n[Test 1c] Single-char writes in a loop (16 chars)\n");
    printf("  Expecting: characters accumulate across positions\n");
    {
        int i;
        const char *s = "SINGLE-CHAR-LOOP";
        for (i = 0; i < 16; i++) {
            wlcd(s + i, 1);
            usleep(100000); /* 100ms per char */
        }
    }
    pause_observe();
}

/* ===== Test 2: newline / CR ===== */

static void test_newline(void)
{
    printf("\n[Test 2a] Newline-separated 4 lines\n");
    printf("  Expecting: \\n moves cursor to next line\n");
    wlcd("LINE1\nLINE2\nLINE3\nLINE4\n", 24);
    pause_observe();

    printf("\n[Test 2b] CR+LF separated\n");
    printf("  Expecting: \\r\\n moves cursor to start of next line\n");
    wlcd("LINE1\r\nLINE2\r\nLINE3\r\nLINE4\r\n", 28);
    pause_observe();

    printf("\n[Test 2c] CR alone (carriage return, no newline)\n");
    printf("  Expecting: \\r returns to column 0 of current line\n");
    wlcd("XXXXXXXXXXXXXXXXYYYYYY\rSTART  ", 29);
    pause_observe();
}

/* ===== Test 3: form feed / clear ===== */

static void test_formfeed(void)
{
    printf("\n[Test 3a] Form feed (0x0C) to clear display\n");
    printf("  Expecting: display blanked then 'AFTER FF' appears\n");
    wlcd("\fAFTER FF        ", 17);
    pause_observe();

    printf("\n[Test 3b] NUL byte (0x00)\n");
    printf("  Expecting: NUL may clear display or be ignored\n");
    wlcd("\x00" "AFTER NUL      ", 16);
    pause_observe();
}

/* ===== Test 4: ANSI escape cursor positioning ===== */

static void test_ansi(void)
{
    /* ANSI ESC[H = cursor home (top-left) */
    /* ANSI ESC[r;cH = cursor to row r, col c (1-based) */

    printf("\n[Test 4a] ANSI ESC[H cursor home\n");
    printf("  Expecting: cursor moves to line 1 col 0, then ANSI-HOME appears\n");
    wlcd("\033[HANSI-HOME     ", 15);
    pause_observe();

    printf("\n[Test 4b] ANSI ESC[2;1H cursor to line 2\n");
    printf("  Expecting: cursor moves to line 2, then LINE2 appears there\n");
    wlcd("\033[2;1HANSI-LINE2     ", 19);
    pause_observe();

    printf("\n[Test 4c] ANSI ESC[4;1H cursor to line 4\n");
    printf("  Expecting: cursor moves to line 4, then LINE4 appears there\n");
    wlcd("\033[4;1HANSI-LINE4     ", 19);
    pause_observe();
}

/* ===== Test 5: \xfe prefix HD44780 command escape ===== */

/*
 * Many uClinux HD44780 drivers use a 0xFE prefix byte before command bytes.
 * Common commands (DDRAM addresses for 16-col display):
 *   0xFE 0x01 = clear display
 *   0xFE 0x80 = cursor home (line 1, col 0)
 *   0xFE 0xC0 = cursor to line 2, col 0
 *   0xFE 0x94 = cursor to line 3, col 0  (0x80 + 0x14)
 *   0xFE 0xD4 = cursor to line 4, col 0  (0xC0 + 0x14)
 */
static void test_fe_commands(void)
{
    printf("\n[Test 5a] \\xfe\\x01 clear display\n");
    printf("  Expecting: display blanked, then AFTER-FE-CLR appears\n");
    wlcd("\xfe\x01" "AFTER-FE-CLR    ", 18);
    pause_observe();

    printf("\n[Test 5b] \\xfe\\x80 cursor to line 1 col 0\n");
    printf("  Expecting: cursor to top-left, FE-LINE1 overwrites line 1\n");
    wlcd("\xfe\x80" "FE-LINE1        ", 18);
    pause_observe();

    printf("\n[Test 5c] \\xfe\\xC0 cursor to line 2\n");
    printf("  Expecting: FE-LINE2 appears on line 2\n");
    wlcd("\xfe\xc0" "FE-LINE2        ", 18);
    pause_observe();

    printf("\n[Test 5d] \\xfe\\x94 cursor to line 3 (0x80+0x14)\n");
    printf("  Expecting: FE-LINE3 appears on line 3\n");
    wlcd("\xfe\x94" "FE-LINE3        ", 18);
    pause_observe();

    printf("\n[Test 5e] \\xfe\\xD4 cursor to line 4 (0xC0+0x14)\n");
    printf("  Expecting: FE-LINE4 appears on line 4\n");
    wlcd("\xfe\xd4" "FE-LINE4        ", 18);
    pause_observe();
}

/* ===== Test 6: raw custom char code bytes ===== */

/*
 * HD44780 CGRAM slots 0-7 are accessed by writing byte values 0x00-0x07.
 * The sn110lcd daemon defines custom chars in CGRAM.
 * This test writes those code bytes to see what glyphs are defined.
 */
static void test_custom_chars(void)
{
    printf("\n[Test 6] Write CGRAM slot bytes 0x00-0x07 on line 1\n");
    printf("  Expecting: up to 8 custom glyphs defined by sn110lcd visible\n");
    printf("  (The flashing heart is likely one of these)\n");
    wlcd("\x00\x01\x02\x03\x04\x05\x06\x07        ", 16);
    pause_observe();
}

/* ===== Test 7: ioctl _IOW('l', 7, 48) — discovered from lxnetdmx ===== */

/*
 * lxnetdmx uses ioctl(fd, 0x40306C07, buf_48) to write 3 lines of display
 * content, bypassing the circular write buffer.  This test probes the
 * ioctl interface systematically to understand the 48-byte payload format.
 */

static void ilcd(int nr, const void *buf, int size)
{
    unsigned long req = LCD_IOC(nr, size);
    int ret = ioctl(g_fd, req, (void *)buf);
    printf("  ioctl(nr=%d, size=%d) -> %d\n", nr, size, ret);
}

static void test_ioctl(void)
{
    char buf[64];
    int i;

    /* --- Test 7a: exact lxnetdmx ioctl with 3 labeled lines --- */
    printf("\n[Test 7a] ioctl _IOW('l', 7, 48): 3 lines of 16 chars\n");
    printf("  Expecting: 3 lines appear (which 3 of the 4 display lines?)\n");
    memcpy(buf +  0, "IOCTL-LINE-1    ", 16);
    memcpy(buf + 16, "IOCTL-LINE-2    ", 16);
    memcpy(buf + 32, "IOCTL-LINE-3    ", 16);
    ilcd(7, buf, 48);
    pause_observe();

    /* --- Test 7b: 64-byte payload (all 4 lines) --- */
    printf("\n[Test 7b] ioctl _IOW('l', 7, 64): 4 lines of 16 chars\n");
    printf("  Expecting: all 4 lines, or error if size must be 48\n");
    memcpy(buf +  0, "IOC64-LINE-1    ", 16);
    memcpy(buf + 16, "IOC64-LINE-2    ", 16);
    memcpy(buf + 32, "IOC64-LINE-3    ", 16);
    memcpy(buf + 48, "IOC64-LINE-4    ", 16);
    ilcd(7, buf, 64);
    pause_observe();

    /* --- Test 7c: 16-byte payload (1 line) --- */
    printf("\n[Test 7c] ioctl _IOW('l', 7, 16): 1 line of 16 chars\n");
    printf("  Expecting: 1 line updated, or error if size must be 48\n");
    memcpy(buf, "IOC16-SINGLE-LN ", 16);
    ilcd(7, buf, 16);
    pause_observe();

    /* --- Test 7d: 32-byte payload (2 lines) --- */
    printf("\n[Test 7d] ioctl _IOW('l', 7, 32): 2 lines of 16 chars\n");
    printf("  Expecting: 2 lines updated, or error\n");
    memcpy(buf +  0, "IOC32-LINE-A    ", 16);
    memcpy(buf + 16, "IOC32-LINE-B    ", 16);
    ilcd(7, buf, 32);
    pause_observe();

    /* --- Test 7e: try different command numbers (nr 0-10) with 48 bytes --- */
    printf("\n[Test 7e] Probing command numbers 0-10 with 48-byte payload\n");
    printf("  Watching for non-error return values\n");
    memcpy(buf +  0, "NR-PROBE LINE 1 ", 16);
    memcpy(buf + 16, "NR-PROBE LINE 2 ", 16);
    memcpy(buf + 32, "NR-PROBE LINE 3 ", 16);
    for (i = 0; i <= 10; i++) {
        ilcd(i, buf, 48);
    }
    pause_observe();

    /* --- Test 7f: ioctl with unique chars to map line positions --- */
    printf("\n[Test 7f] ioctl _IOW('l', 7, 48): positional mapping\n");
    printf("  Each line has unique chars — note which display lines they appear on\n");
    memcpy(buf +  0, "AABBCCDDEEFFGGHH", 16);
    memcpy(buf + 16, "1122334455667788", 16);
    memcpy(buf + 32, "aabbccddeeffgghh", 16);
    ilcd(7, buf, 48);
    pause_observe();

    /* --- Test 7g: open-ioctl-close cycle (lxnetdmx pattern) --- */
    printf("\n[Test 7g] open-ioctl-close cycle (lxnetdmx pattern)\n");
    printf("  Close and reopen fd, then ioctl — tests if ioctl avoids counter\n");
    close(g_fd);
    g_fd = open(LCD_DEV, O_RDWR, 0);
    printf("  reopen: fd=%d\n", g_fd);
    if (g_fd >= 0) {
        memcpy(buf +  0, "REOPEN-CYCLE    ", 16);
        memcpy(buf + 16, "IOCTL-TEST-2    ", 16);
        memcpy(buf + 32, "NO-DRIFT-CHECK  ", 16);
        ilcd(7, buf, 48);
    }
    pause_observe();

    /* --- Test 7h: ioctl then write — check if ioctl affects counter --- */
    printf("\n[Test 7h] ioctl then write(64) — does ioctl move the counter?\n");
    printf("  If ioctl doesn't touch counter, write(64) should still align\n");
    memcpy(buf +  0, "IOC-BEFORE-WRITE", 16);
    memcpy(buf + 16, "LINE-2-IOC      ", 16);
    memcpy(buf + 32, "LINE-3-IOC      ", 16);
    ilcd(7, buf, 48);
    usleep(1000000);
    printf("  Now writing 64 bytes via write()...\n");
    wlcd("WRITE-AFTER-IOC "
         "WRITE-LINE-2    "
         "WRITE-LINE-3    "
         "WRITE-LINE-4    ", 64);
    printf("  Check: are write lines correctly positioned?\n");
    pause_observe();
}

/* ===== Test 8: read() ===== */

static void test_read(void)
{
    char buf[64];
    int n;

    printf("\n[Test 8] read() from /dev/lcd0\n");
    printf("  Expecting: -1 (write-only device) or 0 bytes\n");
    n = read(g_fd, buf, sizeof(buf));
    printf("  read() -> %d\n", n);
    if (n > 0) {
        int i;
        printf("  data: ");
        for (i = 0; i < n && i < 32; i++)
            printf("%02X ", (unsigned char)buf[i]);
        printf("\n");
    }
    pause_observe();
}

/* ===== Test 9: targeted partial writes ===== */

/*
 * The lxnetdmx heart blink only changes 4 characters (positions 13-16 on
 * line 1) each cycle.  How?  Two hypotheses:
 *
 *   A) Shadow buffer: write 64 bytes every time, but most bytes match what's
 *      already in the buffer.  The HD44780 re-draws only changed cells.
 *      Counter wraps to the same position (64 mod 64 = 0).
 *
 *   B) True partial write: write only 4 bytes.  Counter advances by 4.
 *      Then write 60 bytes of padding to wrap counter back (total 64).
 *      Or: lxnetdmx writes exactly 4 bytes and relies on the counter
 *      being at position 12 each cycle via some external alignment.
 *
 * We test both approaches plus a counter-position diagnostic.
 */

static void test_partial(void)
{
    char base[64];
    char overlay[64];
    int cycle;

    /* ---- Setup: establish a known full screen ---- */
    printf("\n[Test 9a] Shadow buffer — setup known screen\n");
    printf("  Writing 64 bytes of known content (all dots + labels)\n");

    /*
     * Line 1: "...............#"   (# = target at position 12-15)
     * Line 2: "PORT 0   PORT 1"
     * Line 3: "TX LIVE  RX IDLE"
     * Line 4: "                "
     *
     * Positions are 0-indexed within each 16-char line.
     */
    memcpy(base +  0, "............####", 16);
    memcpy(base + 16, "PORT 0   PORT 1 ", 16);
    memcpy(base + 32, "TX LIVE  RX IDLE", 16);
    memcpy(base + 48, "                ", 16);

    wlcd(base, 64);
    pause_observe();

    /* ---- Test 9b: shadow buffer blink (change 4 chars in 64-byte write) ---- */
    printf("\n[Test 9b] Shadow buffer — blink 4 chars via full 64-byte write\n");
    printf("  Writing same 64 bytes but toggling positions 12-15 on line 1\n");
    printf("  Watch: only those 4 chars should visually change\n");

    memcpy(overlay, base, 64);

    for (cycle = 0; cycle < 6; cycle++) {
        if (cycle % 2 == 0) {
            /* "blink on" — change #### to XOXO */
            overlay[12] = 'X';
            overlay[13] = 'O';
            overlay[14] = 'X';
            overlay[15] = 'O';
        } else {
            /* "blink off" — restore to #### */
            overlay[12] = '#';
            overlay[13] = '#';
            overlay[14] = '#';
            overlay[15] = '#';
        }
        printf("  cycle %d: pos 12-15 = '%c%c%c%c'\n",
               cycle, overlay[12], overlay[13], overlay[14], overlay[15]);
        wlcd(overlay, 64);
        usleep(800000);  /* 800ms per blink — visible but not tedious */
    }
    pause_observe();

    /* ---- Test 9c: true partial write — only 4 bytes ---- */
    printf("\n[Test 9c] True partial write — only 4 bytes\n");
    printf("  First: establish base screen again\n");
    wlcd(base, 64);
    usleep(1000000);

    printf("  Now writing ONLY 4 bytes. Counter is at 0 after the 64-byte write.\n");
    printf("  These 4 bytes should land at positions 0-3 of line 1.\n");
    wlcd("ABCD", 4);
    pause_observe();

    printf("  Counter is now at 4. Writing 60 bytes of spaces to wrap to 0.\n");
    {
        char pad[60];
        memset(pad, ' ', 60);
        wlcd(pad, 60);
    }
    pause_observe();

    printf("  Counter should be back at 0. Writing 4 bytes again.\n");
    printf("  If alignment works, EFGH appears at positions 0-3 of line 1.\n");
    wlcd("EFGH", 4);
    pause_observe();

    /* ---- Test 9d: partial write at arbitrary offset ---- */
    printf("\n[Test 9d] Partial write at offset 12 (heart position)\n");
    printf("  Strategy: write 12 spaces to advance counter, then 4 target bytes,\n");
    printf("  then 48 spaces to wrap counter back to 0. Total = 64 bytes.\n");
    printf("  First: re-establish base screen\n");
    wlcd(base, 64);
    usleep(1000000);

    printf("  Now: 12 spaces + 'HART' + 48 spaces = 64 bytes\n");
    {
        char partial[64];
        memset(partial, ' ', 64);
        partial[12] = 'H';
        partial[13] = 'A';
        partial[14] = 'R';
        partial[15] = 'T';
        wlcd(partial, 64);
    }
    printf("  Check: positions 12-15 should show HART.\n");
    printf("  But spaces overwrite everything else! This is the shadow buffer problem.\n");
    pause_observe();

    /* ---- Test 9e: shadow buffer with targeted change at offset 12 ---- */
    printf("\n[Test 9e] Shadow buffer with targeted change at offset 12\n");
    printf("  Re-establish base, then write full 64 with only pos 12-15 changed.\n");
    printf("  This preserves surrounding content.\n");
    wlcd(base, 64);
    usleep(1000000);

    memcpy(overlay, base, 64);
    overlay[12] = 'H';
    overlay[13] = 'A';
    overlay[14] = 'R';
    overlay[15] = 'T';
    wlcd(overlay, 64);
    printf("  Check: only positions 12-15 changed, rest of screen intact.\n");
    pause_observe();

    /* ---- Test 9f: rapid shadow-buffer blink (simulating heart) ---- */
    printf("\n[Test 9f] Rapid shadow-buffer blink — 10 cycles at ~500ms\n");
    printf("  Simulating lxnetdmx heart blink using shadow buffer approach.\n");
    printf("  Toggling position 12 between '*' and ' ' (heart on/off).\n");

    wlcd(base, 64);
    usleep(500000);

    for (cycle = 0; cycle < 10; cycle++) {
        memcpy(overlay, base, 64);
        if (cycle % 2 == 0) {
            overlay[12] = '*';
            overlay[13] = ' ';
            overlay[14] = ' ';
            overlay[15] = ' ';
        }
        /* else: overlay already matches base (#### pattern) */
        wlcd(overlay, 64);
        usleep(500000);
    }

    printf("  Done. Counter should still be at 0 (10 * 64 = 640, mod 64 = 0)\n");
    pause_observe();

    /* ---- Test 9g: counter position diagnostic ---- */
    printf("\n[Test 9g] Counter position diagnostic\n");
    printf("  Write varying small sizes to check where bytes land.\n");
    printf("  After each write, check which LCD positions changed.\n");

    /* Reset with known screen */
    wlcd(base, 64);
    usleep(1000000);

    printf("  Writing 1 byte 'Z' — should appear at position 0 of line 1\n");
    wlcd("Z", 1);
    pause_observe();

    printf("  Writing 1 byte 'Y' — should appear at position 1 of line 1\n");
    wlcd("Y", 1);
    pause_observe();

    printf("  Counter is now at 2. Writing 62 spaces to wrap to 0.\n");
    {
        char pad[62];
        memset(pad, ' ', 62);
        wlcd(pad, 62);
    }
    printf("  Check: only positions 0-1 should have changed (Z, Y), rest intact.\n");
    printf("  Actually: 62 spaces overwrote positions 2-63 with spaces!\n");
    printf("  This confirms: ALL written bytes update the display, not just changed ones.\n");
    pause_observe();
}

/* ===== Test 10: NUL / special-byte transparency probe ===== */

/*
 * lxnetdmx overwrites only 4 characters on the display without knowing
 * what the rest of the screen contains.  It can't read the buffer back.
 * Hypothesis: certain byte values (NUL, 0xFF, etc.) might be treated as
 * "transparent" by the driver — written to the circular buffer but NOT
 * forwarded to the HD44780, leaving the existing display content intact.
 *
 * We test each candidate by:
 *   1. Writing a known 64-byte screen (all visible chars)
 *   2. Writing 64 bytes where most positions are the candidate byte
 *      and 4 target positions have visible replacement chars
 *   3. Observing: did the candidate bytes erase the existing content
 *      or leave it untouched?
 */

static void test_nullbyte(void)
{
    char screen[64];
    char probe[64];

    /* Known base screen — all printable, easy to see what changes */
    memcpy(screen +  0, "AAAAAAAAAAAAAAAA", 16);
    memcpy(screen + 16, "BBBBBBBBBBBBBBBB", 16);
    memcpy(screen + 32, "CCCCCCCCCCCCCCCC", 16);
    memcpy(screen + 48, "DDDDDDDDDDDDDDDD", 16);

    /* ---- Test 10a: NUL (0x00) as transparent byte ---- */
    printf("\n[Test 10a] NUL (0x00) transparency test\n");
    printf("  Step 1: establish screen of A/B/C/D\n");
    wlcd(screen, 64);
    pause_observe();

    printf("  Step 2: write 64 bytes — NUL everywhere except pos 12-15 = 'HART'\n");
    printf("  If NUL is transparent: screen shows AAAAAAAAAAAA HART / BBB... / CCC... / DDD...\n");
    printf("  If NUL overwrites: screen shows '            HART' + blanks\n");
    memset(probe, 0x00, 64);
    probe[12] = 'H';
    probe[13] = 'A';
    probe[14] = 'R';
    probe[15] = 'T';
    wlcd(probe, 64);
    pause_observe();

    /* ---- Test 10b: 0xFF as transparent byte ---- */
    printf("\n[Test 10b] 0xFF transparency test\n");
    printf("  Re-establish base screen\n");
    wlcd(screen, 64);
    usleep(1000000);

    printf("  Write 64 bytes — 0xFF everywhere except pos 12-15 = 'XOXO'\n");
    memset(probe, 0xFF, 64);
    probe[12] = 'X';
    probe[13] = 'O';
    probe[14] = 'X';
    probe[15] = 'O';
    wlcd(probe, 64);
    pause_observe();

    /* ---- Test 10c: 0x20 (space) control — this should overwrite ---- */
    printf("\n[Test 10c] Space (0x20) control test — expected to overwrite\n");
    printf("  Re-establish base screen\n");
    wlcd(screen, 64);
    usleep(1000000);

    printf("  Write 64 bytes — spaces everywhere except pos 12-15 = 'TEST'\n");
    memset(probe, ' ', 64);
    probe[12] = 'T';
    probe[13] = 'E';
    probe[14] = 'S';
    probe[15] = 'T';
    wlcd(probe, 64);
    printf("  This SHOULD wipe surrounding content (spaces are visible chars).\n");
    pause_observe();

    /* ---- Test 10d: 0x08 (backspace) ---- */
    printf("\n[Test 10d] 0x08 (backspace) transparency test\n");
    printf("  Re-establish base screen\n");
    wlcd(screen, 64);
    usleep(1000000);

    printf("  Write 64 bytes — 0x08 everywhere except pos 12-15 = '!!!!'\n");
    printf("  0x08 might move cursor back without writing, acting as no-op\n");
    memset(probe, 0x08, 64);
    probe[12] = '!';
    probe[13] = '!';
    probe[14] = '!';
    probe[15] = '!';
    wlcd(probe, 64);
    pause_observe();

    /* ---- Test 10e: NUL via ioctl (lxnetdmx path) ---- */
    printf("\n[Test 10e] NUL via ioctl — same as 10a but through ioctl path\n");
    printf("  Re-establish base screen\n");
    wlcd(screen, 64);
    usleep(1000000);

    printf("  ioctl 48 bytes: NUL fill except pos 12-15 = 'HART'\n");
    memset(probe, 0x00, 48);
    probe[12] = 'H';
    probe[13] = 'A';
    probe[14] = 'R';
    probe[15] = 'T';
    ilcd(7, probe, 48);
    pause_observe();

    /* ---- Test 10f: NUL-only write (no visible chars) ---- */
    printf("\n[Test 10f] Pure NUL write — 64 bytes of 0x00\n");
    printf("  Re-establish base screen\n");
    wlcd(screen, 64);
    usleep(1000000);

    printf("  Write 64 NUL bytes. If NUL is transparent, screen unchanged.\n");
    printf("  If NUL overwrites, screen blanked (CGRAM slot 0 glyph or blank).\n");
    memset(probe, 0x00, 64);
    wlcd(probe, 64);
    pause_observe();

    /* ---- Test 10g: sweep of candidate bytes ---- */
    printf("\n[Test 10g] Byte sweep — test several candidate values\n");
    printf("  For each: fill 64 bytes with candidate, put 'XX' at pos 0-1\n");
    printf("  Watch line 1 carefully — does the fill byte appear as a visible char?\n");
    {
        /* Candidates that might be transparent or special */
        static const unsigned char candidates[] = {
            0x00,  /* NUL */
            0x08,  /* BS */
            0x0A,  /* LF */
            0x0D,  /* CR */
            0x7F,  /* DEL */
            0xFE,  /* command prefix */
            0xFF,  /* all-ones */
        };
        int c;
        for (c = 0; c < 7; c++) {
            wlcd(screen, 64);
            usleep(800000);
            printf("  candidate 0x%02X: ", candidates[c]);
            memset(probe, candidates[c], 64);
            probe[0] = 'X';
            probe[1] = 'X';
            wlcd(probe, 64);
            printf("watch — did fill byte leave existing A's intact or overwrite?\n");
            pause_observe();
        }
    }
}

/* ===== Test 11: close/open counter reset and ioctl NUL behavior ===== */

/*
 * Binary analysis of lxnetdmx reveals it opens, ioctls, and closes /dev/lcd0
 * on EVERY blink cycle.  Two critical questions:
 *
 *   1) Does close() + open() reset the driver's internal counter?
 *      If yes: the heart always lands at the same position because the
 *      counter is re-initialized each cycle.
 *
 *   2) Does the ioctl pass NUL bytes through to the display, or stop
 *      at the first NUL (like write() does)?
 *      The lxnetdmx buffer has NUL at byte 3 and byte 8.
 *      If ioctl stops at NUL: only 3 bytes per cycle.
 *      If ioctl passes NUL through: all 48 bytes, NUL = CGRAM char 0.
 */

static void test_counter_reset(void)
{
    char base[64];
    char marker[4];

    memcpy(base +  0, "AAAAAAAAAAAAAAAA", 16);
    memcpy(base + 16, "BBBBBBBBBBBBBBBB", 16);
    memcpy(base + 32, "CCCCCCCCCCCCCCCC", 16);
    memcpy(base + 48, "DDDDDDDDDDDDDDDD", 16);

    /* ---- Test 11a: close+open counter reset ---- */
    printf("\n[Test 11a] Close + open counter reset test\n");
    printf("  Step 1: write 64 bytes to establish base and align counter\n");
    wlcd(base, 64);
    pause_observe();

    printf("  Step 2: write 4-byte marker '1111' (counter advances by 4)\n");
    wlcd("1111", 4);
    printf("  Note WHERE '1111' appears on the display.\n");
    pause_observe();

    printf("  Step 3: close /dev/lcd0\n");
    close(g_fd);
    printf("  closed.\n");
    usleep(500000);

    printf("  Step 4: re-open /dev/lcd0\n");
    g_fd = open(LCD_DEV, O_RDWR, 0);
    printf("  open: fd=%d\n", g_fd);
    if (g_fd < 0) {
        printf("  ERROR: cannot reopen\n");
        return;
    }
    usleep(500000);

    printf("  Step 5: write 4-byte marker '2222'\n");
    printf("  If counter RESET:  '2222' appears at SAME position as '1111'\n");
    printf("  If counter PERSISTS: '2222' appears 4 positions AFTER '1111'\n");
    wlcd("2222", 4);
    pause_observe();

    /* ---- Test 11b: repeated close+open+write cycle ---- */
    printf("\n[Test 11b] Repeated close+open+write cycles (lxnetdmx pattern)\n");
    printf("  Write base, then 5 cycles of: close, open, write 4 bytes\n");
    printf("  Watch: does the marker stay in one place or drift?\n");

    /* Re-establish base */
    wlcd(base, 64);
    usleep(1000000);
    {
        int cycle;
        for (cycle = 0; cycle < 5; cycle++) {
            close(g_fd);
            usleep(200000);
            g_fd = open(LCD_DEV, O_RDWR, 0);
            if (g_fd < 0) {
                printf("  ERROR: cycle %d reopen failed\n", cycle);
                return;
            }
            marker[0] = '0' + cycle;
            marker[1] = '0' + cycle;
            marker[2] = '0' + cycle;
            marker[3] = '0' + cycle;
            printf("  cycle %d: close+open+write '%c%c%c%c'\n",
                   cycle, marker[0], marker[1], marker[2], marker[3]);
            wlcd(marker, 4);
            usleep(800000);
        }
    }
    printf("  If counter resets: all markers appeared at the same position\n");
    printf("  If counter drifts: markers appeared at 5 different positions\n");
    pause_observe();

    /* ---- Test 11c: ioctl NUL pass-through test ---- */
    printf("\n[Test 11c] ioctl NUL pass-through vs stop\n");
    printf("  Establish base via write(64), then ioctl 48 bytes:\n");
    printf("  [0]='P' [1]='Q' [2]='R' [3]=NUL [4]='S' [5]='T' [6]='U' [7]='V' [8]=NUL ...\n");
    printf("  If ioctl STOPS at NUL: only PQR appears (3 chars)\n");
    printf("  If ioctl PASSES NUL: PQR + CGRAM_char_0 + STUV + CGRAM_char_0 + ... appears\n");

    /* Re-establish base */
    wlcd(base, 64);
    usleep(1000000);
    {
        char ibuf[48];
        memset(ibuf, 'Z', 48);  /* fill with visible char */
        ibuf[0] = 'P';
        ibuf[1] = 'Q';
        ibuf[2] = 'R';
        ibuf[3] = 0x00;   /* NUL - stop marker? */
        ibuf[4] = 'S';
        ibuf[5] = 'T';
        ibuf[6] = 'U';
        ibuf[7] = 'V';
        ibuf[8] = 0x00;   /* NUL - second stop marker? */
        /* bytes 9-47 = 'Z' (visible) */
        ilcd(7, ibuf, 48);
    }
    printf("  Check the display carefully.\n");
    printf("  If you see only PQR (3 chars changed): ioctl stops at NUL.\n");
    printf("  If you see PQR + glyph + STUV + glyph + ZZZ...: ioctl passes all 48 bytes.\n");
    pause_observe();

    /* ---- Test 11d: ioctl with NUL at byte 0 (compare with test 10e) ---- */
    printf("\n[Test 11d] ioctl with NUL at byte 0 — confirm 10e result\n");
    printf("  Re-establish base\n");
    wlcd(base, 64);
    usleep(1000000);

    printf("  ioctl 48 bytes: NUL at byte 0, 'HART' at bytes 1-4, rest 'Z'\n");
    {
        char ibuf[48];
        memset(ibuf, 'Z', 48);
        ibuf[0] = 0x00;
        ibuf[1] = 'H';
        ibuf[2] = 'A';
        ibuf[3] = 'R';
        ibuf[4] = 'T';
        ilcd(7, ibuf, 48);
    }
    printf("  If ioctl stops at NUL: no change (NUL at byte 0 = nothing written)\n");
    printf("  If ioctl passes NUL: glyph + HART + ZZZ... appears (48 chars change)\n");
    pause_observe();

    /* ---- Test 11e: simulated lxnetdmx pattern ---- */
    printf("\n[Test 11e] Simulated lxnetdmx: close+open+ioctl with NUL at byte 3\n");
    printf("  Matches the binary: buf[0-1]=??, buf[2]=0x0C, buf[3]=NUL,\n");
    printf("  buf[4-7]=spaces, buf[8]=NUL, rest=NUL\n");
    printf("  First establish base via write.\n");

    wlcd(base, 64);
    usleep(1000000);

    printf("  Now: close + open + ioctl (lxnetdmx pattern)\n");
    close(g_fd);
    usleep(200000);
    g_fd = open(LCD_DEV, O_RDWR, 0);
    if (g_fd < 0) {
        printf("  ERROR: reopen failed\n");
        return;
    }
    {
        char ibuf[48];
        memset(ibuf, 0x00, 48);  /* all NUL */
        ibuf[0] = 'H';    /* stand-in for unknown buf[0] */
        ibuf[1] = 'R';    /* stand-in for unknown buf[1] */
        ibuf[2] = 0x0C;   /* exact value from binary: CGRAM char? */
        /* ibuf[3] = 0x00 — NUL (already set by memset) */
        ibuf[4] = ' ';
        ibuf[5] = ' ';
        ibuf[6] = ' ';
        ibuf[7] = ' ';
        /* ibuf[8] = 0x00 — NUL (already set by memset) */
        ilcd(7, ibuf, 48);
    }
    printf("  Watch: what changed on the display?\n");
    pause_observe();

    /* ---- Test 11f: 5 cycles of lxnetdmx pattern ---- */
    printf("\n[Test 11f] 5 cycles of close+open+ioctl pattern\n");
    printf("  Watch: does the changed area stay fixed or drift?\n");

    /* Re-establish base */
    wlcd(base, 64);
    usleep(1000000);
    {
        int cycle;
        char ibuf[48];

        for (cycle = 0; cycle < 5; cycle++) {
            close(g_fd);
            usleep(400000);
            g_fd = open(LCD_DEV, O_RDWR, 0);
            if (g_fd < 0) {
                printf("  ERROR: cycle %d reopen failed\n", cycle);
                return;
            }
            memset(ibuf, 0x00, 48);
            ibuf[0] = 'H';
            ibuf[1] = 'R';
            ibuf[2] = (cycle % 2 == 0) ? 0x0C : ' ';  /* toggle */
            ibuf[4] = ' ';
            ibuf[5] = ' ';
            ibuf[6] = ' ';
            ibuf[7] = ' ';
            printf("  cycle %d: close+open+ioctl (buf[2]=%s)\n",
                   cycle, (cycle % 2 == 0) ? "0x0C" : "space");
            ilcd(7, ibuf, 48);
            usleep(600000);
        }
    }
    printf("  If counter resets on open: changes always at the same position\n");
    printf("  If counter drifts: changes walk across the display\n");
    pause_observe();
}

/* ===== Main ===== */

static void usage(void)
{
    printf("lcd_probe — /dev/lcd0 interface discovery for SN110\n\n");
    printf("Usage:\n");
    printf("  lcd_probe all        Run all tests in sequence\n");
    printf("  lcd_probe text       Plain ASCII writes\n");
    printf("  lcd_probe newline    Newline / CR as line separator\n");
    printf("  lcd_probe formfeed   Form feed as clear-screen\n");
    printf("  lcd_probe ansi       ANSI ESC cursor positioning\n");
    printf("  lcd_probe fe         \\xfe-prefix HD44780 commands\n");
    printf("  lcd_probe chars      Raw custom char code bytes 0x00-0x07\n");
    printf("  lcd_probe ioctl      ioctl _IOW('l',7,48) from lxnetdmx\n");
    printf("  lcd_probe read       Try read() from the device\n");
    printf("  lcd_probe partial    Targeted partial-write experiments\n");
    printf("  lcd_probe nulltest   NUL/special byte transparency probe\n");
    printf("  lcd_probe counter    Close/open counter reset + ioctl NUL behavior\n");
    printf("\nTip: run 'lcd_probe all' first, then re-run specific tests.\n");
    printf("Watch the LCD screen between each step (2 second pauses).\n");
}

int main(int argc, char *argv[])
{
    const char *cmd;

    if (argc < 2) {
        usage();
        return 1;
    }
    cmd = argv[1];

    printf("Opening %s...\n", LCD_DEV);
    g_fd = open(LCD_DEV, O_RDWR, 0);
    if (g_fd < 0) {
        printf("ERROR: cannot open %s\n", LCD_DEV);
        return 1;
    }
    printf("OK: fd=%d\n", g_fd);

    if (strcmp(cmd, "all") == 0) {
        test_text();
        test_newline();
        test_formfeed();
        test_ansi();
        test_fe_commands();
        test_custom_chars();
        test_ioctl();
        test_read();
        test_partial();
        test_nullbyte();
        test_counter_reset();
    } else if (strcmp(cmd, "text") == 0) {
        test_text();
    } else if (strcmp(cmd, "newline") == 0) {
        test_newline();
    } else if (strcmp(cmd, "formfeed") == 0) {
        test_formfeed();
    } else if (strcmp(cmd, "ansi") == 0) {
        test_ansi();
    } else if (strcmp(cmd, "fe") == 0) {
        test_fe_commands();
    } else if (strcmp(cmd, "chars") == 0) {
        test_custom_chars();
    } else if (strcmp(cmd, "ioctl") == 0) {
        test_ioctl();
    } else if (strcmp(cmd, "read") == 0) {
        test_read();
    } else if (strcmp(cmd, "partial") == 0) {
        test_partial();
    } else if (strcmp(cmd, "nulltest") == 0) {
        test_nullbyte();
    } else if (strcmp(cmd, "counter") == 0) {
        test_counter_reset();
    } else {
        printf("Unknown command: %s\n\n", cmd);
        usage();
        close(g_fd);
        return 1;
    }

    printf("\nDone.\n");
    close(g_fd);
    return 0;
}
