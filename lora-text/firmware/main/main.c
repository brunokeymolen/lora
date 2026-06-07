/**
 * LoRa Text Messenger — Heltec WiFi LoRa 32 V4.3 (ESP32-S3 + SX1262)
 *
 * Each device acts as a BLE GATT server using the Nordic UART Service (NUS)
 * profile, which is supported by nRF Connect and many BLE UART apps out of
 * the box, and by the companion Flutter app in lora-text/app/.
 *
 * Message flow
 * ────────────
 *  Phone → BLE write (NUS TX char) → lora_task TX queue
 *       → AES-256-GCM encrypt → LoRa TX packet
 *
 *  LoRa RX packet → AES-256-GCM decrypt + auth verify
 *       → BLE notify (NUS RX char) → phone
 *       → OLED display update
 *
 * BLE service (Nordic UART Service)
 * ──────────────────────────────────
 *  Service UUID  : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *  TX char (W)   : 6E400002-… — phone writes raw UTF-8 text (max 180 bytes)
 *  RX char (N)   : 6E400003-… — ESP32 sends JSON notification:
 *      {"from":"AABBCCDDEEFF","text":"hello","rssi":-95,"snr":5}
 *
 * LoRa wire packet (AES-256-GCM encrypted)
 * ─────────────────────────────────────────
 *  [version:1][type:1][src_mac:6][dst_mac:6][msg_id:2][iv:12][tag:16][ct:N]
 *   Fixed header = 44 bytes.  N ≤ 180 bytes.  Total ≤ 224 bytes.
 *
 * Encryption
 * ──────────
 *  Key  : SHA-256(CONFIG_LORA_PSK) → 32-byte AES key
 *  Mode : AES-256-GCM (authenticated; wrong PSK → auth tag mismatch → drop)
 *  Nonce: 12 random bytes per message (in packet header)
 *  AAD  : src_mac(6) + msg_id(2) — covers the cleartext header for integrity
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* mbedTLS */
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"

#include "ra01s.h"
#include "ssd1306.h"

_Static_assert(sizeof(CONFIG_LORA_PSK) > 1,
               "CONFIG_LORA_PSK must be set in local sdkconfig or menuconfig");

static const char *TAG = "LORA_TEXT";

/* ── Board ──────────────────────────────────────────────────────────────── */
#define BTN_GPIO        CONFIG_BUTTON_GPIO

/* ── LoRa parameters ────────────────────────────────────────────────────── */
#define LORA_FREQ_HZ    868000000UL
#define LORA_TX_POWER   CONFIG_TX_POWER_DBM
#define LORA_TCXO_V     1.8f
#define LORA_USE_LDO    false
#define LORA_SF         CONFIG_SF_RATE
#define LORA_BW         CONFIG_BANDWIDTH
#define LORA_CR         CONFIG_CODING_RATE
#define LORA_PREAMBLE   16
#define LORA_CRC_ON     true

/* ── Packet protocol ────────────────────────────────────────────────────── */
#define PKT_VERSION     0x01
#define PKT_TYPE_TEXT   0x01
#define MAC_LEN         6
#define MAX_TEXT_LEN    180  /* max UTF-8 bytes per message */

/* Fixed header: 1+1+6+6+2+12+16 = 44 bytes */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;
    uint8_t  src_mac[MAC_LEN];
    uint8_t  dst_mac[MAC_LEN];
    uint16_t msg_id;        /* big-endian */
    uint8_t  iv[12];        /* AES-GCM nonce, random per message */
    uint8_t  tag[16];       /* AES-GCM authentication tag */
    /* ciphertext follows immediately */
} lora_pkt_hdr_t;

#define LORA_HDR_SIZE   ((size_t)sizeof(lora_pkt_hdr_t))   /* 44 */
#define LORA_MAX_PKT    (LORA_HDR_SIZE + MAX_TEXT_LEN)      /* 224 */

static const uint8_t BCAST_MAC[MAC_LEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── TX queue (BLE → LoRa) ──────────────────────────────────────────────── */
typedef struct {
    uint8_t text[MAX_TEXT_LEN + 1];
    uint8_t len;
} text_msg_t;

static QueueHandle_t s_tx_queue;

/* ── Crypto ─────────────────────────────────────────────────────────────── */
static uint8_t s_aes_key[32]; /* SHA-256(CONFIG_LORA_PSK) */

/* ── Device identity ────────────────────────────────────────────────────── */
static uint8_t s_my_mac[MAC_LEN]; /* base MAC */

/* ── BLE state ──────────────────────────────────────────────────────────── */
static uint8_t  s_ble_addr_type;
static uint16_t s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_nus_rx_handle;   /* RX notify characteristic handle */

/* ── Statistics ─────────────────────────────────────────────────────────── */
static volatile uint32_t s_tx_count = 0;
static volatile uint32_t s_rx_count = 0;
static volatile uint16_t s_msg_id   = 0;

/* ── OLED ───────────────────────────────────────────────────────────────── */
static ssd1306_t s_oled;
static bool      s_oled_ready = false;
static bool      s_display_on = true;

#define DISPLAY_SLEEP_US  ((int64_t)CONFIG_DISPLAY_SLEEP_S * 1000000LL)
static volatile int64_t s_last_activity_us = 0;

static void activity_touch(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* ── Display state (written from lora_task, read from display_task) ─────── */
static volatile struct {
    bool  ble_connected;
    char  from[13];                  /* last sender: 12 hex chars + NUL */
    char  text[MAX_TEXT_LEN + 1];    /* last received plaintext */
    int8_t rssi;
    int8_t snr;
    char  status[32];
} g;

/* ── Button ─────────────────────────────────────────────────────────────── */
static SemaphoreHandle_t s_btn_sem;

static void IRAM_ATTR btn_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_btn_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Crypto helpers
 * ════════════════════════════════════════════════════════════════════════════ */

static void crypto_init(void)
{
    mbedtls_sha256((const unsigned char *)CONFIG_LORA_PSK,
                   strlen(CONFIG_LORA_PSK), s_aes_key, 0 /* SHA-256 */);
    ESP_LOGI(TAG, "AES-256 key derived from PSK (first byte: %02X)", s_aes_key[0]);
}

/**
 * Encrypt plaintext with AES-256-GCM.
 * Fills iv_out[12] with random bytes.  tag_out[16] receives auth tag.
 * ct_out must be at least pt_len bytes.
 * Returns 0 on success, -1 on mbedtls error.
 */
static int lora_encrypt(const uint8_t *pt, size_t pt_len,
                        const uint8_t *src_mac, uint16_t msg_id,
                        uint8_t *iv_out, uint8_t *tag_out, uint8_t *ct_out)
{
    esp_fill_random(iv_out, 12);

    /* AAD covers the cleartext header fields for integrity */
    uint8_t aad[8];
    memcpy(aad, src_mac, 6);
    aad[6] = (uint8_t)(msg_id >> 8);
    aad[7] = (uint8_t)(msg_id & 0xFF);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_aes_key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                        pt_len, iv_out, 12,
                                        aad, sizeof(aad),
                                        pt, ct_out,
                                        16, tag_out);
    }
    mbedtls_gcm_free(&gcm);
    return (rc == 0) ? 0 : -1;
}

/**
 * Decrypt ciphertext with AES-256-GCM and verify auth tag.
 * Returns 0 on success (auth OK), -1 on auth failure (wrong PSK or corrupted).
 * pt_out must be at least ct_len bytes.
 */
static int lora_decrypt(const uint8_t *ct, size_t ct_len,
                        const uint8_t *src_mac, uint16_t msg_id,
                        const uint8_t *iv, const uint8_t *tag,
                        uint8_t *pt_out)
{
    uint8_t aad[8];
    memcpy(aad, src_mac, 6);
    aad[6] = (uint8_t)(msg_id >> 8);
    aad[7] = (uint8_t)(msg_id & 0xFF);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_aes_key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&gcm, ct_len,
                                       iv, 12,
                                       aad, sizeof(aad),
                                       tag, 16,
                                       ct, pt_out);
    }
    mbedtls_gcm_free(&gcm);
    return (rc == 0) ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * BLE — Nordic UART Service (NUS)
 *
 * Using NUS UUIDs means the device works immediately with nRF Connect and
 * other BLE UART apps, in addition to the dedicated Flutter app.
 * ════════════════════════════════════════════════════════════════════════════ */

/* UUID bytes are in little-endian (reversed from standard notation) */
/* 6E400001-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E);
/* 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (phone → ESP32, Write) */
static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E);
/* 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (ESP32 → phone, Notify) */
static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E);

/* Called when phone writes to TX characteristic (phone → ESP32) */
static int nus_tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > MAX_TEXT_LEN) {
        ESP_LOGW(TAG, "BLE write: bad length %u", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    text_msg_t msg = {0};
    msg.len = (uint8_t)len;
    ble_hs_mbuf_to_flat(ctxt->om, msg.text, len, NULL);
    msg.text[len] = '\0';

    if (xQueueSend(s_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, dropping message");
    }
    return 0;
}

/* Called when phone reads RX characteristic — we use notify, so just return 0 */
static int nus_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* NUS TX: phone writes text to this characteristic */
                .uuid      = &nus_tx_uuid.u,
                .access_cb = nus_tx_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* NUS RX: ESP32 sends JSON notifications to phone */
                .uuid       = &nus_rx_uuid.u,
                .access_cb  = nus_rx_access_cb,
                .val_handle = &s_nus_rx_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 } /* terminator */
        },
    },
    { 0 } /* terminator */
};

/* Send a string notification to the connected phone */
static void ble_notify_str(const char *str)
{
    if (s_ble_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(str, strlen(str));
    if (om == NULL) return;
    int rc = ble_gatts_notify_custom(s_ble_conn_handle, s_nus_rx_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "BLE notify failed: %d", rc);
    }
}

/* Escape " and \ for JSON string values */
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 3 < dst_size; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* ── BLE GAP / advertising ──────────────────────────────────────────────── */

static void ble_advertise(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble_conn_handle = event->connect.conn_handle;
            g.ble_connected   = true;
            snprintf((char *)g.status, sizeof(g.status), "BLE connected");
            activity_touch();
            ESP_LOGI(TAG, "BLE connected, handle=%u", s_ble_conn_handle);
        } else {
            g.ble_connected = false;
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g.ble_connected   = false;
        snprintf((char *)g.status, sizeof(g.status), "BLE disconnected");
        activity_touch();
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        ble_advertise();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: conn=%u mtu=%u",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void ble_advertise(void)
{
    int rc;

    /* Primary adv packet: flags + NUS service UUID (needed for iOS/Android scan filter) */
    struct ble_hs_adv_fields fields = {0};
    fields.flags                   = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128                = &nus_svc_uuid;
    fields.num_uuids128            = 1;
    fields.uuids128_is_complete    = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    /* Scan response: full device name (LoRaText-XXXXXX) */
    struct ble_hs_adv_fields rsp = {0};
    const char *name             = ble_svc_gap_device_name();
    rsp.name                     = (uint8_t *)name;
    rsp.name_len                 = (uint8_t)strlen(name);
    rsp.name_is_complete         = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv = {0};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER,
                           &adv, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising as '%s'", name);
}

static void ble_on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_ble_addr_type);
    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run(); /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ════════════════════════════════════════════════════════════════════════════
 * Display
 * ════════════════════════════════════════════════════════════════════════════ */

static void display_update(void)
{
    if (!s_oled_ready || !s_display_on) return;

    /* Line 0: own MAC (short) + TX power */
    ssd1306_printf(&s_oled, 0, "%02X%02X%02X %ddBm SF%d",
                   s_my_mac[3], s_my_mac[4], s_my_mac[5],
                   LORA_TX_POWER, LORA_SF);

    /* Line 1: BLE status + TX/RX counters */
    ssd1306_printf(&s_oled, 1, "BLE:%-4s TX:%-3u RX:%-3u",
                   g.ble_connected ? "CONN" : "ADV",
                   (unsigned)s_tx_count, (unsigned)s_rx_count);

    /* Line 2: last sender */
    if (g.from[0]) {
        /* Show last 6 hex chars of MAC (3 bytes) for brevity */
        ssd1306_printf(&s_oled, 2, "From: %s", g.from + 6);
    } else {
        ssd1306_printf(&s_oled, 2, "From: ---");
    }

    /* Lines 3-5: last received message (wrap at 21 chars per line) */
    char txt_copy[MAX_TEXT_LEN + 1];
    memcpy(txt_copy, (const void *)g.text, MAX_TEXT_LEN + 1);
    size_t tlen = strnlen(txt_copy, MAX_TEXT_LEN);
    for (int l = 0; l < 3; l++) {
        size_t off = (size_t)l * 21;
        if (off < tlen) {
            size_t clen = tlen - off;
            if (clen > 21) clen = 21;
            ssd1306_printf(&s_oled, 3 + l, "%.*s", (int)clen, txt_copy + off);
        } else {
            ssd1306_printf(&s_oled, 3 + l, "");
        }
    }

    /* Line 6: last RSSI/SNR */
    ssd1306_printf(&s_oled, 6, "rssi:%-4d snr:%-4d", g.rssi, g.snr);

    /* Line 7: status */
    ssd1306_printf(&s_oled, 7, "%.21s", g.status);

    ssd1306_flush(&s_oled);
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_oled_ready) {
            int64_t idle = esp_timer_get_time() - s_last_activity_us;
            if (s_display_on && idle > DISPLAY_SLEEP_US) {
                s_display_on = false;
                ssd1306_power(&s_oled, false);
                ESP_LOGI(TAG, "Display off (idle %llds)",
                         (long long)(idle / 1000000));
            }
        }
        display_update();
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Main LoRa task
 * ════════════════════════════════════════════════════════════════════════════ */

static void lora_task(void *arg)
{
    (void)arg;
    uint8_t rx_buf[LORA_MAX_PKT + 8];

    ESP_LOGI(TAG, "LoRa task started");
    snprintf((char *)g.status, sizeof(g.status), "Waiting...");

    for (;;) {

        /* ── Button: wake display only ────────────────────────────────── */
        if (xSemaphoreTake(s_btn_sem, 0) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            while (xSemaphoreTake(s_btn_sem, 0) == pdTRUE); /* drain bounces */

            if (!s_display_on) {
                s_display_on = true;
                ssd1306_power(&s_oled, true);
                ssd1306_flush(&s_oled);
                activity_touch();
                ESP_LOGI(TAG, "Display woken by button");
            }
        }

        /* ── TX: pick up message from BLE queue and send over LoRa ─── */
        text_msg_t msg;
        if (xQueueReceive(s_tx_queue, &msg, 0) == pdTRUE) {
            uint16_t mid = ++s_msg_id;
            uint8_t  iv[12], tag[16], ct[MAX_TEXT_LEN];

            if (lora_encrypt(msg.text, msg.len,
                             s_my_mac, mid, iv, tag, ct) != 0) {
                ESP_LOGE(TAG, "Encrypt failed");
                goto rx_poll;
            }

            /* Assemble packet */
            uint8_t pkt[LORA_MAX_PKT];
            lora_pkt_hdr_t *hdr = (lora_pkt_hdr_t *)pkt;
            hdr->version = PKT_VERSION;
            hdr->type    = PKT_TYPE_TEXT;
            memcpy(hdr->src_mac, s_my_mac,   MAC_LEN);
            memcpy(hdr->dst_mac, BCAST_MAC,  MAC_LEN);
            hdr->msg_id  = __builtin_bswap16(mid);
            memcpy(hdr->iv,  iv,  12);
            memcpy(hdr->tag, tag, 16);
            memcpy(pkt + LORA_HDR_SIZE, ct, msg.len);
            size_t pkt_len = LORA_HDR_SIZE + msg.len;

            snprintf((char *)g.status, sizeof(g.status), "Sending #%u...", mid);
            display_update();

            if (LoRaSend(pkt, (uint8_t)pkt_len, SX126x_TXMODE_SYNC)) {
                s_tx_count++;
                snprintf((char *)g.status, sizeof(g.status), "Sent #%u OK", mid);
                ESP_LOGI(TAG, "Sent msg #%u (%u bytes encrypted)", mid,
                         (unsigned)pkt_len);
            } else {
                snprintf((char *)g.status, sizeof(g.status), "TX FAILED");
                ESP_LOGE(TAG, "LoRa TX failed");
            }
            activity_touch();
        }

rx_poll:
        /* ── RX: poll for incoming LoRa packet ───────────────────────── */
        {
            uint8_t rxlen = LoRaReceive(rx_buf, sizeof(rx_buf));
            if (rxlen < (uint8_t)(LORA_HDR_SIZE + 1)) goto next_iter;

            lora_pkt_hdr_t *hdr = (lora_pkt_hdr_t *)rx_buf;

            if (hdr->version != PKT_VERSION || hdr->type != PKT_TYPE_TEXT) {
                goto next_iter;
            }

            /* Drop our own re-transmissions (shouldn't happen on half-duplex) */
            if (memcmp(hdr->src_mac, s_my_mac, MAC_LEN) == 0) {
                goto next_iter;
            }

            size_t  ct_len = (size_t)rxlen - LORA_HDR_SIZE;
            uint16_t mid   = __builtin_bswap16(hdr->msg_id);
            uint8_t  plaintext[MAX_TEXT_LEN + 1];

            if (lora_decrypt(rx_buf + LORA_HDR_SIZE, ct_len,
                             hdr->src_mac, mid,
                             hdr->iv, hdr->tag,
                             plaintext) != 0)
            {
                ESP_LOGW(TAG, "Decrypt/auth failed — wrong PSK or corrupted");
                goto next_iter;
            }
            plaintext[ct_len] = '\0';

            int8_t rssi, snr;
            GetPacketStatus(&rssi, &snr);

            s_rx_count++;
            g.rssi = rssi;
            g.snr  = snr;
            snprintf((char *)g.from, sizeof(g.from),
                     "%02X%02X%02X%02X%02X%02X",
                     hdr->src_mac[0], hdr->src_mac[1], hdr->src_mac[2],
                     hdr->src_mac[3], hdr->src_mac[4], hdr->src_mac[5]);
            strncpy((char *)g.text, (char *)plaintext, MAX_TEXT_LEN);
            g.text[MAX_TEXT_LEN] = '\0';
            snprintf((char *)g.status, sizeof(g.status),
                     "#%u rssi:%d snr:%d", mid, rssi, snr);
            activity_touch();

            ESP_LOGI(TAG, "RX from %s: '%s' rssi=%d snr=%d",
                     (char *)g.from, (char *)plaintext, rssi, snr);

            /* Notify connected phone with JSON */
            if (s_ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                char escaped[MAX_TEXT_LEN * 2 + 2];
                json_escape((char *)plaintext, escaped, sizeof(escaped));
                char json[MAX_TEXT_LEN * 2 + 80];
                snprintf(json, sizeof(json),
                         "{\"from\":\"%s\",\"text\":\"%s\","
                         "\"rssi\":%d,\"snr\":%d}",
                         g.from, escaped, rssi, snr);
                ble_notify_str(json);
            }
        }

next_iter:
        vTaskDelay(1);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * app_main
 * ════════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    /* Create TX queue early so BLE callbacks can use it immediately */
    s_tx_queue = xQueueCreate(4, sizeof(text_msg_t));
    configASSERT(s_tx_queue);

    /* NVS flash (required by NimBLE) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Device identity */
    esp_base_mac_addr_get(s_my_mac);
    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_my_mac[0], s_my_mac[1], s_my_mac[2],
             s_my_mac[3], s_my_mac[4], s_my_mac[5]);

    /* Derive AES-256 key from PSK */
    crypto_init();

    /* ── OLED ─────────────────────────────────────────────────────────── */
    esp_err_t oled_ret = ssd1306_init(&s_oled,
                                       CONFIG_OLED_SDA_GPIO,
                                       CONFIG_OLED_SCL_GPIO,
                                       CONFIG_OLED_VEXT_GPIO,
                                       CONFIG_OLED_RST_GPIO);
    if (oled_ret == ESP_OK) {
        s_oled_ready = true;
        ssd1306_printf(&s_oled, 0, "LoRa Text Messenger");
        ssd1306_printf(&s_oled, 1, "868MHz SF%d %ddBm", LORA_SF, LORA_TX_POWER);
        ssd1306_printf(&s_oled, 2, "Initialising...");
        ssd1306_flush(&s_oled);
    } else {
        ESP_LOGW(TAG, "OLED init skipped: %s", esp_err_to_name(oled_ret));
    }

    /* ── Button ───────────────────────────────────────────────────────── */
    s_btn_sem = xSemaphoreCreateBinary();
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_GPIO, btn_isr, NULL));

    /* ── FEM (Front-End Module) power-up ─────────────────────────────────
     * GPIO7: LDO power to the KCT8103L FEM chip
     * GPIO2: CSD chip-enable (active HIGH) — without this the LNA is bypassed
     * GPIO5 (TXEN): driven by ra01s driver to select PA/LNA mode per packet */
    gpio_config_t fem_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_FEM_VCC_GPIO) |
                        (1ULL << CONFIG_FEM_CSD_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&fem_cfg));
    gpio_set_level(CONFIG_FEM_VCC_GPIO, 1);
    gpio_set_level(CONFIG_FEM_CSD_GPIO, 1);
    ESP_LOGI(TAG, "FEM enabled: VCC=GPIO%d CSD=GPIO%d TXEN=GPIO%d",
             CONFIG_FEM_VCC_GPIO, CONFIG_FEM_CSD_GPIO, CONFIG_TXEN_GPIO);

    /* ── SX1262 ──────────────────────────────────────────────────────── */
    LoRaInit();
    if (LoRaBegin(LORA_FREQ_HZ, (int8_t)LORA_TX_POWER,
                  LORA_TCXO_V, LORA_USE_LDO) != 0)
    {
        ESP_LOGE(TAG, "SX1262 init FAILED — check wiring!");
        if (s_oled_ready) {
            ssd1306_printf(&s_oled, 2, "SX1262 FAIL :(");
            ssd1306_flush(&s_oled);
        }
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    SetDio2AsRfSwitchCtrl(1);
    LoRaConfig(LORA_SF, LORA_BW, LORA_CR, LORA_PREAMBLE, 0, LORA_CRC_ON, false);
    ESP_LOGI(TAG, "SX1262 ready: %luHz SF%d BW-idx%d CR4/%d %ddBm",
             LORA_FREQ_HZ, LORA_SF, LORA_BW, LORA_CR + 4, LORA_TX_POWER);

    if (s_oled_ready) {
        ssd1306_printf(&s_oled, 2, "SX1262 OK");
        ssd1306_flush(&s_oled);
    }

    /* ── NimBLE ──────────────────────────────────────────────────────── */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;
    /* No bonding needed for this application */
    ble_hs_cfg.store_status_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Set device name before advertising starts */
    char dev_name[32];
    snprintf(dev_name, sizeof(dev_name), "LoRaText-%02X%02X%02X",
             s_my_mac[3], s_my_mac[4], s_my_mac[5]);
    ble_svc_gap_device_name_set(dev_name);

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    ESP_ERROR_CHECK(rc != 0 ? ESP_FAIL : ESP_OK);
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    ESP_ERROR_CHECK(rc != 0 ? ESP_FAIL : ESP_OK);

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "NimBLE started, device name: %s", dev_name);

    if (s_oled_ready) {
        ssd1306_printf(&s_oled, 3, "BLE: %s", dev_name + 9); /* skip "LoRaText-" */
        ssd1306_printf(&s_oled, 4, "Connect phone now");
        ssd1306_flush(&s_oled);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* ── Start tasks ─────────────────────────────────────────────────── */
    activity_touch();
    if (s_oled_ready) {
        xTaskCreate(display_task, "display", 3072, NULL, 3, NULL);
    }
    xTaskCreate(lora_task, "lora", 8192, NULL, 5, NULL);
}
