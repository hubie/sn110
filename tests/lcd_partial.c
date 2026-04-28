/*
 * lcd_partial — Prove that ioctl cmd 7 does cursor-positioned partial writes
 *
 * Test 1 (proof): Write a known full screen, then use ioctl cmd 7 to
 *   overwrite only positions 12-15 on line 1. Verify the rest is intact.
 *
 * Test 2 (blink): Replicate the factory heartbeat — blink a character
 *   at position 12 on line 1 without disturbing other content.
 *
 * Usage:
 *   lcd_partial proof     Test 1: partial write proof
 *   lcd_partial blink     Test 2: heartbeat blink replica
 *   lcd_partial modes     Test 3: probe OR/XOR/AND write modes
 *   lcd_partial cursors   Test 4: verify cursor positions across all 4 lines
 *
 * Run on device via telnet. Watch the LCD between steps.
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
 * Ioctl encoding: _IOW('l', nr, size)
 *   dir=1 (write) in bits 30-31, size in bits 16-29,
 *   type='l' in bits 8-15, nr in bits 0-7
 */
#define LCD_IOC_W(nr, size) \
    ((1U << 30) | ((unsigned)(size) << 16) | ('l' << 8) | (nr))

#define LCD_CMD7   LCD_IOC_W(7, 48)   /* cursor + write (48 bytes) */
#define LCD_CMD1   LCD_IOC_W(1, 4)    /* set write mode (4 bytes)  */
#define LCD_CLEAR  0x6C02             /* clear screen (no arg)     */

static int g_fd = -1;

static void lcd_open(void)
{
    g_fd = open(LCD_DEV, O_RDWR, 0);
    if (g_fd < 0) {
        printf("ERROR: cannot open %s\n", LCD_DEV);
        _exit(1);
    }
}

static void lcd_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

/* Write raw bytes to /dev/lcd0 via write() */
static void lcd_write(const void *buf, int len)
{
    write(g_fd, buf, len);
}

/* Send ioctl command 7: set cursor + write NUL-terminated string */
static void lcd_cmd7(int col, int row, const char *text)
{
    char buf[48];
    int len;

    memset(buf, 0, 48);
    buf[2] = (char)col;
    buf[3] = (char)row;

    /* Copy text into buf[4..], leave room for NUL */
    len = 0;
    while (text[len] && len < 43)
        len++;
    memcpy(&buf[4], text, len);
    /* buf[4+len] is already 0 from memset — NUL terminator */

    ioctl(g_fd, LCD_CMD7, buf);
}

/* Set write mode: 0=OR, 1=XOR, 3=AND */
static void lcd_set_mode(int mode)
{
    char buf[4];
    memset(buf, 0, 4);
    buf[0] = (char)mode;
    ioctl(g_fd, LCD_CMD1, buf);
}

static void pause_ms(int ms)
{
    usleep(ms * 1000);
}

/* ===== Test 1: Partial write proof ===== */

static void test_proof(void)
{
    printf("\n=== Test: Partial Write Proof ===\n");
    printf("This test proves ioctl cmd 7 updates only the targeted\n");
    printf("positions, leaving the rest of the screen untouched.\n\n");

    /* Step 1: write a known full screen via write() */
    printf("[Step 1] Write known screen via write(64):\n");
    printf("  Line 1: 0123456789AB####\n");
    printf("  Line 2: BBBBBBBBBBBBBBBB\n");
    printf("  Line 3: CCCCCCCCCCCCCCCC\n");
    printf("  Line 4: DDDDDDDDDDDDDDDD\n");
    lcd_open();
    lcd_write(
        "0123456789AB####"
        "BBBBBBBBBBBBBBBB"
        "CCCCCCCCCCCCCCCC"
        "DDDDDDDDDDDDDDDD",
        64);
    lcd_close();
    printf("  Written. Observe the LCD.\n");
    pause_ms(3000);

    /* Step 2: use ioctl cmd 7 to write only at position 12 */
    printf("\n[Step 2] ioctl cmd 7: col=12, row=0, text=\"WXYZ\"\n");
    printf("  Expected: only positions 12-15 on line 1 change to WXYZ\n");
    printf("  Everything else (0-11 on line 1, lines 2-4) stays intact.\n");
    lcd_open();
    lcd_cmd7(12, 0, "WXYZ");
    lcd_close();
    printf("  Done. CHECK THE LCD:\n");
    printf("  Line 1 should read: 0123456789ABWXYZ\n");
    printf("  Lines 2-4 should still be BBBB.../CCCC.../DDDD...\n");
    pause_ms(3000);

    /* Step 3: write at a different position to double-check */
    printf("\n[Step 3] ioctl cmd 7: col=4, row=0, text=\"HI\"\n");
    printf("  Expected: positions 4-5 on line 1 change to HI\n");
    printf("  Positions 0-3, 6-11, 12-15 unchanged.\n");
    lcd_open();
    lcd_cmd7(4, 0, "HI");
    lcd_close();
    printf("  Done. CHECK THE LCD:\n");
    printf("  Line 1 should read: 0123HI6789ABWXYZ\n");
    pause_ms(3000);

    /* Step 4: write on a different line */
    printf("\n[Step 4] ioctl cmd 7: col=0, row=2, text=\"LINE3-CHANGED\"\n");
    printf("  Expected: line 3 starts with LINE3-CHANGED, rest of line 3 intact\n");
    printf("  Lines 1, 2, 4 completely untouched.\n");
    lcd_open();
    lcd_cmd7(0, 2, "LINE3-CHANGED");
    lcd_close();
    printf("  Done. CHECK THE LCD:\n");
    printf("  Line 1: 0123HI6789ABWXYZ  (unchanged)\n");
    printf("  Line 2: BBBBBBBBBBBBBBBB  (unchanged)\n");
    printf("  Line 3: LINE3-CHANGEDCCC  (first 13 changed, last 3 intact)\n");
    printf("  Line 4: DDDDDDDDDDDDDDDD  (unchanged)\n");
    pause_ms(3000);

    /* Step 5: single character write */
    printf("\n[Step 5] ioctl cmd 7: col=15, row=3, text=\"!\"\n");
    printf("  Expected: only the very last character on line 4 changes to !\n");
    lcd_open();
    lcd_cmd7(15, 3, "!");
    lcd_close();
    printf("  Done. Line 4 should read: DDDDDDDDDDDDDDD!\n");
    pause_ms(3000);

    printf("\n=== Proof complete ===\n");
    printf("If each step changed ONLY the targeted positions,\n");
    printf("then cursor+NUL partial writes are confirmed.\n");
}

/* ===== Test 2: Heartbeat blink replica ===== */

static void test_blink(void)
{
    int cycle;

    printf("\n=== Test: Heartbeat Blink Replica ===\n");
    printf("Replicates the factory heartbeat pattern.\n");
    printf("Blinks a character at position 12, line 1.\n");
    printf("The rest of the screen should stay completely still.\n\n");

    /* Set up a screen with content on all 4 lines */
    printf("[Setup] Writing base screen...\n");
    lcd_open();
    lcd_write(
        "sn110dmx v0.2   "
        "P0:TX  sACN u1  "
        "P1:RX  sACN u2  "
        "192.168.2.231   ",
        64);
    lcd_close();
    printf("  Base screen written. Starting blink in 2 seconds...\n");
    pause_ms(2000);

    /* Blink loop: open, ioctl, close each cycle (like lxnetdmx) */
    printf("[Blink] 20 cycles, 500ms each\n");
    printf("  Watch position 12 on line 1 — it should toggle\n");
    printf("  between '*' and ' ' while everything else is frozen.\n");

    for (cycle = 0; cycle < 20; cycle++) {
        lcd_open();
        if (cycle % 2 == 0)
            lcd_cmd7(12, 0, "*   ");  /* heart on + 3 spaces */
        else
            lcd_cmd7(12, 0, "    ");  /* 4 spaces (heart off) */
        lcd_close();
        pause_ms(500);
    }

    printf("  Blink complete.\n");
    printf("  If the rest of the display never flickered or changed,\n");
    printf("  the partial write mechanism is confirmed.\n");
}

/* ===== Test 3: Write modes (OR / XOR / AND) ===== */

static void test_modes(void)
{
    printf("\n=== Test: Write Mode Probe ===\n");
    printf("Tests the OR/XOR/AND modes to see if they affect ioctl cmd 7.\n\n");

    /* Establish known screen */
    printf("[Setup] Writing base screen of all '#' (0x23)...\n");
    lcd_open();
    lcd_write(
        "################"
        "################"
        "################"
        "################",
        64);
    lcd_close();
    pause_ms(2000);

    /* Test OR mode (0) */
    printf("[Mode 0 = OR] Setting mode, then writing 'AA' at col 0, row 0\n");
    printf("  In OR mode, the driver may OR new bytes with existing buffer.\n");
    printf("  '#'=0x23, 'A'=0x41. 0x23|0x41 = 0x63 = 'c'\n");
    printf("  If OR mode works: position 0-1 show 'cc' (not 'AA')\n");
    printf("  If OR mode doesn't affect cmd 7: shows 'AA'\n");
    lcd_open();
    lcd_set_mode(0);
    lcd_cmd7(0, 0, "AA");
    lcd_close();
    printf("  Check positions 0-1 on line 1.\n");
    pause_ms(3000);

    /* Test XOR mode (1) */
    printf("\n[Mode 1 = XOR] Re-establish base, set mode, write 'AA'\n");
    lcd_open();
    lcd_write(
        "################"
        "################"
        "################"
        "################",
        64);
    lcd_close();
    pause_ms(1000);
    printf("  '#'=0x23, 'A'=0x41. 0x23^0x41 = 0x62 = 'b'\n");
    printf("  If XOR works: 'bb'. If not: 'AA'\n");
    lcd_open();
    lcd_set_mode(1);
    lcd_cmd7(0, 0, "AA");
    lcd_close();
    printf("  Check positions 0-1 on line 1.\n");
    pause_ms(3000);

    /* Test AND mode (3) */
    printf("\n[Mode 3 = AND] Re-establish base, set mode, write 'AA'\n");
    lcd_open();
    lcd_write(
        "################"
        "################"
        "################"
        "################",
        64);
    lcd_close();
    pause_ms(1000);
    printf("  '#'=0x23, 'A'=0x41. 0x23&0x41 = 0x01 = CGRAM char 1\n");
    printf("  If AND works: CGRAM glyph. If not: 'AA'\n");
    lcd_open();
    lcd_set_mode(3);
    lcd_cmd7(0, 0, "AA");
    lcd_close();
    printf("  Check positions 0-1 on line 1.\n");
    pause_ms(3000);

    printf("\n=== Mode test complete ===\n");
    printf("Note the results — they tell us whether the modes affect\n");
    printf("ioctl cmd 7, or only the write() path.\n");
}

/* ===== Test 4: Cursor positions on all 4 lines ===== */

static void test_cursors(void)
{
    printf("\n=== Test: Cursor Positions Across All Lines ===\n");
    printf("Verifies that row 0-3 maps to lines 1-4.\n\n");

    /* Clear screen to spaces */
    printf("[Setup] Writing blank screen...\n");
    lcd_open();
    lcd_write(
        "                "
        "                "
        "                "
        "                ",
        64);
    lcd_close();
    pause_ms(1000);

    /* Write a marker on each line via ioctl cmd 7 */
    printf("[Row 0] Writing 'ROW0' at col=0, row=0\n");
    lcd_open();
    lcd_cmd7(0, 0, "ROW0");
    lcd_close();
    pause_ms(1500);

    printf("[Row 1] Writing 'ROW1' at col=0, row=1\n");
    lcd_open();
    lcd_cmd7(0, 1, "ROW1");
    lcd_close();
    pause_ms(1500);

    printf("[Row 2] Writing 'ROW2' at col=0, row=2\n");
    lcd_open();
    lcd_cmd7(0, 2, "ROW2");
    lcd_close();
    pause_ms(1500);

    printf("[Row 3] Writing 'ROW3' at col=0, row=3\n");
    lcd_open();
    lcd_cmd7(0, 3, "ROW3");
    lcd_close();
    pause_ms(1500);

    printf("\n  Expected:\n");
    printf("  Line 1: ROW0\n");
    printf("  Line 2: ROW1\n");
    printf("  Line 3: ROW2\n");
    printf("  Line 4: ROW3\n");
    printf("\n  If a ROW marker appears on the wrong line,\n");
    printf("  the row mapping is different from expected.\n");
    pause_ms(2000);

    /* Now add column markers */
    printf("\n[Columns] Writing at col=8 on each row\n");
    lcd_open();
    lcd_cmd7(8, 0, "col8");
    lcd_close();
    lcd_open();
    lcd_cmd7(8, 1, "col8");
    lcd_close();
    lcd_open();
    lcd_cmd7(8, 2, "col8");
    lcd_close();
    lcd_open();
    lcd_cmd7(8, 3, "col8");
    lcd_close();

    printf("  Expected: each line reads 'ROW#    col8    '\n");
    printf("  (ROW marker at 0-3, 'col8' at 8-11, rest blank)\n");

    printf("\n=== Cursor test complete ===\n");
}

/* ===== Main ===== */

static void usage(void)
{
    printf("lcd_partial — LCD partial write test for SN110\n\n");
    printf("Usage:\n");
    printf("  lcd_partial proof     Prove partial writes work\n");
    printf("  lcd_partial blink     Replicate factory heartbeat blink\n");
    printf("  lcd_partial modes     Probe OR/XOR/AND write modes\n");
    printf("  lcd_partial cursors   Verify cursor row/col mapping\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "proof") == 0)
        test_proof();
    else if (strcmp(argv[1], "blink") == 0)
        test_blink();
    else if (strcmp(argv[1], "modes") == 0)
        test_modes();
    else if (strcmp(argv[1], "cursors") == 0)
        test_cursors();
    else {
        printf("Unknown command: %s\n\n", argv[1]);
        usage();
        return 1;
    }

    printf("\nDone.\n");
    return 0;
}
