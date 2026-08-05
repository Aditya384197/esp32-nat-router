#ifndef ROUTER_CONFIG_H
#define ROUTER_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#define DEFAULT_AP_SSID     "ESP32_Router"
#define DEFAULT_AP_PASS     "12345678"
#define DEFAULT_AP_IP       "192.168.4.1"
#define DEFAULT_AP_GW       "192.168.4.1"
#define DEFAULT_AP_NETMASK  "255.255.255.0"
#define DEFAULT_AP_CHANNEL  6
#define DEFAULT_AP_MAX_CONN 5
#define DEFAULT_ADMIN_PASS  "admin"

typedef struct {
    atomic_char sta_ssid[33];
    atomic_char sta_pass[65];
    atomic_bool sta_connected;
    atomic_int  sta_rssi;
    
    atomic_char ap_ssid[33];
    atomic_char ap_pass[65];
    atomic_char ap_ip[16];
    atomic_int  ap_channel;
    atomic_int  sta_clients;
    
    atomic_char admin_pass[33];
    
    atomic_bool nat_enabled;
    atomic_uint uptime_sec;
} router_config_t;

extern router_config_t g_router;

void nvs_config_load(void);
void nvs_config_save(void);
void nvs_config_reset_factory(void);

// 🔥 FIXED: अब atomic_char* का उपयोग
static inline void atomic_str_get(atomic_char *src, char *dst, size_t len) {
    for (size_t i = 0; i < len - 1; i++) {
        dst[i] = atomic_load(&src[i]);
        if (dst[i] == '\0') break;
    }
    dst[len-1] = '\0';
}

static inline void atomic_str_set(atomic_char *dst, const char *src, size_t len) {
    size_t i;
    for (i = 0; i < len - 1 && src[i]; i++) {
        atomic_store(&dst[i], src[i]);
    }
    for (; i < len; i++) {
        atomic_store(&dst[i], '\0');
    }
}

void html_escape(const char *src, char *dst, size_t dst_len);
void url_encode(const char *src, char *dst, size_t dst_len);

#endif
