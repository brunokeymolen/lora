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

/**
 * LoRa Ping-Pong for Heltec WiFi LoRa 32 V4 (ESP32-S3 + SX1262)
 *
 * IDENTICAL firmware runs on BOTH devices.
 *
 * Design
 * ──────
 * LoRa is half-duplex: the radio is either transmitting or receiving, never
 * both.  When your radio is transmitting it is physically incapable of
 * receiving, so you can never hear your own packets – no filtering required.
 *
 * Every device:
 *   • Listens continuously (IDLE state).
 *   • On button press → sends a PING (with its 3-byte device ID + seq),
 *     then waits up to PONG_TIMEOUT_MS for a matching PONG.
 *   • On receiving a PING → waits a short random delay, then replies PONG
 *     (carrying the measured RSSI/SNR back to the sender).
 *
 * Collision avoidance
 * ────────────────────
 *   1. Device ID in every packet: a PONG responder includes the original
 *      sender's ID as dst_id, so only the right device acts on a PONG.
 *   2. Random PONG reply delay (0..PONG_DELAY_MAX_MS): prevents simultaneous
 *      replies if both boards hear the same burst.
 *   3. State guard: a device that is currently waiting for its own PONG does
 *      not reply to incoming PINGs (it is busy / the air is in use).
 *   4. Seen-packet deduplication: ignores duplicate (src_id, seq) pairs
 *      within a short window to handle re-transmissions gracefully.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "driver/gpio.h"

#include "ra01s.h"
#include "ssd1306.h"

static const char *TAG = "PING_PONG";

/* ── Board pin definitions ────────────────────────────────────────────────── */
#define BTN_GPIO   CONFIG_BUTTON_GPIO   // 0 – BOOT/USR button, active LOW

/* ── LoRa parameters ─────────────────────────────────────────────────────── */
#define LORA_FREQ_HZ    868000000UL // EU868 ISM band
#define LORA_TX_POWER   CONFIG_TX_POWER_DBM
#define LORA_TCXO_V     1.8f        // TCXO on Heltec V4
#define LORA_USE_LDO    false       // DC-DC regulator on Heltec V4
#define LORA_SF         CONFIG_SF_RATE
#define LORA_BW         CONFIG_BANDWIDTH  // raw SX1262 value: 4=125kHz, 5=250kHz, 6=500kHz
#define LORA_CR         CONFIG_CODING_RATE
/* Longer preamble (16 vs default 8 symbols) gives the initiator more time to
 * finish its TX→RX transition (~5-10 ms) before sync detection begins.
 * At SF7/BW125 each symbol = 1.024 ms, so 16 symbols = 16.4 ms preamble. */
#define LORA_PREAMBLE   16
#define LORA_CRC_ON     true

/* ── Packet protocol ─────────────────────────────────────────────────────── */
#define PKT_TYPE_PING   0x50
#define PKT_TYPE_PONG   0x60
#define DEV_ID_LEN      3           // bytes from MAC used as device ID

typedef struct __attribute__((packed)) {
    uint8_t  type;              // PKT_TYPE_PING / PKT_TYPE_PONG
    uint8_t  src_id[DEV_ID_LEN];// sender's truncated MAC
    uint8_t  dst_id[DEV_ID_LEN];// intended recipient (PONG) or 0xFF…FF (PING broadcast)
    uint16_t seq;               // big-endian sequence number
    int8_t   rssi_rep;          // RSSI the responder measured (PONG only)
    int8_t   snr_rep;           // SNR  the responder measured (PONG only)
    uint8_t  tx_power;          // sender's TX power in dBm
    uint16_t uptime_s;          // big-endian sender uptime in seconds
} lora_pkt_t;

/* ── Deduplication cache ─────────────────────────────────────────────────── */
#define SEEN_CACHE_SIZE 8

typedef struct {
    uint8_t  src_id[DEV_ID_LEN];
    uint16_t seq;
    int64_t  seen_at_us;
} seen_entry_t;

static seen_entry_t s_seen[SEEN_CACHE_SIZE];
static uint8_t      s_seen_idx = 0;

/** Returns true if this (src_id, seq) was seen within the last 10 seconds. */
static bool seen_check_and_add(const uint8_t *src, uint16_t seq)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
        if (memcmp(s_seen[i].src_id, src, DEV_ID_LEN) == 0 &&
            s_seen[i].seq == seq &&
            (now - s_seen[i].seen_at_us) < 10000000LL) {
            return true; // duplicate
        }
    }
    // add to ring buffer
    memcpy(s_seen[s_seen_idx].src_id, src, DEV_ID_LEN);
    s_seen[s_seen_idx].seq       = seq;
    s_seen[s_seen_idx].seen_at_us = now;
    s_seen_idx = (s_seen_idx + 1) % SEEN_CACHE_SIZE;
    return false;
}

/* ── Application state ────────────────────────────────────────────────────── */
typedef enum { STATE_IDLE, STATE_WAIT_PONG } app_state_t;

static volatile app_state_t s_state = STATE_IDLE;
static SemaphoreHandle_t    s_btn_sem;

static uint8_t  s_my_id[DEV_ID_LEN]; // our device ID (last 3 MAC bytes)
static uint16_t s_tx_seq = 0;

static volatile struct {
    int8_t  last_tx_rssi; // RSSI peer measured on our TX (from PONG packet)
    int8_t  last_tx_snr;
    int8_t  last_rx_rssi; // RSSI we measured on last RX
    int8_t  last_rx_snr;
    int32_t last_rtt_ms;
    uint16_t ping_sent;
    uint16_t pong_ok;
    uint16_t pong_timeout;
    uint16_t pong_sent;   // times we replied to a peer's PING
    char status[22];
} g;

/* ── OLED ─────────────────────────────────────────────────────────────────── */
static ssd1306_t s_oled;
static bool      s_oled_ready  = false;
static bool      s_display_on  = true;

#define DISPLAY_SLEEP_US  ((int64_t)CONFIG_DISPLAY_SLEEP_S * 1000000LL)
static volatile int64_t s_last_activity_us = 0;

static void activity_touch(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* ── Button ISR ──────────────────────────────────────────────────────────── */
static void IRAM_ATTR btn_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_btn_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── Display ─────────────────────────────────────────────────────────────── */
static void display_update(void)
{
    if (!s_oled_ready || !s_display_on) {
        return;
    }

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    ssd1306_printf(&s_oled, 0, "%02X%02X%02X 868MHz %ddBm",
                   s_my_id[0], s_my_id[1], s_my_id[2], LORA_TX_POWER);
    ssd1306_printf(&s_oled, 1, "Up:%"PRIu32"s SF%d BW125", up, LORA_SF);
    ssd1306_printf(&s_oled, 2, "Sent:%-4u OK:%-4u TO:%-2u",
                   g.ping_sent, g.pong_ok, g.pong_timeout);
    ssd1306_printf(&s_oled, 3, "Replied:%-4u", g.pong_sent);
    ssd1306_printf(&s_oled, 4, "RSSI tx:%-4d rx:%-4d",
                   g.last_tx_rssi, g.last_rx_rssi);
    ssd1306_printf(&s_oled, 5, "SNR  tx:%-4d rx:%-4d",
                   g.last_tx_snr,  g.last_rx_snr);
    ssd1306_printf(&s_oled, 6, "RTT: %"PRId32" ms", g.last_rtt_ms);
    ssd1306_printf(&s_oled, 7, "%.21s", g.status);
    ssd1306_flush(&s_oled);
}

static void display_task(void *arg)
{
    for (;;) {
        if (s_oled_ready) {
            int64_t idle = esp_timer_get_time() - s_last_activity_us;
            if (s_display_on && idle > DISPLAY_SLEEP_US) {
                s_display_on = false;
                ssd1306_power(&s_oled, false);
                ESP_LOGI(TAG, "Display off (idle %llds)", (long long)(idle / 1000000));
            }
        }
        display_update();
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

/* ── Packet helpers ───────────────────────────────────────────────────────── */
static bool send_pkt(uint8_t type, const uint8_t *dst,
                     uint16_t seq, int8_t rssi_rep, int8_t snr_rep)
{
    lora_pkt_t pkt = {
        .type      = type,
        .seq       = __builtin_bswap16(seq),
        .rssi_rep  = rssi_rep,
        .snr_rep   = snr_rep,
        .tx_power  = (uint8_t)LORA_TX_POWER,
        .uptime_s  = __builtin_bswap16(
            (uint16_t)(esp_timer_get_time() / 1000000ULL)),
    };
    memcpy(pkt.src_id, s_my_id, DEV_ID_LEN);
    memcpy(pkt.dst_id, dst,     DEV_ID_LEN);
    return LoRaSend((uint8_t *)&pkt, sizeof(pkt), SX126x_TXMODE_SYNC);
}

/* ── Main LoRa task ──────────────────────────────────────────────────────── */
static void lora_task(void *arg)
{
    static const uint8_t BCAST[DEV_ID_LEN] = {0xFF, 0xFF, 0xFF};
    uint8_t rx_buf[sizeof(lora_pkt_t) + 4];

    ESP_LOGI(TAG, "LoRa task started. My ID: %02X:%02X:%02X",
             s_my_id[0], s_my_id[1], s_my_id[2]);
    snprintf((char *)g.status, sizeof(g.status), "Press BTN to PING");

    for (;;) {
        /* ── Button press → send PING ──────────────────────────────────── */
        if (xSemaphoreTake(s_btn_sem, 0) == pdTRUE) {
            /* Drain extra bounces */
            vTaskDelay(pdMS_TO_TICKS(50));
            while (xSemaphoreTake(s_btn_sem, 0) == pdTRUE);

            /* Wake display first if it is sleeping; don't send PING on wake press */
            if (!s_display_on) {
                s_display_on = true;
                ssd1306_power(&s_oled, true);
                ssd1306_flush(&s_oled); // restore framebuffer content
                activity_touch();
                ESP_LOGI(TAG, "Display woken by button");
                goto rx_poll;
            }

            activity_touch();

            /* Don't interrupt an in-progress wait */
            if (s_state == STATE_WAIT_PONG) {
                ESP_LOGW(TAG, "Already waiting for PONG, ignoring button");
                goto rx_poll;
            }

            s_tx_seq++;
            g.ping_sent++;
            snprintf((char *)g.status, sizeof(g.status),
                     "PING #%u ...", s_tx_seq);
            ESP_LOGI(TAG, "Sending PING seq=%u", s_tx_seq);

            int64_t t_send = esp_timer_get_time();
            if (!send_pkt(PKT_TYPE_PING, BCAST, s_tx_seq, 0, 0)) {
                ESP_LOGE(TAG, "TX failed");
                snprintf((char *)g.status, sizeof(g.status), "TX FAILED");
                goto rx_poll;
            }
            s_state = STATE_WAIT_PONG;
            snprintf((char *)g.status, sizeof(g.status),
                     "Waiting PONG #%u", s_tx_seq);

            /* Wait for matching PONG */
            int64_t deadline = t_send + (int64_t)CONFIG_PONG_TIMEOUT_MS * 1000;
            bool got = false;
            while (!got && esp_timer_get_time() < deadline) {
                uint8_t rxlen = LoRaReceive(rx_buf, sizeof(rx_buf));
                if (rxlen >= (uint8_t)sizeof(lora_pkt_t)) {
                    lora_pkt_t *p = (lora_pkt_t *)rx_buf;
                    uint16_t pseq = __builtin_bswap16(p->seq);
                    if (p->type == PKT_TYPE_PONG &&
                        pseq == s_tx_seq &&
                        memcmp(p->dst_id, s_my_id, DEV_ID_LEN) == 0)
                    {
                        int64_t rtt = (esp_timer_get_time() - t_send) / 1000;
                        g.last_rtt_ms  = (int32_t)rtt;
                        g.pong_ok++;
                        int8_t r, s;
                        GetPacketStatus(&r, &s);
                        g.last_rx_rssi = r;
                        g.last_rx_snr  = s;
                        g.last_tx_rssi = p->rssi_rep;
                        g.last_tx_snr  = p->snr_rep;
                        snprintf((char *)g.status, sizeof(g.status),
                                 "PONG#%u RTT%"PRId32"ms", pseq, g.last_rtt_ms);
                        ESP_LOGI(TAG,
                            "PONG seq=%u RTT=%"PRId32"ms "
                            "my_rx rssi=%d snr=%d  "
                            "peer_rx rssi=%d snr=%d",
                            pseq, g.last_rtt_ms, r, s,
                            p->rssi_rep, p->snr_rep);
                        activity_touch();
                        got = true;
                    }
                }
                vTaskDelay(1);
            }

            if (!got) {
                g.pong_timeout++;
                snprintf((char *)g.status, sizeof(g.status),
                         "TIMEOUT #%u", s_tx_seq);
                ESP_LOGW(TAG, "No PONG (seq=%u, TO=%dms)",
                         s_tx_seq, CONFIG_PONG_TIMEOUT_MS);
            }
            s_state = STATE_IDLE;
            goto rx_poll;
        }

rx_poll:
        /* ── Listen for incoming PING ──────────────────────────────────── */
        if (s_state == STATE_IDLE) {
            uint8_t rxlen = LoRaReceive(rx_buf, sizeof(rx_buf));
            if (rxlen >= (uint8_t)sizeof(lora_pkt_t)) {
                lora_pkt_t *p = (lora_pkt_t *)rx_buf;
                uint16_t pseq = __builtin_bswap16(p->seq);

                /* Only reply to PINGs */
                if (p->type != PKT_TYPE_PING) goto next_iter;

                /* Ignore our own re-echoed packets (shouldn't happen, but safe) */
                if (memcmp(p->src_id, s_my_id, DEV_ID_LEN) == 0) goto next_iter;

                /* Deduplication */
                if (seen_check_and_add(p->src_id, pseq)) {
                    ESP_LOGD(TAG, "Duplicate PING ignored (seq=%u)", pseq);
                    goto next_iter;
                }

                /* Capture radio stats before any delay */
                int8_t rx_rssi, rx_snr;
                GetPacketStatus(&rx_rssi, &rx_snr);
                g.last_rx_rssi = rx_rssi;
                g.last_rx_snr  = rx_snr;

                ESP_LOGI(TAG,
                    "PING from %02X:%02X:%02X seq=%u rssi=%d snr=%d pwr=%d",
                    p->src_id[0], p->src_id[1], p->src_id[2],
                    pseq, rx_rssi, rx_snr, p->tx_power);

                snprintf((char *)g.status, sizeof(g.status),
                         "PING %02X%02X%02X #%u",
                         p->src_id[0], p->src_id[1], p->src_id[2], pseq);
                activity_touch();

                /* Minimum 20 ms delay before replying: gives the initiator time
                 * to complete its TX→RX transition (SetRx SPI + status ~10 ms)
                 * so it doesn't miss the preamble. Plus random jitter to prevent
                 * simultaneous replies when multiple devices hear the same PING. */
                uint32_t delay_ms = 20 + (esp_random() % (CONFIG_PONG_DELAY_MAX_MS + 1));
                vTaskDelay(pdMS_TO_TICKS(delay_ms));

                if (send_pkt(PKT_TYPE_PONG, p->src_id,
                             pseq, rx_rssi, rx_snr))
                {
                    g.pong_sent++;
                    snprintf((char *)g.status, sizeof(g.status),
                             "PONG>%02X%02X%02X #%u",
                             p->src_id[0], p->src_id[1], p->src_id[2], pseq);
                    ESP_LOGI(TAG, "PONG sent to %02X:%02X:%02X seq=%u",
                             p->src_id[0], p->src_id[1], p->src_id[2], pseq);
                } else {
                    ESP_LOGE(TAG, "PONG TX failed");
                    snprintf((char *)g.status, sizeof(g.status), "TX FAILED");
                }
            }
        }

next_iter:
        vTaskDelay(1);
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */
void app_main(void)
{
    /* Derive device ID from last 3 bytes of base MAC */
    uint8_t mac[6];
    esp_base_mac_addr_get(mac);
    memcpy(s_my_id, mac + 3, DEV_ID_LEN);

    ESP_LOGI(TAG, "Device ID: %02X:%02X:%02X",
             s_my_id[0], s_my_id[1], s_my_id[2]);

    /* ── OLED ─────────────────────────────────────────────────────────────── */
    esp_err_t oled_ret = ssd1306_init(&s_oled,
                                      CONFIG_OLED_SDA_GPIO,
                                      CONFIG_OLED_SCL_GPIO,
                                      CONFIG_OLED_VEXT_GPIO,
                                      CONFIG_OLED_RST_GPIO);
    if (oled_ret == ESP_OK) {
        s_oled_ready = true;
        ssd1306_printf(&s_oled, 0, "LoRa Ping-Pong");
        ssd1306_printf(&s_oled, 1, "ID:%02X%02X%02X 868MHz",
                       s_my_id[0], s_my_id[1], s_my_id[2]);
        ssd1306_printf(&s_oled, 2, "Initialising...");
        ssd1306_flush(&s_oled);
    } else {
        ESP_LOGW(TAG, "OLED init skipped: %s", esp_err_to_name(oled_ret));
    }

    /* ── Button ───────────────────────────────────────────────────────────── */
    s_btn_sem = xSemaphoreCreateBinary();
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_GPIO, btn_isr, NULL));

    /* ── FEM (Front-End Module) power-up ─────────────────────────────────────
     * Heltec V4 uses a GC1109 (V4.2) or KCT8103L (V4.3) FEM with an integrated
     * PA and LNA.  Without these two GPIOs held HIGH the FEM is disabled and
     * the board runs with the SX1262 internal LNA only — significantly worse RX.
     *  GPIO7 (FEM_VCC) – LDO that powers the FEM chip
     *  GPIO2 (FEM_CSD) – chip-enable, active HIGH
     * TXEN (GPIO46 V4.2 / GPIO5 V4.3) is driven by the ra01s driver to switch
     * the FEM between PA mode (TX) and LNA mode (RX). */
    gpio_config_t fem = {
        .pin_bit_mask = (1ULL << CONFIG_FEM_VCC_GPIO) | (1ULL << CONFIG_FEM_CSD_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&fem));
    gpio_set_level(CONFIG_FEM_VCC_GPIO, 1);
    gpio_set_level(CONFIG_FEM_CSD_GPIO, 1);
    ESP_LOGI(TAG, "FEM enabled: VCC=GPIO%d CSD=GPIO%d TXEN=GPIO%d",
             CONFIG_FEM_VCC_GPIO, CONFIG_FEM_CSD_GPIO, CONFIG_TXEN_GPIO);

    /* ── SX1262 ──────────────────────────────────────────────────────────── */
    LoRaInit();
    if (LoRaBegin(LORA_FREQ_HZ, (int8_t)LORA_TX_POWER,
                  LORA_TCXO_V, LORA_USE_LDO) != 0)
    {
        ESP_LOGE(TAG, "SX1262 init FAILED – check wiring!");
        if (s_oled_ready) {
            ssd1306_printf(&s_oled, 2, "SX1262 FAIL :(");
            ssd1306_flush(&s_oled);
        }
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* DIO2 drives the RF switch (PA/LNA) automatically */
    SetDio2AsRfSwitchCtrl(1);

    LoRaConfig(LORA_SF, LORA_BW, LORA_CR,
               LORA_PREAMBLE, 0, LORA_CRC_ON, false);

    ESP_LOGI(TAG, "SX1262 ready: %luHz SF%d BW-idx%d CR4/%d %ddBm",
             LORA_FREQ_HZ, LORA_SF, LORA_BW, LORA_CR + 4, LORA_TX_POWER);

    if (s_oled_ready) {
        ssd1306_printf(&s_oled, 2, "SX1262 OK – ready");
        ssd1306_flush(&s_oled);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    /* ── Tasks ────────────────────────────────────────────────────────────── */
    activity_touch(); // start the display sleep timer from boot
    if (s_oled_ready) {
        xTaskCreate(display_task, "display", 3072, NULL, 3, NULL);
    }
    xTaskCreate(lora_task,    "lora",    4096, NULL, 5, NULL);
}
