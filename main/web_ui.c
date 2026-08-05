#include "web_ui.h"
#include "router_config.h"
#include "router_core.h"
#include "led_status.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "WEB";
static httpd_handle_t s_server = NULL;

// ---------- HTML ----------
static const char *HTML_HEAD =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Router</title>"
    "<style>"
    "body{font-family:monospace;background:#0a0a0a;color:#00ff88;margin:0;padding:16px}"
    "h1{color:#00ffcc;border-bottom:1px solid #00ff88;padding-bottom:8px}"
    "h2{color:#00ccff;margin-top:24px}"
    ".card{background:#111;border:1px solid #00ff88;border-radius:8px;padding:16px;margin:12px 0}"
    ".stat{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #1a1a1a}"
    ".val{color:#fff}"
    "input,select{background:#1a1a1a;color:#00ff88;border:1px solid #00ff88;padding:8px;width:100%;box-sizing:border-box;margin:4px 0;font-family:monospace;font-size:14px}"
    "button{background:#00ff88;color:#000;border:none;padding:10px 24px;cursor:pointer;font-weight:bold;font-family:monospace;font-size:14px;margin-top:8px;border-radius:4px}"
    "button:hover{background:#00ffcc}"
    ".warn{color:#ff8800}.ok{color:#00ff88}.err{color:#ff4444}"
    "a{color:#00ccff}nav{margin-bottom:16px}nav a{margin-right:16px}"
    "</style></head><body>"
    "<h1>📡 ESP32 NAT Router</h1>"
    "<nav>"
    "<a href='/'>Status</a>"
    "<a href='/config'>Configure</a>"
    "<a href='/scan'>WiFi Scan</a>"
    "<a href='/reconnect'>Reconnect</a>"
    "<a href='/factory'>Factory Reset</a>"
    "</nav>";

static const char *HTML_FOOT = "</body></html>";

// ---------- ऑथेंटिकेशन (Base64) ----------
static bool check_auth(httpd_req_t *req) {
    char auth_hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        char admin_pass_buf[32];
        atomic_str_get(g_router.admin_pass, admin_pass_buf, sizeof(admin_pass_buf));
        if (strcmp(admin_pass_buf, "admin") == 0) {
            return true;
        }
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Router\"");
        httpd_resp_send(req, "Authentication required", -1);
        return false;
    }

    const char *basic_prefix = "Basic ";
    if (strncasecmp(auth_hdr, basic_prefix, strlen(basic_prefix)) != 0) {
        httpd_resp_send(req, "Invalid auth scheme", -1);
        return false;
    }
    const char *b64 = auth_hdr + strlen(basic_prefix);

    unsigned char decoded[128];
    size_t decoded_len;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_len,
                                    (const unsigned char *)b64, strlen(b64));
    if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        httpd_resp_send(req, "Credentials too long", -1);
        return false;
    } else if (ret != 0) {
        httpd_resp_send(req, "Invalid credentials", -1);
        return false;
    }
    decoded[decoded_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        httpd_resp_send(req, "Invalid format", -1);
        return false;
    }
    *colon = '\0';
    char *username = (char *)decoded;
    char *password = colon + 1;

    char admin_pass_buf[32];
    atomic_str_get(g_router.admin_pass, admin_pass_buf, sizeof(admin_pass_buf));

    if (strcmp(username, "admin") == 0 &&
        strcmp(password, admin_pass_buf) == 0) {
        return true;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Router\"");
    httpd_resp_send(req, "Invalid credentials", -1);
    return false;
}

// ---------- URL डिकोड ----------
static void url_decode(char *dst, const char *src, size_t dstlen) {
    char a, b;
    size_t i = 0;
    while (*src && i < dstlen - 1) {
        if (*src == '%' && (a = src[1]) && (b = src[2]) &&
            isxdigit(a) && isxdigit(b)) {
            a = (a >= 'a') ? a - 'a' + 10 : (a >= 'A') ? a - 'A' + 10 : a - '0';
            b = (b >= 'a') ? b - 'a' + 10 : (b >= 'A') ? b - 'A' + 10 : b - '0';
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static char *get_param(const char *body, const char *key, char *out, size_t outlen) {
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return out; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= outlen) len = outlen - 1;
    char tmp[256] = {0};
    memcpy(tmp, p, len);
    url_decode(out, tmp, outlen);
    return out;
}

// ---------- स्टेटस हैंडलर ----------
static esp_err_t handle_status(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char buf[4096];
    char uptime[32];
    uint32_t u = atomic_load(&g_router.uptime_sec);
    snprintf(uptime, sizeof(uptime), "%02lu:%02lu:%02lu",
             u/3600, (u%3600)/60, u%60);

    const char *rssi_q = "N/A";
    if (atomic_load(&g_router.sta_connected)) {
        int r = atomic_load(&g_router.sta_rssi);
        rssi_q = r > -50 ? "Excellent" : r > -60 ? "Good" :
                 r > -70 ? "Fair" : r > -80 ? "Weak" : "Poor";
    }

    char sta_ssid_buf[64], ap_ssid_buf[64], ap_ip_buf[16];
    atomic_str_get(g_router.sta_ssid, sta_ssid_buf, sizeof(sta_ssid_buf));
    atomic_str_get(g_router.ap_ssid, ap_ssid_buf, sizeof(ap_ssid_buf));
    atomic_str_get(g_router.ap_ip, ap_ip_buf, sizeof(ap_ip_buf));
    char sta_ssid_esc[64], ap_ssid_esc[64];
    html_escape(sta_ssid_buf, sta_ssid_esc, sizeof(sta_ssid_esc));
    html_escape(ap_ssid_buf, ap_ssid_esc, sizeof(ap_ssid_esc));

    snprintf(buf, sizeof(buf),
        "%s"
        "<div class='card'>"
        "<h2>System Status</h2>"
        "<div class='stat'><span>Uptime</span><span class='val'>%s</span></div>"
        "<div class='stat'><span>Uplink (STA)</span><span class='%s'>%s</span></div>"
        "<div class='stat'><span>RSSI</span><span class='val'>%d dBm (%s)</span></div>"
        "<div class='stat'><span>NAT</span><span class='%s'>%s</span></div>"
        "<div class='stat'><span>Clients</span><span class='val'>%d</span></div>"
        "</div>"
        "<div class='card'>"
        "<h2>Network</h2>"
        "<div class='stat'><span>AP SSID</span><span class='val'>%s</span></div>"
        "<div class='stat'><span>AP IP</span><span class='val'>%s</span></div>"
        "<div class='stat'><span>AP Channel</span><span class='val'>%d</span></div>"
        "<div class='stat'><span>Uplink SSID</span><span class='val'>%s</span></div>"
        "</div>"
        "%s",
        HTML_HEAD, uptime,
        atomic_load(&g_router.sta_connected) ? "ok" : "err",
        atomic_load(&g_router.sta_connected) ? "CONNECTED ✅" : "DISCONNECTED ❌",
        atomic_load(&g_router.sta_rssi), rssi_q,
        atomic_load(&g_router.nat_enabled) ? "ok" : "warn",
        atomic_load(&g_router.nat_enabled) ? "ENABLED ✅" : "DISABLED ⚠️",
        atomic_load(&g_router.sta_clients),
        ap_ssid_esc, ap_ip_buf, atomic_load(&g_router.ap_channel),
        sta_ssid_esc,
        HTML_FOOT
    );
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

// ---------- कॉन्फ़िग GET (🔥 FIX #1: httpd_req_get_url_query_str) ----------
static esp_err_t handle_config_get(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char buf[4096];
    char prefill_ssid[64] = "";
    char query_buf[128];
    const char *query = NULL;
    
    // 🔥 FIX #1: सही API उपयोग करें
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        query = query_buf;
        char param[64];
        if (httpd_query_key_value(query, "ssid", param, sizeof(param)) == ESP_OK) {
            url_decode(prefill_ssid, param, sizeof(prefill_ssid));
        }
    }

    char sta_ssid_buf[64], ap_ssid_buf[64], ap_ip_buf[16];
    atomic_str_get(g_router.sta_ssid, sta_ssid_buf, sizeof(sta_ssid_buf));
    atomic_str_get(g_router.ap_ssid, ap_ssid_buf, sizeof(ap_ssid_buf));
    atomic_str_get(g_router.ap_ip, ap_ip_buf, sizeof(ap_ip_buf));
    char sta_ssid_esc[64], ap_ssid_esc[64], ap_ip_esc[20];
    html_escape(sta_ssid_buf, sta_ssid_esc, sizeof(sta_ssid_esc));
    html_escape(ap_ssid_buf, ap_ssid_esc, sizeof(ap_ssid_esc));
    html_escape(ap_ip_buf, ap_ip_esc, sizeof(ap_ip_esc));

    char ch_sel[13][12] = {0};
    for (int i = 1; i <= 13; i++) {
        if (atomic_load(&g_router.ap_channel) == i)
            strcpy(ch_sel[i-1], " selected");
    }

    char sta_ssid_val[64];
    if (strlen(prefill_ssid) > 0) {
        strncpy(sta_ssid_val, prefill_ssid, sizeof(sta_ssid_val));
    } else {
        strncpy(sta_ssid_val, sta_ssid_esc, sizeof(sta_ssid_val));
    }

    snprintf(buf, sizeof(buf),
        "%s"
        "<div class='card'>"
        "<h2>Configure Router</h2>"
        "<form method='POST' action='/config'>"
        "<h3>Uplink WiFi (Internet)</h3>"
        "<label>SSID<input name='sta_ssid' value='%s' maxlength='32'></label>"
        "<label>Password<input name='sta_pass' type='password' value='' "
               "placeholder='leave blank to keep current' maxlength='64'></label>"
        "<h3>Access Point (Your Network)</h3>"
        "<label>SSID<input name='ap_ssid' value='%s' maxlength='32'></label>"
        "<label>Password (min 8 chars for WPA2, blank=Open)"
        "<input name='ap_pass' type='password' value='' "
               "placeholder='leave blank to keep current' maxlength='64'></label>"
        "<label>Channel"
        "<select name='ap_channel'>"
        "<option value='1'%s>1</option><option value='2'%s>2</option>"
        "<option value='3'%s>3</option><option value='4'%s>4</option>"
        "<option value='5'%s>5</option><option value='6'%s>6</option>"
        "<option value='7'%s>7</option><option value='8'%s>8</option>"
        "<option value='9'%s>9</option><option value='10'%s>10</option>"
        "<option value='11'%s>11</option><option value='12'%s>12</option>"
        "<option value='13'%s>13</option>"
        "</select></label>"
        "<label>AP IP Address<input name='ap_ip' value='%s' maxlength='15'></label>"
        "<label>Admin Password (for web UI)<input name='admin_pass' type='password' value='' "
               "placeholder='leave blank to keep current' maxlength='32'></label>"
        "<button type='submit'>💾 Save &amp; Reboot</button>"
        "</form></div>"
        "%s",
        HTML_HEAD,
        sta_ssid_val,
        ap_ssid_esc,
        ch_sel[0], ch_sel[1], ch_sel[2], ch_sel[3], ch_sel[4],
        ch_sel[5], ch_sel[6], ch_sel[7], ch_sel[8], ch_sel[9],
        ch_sel[10], ch_sel[11], ch_sel[12],
        ap_ip_esc,
        HTML_FOOT
    );
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

// ---------- कॉन्फ़िग POST (🔥 FIX #2: sta_pass_tmp हटाया) ----------
static esp_err_t handle_config_post(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    char body[1024] = {0};
    size_t received = 0;
    while (received < req->content_len && received < sizeof(body) - 1) {
        int ret = httpd_req_recv(req, body + received, sizeof(body) - received - 1);
        if (ret <= 0) break;
        received += ret;
    }
    body[received] = '\0';

    char tmp[64];
    char sta_ssid_tmp[64];   // 🔥 FIX #2: sta_pass_tmp हटा दिया
    get_param(body, "sta_ssid", sta_ssid_tmp, sizeof(sta_ssid_tmp));
    atomic_str_set(g_router.sta_ssid, sta_ssid_tmp, sizeof(g_router.sta_ssid));
    get_param(body, "sta_pass", tmp, sizeof(tmp));
    if (strlen(tmp) > 0)
        atomic_str_set(g_router.sta_pass, tmp, sizeof(g_router.sta_pass));

    char ap_ssid_tmp[64], ap_pass_tmp[64], ap_ip_tmp[16];
    get_param(body, "ap_ssid", ap_ssid_tmp, sizeof(ap_ssid_tmp));
    if (strlen(ap_ssid_tmp) == 0)
        strcpy(ap_ssid_tmp, DEFAULT_AP_SSID);
    atomic_str_set(g_router.ap_ssid, ap_ssid_tmp, sizeof(g_router.ap_ssid));

    get_param(body, "ap_pass", ap_pass_tmp, sizeof(ap_pass_tmp));
    if (strlen(ap_pass_tmp) > 0)
        atomic_str_set(g_router.ap_pass, ap_pass_tmp, sizeof(g_router.ap_pass));

    get_param(body, "ap_channel", tmp, sizeof(tmp));
    int ch = atoi(tmp);
    if (ch >= 1 && ch <= 13) atomic_store(&g_router.ap_channel, ch);

    get_param(body, "ap_ip", ap_ip_tmp, sizeof(ap_ip_tmp));
    if (strlen(ap_ip_tmp) < 7)
        strcpy(ap_ip_tmp, DEFAULT_AP_IP);
    atomic_str_set(g_router.ap_ip, ap_ip_tmp, sizeof(g_router.ap_ip));

    char admin_tmp[32];
    get_param(body, "admin_pass", admin_tmp, sizeof(admin_tmp));
    if (strlen(admin_tmp) > 0)
        atomic_str_set(g_router.admin_pass, admin_tmp, sizeof(g_router.admin_pass));

    nvs_config_save();

    const char *resp =
        "<!DOCTYPE html><html><body style='background:#0a0a0a;color:#00ff88;"
        "font-family:monospace;padding:20px'>"
        "<h1>✅ Saved! Rebooting in 2s...</h1>"
        "<script>setTimeout(()=>location.href='/',4000)</script>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// ---------- स्कैन हैंडलर (🔥 FIX #3: row बफर 512) ----------
static esp_err_t handle_scan(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>📶 WiFi Networks</h2>"
        "<p class='warn'>Scanning...</p>");

    uint16_t ap_count = 0;
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 80,
                .max = 200
            }
        }
    };
    esp_wifi_scan_start(&scan_cfg, true);
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        httpd_resp_sendstr_chunk(req, "<p class='err'>No networks found</p>");
    } else {
        wifi_ap_record_t *list = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (list == NULL) {
            httpd_resp_sendstr_chunk(req, "<p class='err'>Memory error</p>");
        } else {
            esp_wifi_scan_get_ap_records(&ap_count, list);
            httpd_resp_sendstr_chunk(req,
                "<table style='width:100%;border-collapse:collapse'>"
                "<tr style='color:#00ccff'>"
                "<th align='left'>SSID</th><th>Ch</th><th>RSSI</th><th>Security</th><th>Select</th>"
                "</tr>");

            // 🔥 FIX #3: row बफर 256 → 512
            char row[512];
            for (int i = 0; i < ap_count; i++) {
                const char *auth = "OPEN";
                switch (list[i].authmode) {
                    case WIFI_AUTH_WEP:           auth = "WEP";    break;
                    case WIFI_AUTH_WPA_PSK:       auth = "WPA";    break;
                    case WIFI_AUTH_WPA2_PSK:      auth = "WPA2";   break;
                    case WIFI_AUTH_WPA_WPA2_PSK:  auth = "WPA/2";  break;
                    case WIFI_AUTH_WPA3_PSK:      auth = "WPA3";   break;
                    default: break;
                }
                int q = list[i].rssi > -50 ? 5 :
                        list[i].rssi > -60 ? 4 :
                        list[i].rssi > -70 ? 3 :
                        list[i].rssi > -80 ? 2 : 1;
                char bars[6] = {0};
                for (int b = 0; b < 5; b++) bars[b] = b < q ? '#' : '.';

                char ssid_esc[64];
                html_escape((char *)list[i].ssid, ssid_esc, sizeof(ssid_esc));
                char ssid_url[64];
                url_encode((char *)list[i].ssid, ssid_url, sizeof(ssid_url));

                snprintf(row, sizeof(row),
                    "<tr style='border-bottom:1px solid #222'>"
                    "<td>%s</td><td align='center'>%d</td>"
                    "<td align='center'>%d [%s]</td>"
                    "<td align='center'>%s</td>"
                    "<td align='center'>"
                    "<a href='/config?ssid=%s'>Use</a>"
                    "</td></tr>",
                    ssid_esc,
                    list[i].primary,
                    list[i].rssi, bars,
                    auth,
                    ssid_url
                );
                httpd_resp_sendstr_chunk(req, row);
            }
            httpd_resp_sendstr_chunk(req, "</table>");
            free(list);
        }
    }
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, HTML_FOOT);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ---------- रीकनेक्ट ----------
static esp_err_t handle_reconnect(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    router_reconnect();
    const char *resp =
        "<!DOCTYPE html><html><body style='background:#0a0a0a;color:#00ff88;"
        "font-family:monospace;padding:20px'>"
        "<h1>🔄 Reconnecting...</h1>"
        "<script>setTimeout(()=>location.href='/',3000)</script>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// ---------- फ़ैक्टरी रीसेट ----------
static esp_err_t handle_factory(httpd_req_t *req) {
    if (!check_auth(req)) return ESP_OK;
    nvs_config_reset_factory();
    const char *resp =
        "<!DOCTYPE html><html><body style='background:#0a0a0a;color:#00ff88;"
        "font-family:monospace;padding:20px'>"
        "<h1>🗑️ Factory Reset Done! Rebooting...</h1>"
        "<script>setTimeout(()=>location.href='/',5000)</script>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ---------- वेब सर्वर टास्क ----------
static void web_server_task(void *arg) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.stack_size      = 10240;
    config.task_priority   = 5;

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t uris[] = {
            { "/",        HTTP_GET,  handle_status,     NULL },
            { "/config",  HTTP_GET,  handle_config_get, NULL },
            { "/config",  HTTP_POST, handle_config_post,NULL },
            { "/scan",    HTTP_GET,  handle_scan,       NULL },
            { "/reconnect",HTTP_GET, handle_reconnect,  NULL },
            { "/factory", HTTP_GET,  handle_factory,    NULL },
        };
        for (int i = 0; i < 6; i++)
            httpd_register_uri_handler(s_server, &uris[i]);

        char ap_ip_buf[16];
        atomic_str_get(g_router.ap_ip, ap_ip_buf, sizeof(ap_ip_buf));
        ESP_LOGI(TAG, "Web UI started at http://%s", ap_ip_buf);
        
        char admin_pass_buf[32];
        atomic_str_get(g_router.admin_pass, admin_pass_buf, sizeof(admin_pass_buf));
        if (strcmp(admin_pass_buf, "admin") == 0) {
            ESP_LOGW(TAG, "Admin password is set to default, please change it!");
        } else {
            ESP_LOGI(TAG, "Admin password is configured");
        }
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
    vTaskDelete(NULL);
}

esp_err_t web_ui_start(void) {
    xTaskCreatePinnedToCore(web_server_task, "web_server", 10240, NULL, 5, NULL, 0);
    return ESP_OK;
}
