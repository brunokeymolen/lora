/*
 * TrailText
 * Text when networks fail.
 *
 * Copyright (c) 2026 Bruno Keymolen
 *
 * This work is licensed under the Creative Commons
 * Attribution-NonCommercial-ShareAlike 4.0 International License.
 *
 * You are free to share and adapt this work for non-commercial purposes,
 * provided that appropriate credit is given and any derivative works are
 * distributed under the same license.
 *
 * License: CC BY-NC-SA 4.0
 * See: https://creativecommons.org/licenses/by-nc-sa/4.0/
 */

#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#define SSD1306_I2C_ADDR    0x3C
#define SSD1306_WIDTH       128
#define SSD1306_HEIGHT      64
#define SSD1306_PAGES       (SSD1306_HEIGHT / 8)   // 8
#define SSD1306_COLS        (SSD1306_WIDTH / 6)    // 21 chars (6 px wide each)

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t fb[SSD1306_PAGES][SSD1306_WIDTH]; // framebuffer
} ssd1306_t;

/**
 * Initialise the display. Powers up VEXT if vext_gpio >= 0 (active-LOW) and
 * toggles RESET if rst_gpio >= 0.
 */
esp_err_t ssd1306_init(ssd1306_t *dev, int sda_gpio, int scl_gpio, int vext_gpio, int rst_gpio);

/** Clear framebuffer and flush to display. */
void ssd1306_clear(ssd1306_t *dev);

/** Flush the entire framebuffer to the display. */
void ssd1306_flush(ssd1306_t *dev);

/** Draw a single ASCII character at text column/row (0-based). */
void ssd1306_putchar(ssd1306_t *dev, uint8_t col, uint8_t row, char c);

/** Draw a NUL-terminated string starting at col/row, wraps at screen edge. */
void ssd1306_puts(ssd1306_t *dev, uint8_t col, uint8_t row, const char *str);

/** printf into a single text row (rows 0-7). */
void ssd1306_printf(ssd1306_t *dev, uint8_t row, const char *fmt, ...);

/** Turn the display panel on (true) or off (false). Off blanks the panel but
 *  preserves the framebuffer; calling ssd1306_flush() after ssd1306_power(true)
 *  restores the previous content. */
void ssd1306_power(ssd1306_t *dev, bool on);
