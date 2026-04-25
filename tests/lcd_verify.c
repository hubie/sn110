/*
 * lcd_verify — Verify LCD driver findings from kernel disassembly
 *
 * Tests findings that were derived from ARM disassembly but never
 * confirmed on hardware. Each test is designed for visual observation
 * via telnet while watching the physical LCD.
 *
 * Usage:
 *   lcd_verify filter      Test which chars 0x08-0x1F pass through vs. are filtered
 *   lcd_verify width       Test character cell width (5 vs 6 pixel columns)
 *   lcd_verify contrast    Sweep contrast via ioctl cmd 17
 *   lcd_verify backlight   Toggle backlight via ioctl cmd 18
 *   lcd_verify pixel       Test pixel write commands (cmds 14, 15, 16)
 *   lcd_verify allchars    Display all printable characters 0x20-0x7E
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

#define LCD_CMD7   LCD_IOC_W(7, 48)    /* cursor + write (48 bytes)  */
#define LCD_CMD14  LCD_IOC_W(14, 12)   /* pixel rect draw (12 bytes) */
#define LCD_CMD15  LCD_IOC_W(15, 8)    /* pixel column draw (8 bytes)*/
#define LCD_CMD16  LCD_IOC_W(16, 8)    /* pixel column draw v2       */
#define LCD_CMD17  LCD_IOC_W(17, 4)    /* contrast (4 bytes)         */
#define LCD_CMD18  LCD_IOC_W(18, 4)    /* backlight (4 bytes)        */
#define LCD_CLEAR  0x6C02              /* clear screen (no arg)      */

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

static void lcd_write(const void *buf, int len)
{
    write(g_fd, buf, len);
}

static void lcd_clear(void)
{
    ioctl(g_fd, LCD_CLEAR, 0);
}

/* Cursor-positioned NUL-terminated write (confirmed working) */
static void lcd_cmd7(int col, int row, const char *text)
{
    char buf[48];
    int len;

    memset(buf, 0, 48);
    buf[2] = (char)col;
    buf[3] = (char)row;
    for (len = 0; text[len] && len < 43; len++)
        buf[4 + len] = text[len];

    ioctl(g_fd, LCD_CMD7, buf);
}

static void pause_ms(int ms)
{
    usleep(ms * 1000);
}

/* ===== Test: Character filter (0x08-0x1F) ===== */

static void test_filter(void)
{
    char label[16];

    printf("\n=== Test: Character Filter (0x08-0x1F) ===\n");
    printf("Tests which control characters pass through lcd_write_line\n");
    printf("vs. which are replaced with space.\n\n");
    printf("Disassembly prediction:\n");
    printf("  0x01-0x07: PASS (CGRAM glyphs)\n");
    printf("  0x08-0x1F: FILTERED (replaced with space)\n");
    printf("  Exception: 0x0A (newline) pads line with spaces\n\n");

    /* First show the known-good CGRAM chars for reference */
    printf("[CGRAM reference] Writing chars 0x01-0x07 on line 1\n");
    lcd_open();
    lcd_clear();
    lcd_cmd7(0, 0, "CGRAM 1-7:");
    lcd_close();
    pause_ms(500);

    lcd_open();
    {
        char cgram_line[48];
        memset(cgram_line, 0, 48);
        cgram_line[2] = 11;  /* col 11 */
        cgram_line[3] = 0;   /* row 0 */
        cgram_line[4] = 0x01;
        cgram_line[5] = 0x02;
        cgram_line[6] = 0x03;
        cgram_line[7] = 0x04;
        cgram_line[8] = 0x05;
        /* NUL at cgram_line[9] terminates */
        ioctl(g_fd, LCD_CMD7, cgram_line);
    }
    lcd_close();
    printf("  Line 1 should show 'CGRAM 1-7:' then 5 glyphs\n");
    printf("  (Can't show 0x06 and 0x07 — only 5 positions left)\n");
    pause_ms(3000);

    /* Now test 0x08-0x1F one at a time on line 2 */
    printf("\n[Filter test] Writing chars 0x08-0x1F on lines 2-4\n");
    printf("Each char shown with its hex code. If filtered, it appears as space.\n\n");

    /*
     * Show in groups of 4 chars per line:
     *   Line 2: 08=? 09=? 0B=? 0C=?
     *   Line 3: 0D=? 0E=? 0F=? 10=?
     *   Line 4: 11=? 12=? 13=? etc.
     * Skip 0x0A (newline) — it would corrupt the display layout.
     */
    {
        /* Control chars to test (skip 0x0A which is newline) */
        int chars[] = {
            0x08, 0x09, 0x0B, 0x0C,
            0x0D, 0x0E, 0x0F, 0x10,
            0x11, 0x12, 0x13, 0x14
        };
        int row, col, i;
        char buf[48];

        for (i = 0; i < 12; i++) {
            row = 1 + (i / 4);  /* rows 1-3 (lines 2-4) */
            col = (i % 4) * 4;  /* columns 0, 4, 8, 12 */

            /* Write hex label */
            label[0] = "0123456789ABCDEF"[(chars[i] >> 4) & 0xF];
            label[1] = "0123456789ABCDEF"[chars[i] & 0xF];
            label[2] = '=';
            label[3] = '\0';
            lcd_open();
            lcd_cmd7(col, row, label);
            lcd_close();

            /* Write the test character */
            memset(buf, 0, 48);
            buf[2] = (char)(col + 3);
            buf[3] = (char)row;
            buf[4] = (char)chars[i];
            /* buf[5] = 0 (NUL terminator) */

            lcd_open();
            ioctl(g_fd, LCD_CMD7, buf);
            lcd_close();
        }
    }

    printf("  CHECK THE LCD:\n");
    printf("  Line 2: 08=? 09=? 0B=? 0C=?\n");
    printf("  Line 3: 0D=? 0E=? 0F=? 10=?\n");
    printf("  Line 4: 11=? 12=? 13=? 14=?\n\n");
    printf("  If a char shows a visible glyph, it PASSES through.\n");
    printf("  If it shows a blank space, it is FILTERED.\n");
    printf("  Note: some may show unexpected glyphs (black box, etc.)\n");
    pause_ms(8000);

    /* Second page: remaining chars 0x15-0x1F */
    printf("\n[Filter test page 2] Chars 0x15-0x1F\n");
    lcd_open();
    lcd_clear();
    lcd_close();
    pause_ms(300);

    {
        int chars2[] = {
            0x15, 0x16, 0x17, 0x18,
            0x19, 0x1A, 0x1B, 0x1C,
            0x1D, 0x1E, 0x1F, 0x7F
        };
        int row, col, i;
        char buf[48];

        for (i = 0; i < 12; i++) {
            row = (i / 4);
            col = (i % 4) * 4;

            label[0] = "0123456789ABCDEF"[(chars2[i] >> 4) & 0xF];
            label[1] = "0123456789ABCDEF"[chars2[i] & 0xF];
            label[2] = '=';
            label[3] = '\0';
            lcd_open();
            lcd_cmd7(col, row, label);
            lcd_close();

            memset(buf, 0, 48);
            buf[2] = (char)(col + 3);
            buf[3] = (char)row;
            buf[4] = (char)chars2[i];

            lcd_open();
            ioctl(g_fd, LCD_CMD7, buf);
            lcd_close();
        }
    }

    printf("  CHECK THE LCD:\n");
    printf("  Line 1: 15=? 16=? 17=? 18=?\n");
    printf("  Line 2: 19=? 1A=? 1B=? 1C=?\n");
    printf("  Line 3: 1D=? 1E=? 1F=? 7F=?\n\n");
    printf("  0x1A and 0x1B are the heartbeat port indicators.\n");
    printf("  0x7F is DEL — predicted to show a 'house' glyph.\n");
    pause_ms(8000);

    printf("\n=== Filter test complete ===\n");
}

/* ===== Test: Character cell width ===== */

static void test_width(void)
{
    printf("\n=== Test: Character Cell Width ===\n");
    printf("Determines if char cells are 5 or 6 pixels wide.\n");
    printf("Chars 1,2,5,6 have 6-column font data.\n");
    printf("Chars 3,4,7 have 5-column font data.\n\n");

    lcd_open();
    lcd_clear();
    lcd_close();
    pause_ms(300);

    /* Write a mix of 5-col and 6-col chars side by side */
    printf("[Step 1] Line 1: alternating 5-col (heart) and 6-col (smiley)\n");
    printf("  If cells are 5px wide, the 6-col chars will be clipped.\n");
    printf("  If cells are 6px wide, all chars render fully.\n");

    lcd_open();
    {
        char buf[48];
        int i;
        memset(buf, 0, 48);
        buf[2] = 0;  /* col 0 */
        buf[3] = 0;  /* row 0 */
        /* Alternate: heart(5col), smiley(6col), heart, smiley... */
        for (i = 0; i < 16; i++)
            buf[4 + i] = (i % 2 == 0) ? 0x03 : 0x01;
        ioctl(g_fd, LCD_CMD7, buf);
    }
    lcd_close();
    printf("  Line 1: alternating heart/smiley × 16\n");
    printf("  Look for clipping on the smiley outlines.\n");
    pause_ms(4000);

    printf("\n[Step 2] Line 2: all 6-col chars (smileys)\n");
    printf("  Line 3: all 5-col chars (hearts)\n");
    lcd_open();
    {
        char buf[48];
        int i;
        /* Line 2: all smileys (6-col) */
        memset(buf, 0, 48);
        buf[2] = 0;
        buf[3] = 1;
        for (i = 0; i < 16; i++)
            buf[4 + i] = 0x01;
        ioctl(g_fd, LCD_CMD7, buf);
    }
    lcd_close();

    lcd_open();
    {
        char buf[48];
        int i;
        /* Line 3: all hearts (5-col) */
        memset(buf, 0, 48);
        buf[2] = 0;
        buf[3] = 2;
        for (i = 0; i < 16; i++)
            buf[4 + i] = 0x03;
        ioctl(g_fd, LCD_CMD7, buf);
    }
    lcd_close();

    printf("  Compare the spacing between chars on lines 2 and 3.\n");
    printf("  If 6-col chars are wider, the smiley line will look\n");
    printf("  more tightly packed than the heart line.\n");
    pause_ms(4000);

    printf("\n[Step 3] Line 4: all 7 CGRAM chars for visual reference\n");
    lcd_open();
    {
        char buf[48];
        memset(buf, 0, 48);
        buf[2] = 0;
        buf[3] = 3;
        buf[4]  = '1';  buf[5]  = 0x01;
        buf[6]  = '2';  buf[7]  = 0x02;
        buf[8]  = '3';  buf[9]  = 0x03;
        buf[10] = '4';  buf[11] = 0x04;
        buf[12] = '5';  buf[13] = 0x05;
        buf[14] = '6';  buf[15] = 0x06;
        buf[16] = '7';  buf[17] = 0x07;
        ioctl(g_fd, LCD_CMD7, buf);
    }
    lcd_close();
    printf("  Line 4: '1' smiley '2' filled_smiley '3' heart ... etc.\n");
    pause_ms(5000);

    printf("\n=== Width test complete ===\n");
}

/* ===== Test: Contrast (cmd 17) ===== */

static void test_contrast(void)
{
    int values[] = { 0, 8, 16, 24, 32, 40, 48, 56, 63 };
    int i;

    printf("\n=== Test: Contrast (ioctl cmd 17) ===\n");
    printf("Prediction: cmd 17 sets contrast, signed byte, clamped to 0x3F.\n");
    printf("Sweeping values 0 through 63.\n\n");

    /* Put reference text on screen first */
    lcd_open();
    lcd_clear();
    lcd_cmd7(0, 0, "Contrast test");
    lcd_cmd7(0, 1, "Watch for change");
    lcd_cmd7(0, 2, "################");
    lcd_cmd7(0, 3, "................");
    lcd_close();
    pause_ms(2000);

    for (i = 0; i < 9; i++) {
        char buf[4];
        memset(buf, 0, 4);
        buf[0] = (char)values[i];

        printf("[Contrast %2d/63] ", values[i]);
        lcd_open();
        ioctl(g_fd, LCD_CMD17, buf);
        lcd_close();

        /* Also update line 1 to show current value */
        lcd_open();
        {
            char msg[16];
            int v = values[i];
            msg[0] = 'C';
            msg[1] = '=';
            msg[2] = '0' + (v / 10);
            msg[3] = '0' + (v % 10);
            msg[4] = ' ';
            msg[5] = '\0';
            lcd_cmd7(11, 0, msg);
        }
        lcd_close();

        printf("Watch the display...\n");
        pause_ms(3000);
    }

    /* Try negative values (cmd 17 takes signed byte) */
    printf("\n[Negative values] Testing -1, -32, -64\n");
    {
        int neg_vals[] = { -1, -32, -64 };
        int j;
        for (j = 0; j < 3; j++) {
            char buf[4];
            memset(buf, 0, 4);
            buf[0] = (char)neg_vals[j];

            printf("  Contrast %d... ", neg_vals[j]);
            lcd_open();
            ioctl(g_fd, LCD_CMD17, buf);
            lcd_close();

            lcd_open();
            {
                char msg[16];
                msg[0] = 'C'; msg[1] = '=';
                msg[2] = '-';
                msg[3] = '0' + ((-neg_vals[j]) / 10);
                msg[4] = '0' + ((-neg_vals[j]) % 10);
                msg[5] = '\0';
                lcd_cmd7(11, 0, msg);
            }
            lcd_close();
            printf("watch...\n");
            pause_ms(3000);
        }
    }

    /* Restore to middle value */
    printf("\n[Restore] Setting contrast to 32\n");
    {
        char buf[4];
        memset(buf, 0, 4);
        buf[0] = 32;
        lcd_open();
        ioctl(g_fd, LCD_CMD17, buf);
        lcd_close();
    }

    printf("\n=== Contrast test complete ===\n");
    printf("If nothing changed, cmd 17 may not be contrast —\n");
    printf("try swapping with cmd 18 (backlight test).\n");
}

/* ===== Test: Backlight (cmd 18) ===== */

static void test_backlight(void)
{
    printf("\n=== Test: Backlight (ioctl cmd 18) ===\n");
    printf("Prediction: cmd 18 controls backlight.\n");
    printf("Testing various values.\n\n");

    /* Reference text */
    lcd_open();
    lcd_clear();
    lcd_cmd7(0, 0, "Backlight test");
    lcd_cmd7(0, 1, "Watch for change");
    lcd_cmd7(0, 2, "################");
    lcd_cmd7(0, 3, "ABCDEFGHIJKLMNOP");
    lcd_close();
    pause_ms(2000);

    /* Try value 0 (off?) */
    printf("[BL=0] Setting backlight to 0...\n");
    {
        char buf[4];
        memset(buf, 0, 4);
        buf[0] = 0;
        lcd_open();
        ioctl(g_fd, LCD_CMD18, buf);
        lcd_close();
    }
    printf("  Did the backlight turn OFF? (3 seconds)\n");
    pause_ms(3000);

    /* Try value 1 (on?) */
    printf("[BL=1] Setting backlight to 1...\n");
    {
        char buf[4];
        memset(buf, 0, 4);
        buf[0] = 1;
        lcd_open();
        ioctl(g_fd, LCD_CMD18, buf);
        lcd_close();
    }
    printf("  Did the backlight turn ON? (3 seconds)\n");
    pause_ms(3000);

    /* Try value 0 again */
    printf("[BL=0] Setting backlight to 0 again...\n");
    {
        char buf[4];
        memset(buf, 0, 4);
        buf[0] = 0;
        lcd_open();
        ioctl(g_fd, LCD_CMD18, buf);
        lcd_close();
    }
    printf("  Backlight off? (3 seconds)\n");
    pause_ms(3000);

    /* Try higher values in case it's a brightness level */
    printf("[BL sweep] Testing values 0, 32, 64, 128, 255...\n");
    {
        int vals[] = { 0, 32, 64, 128, 255 };
        int i;
        for (i = 0; i < 5; i++) {
            char buf[4];
            memset(buf, 0, 4);
            buf[0] = (char)vals[i];
            printf("  BL=%3d... ", vals[i]);
            lcd_open();
            ioctl(g_fd, LCD_CMD18, buf);
            lcd_close();
            printf("watch. (2 sec)\n");
            pause_ms(2000);
        }
    }

    /* Restore backlight on */
    printf("[Restore] Setting backlight to 1\n");
    {
        char buf[4];
        memset(buf, 0, 4);
        buf[0] = 1;
        lcd_open();
        ioctl(g_fd, LCD_CMD18, buf);
        lcd_close();
    }

    printf("\n=== Backlight test complete ===\n");
    printf("If nothing changed, cmd 18 may not be backlight —\n");
    printf("try the contrast test (cmd 17 and 18 may be swapped).\n");
}

/* ===== Test: Pixel write commands ===== */

/*
 * Pixel framebuffer layout (from disassembly):
 *   byte_offset = row + (col / 8) * 100
 *   bit_position = col & 7
 *
 * Cmd 15 (_IOW('l', 15, 8)) handler reads:
 *   buf[1]   = pixel data byte (8 bits = 8 rows of one column)
 *   buf[2-3] = halfword param 1 (start position?)
 *   buf[4-5] = halfword param 2 (end position?)
 *   buf[6-7] = halfword param 3 (limit?)
 *
 * Cmd 14 (_IOW('l', 14, 12)) handler reads:
 *   buf[0-1] = halfword param 1 (row/y start?)
 *   buf[2-3] = halfword param 2 (col/x start?)
 *   buf[4-5] = halfword param 3 (row/y end?)
 *   buf[6-7] = halfword param 4 (col/x end?)
 *   buf[9]   = pixel data byte
 *
 * Since we're not sure of the exact argument interpretation,
 * we try several hypotheses and observe what appears.
 */

static void lcd_cmd15(int p0, int p1, int p2, int p3)
{
    char buf[8];
    memset(buf, 0, 8);
    buf[1] = (char)p0;          /* pixel data byte */
    buf[2] = (char)(p1 & 0xFF);
    buf[3] = (char)((p1 >> 8) & 0xFF);
    buf[4] = (char)(p2 & 0xFF);
    buf[5] = (char)((p2 >> 8) & 0xFF);
    buf[6] = (char)(p3 & 0xFF);
    buf[7] = (char)((p3 >> 8) & 0xFF);
    ioctl(g_fd, LCD_CMD15, buf);
}

static void lcd_cmd14(int p0, int p1, int p2, int p3, int pixel)
{
    char buf[12];
    memset(buf, 0, 12);
    buf[0] = (char)(p0 & 0xFF);
    buf[1] = (char)((p0 >> 8) & 0xFF);
    buf[2] = (char)(p1 & 0xFF);
    buf[3] = (char)((p1 >> 8) & 0xFF);
    buf[4] = (char)(p2 & 0xFF);
    buf[5] = (char)((p2 >> 8) & 0xFF);
    buf[6] = (char)(p3 & 0xFF);
    buf[7] = (char)((p3 >> 8) & 0xFF);
    buf[9] = (char)pixel;
    ioctl(g_fd, LCD_CMD14, buf);
}

static void lcd_cmd16(int p0, int p1, int p2, int p3)
{
    char buf[8];
    memset(buf, 0, 8);
    buf[1] = (char)p0;
    buf[2] = (char)(p1 & 0xFF);
    buf[3] = (char)((p1 >> 8) & 0xFF);
    buf[4] = (char)(p2 & 0xFF);
    buf[5] = (char)((p2 >> 8) & 0xFF);
    buf[6] = (char)(p3 & 0xFF);
    buf[7] = (char)((p3 >> 8) & 0xFF);
    ioctl(g_fd, LCD_CMD16, buf);
}

static void test_pixel(void)
{
    printf("\n=== Test: Pixel Write Commands (cmds 14, 15, 16) ===\n");
    printf("These commands write directly to the pixel framebuffer.\n");
    printf("Argument format is reconstructed from disassembly —\n");
    printf("we try several interpretations to see what works.\n\n");

    /* Start with a known screen so we can see changes */
    printf("[Setup] Writing reference screen\n");
    lcd_open();
    lcd_write(
        "                "
        "                "
        "                "
        "                ",
        64);
    lcd_close();
    pause_ms(1000);

    /*
     * Hypothesis A for cmd 15:
     *   p0 = pixel data (0xFF = all 8 rows lit)
     *   p1 = start column (pixel x)
     *   p2 = end column (pixel x)
     *   p3 = row range limit
     *
     * Try drawing a vertical stripe at pixel column 0.
     */
    printf("[Cmd 15, Hyp A] pixel=0xFF, start=0, end=0, limit=31\n");
    printf("  Expecting: vertical stripe at left edge\n");
    lcd_open();
    lcd_cmd15(0xFF, 0, 0, 31);
    lcd_close();
    pause_ms(3000);

    printf("[Cmd 15, Hyp A] pixel=0xFF, start=0, end=7, limit=31\n");
    printf("  Expecting: 8-pixel-wide vertical bar at left edge\n");
    lcd_open();
    lcd_cmd15(0xFF, 0, 7, 31);
    lcd_close();
    pause_ms(3000);

    /*
     * Hypothesis B for cmd 15:
     *   p0 = pixel data
     *   p1 = column
     *   p2 = start row
     *   p3 = end row
     */
    printf("[Cmd 15, Hyp B] pixel=0xFF, col=40, start_row=0, end_row=7\n");
    printf("  Expecting: vertical stripe near center\n");
    lcd_open();
    lcd_cmd15(0xFF, 40, 0, 7);
    lcd_close();
    pause_ms(3000);

    /* Clear and try cmd 14 */
    printf("\n[Clear] Resetting screen\n");
    lcd_open();
    lcd_clear();
    lcd_close();
    pause_ms(500);

    lcd_open();
    lcd_write(
        "                "
        "                "
        "                "
        "                ",
        64);
    lcd_close();
    pause_ms(500);

    /*
     * Hypothesis A for cmd 14:
     *   p0 = y_start (row start)
     *   p1 = x_start (col start)
     *   p2 = y_end   (row end)
     *   p3 = x_end   (col end)
     *   pixel = fill pattern
     *
     * Try filling a small rectangle.
     */
    printf("[Cmd 14, Hyp A] y0=0, x0=0, y1=7, x1=4, pixel=0xFF\n");
    printf("  Expecting: filled block in top-left character cell\n");
    lcd_open();
    lcd_cmd14(0, 0, 7, 4, 0xFF);
    lcd_close();
    pause_ms(3000);

    printf("[Cmd 14, Hyp A] y0=0, x0=40, y1=15, x1=44, pixel=0xFF\n");
    printf("  Expecting: filled block near center of line 1-2\n");
    lcd_open();
    lcd_cmd14(0, 40, 15, 44, 0xFF);
    lcd_close();
    pause_ms(3000);

    /*
     * Hypothesis B for cmd 14:
     *   p0 = x_start
     *   p1 = y_start
     *   p2 = x_end
     *   p3 = y_end
     */
    printf("[Cmd 14, Hyp B] x0=60, y0=0, x1=64, y1=7, pixel=0xFF\n");
    printf("  Expecting: filled block at different position\n");
    lcd_open();
    lcd_cmd14(60, 0, 64, 7, 0xFF);
    lcd_close();
    pause_ms(3000);

    /* Try cmd 14 with a pattern instead of solid fill */
    printf("[Cmd 14, pattern] Same coords, pixel=0xAA (alternating)\n");
    printf("  Expecting: striped pattern if it works\n");
    lcd_open();
    lcd_cmd14(0, 16, 7, 20, 0xAA);
    lcd_close();
    pause_ms(3000);

    /* Try cmd 16 */
    printf("\n[Clear] Resetting screen\n");
    lcd_open();
    lcd_clear();
    lcd_close();
    pause_ms(500);

    lcd_open();
    lcd_write(
        "                "
        "                "
        "                "
        "                ",
        64);
    lcd_close();
    pause_ms(500);

    printf("[Cmd 16, Hyp A] pixel=0xFF, start=0, end=0, limit=31\n");
    lcd_open();
    lcd_cmd16(0xFF, 0, 0, 31);
    lcd_close();
    pause_ms(3000);

    printf("[Cmd 16, Hyp A] pixel=0xFF, start=0, end=7, limit=31\n");
    lcd_open();
    lcd_cmd16(0xFF, 0, 7, 31);
    lcd_close();
    pause_ms(3000);

    printf("\n=== Pixel test complete ===\n");
    printf("Record which hypotheses (if any) produced visible results.\n");
    printf("If NONE worked, the argument format differs from our\n");
    printf("predictions — the disassembly needs reinterpretation.\n");
}

/* ===== Test: All printable characters ===== */

static void test_allchars(void)
{
    int page, start, i, row, col;

    printf("\n=== Test: All Printable Characters ===\n");
    printf("Displays all chars 0x20-0x7E to verify font table.\n\n");

    /* 95 printable chars (0x20-0x7E) shown in pages of 64 */
    for (page = 0; page < 2; page++) {
        start = 0x20 + (page * 64);
        if (start > 0x7E)
            break;

        printf("[Page %d] Chars 0x%02X-0x%02X\n", page + 1,
               start, (start + 63 > 0x7E) ? 0x7E : start + 63);

        lcd_open();
        lcd_clear();
        lcd_close();
        pause_ms(300);

        for (i = 0; i < 64 && (start + i) <= 0x7E; i++) {
            row = i / 16;
            col = i % 16;

            lcd_open();
            {
                char buf[48];
                memset(buf, 0, 48);
                buf[2] = (char)col;
                buf[3] = (char)row;
                buf[4] = (char)(start + i);
                ioctl(g_fd, LCD_CMD7, buf);
            }
            lcd_close();
        }

        printf("  Verify characters match standard ASCII.\n");
        if (page == 0)
            printf("  Line 1: SP ! \" # $ %% & ' ( ) * + , - . /\n");
        pause_ms(6000);
    }

    printf("\n=== All chars test complete ===\n");
}

/* ===== Test: Reset (cmd 8) — does it clear the pixel framebuffer? ===== */

#define LCD_CMD8  LCD_IOC_W(8, 4)

static void test_reset(void)
{
    printf("\n=== Test: Reset (ioctl cmd 8) ===\n");
    printf("Cmd 8 memsets 500 bytes to 0 — does that include the\n");
    printf("pixel framebuffer?\n\n");

    /* Step 1: draw recognizable pixel content */
    printf("[Step 1] Drawing pixel patterns with cmd 15\n");
    lcd_open();
    lcd_clear();
    lcd_close();
    pause_ms(300);

    /* Horizontal bar across top */
    lcd_open();
    lcd_cmd15(0xFF, 0, 0, 31);
    lcd_close();

    /* Another bar further right */
    lcd_open();
    lcd_cmd15(0xFF, 40, 40, 60);
    lcd_close();

    /* Also write some text so we can see if that clears too */
    lcd_open();
    lcd_cmd7(0, 2, "TEXT BEFORE RST");
    lcd_close();

    printf("  You should see: pixel bars at top + text on line 3\n");
    printf("  (5 seconds to observe)\n");
    pause_ms(5000);

    /* Step 2: fire cmd 8 */
    printf("[Step 2] Firing ioctl cmd 8 (reset)...\n");
    {
        char buf[4];
        memset(buf, 0, 4);
        lcd_open();
        ioctl(g_fd, LCD_CMD8, buf);
        lcd_close();
    }
    printf("  Done. Check the LCD:\n");
    printf("  - Did the pixel bars disappear?\n");
    printf("  - Did the text disappear?\n");
    printf("  - Is the screen fully blank?\n");
    printf("  (5 seconds to observe)\n");
    pause_ms(5000);

    /* Step 3: write new text to confirm the display still works */
    printf("[Step 3] Writing new text to confirm display works\n");
    lcd_open();
    lcd_cmd7(0, 0, "After reset");
    lcd_close();
    printf("  Line 1 should say 'After reset'\n");
    printf("  Any remaining pixel artifacts?\n");
    pause_ms(5000);

    printf("\n=== Reset test complete ===\n");
}

/* ===== Main ===== */

static void usage(void)
{
    printf("lcd_verify — Verify LCD driver disassembly findings\n\n");
    printf("Usage:\n");
    printf("  lcd_verify filter      Chars 0x08-0x1F: filtered or passed?\n");
    printf("  lcd_verify width       Character cell width: 5 or 6 pixels?\n");
    printf("  lcd_verify contrast    Sweep contrast via ioctl cmd 17\n");
    printf("  lcd_verify backlight   Toggle backlight via ioctl cmd 18\n");
    printf("  lcd_verify pixel       Test pixel write commands 14, 15, 16\n");
    printf("  lcd_verify allchars    Display all printable chars 0x20-0x7E\n");
    printf("  lcd_verify reset       Does cmd 8 clear the pixel framebuffer?\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "filter") == 0)
        test_filter();
    else if (strcmp(argv[1], "width") == 0)
        test_width();
    else if (strcmp(argv[1], "contrast") == 0)
        test_contrast();
    else if (strcmp(argv[1], "backlight") == 0)
        test_backlight();
    else if (strcmp(argv[1], "pixel") == 0)
        test_pixel();
    else if (strcmp(argv[1], "allchars") == 0)
        test_allchars();
    else if (strcmp(argv[1], "reset") == 0)
        test_reset();
    else {
        printf("Unknown command: %s\n\n", argv[1]);
        usage();
        return 1;
    }

    printf("\nDone.\n");
    return 0;
}
