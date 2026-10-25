// Dust bridge: any ESP32 on the wall's channel, plugged into the Jetson by USB.
//
// It knows nothing about Dust. Every ESP-NOW packet it hears goes up the link to the Jetson
// (with who sent it and how strong it was); every packet the Jetson sends down goes out over
// ESP-NOW, broadcast or to one tile. All the protocol lives in the tiles and in jetson/, so this
// never needs reflashing when the protocol changes. Frames on the link: link_frame.h.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "link_frame.h"
#if CONFIG_BRIDGE_LINK_USB_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#endif

#define BRIDGE_VERSION 1
#define MAX_PEERS      16      // unicast peers kept (tiles the Jetson streams audio to)

static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static QueueHandle_t s_up;   // heard over the air, waiting to go up the link
static uint8_t  s_channel = CONFIG_BRIDGE_CHANNEL, s_mac[6];
static volatile uint32_t s_heard, s_sent, s_failed, s_dropped;
static uint8_t  s_peers[MAX_PEERS][6]; static int64_t s_peer_used[MAX_PEERS];

/* ---------- the link ---------- */
static void link_init(void)
{
#if CONFIG_BRIDGE_LINK_USB_JTAG
    usb_serial_jtag_driver_config_t cfg = { .rx_buffer_size = 4096, .tx_buffer_size = 8192 };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
#else
    uart_config_t cfg = { .baud_rate = CONFIG_BRIDGE_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                          .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 8192, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
#endif
}
static int link_read(uint8_t *buf, int cap)
{
#if CONFIG_BRIDGE_LINK_USB_JTAG
    return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(20));
#else
    return uart_read_bytes(UART_NUM_0, buf, cap, pdMS_TO_TICKS(20));
#endif
}
static void link_write(const uint8_t *buf, int n)
{
#if CONFIG_BRIDGE_LINK_USB_JTAG
    usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(50));
#else
    uart_write_bytes(UART_NUM_0, buf, n);
#endif
}

/* ---------- radio ---------- */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > LF_MAX_DATA) return;
    lf_msg_t m = { .kind = LF_RX, .rssi = (int8_t)info->rx_ctrl->rssi, .len = (uint8_t)len };
    memcpy(m.mac, info->src_addr, 6);
    memcpy(m.data, data, (size_t)len);
    s_heard++;
    if (xQueueSend(s_up, &m, 0) != pdTRUE) s_dropped++;
}

// Unicast needs the destination as a peer; keep the most recently used ones.
static void ensure_peer(const uint8_t mac[6])
{
    int64_t t = esp_timer_get_time();
    int oldest = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!memcmp(s_peers[i], mac, 6)) { s_peer_used[i] = t; return; }
        if (s_peer_used[i] < s_peer_used[oldest]) oldest = i;
    }
    if (s_peer_used[oldest]) esp_now_del_peer(s_peers[oldest]);
    esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(p.peer_addr, mac, 6);
    if (esp_now_add_peer(&p) == ESP_OK) { memcpy(s_peers[oldest], mac, 6); s_peer_used[oldest] = t; }
}

static void send_air(const lf_msg_t *m)
{
    if (memcmp(m->mac, BCAST, 6)) ensure_peer(m->mac);
    if (esp_now_send(m->mac, m->data, m->len) == ESP_OK) s_sent++; else s_failed++;
}

/* ---------- tasks ---------- */
static void up_task(void *arg)   // air → Jetson, plus a status frame every second
{
    (void)arg;
    esp_task_wdt_add(NULL);
    static uint8_t enc[LF_MAX_ENC];
    int64_t last_status = 0;
    for (;;) {
        lf_msg_t m;
        if (xQueueReceive(s_up, &m, pdMS_TO_TICKS(100)) == pdTRUE) link_write(enc, lf_encode(&m, enc));
        esp_task_wdt_reset();
        int64_t t = esp_timer_get_time();
        if (t - last_status > 1000000) {
            last_status = t;
            lf_status_t st = { BRIDGE_VERSION, s_channel, { 0 }, s_heard, s_sent, s_failed, s_dropped };
            memcpy(st.mac, s_mac, 6);
            lf_msg_t sm = { .kind = LF_STATUS, .len = sizeof st };
            memcpy(sm.data, &st, sizeof st);
            link_write(enc, lf_encode(&sm, enc));
        }
    }
}

static void down_task(void *arg)   // Jetson → air
{
    (void)arg;
    esp_task_wdt_add(NULL);
    static lf_reader_t rd;
    uint8_t buf[256];
    for (;;) {
        int n = link_read(buf, sizeof buf);
        esp_task_wdt_reset();
        for (int i = 0; i < n; i++) {
            lf_msg_t m;
            if (lf_feed(&rd, buf[i], &m) != 1) continue;
            if (m.kind == LF_TX && m.len > 0) send_air(&m);
            else if (m.kind == LF_CHANNEL && m.len == 1 && m.data[0] >= 1 && m.data[0] <= 13) {
                s_channel = m.data[0];
                esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
            }
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);   // the link carries frames, not text
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    link_init();
    s_up = xQueueCreate(64, sizeof(lf_msg_t));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE));
    esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(peer.peer_addr, BCAST, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    xTaskCreate(up_task, "up", 4096, NULL, 6, NULL);
    xTaskCreate(down_task, "down", 4096, NULL, 6, NULL);
}
