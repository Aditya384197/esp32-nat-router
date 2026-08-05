set(COMPONENT_SRCDIRS .)
set(COMPONENT_ADD_INCLUDEDIRS .)

idf_component_register(
    SRCS 
        main.c 
        router_core.c 
        web_ui.c 
        led_status.c
    INCLUDE_DIRS 
        "."
    PRIV_REQUIRES 
        nvs_flash 
        esp_wifi 
        esp_netif 
        esp_event 
        lwip 
        esp_http_server
        mbedtls
        driver          # 🔥 यह लाइन जोड़ें
)
