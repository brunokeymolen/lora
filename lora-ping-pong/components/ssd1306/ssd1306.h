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
