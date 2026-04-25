/*
 * lcd_hw.c — Low-level LCD ioctl helpers for /dev/lcd0
 *
 * Every function opens the device, performs a single ioctl, and closes.
 * This matches the factory firmware's pattern and avoids cursor state
 * leaking between callers.
 *
 * See docs/lcd-partial-write.md for the full ioctl command table and
 * kernel address references.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "lcd_hw.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* ioctl prototype — declared here to avoid pulling in sys/ioctl.h
 * which may not exist in the minimal uClinux libc. */
int ioctl(int fd, unsigned long request, ...);

/* Low-level cmd 7 write. Bypasses the line change cache —
 * only lcd_hw_write_line() benefits from change detection. */
static void lcd_hw_write(int col, int row, const char *text)
{
    char buf[48];
    int fd, len;

    memset(buf, 0, sizeof(buf));
    buf[2] = (char)col;
    buf[3] = (char)row;

    len = strlen(text);
    if (len > 43)
        len = 43;  /* 48 - 4 header - 1 NUL */
    memcpy(&buf[4], text, len);
    /* buf[4+len] is already 0 from memset */

    fd = open(LCD_DEV, O_RDWR);
    if (fd < 0) return;
    ioctl(fd, LCD_IOC_CMD7, buf);
    close(fd);
}

/* Previous line contents — skip ioctl when unchanged */
static char prev_lines[LCD_ROWS][LCD_COLS + 1];
static int  prev_valid = 0;  /* set to 0 on clear */

void lcd_hw_write_line(int row, const char *text)
{
    char line[LCD_COLS + 1];
    int len, i;

    if (row < 0 || row >= LCD_ROWS)
        return;

    len = strlen(text);
    if (len > LCD_COLS)
        len = LCD_COLS;
    memcpy(line, text, len);
    for (i = len; i < LCD_COLS; i++)
        line[i] = ' ';
    line[LCD_COLS] = '\0';

    /* Skip write if line hasn't changed */
    if (prev_valid && memcmp(prev_lines[row], line, LCD_COLS) == 0)
        return;

    memcpy(prev_lines[row], line, LCD_COLS + 1);
    prev_valid = 1;
    lcd_hw_write(0, row, line);
}

void lcd_hw_clear_chars(void)
{
    int fd = open(LCD_DEV, O_RDWR);
    if (fd < 0) return;
    ioctl(fd, LCD_IOC_CLEAR, 0);
    close(fd);
    prev_valid = 0;  /* force next write_line to go through */
}

void lcd_hw_clear_pixels(void)
{
    char buf[4];
    int fd;

    memset(buf, 0, sizeof(buf));
    fd = open(LCD_DEV, O_RDWR);
    if (fd < 0) return;
    ioctl(fd, LCD_IOC_CMD8, buf);
    close(fd);
}

void lcd_hw_contrast(int value)
{
    int fd;
    char buf[4];

    /* CRITICAL: negative values crash the device. Clamp to 0-63. */
    if (value < 0) value = 0;
    if (value > 63) value = 63;

    memset(buf, 0, sizeof(buf));
    buf[0] = (char)value;

    fd = open(LCD_DEV, O_RDWR);
    if (fd < 0) return;
    ioctl(fd, LCD_IOC_CONTRAST, buf);
    close(fd);
}

void lcd_hw_backlight(int on)
{
    int fd;
    char buf[4];

    memset(buf, 0, sizeof(buf));
    buf[0] = on ? 1 : 0;

    fd = open(LCD_DEV, O_RDWR);
    if (fd < 0) return;
    ioctl(fd, LCD_IOC_BACKLIGHT, buf);
    close(fd);
}
