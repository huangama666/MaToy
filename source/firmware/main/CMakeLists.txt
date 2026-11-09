idf_component_register(SRCS "screen_app.c" "voice_link.c" "radio_player.c" "radio_ui.c" INCLUDE_DIRS "."
    EMBED_FILES "../assets/images/live_control_standby.rgb565" "../assets/images/live_control_standby_portrait.rgb565"
    REQUIRES bsp esp_event esp_netif esp_wifi esp_driver_usb_serial_jtag esp_hw_support
    esp_http_client mbedtls nvs_flash)
