#include "router_core.h"
#include "router_config.h"
#include "led_status.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/dhcps.h"          // 🔥 यह Include जोड़ा गया है
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "ROUTER";

#define STA_CONNECTED_BIT   BIT0
#define STA_FAILED_BIT      BIT1
#define STA_GOT_IP_BIT      BIT2

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

static atomic_bool sta_connecting = false;

router_config_t g_router = {0};

// ---------- NVS हेल्पर ----------
static void nvs_get_str_atomic(nvs_handle_t nvs, const char *key, atomic_char *dst, size_t len) {
    char tmp[64] = {0};
    size_t sz = len;
    if (nvs_get_str(nvs, key, tmp, &sz) == ESP_OK) {
        atomic_str_set(dst, tmp, len);
    } else {
        atomic_str_set(dst, "", len);
    }
}

static void nvs_set_str_atomic(nvs_handle_t nvs, const char *key, atomic_char *src, size_t len) {
    char tmp[64] = {0};
    atomic_str_get(src, tmp, len);
    nvs_set_str(nvs, key, tmp);
}

// ---------- NVS फंक्शन्स ----------
void nvs_config_load(void) {
    atomic_str_set(g_router.sta_ssid, "", sizeof(g_router.sta_ssid));
    atomic_str_set(g_router.sta_pass, "", sizeof(g_router.sta_pass));
    atomic_str_set(g_router.ap_ssid, DEFAULT_AP_SSID, sizeof(g_router.ap_ssid));
    atomic_str_set(g_router.ap_pass, DEFAULT_AP_PASS, sizeof(g_router.ap_pass));
    atomic_str_set(g_router.ap_ip, DEFAULT_AP_IP, sizeof(g_router.ap_ip));
    atomic_str_set(g_router.admin_pass, DEFAULT_ADMIN_PASS, sizeof(g_router.admin_pass));
    atomic_store(&g_router.ap_channel, DEFAULT_AP_CHANNEL);
    atomic_store(&g_router.sta_connected, false);
    atomic_store(&g_router.nat_enabled, false);
    atomic_store(&g_router.sta_clients, 0);
    atomic_store(&g_router.sta_rssi, 0);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("router", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "NVS open failed: %s, using defaults", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "No saved config, using defaults");
        }
        return;
    }

    nvs_get_str_atomic(nvs, "sta_ssid", g_router.sta_ssid, sizeof(g_router.sta_ssid));
    nvs_get_str_atomic(nvs, "sta_pass", g_router.sta_pass, sizeof(g_router.sta_pass));
    nvs_get_str_atomic(nvs, "ap_ssid",  g_router.ap_ssid,  sizeof(g_router.ap_ssid));
    nvs_get_str_atomic(nvs, "ap_pass",  g_router.ap_pass,  sizeof(g_router.ap_pass));
    nvs_get_str_atomic(nvs, "ap_ip",    g_router.ap_ip,    sizeof(g_router.ap_ip));
    nvs_get_str_atomic(nvs, "admin_pass", g_router.admin_pass, sizeof(g_router.admin_pass));
    
    uint8_t ch;
    if (nvs_get_u8(nvs, "ap_ch", &ch) == ESP_OK)
        atomic_store(&g_router.ap_channel, (int)ch);
    
    nvs_close(nvs);
    ESP_LOGI(TAG, "Config loaded from NVS");
}

void nvs_config_save(void) {
    nvs_handle_t nvs;
    if (nvs_open("router", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    nvs_set_str_atomic(nvs, "sta_ssid", g_router.sta_ssid, sizeof(g_router.sta_ssid));
    nvs_set_str_atomic(nvs, "sta_pass", g_router.sta_pass, sizeof(g_router.sta_pass));
    nvs_set_str_atomic(nvs, "ap_ssid",  g_router.ap_ssid,  sizeof(g_router.ap_ssid));
    nvs_set_str_atomic(nvs, "ap_pass",  g_router.ap_pass,  sizeof(g_router.ap_pass));
    nvs_set_str_atomic(nvs, "ap_ip",    g_router.ap_ip,    sizeof(g_router.ap_ip));
    nvs_set_str_atomic(nvs, "admin_pass", g_router.admin_pass, sizeof(g_router.admin_pass));
    nvs_set_u8(nvs, "ap_ch", (uint8_t)atomic_load(&g_router.ap_channel));
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Config saved to NVS");
}

void nvs_config_reset_factory(void) {
    nvs_handle_t nvs;
    if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    atomic_str_set(g_router.sta_ssid, "", sizeof(g_router.sta_ssid));
    atomic_str_set(g_router.sta_pass, "", sizeof(g_router.sta_pass));
    atomic_str_set(g_router.ap_ssid, DEFAULT_AP_SSID, sizeof(g_router.ap_ssid));
    atomic_str_set(g_router.ap_pass, DEFAULT_AP_PASS, sizeof(g_router.ap_pass));
    atomic_str_set(g_router.ap_ip, DEFAULT_AP_IP, sizeof(g_router.ap_ip));
    atomic_str_set(g_router.admin_pass, DEFAULT_ADMIN_PASS, sizeof(g_router.admin_pass));
    atomic_store(&g_router.ap_channel, DEFAULT_AP_CHANNEL);
    ESP_LOGI(TAG, "Factory reset done");
}

// ---------- HTML/URL हेल्पर ----------
void html_escape(const char *src, char *dst, size_t dst_len) {
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 1) {
        switch (src[i]) {
            case '&':  if (j < dst_len - 5) { dst[j++]='&'; dst[j++]='a'; dst[j++]='m'; dst[j++]='p'; dst[j++]=';'; } break;
            case '<':  if (j < dst_len - 4) { dst[j++]='&'; dst[j++]='l'; dst[j++]='t'; dst[j++]=';'; } break;
            case '>':  if (j < dst_len - 4) { dst[j++]='&'; dst[j++]='g'; dst[j++]='t'; dst[j++]=';'; } break;
            case '"':  if (j < dst_len - 6) { dst[j++]='&'; dst[j++]='q'; dst[j++]='u'; dst[j++]='o'; dst[j++]='t'; dst[j++]=';'; } break;
            case '\'': if (j < dst_len - 5) { dst[j++]='&'; dst[j++]='#'; dst[j++]='3'; dst[j++]='9'; dst[j++]=';'; } break;
            default:   dst[j++] = src[i]; break;
        }
        i++;
    }
    dst[j] = '\0';
}

void url_encode(const char *src, char *dst, size_t dst_len) {
    const char *hex = "0123456789ABCDEF";
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 4) {
        unsigned char c = src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else {
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0xF];
            dst[j++] = hex[c & 0xF];
        }
        i++;
    }
    dst[j] = '\0';
}

// ---------- इवेंट हैंडलर ----------
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting...");
            atomic_store(&sta_connecting, true);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "STA disconnected (reason %d)", d->reason);
            atomic_store(&sta_connecting, false);
            atomic_store(&g_router.sta_connected, false);
            
            if (atomic_load(&g_router.nat_enabled)) {
                struct netif *ap_lwip = (struct netif *)esp_netif_get_netif_impl(s_ap_netif);
                if (ap_lwip) {
                    ip_napt_enable_netif(ap_lwip, 0);
                    atomic_store(&g_router.nat_enabled, false);
                    ESP_LOGI(TAG, "NAPT disabled");
                }
            }
            led_set(LED_BLINK_SLOW);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *d = (wifi_event_ap_staconnected_t *)data;
            atomic_fetch_add(&g_router.sta_clients, 1);
            ESP_LOGI(TAG, "Client joined: " MACSTR " (total %d)",
                     MAC2STR(d->mac), (int)atomic_load(&g_router.sta_clients));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *d = (wifi_event_ap_stadisconnected_t *)data;
            if (atomic_load(&g_router.sta_clients) > 0)
                atomic_fetch_sub(&g_router.sta_clients, 1);
            ESP_LOGI(TAG, "Client left: " MACSTR " (total %d)",
                     MAC2STR(d->mac), (int)atomic_load(&g_router.sta_clients));
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        atomic_store(&sta_connecting, false);
        atomic_store(&g_router.sta_connected, true);
        
        struct netif *ap_lwip = (struct netif *)esp_netif_get_netif_impl(s_ap_netif);
        if (ap_lwip) {
            ip_napt_enable_netif(ap_lwip, 1);
            atomic_store(&g_router.nat_enabled, true);
            ESP_LOGI(TAG, "NAPT enabled");
        }
        led_set(LED_ON_SOLID);
        xEventGroupSetBits(s_wifi_event_group, STA_GOT_IP_BIT);
    }
}

// ---------- AP नेटिफ सेटअप ----------
static void setup_ap_netif(void) {
    char ap_ip_buf[16];
    atomic_str_get(g_router.ap_ip, ap_ip_buf, sizeof(ap_ip_buf));

    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(ap_ip_buf, &ip_info.ip);
    esp_netif_str_to_ip4(DEFAULT_AP_GW,  &ip_info.gw);
    esp_netif_str_to_ip4(DEFAULT_AP_NETMASK, &ip_info.netmask);

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);

    uint32_t ip_addr = ip_info.ip.addr;
    uint32_t netmask = ip_info.netmask.addr;
    uint32_t subnet = ip_addr & netmask;
    uint32_t start = subnet | 2;
    uint32_t end   = subnet | 254;
    dhcps_lease_t lease;
    lease.enable = true;
    lease.start_ip.addr = start;
    lease.end_ip.addr   = end;
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DHCP_IP_ADDRESS, &lease, sizeof(lease));
    esp_netif_dhcps_start(s_ap_netif);
    ESP_LOGI(TAG, "AP DHCP range: " IPSTR " - " IPSTR, IP2STR(&lease.start_ip), IP2STR(&lease.end_ip));
}

// ---------- परफॉर्मेंस ट्यूनिंग ----------
static void apply_performance_tuning(void) {
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(80);
    esp_err_t err_ap = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
    esp_err_t err_sta = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    if (err_ap == ESP_OK && err_sta == ESP_OK)
        ESP_LOGI(TAG, "HT40 enabled on both interfaces");
    else {
        if (err_ap != ESP_OK) ESP_LOGW(TAG, "AP HT40 set failed (%d)", err_ap);
        if (err_sta != ESP_OK) ESP_LOGW(TAG, "STA HT40 set failed (%d)", err_sta);
    }
    ESP_LOGI(TAG, "Performance: PS_NONE, Tx=20dBm");
}

// ---------- STA रीकनेक्ट टास्क ----------
static void sta_reconnect_task(void *arg) {
    while (1) {
        char sta_ssid_buf[64];
        atomic_str_get(g_router.sta_ssid, sta_ssid_buf, sizeof(sta_ssid_buf));

        if (!atomic_load(&g_router.sta_connected) && 
            !atomic_load(&sta_connecting) && 
            strlen(sta_ssid_buf) > 0) {
            
            ESP_LOGI(TAG, "Reconnecting to STA...");
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            atomic_store(&sta_connecting, true);
            esp_wifi_connect();
            
            xEventGroupClearBits(s_wifi_event_group, STA_GOT_IP_BIT);
            EventBits_t bits = xEventGroupWaitBits(
                s_wifi_event_group,
                STA_GOT_IP_BIT,
                pdFALSE, pdFALSE,
                pdMS_TO_TICKS(15000));
            if (bits & STA_GOT_IP_BIT) {
                ESP_LOGI(TAG, "Reconnect successful");
            } else {
                ESP_LOGW(TAG, "Reconnect failed, will retry...");
                atomic_store(&sta_connecting, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// ---------- मुख्य इनिट ----------
esp_err_t router_core_init(void) {
    nvs_config_load();
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num  = 16;
    cfg.dynamic_rx_buf_num = 64;
    cfg.tx_buf_type        = 1;
    cfg.dynamic_tx_buf_num = 64;
    cfg.ampdu_rx_enable    = 1;
    cfg.ampdu_tx_enable    = 1;
    cfg.nvs_enable         = 0;
    cfg.rx_ba_win = 32;   // केवल rx_ba_win मौजूद है (tx_ba_win हटा दिया)
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // AP config
    wifi_config_t ap_cfg = {
        .ap = {
            .channel         = atomic_load(&g_router.ap_channel),
            .max_connection  = DEFAULT_AP_MAX_CONN,
            .pmf_cfg.required = false,
        }
    };
    atomic_str_get(g_router.ap_ssid, (char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid));
    atomic_str_get(g_router.ap_pass, (char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    if (strlen((char *)ap_cfg.ap.password) < 8) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ap_cfg.ap.password[0] = '\0';
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    // STA config
    wifi_config_t sta_cfg = { 0 };
    atomic_str_get(g_router.sta_ssid, (char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid));
    atomic_str_get(g_router.sta_pass, (char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.scan_method    = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method    = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.rssi = -127;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    apply_performance_tuning();
    setup_ap_netif();

    ESP_ERROR_CHECK(esp_wifi_start());
    char ap_ssid_buf[64], ap_ip_buf[16];
    atomic_str_get(g_router.ap_ssid, ap_ssid_buf, sizeof(ap_ssid_buf));
    atomic_str_get(g_router.ap_ip, ap_ip_buf, sizeof(ap_ip_buf));
    ESP_LOGI(TAG, "WiFi APSTA started");
    ESP_LOGI(TAG, "AP: SSID='%s' CH=%d", ap_ssid_buf, atomic_load(&g_router.ap_channel));

    char sta_ssid_buf[64];
    atomic_str_get(g_router.sta_ssid, sta_ssid_buf, sizeof(sta_ssid_buf));
    if (strlen(sta_ssid_buf) > 0) {
        ESP_LOGI(TAG, "Connecting to '%s'...", sta_ssid_buf);
        led_set(LED_BLINK_FAST);
        atomic_store(&sta_connecting, true);
        xEventGroupClearBits(s_wifi_event_group, STA_GOT_IP_BIT);
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            STA_GOT_IP_BIT,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(15000));
        if (bits & STA_GOT_IP_BIT) {
            ESP_LOGI(TAG, "STA connected ✅");
            led_set(LED_ON_SOLID);
        } else {
            ESP_LOGW(TAG, "STA connection failed — AP still running");
            atomic_store(&sta_connecting, false);
            led_set(LED_BLINK_SLOW);
        }
    } else {
        ESP_LOGW(TAG, "No STA SSID set — configure via web UI");
        led_set(LED_BLINK_SLOW);
    }

    xTaskCreatePinnedToCore(sta_reconnect_task, "sta_rec", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(router_stats_task, "stats", 4096, NULL, 5, NULL, 1);

    return ESP_OK;
}

void router_reconnect(void) {
    char sta_ssid_buf[64];
    atomic_str_get(g_router.sta_ssid, sta_ssid_buf, sizeof(sta_ssid_buf));
    if (strlen(sta_ssid_buf) > 0 && !atomic_load(&sta_connecting)) {
        atomic_store(&sta_connecting, true);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    }
}

void router_stats_task(void *arg) {
    while (1) {
        if (atomic_load(&g_router.sta_connected)) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                atomic_store(&g_router.sta_rssi, ap.rssi);
            }
        }
        atomic_fetch_add(&g_router.uptime_sec, 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
