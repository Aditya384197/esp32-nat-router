# यह 2 लाइनें ESP-IDF को बताती हैं कि हमारी .c फाइलें कहाँ हैं
set(COMPONENT_SRCDIRS .)
set(COMPONENT_ADD_INCLUDEDIRS .)

# यह आपका मूल कोड है (अब esp_http_server के साथ)
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
        esp_http_server    # 🔥 यहाँ बदलाव किया है
)
