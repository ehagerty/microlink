/**
 * @file main.c
 * @brief MicroLink v2 Cellular Connect Example
 *
 * Connects to Tailscale over 4G cellular using SIM7600 module.
 * No WiFi required — all traffic goes through the AT socket bridge,
 * which uses the SIM7600's internal TCP/IP stack (AT+NETOPEN/CIPOPEN).
 *
 * Hardware:
 *   Seeed Studio XIAO ESP32S3 + Waveshare SIM7600X 4G Module
 *   SIM7600 TXD  →  XIAO D7 (GPIO44)
 *   SIM7600 RXD  →  XIAO D6 (GPIO43)
 *   GND connected, separate USB-C power supplies
 *
 * Flow:
 *   1. Initialize SIM7600 modem (AT commands, SIM check, network registration)
 *   2. Connect cellular data (PPP primary, AT socket bridge fallback)
 *   3. Initialize MicroLink (Tailscale) over cellular
 *   4. Send/receive UDP messages over the VPN tunnel
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "microlink.h"
#include "microlink_internal.h"
#include "ml_cellular.h"
#include "ml_config_httpd.h"

static const char *TAG = "cell_main";

#define TAILSCALE_AUTH_KEY   CONFIG_ML_TAILSCALE_AUTH_KEY
#define MSG_PORT             9000
#define MSG_SEND_INTERVAL_MS 5000

/* MicroLink handle */
static microlink_t *ml = NULL;

/* UDP socket */
static microlink_udp_socket_t *udp_sock = NULL;

/* Message counters */
static uint32_t msg_tx_count = 0;
static uint32_t msg_rx_count = 0;

/* ============================================================================
 * UDP RX Callback
 * ========================================================================== */

static void on_udp_rx(microlink_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port,
                       const uint8_t *data, size_t len, void *user_data) {
    msg_rx_count++;
    char ip_str[16];
    microlink_ip_to_str(src_ip, ip_str);

    char msg[256];
    size_t copy_len = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';
    if (copy_len > 0 && msg[copy_len - 1] == '\n') msg[copy_len - 1] = '\0';

    ESP_LOGI(TAG, "UDP RX #%lu from %s:%u [%d bytes]: \"%s\"",
             (unsigned long)msg_rx_count, ip_str, src_port, (int)len, msg);

    /* Echo back */
    char reply[300];
    int reply_len = snprintf(reply, sizeof(reply), "ECHO(4G): %s", msg);
    if (reply_len > 0) {
        esp_err_t err = microlink_udp_send(sock, src_ip, src_port, reply, reply_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "UDP TX echo -> %s:%u", ip_str, src_port);
        }
    }
}

/* ============================================================================
 * Callbacks
 * ========================================================================== */

static void on_state_change(microlink_t *ml_handle, microlink_state_t state, void *user_data) {
    const char *state_names[] = {
        "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING",
        "CONNECTED", "RECONNECTING", "ERROR"
    };
    const char *name = (state < sizeof(state_names)/sizeof(state_names[0]))
                       ? state_names[state] : "UNKNOWN";
    ESP_LOGI(TAG, "MicroLink state: %s", name);

    if (state == ML_STATE_CONNECTED) {
        uint32_t ip = microlink_get_vpn_ip(ml_handle);
        char ip_str[16];
        microlink_ip_to_str(ip, ip_str);
        ESP_LOGI(TAG, "Connected! VPN IP: %s (via 4G cellular)", ip_str);
    }
}

static void on_peer_update(microlink_t *ml_handle, const microlink_peer_info_t *peer,
                             void *user_data) {
    char ip_str[16];
    microlink_ip_to_str(peer->vpn_ip, ip_str);
    ESP_LOGI(TAG, "Peer: %s (%s) online=%d direct=%d",
             peer->hostname, ip_str, peer->online, peer->direct_path);
}

/* ============================================================================
 * Main
 * ========================================================================== */

void app_main(void) {
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize esp_netif and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    printf("\n");
    printf("================================================================\n");
    printf("  MicroLink v2 — Cellular Connect (SIM7600 4G)\n");
    printf("  PPP primary, AT socket bridge fallback\n");
    printf("================================================================\n\n");

    ESP_LOGI(TAG, "Free heap: %lu bytes (PSRAM: %lu bytes)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ================================================================
     * Step 1: Initialize SIM7600 cellular modem
     * ================================================================ */

    ESP_LOGI(TAG, "=== Step 1: Initializing SIM7600 modem ===");

    ml_cellular_config_t cell_config = ML_CELLULAR_DEFAULT_CONFIG();
    cell_config.tx_pin = CONFIG_ML_CELLULAR_TX_PIN;
    cell_config.rx_pin = CONFIG_ML_CELLULAR_RX_PIN;

    /* Apply Kconfig settings */
    const char *sim_pin = CONFIG_ML_CELLULAR_SIM_PIN;
    if (sim_pin && sim_pin[0]) {
        cell_config.sim_pin = sim_pin;
    }
    const char *apn = CONFIG_ML_CELLULAR_APN;
    if (apn && apn[0]) {
        cell_config.apn = apn;
    }
    const char *ppp_user = CONFIG_ML_CELLULAR_PPP_USER;
    if (ppp_user && ppp_user[0]) { cell_config.ppp_user = ppp_user; }
    const char *ppp_pass = CONFIG_ML_CELLULAR_PPP_PASS;
    if (ppp_pass && ppp_pass[0]) { cell_config.ppp_pass = ppp_pass; }

    /* NVS overrides (web UI settings take precedence over Kconfig) */
    static char nvs_apn[32] = "";
    if (ml_config_get_nvs_apn(nvs_apn, sizeof(nvs_apn))) {
        cell_config.apn = nvs_apn;
    }
    static char nvs_ppp_user[32] = "", nvs_ppp_pass[32] = "";
    if (ml_config_get_nvs_ppp(nvs_ppp_user, sizeof(nvs_ppp_user),
                               nvs_ppp_pass, sizeof(nvs_ppp_pass))) {
        cell_config.ppp_user = nvs_ppp_user;
        cell_config.ppp_pass = nvs_ppp_pass;
    }

    ret = ml_cellular_init(&cell_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cellular init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Check: wiring, SIM card, antenna, power supply");
        return;
    }

    /* Print modem info */
    ml_cellular_info_t cell_info;
    ml_cellular_get_info(&cell_info);
    ESP_LOGI(TAG, "Model: %s  IMEI: %s", cell_info.model, cell_info.imei);
    ESP_LOGI(TAG, "ICCID: %s", cell_info.iccid);
    ESP_LOGI(TAG, "Operator: %s  Signal: %d (%d dBm)",
             cell_info.operator_name, cell_info.rssi, cell_info.rssi_dbm);

    /* ================================================================
     * Step 2: Connect cellular data (PPP primary, AT socket fallback)
     * Tries PPP first for low latency + real UDP sockets.
     * Falls back to AT socket bridge if CHAP auth fails.
     * ================================================================ */

    ESP_LOGI(TAG, "=== Step 2: Connecting cellular data ===");

    ret = ml_cellular_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cellular data connection failed: %s", esp_err_to_name(ret));
        return;
    }

    ml_cellular_data_mode_t data_mode = ml_cellular_get_data_mode();
    ESP_LOGI(TAG, "Cellular data active via %s",
             data_mode == ML_DATA_MODE_PPP ? "PPP (lwIP sockets)"
                                           : "AT socket bridge");

    /* ================================================================
     * Step 3: Initialize MicroLink (Tailscale)
     * ================================================================ */

    ESP_LOGI(TAG, "=== Step 3: Starting MicroLink (Tailscale) over 4G ===");

    /* Use IMEI-based device name, fall back to MAC, fall back to Kconfig */
    const char *device_name = microlink_imei_device_name();
    if (!device_name || device_name[0] == '\0') {
        const char *kconfig_name = CONFIG_ML_DEVICE_NAME;
        if (kconfig_name && kconfig_name[0]) {
            device_name = kconfig_name;
        } else {
            device_name = microlink_default_device_name();
        }
    }
    ESP_LOGI(TAG, "Device name: %s", device_name);

    microlink_config_t config = {
        .auth_key = TAILSCALE_AUTH_KEY,
        .device_name = device_name,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
    };

    ml = microlink_init(&config);
    if (!ml) {
        ESP_LOGE(TAG, "Failed to initialize MicroLink");
        return;
    }

    microlink_set_state_callback(ml, on_state_change, NULL);
    microlink_set_peer_callback(ml, on_peer_update, NULL);

    ESP_ERROR_CHECK(microlink_start(ml));

    /* Wait for CONNECTED state */
    ESP_LOGI(TAG, "Waiting for Tailscale connection...");
    while (!microlink_is_connected(ml)) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* ================================================================
     * Step 4: Create UDP socket for messaging
     * ================================================================ */

    udp_sock = microlink_udp_create(ml, MSG_PORT);
    if (!udp_sock) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
    } else {
        ESP_LOGI(TAG, "UDP socket listening on port %d", MSG_PORT);
        microlink_udp_set_rx_callback(udp_sock, on_udp_rx, NULL);
    }

    /* Parse target peer IP from Kconfig */
    uint32_t target_ip = 0;
    const char *target_ip_str = CONFIG_ML_EXAMPLE_TARGET_PEER_IP;
    if (target_ip_str && target_ip_str[0] != '\0') {
        target_ip = microlink_parse_ip(target_ip_str);
        if (target_ip != 0) {
            ESP_LOGI(TAG, "Will send messages to %s:%d every %dms",
                     target_ip_str, MSG_PORT, MSG_SEND_INTERVAL_MS);
        }
    } else {
        ESP_LOGI(TAG, "No target peer IP configured (receive-only mode)");
    }

    /* ================================================================
     * Main loop — send messages, print status
     * ================================================================ */

    uint64_t last_send_ms = 0;
    uint64_t last_status_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint64_t now = (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* Periodic send to target peer */
        if (udp_sock && target_ip != 0 && now - last_send_ms >= MSG_SEND_INTERVAL_MS) {
            last_send_ms = now;
            msg_tx_count++;
            char msg[128];
            int msg_len = snprintf(msg, sizeof(msg), "hello from ESP32-4G #%lu",
                                   (unsigned long)msg_tx_count);
            esp_err_t err = microlink_udp_send(udp_sock, target_ip, MSG_PORT, msg, msg_len);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "UDP TX #%lu -> %s:%d", (unsigned long)msg_tx_count,
                         target_ip_str, MSG_PORT);
            } else {
                ESP_LOGW(TAG, "UDP TX #%lu FAILED (err=%d)", (unsigned long)msg_tx_count, err);
            }
        }

        /* Periodic status (every 30s) */
        if (now - last_status_ms >= 30000) {
            last_status_ms = now;

            /* Refresh signal quality */
            ml_cellular_info_t info;
            ml_cellular_get_info(&info);

            if (microlink_is_connected(ml)) {
                int peer_count = microlink_get_peer_count(ml);
                ESP_LOGI(TAG, "Status: CONNECTED(4G) | Signal: %d dBm | Peers: %d | TX: %lu | RX: %lu | Heap: %lu",
                         info.rssi_dbm, peer_count,
                         (unsigned long)msg_tx_count, (unsigned long)msg_rx_count,
                         (unsigned long)esp_get_free_heap_size());

                for (int i = 0; i < peer_count; i++) {
                    microlink_peer_info_t pinfo;
                    if (microlink_get_peer_info(ml, i, &pinfo) == ESP_OK) {
                        char ip_str[16];
                        microlink_ip_to_str(pinfo.vpn_ip, ip_str);
                        ESP_LOGI(TAG, "  [%d] %s (%s) %s",
                                 i, pinfo.hostname, ip_str,
                                 pinfo.direct_path ? "DIRECT" : "DERP");
                    }
                }
            }
        }
    }
}
