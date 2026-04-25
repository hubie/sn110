/*
 * lcd_hw.h — Low-level LCD ioctl helpers for /dev/lcd0
 *
 * Each function opens /dev/lcd0, issues a single ioctl, and closes.
 * This matches the factory firmware's open-ioctl-close pattern and
 * avoids cursor state leaking between callers.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_LCD_HW_H
#define SN110_LCD_HW_H

#define LCD_DEV       "/dev/lcd0"
#define LCD_COLS      16
#define LCD_ROWS      4

/*
 * Ioctl command encodings (from kernel disassembly at 0x6B9CC).
 *
 * Linux _IOW('l', nr, size) = (1 << 30) | (size << 16) | ('l' << 8) | nr
 */
#define LCD_IOC_CLEAR     0x6C02                                    /* cmd 2: clear char buffer */
#define LCD_IOC_CMD7      ((1U << 30) | (48 << 16) | ('l' << 8) | 7)  /* cmd 7: cursor + write */
#define LCD_IOC_CMD8      ((1U << 30) | (4 << 16)  | ('l' << 8) | 8)  /* cmd 8: clear pixel FB */
#define LCD_IOC_CONTRAST  ((1U << 30) | (4 << 16)  | ('l' << 8) | 17) /* cmd 17: contrast */
#define LCD_IOC_BACKLIGHT ((1U << 30) | (4 << 16)  | ('l' << 8) | 18) /* cmd 18: backlight */

/*
 * Write a NUL-terminated string at (col, row) via ioctl cmd 7.
 * Max 43 characters (48-byte buffer minus 4-byte header minus NUL).
 * Positions before col and after the NUL terminator are untouched.
 */
void lcd_hw_write(int col, int row, const char *text);

/*
 * Write a full line (pad/truncate to LCD_COLS) at the given row.
 * Equivalent to lcd_hw_write(0, row, <16-char padded string>).
 */
void lcd_hw_write_line(int row, const char *text);

/* Clear character buffer (cmd 2) + reset cursor to 0 */
void lcd_hw_clear_chars(void);

/* Clear pixel framebuffer (cmd 8) — does not touch character buffer */
void lcd_hw_clear_pixels(void);

/*
 * Set contrast (cmd 17). Clamped to 0-63 in userspace.
 * WARNING: negative values crash the device. This function prevents that.
 */
void lcd_hw_contrast(int value);

/* Set backlight (cmd 18): 0=off, nonzero=on. No dimming. */
void lcd_hw_backlight(int on);

#endif /* SN110_LCD_HW_H */
