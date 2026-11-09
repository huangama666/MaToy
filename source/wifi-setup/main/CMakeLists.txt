idf_component_register(SRCS "vnc_app.c" "vnc_client.c" "vnc_geometry.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer esp_event esp_netif esp_wifi esp_driver_usb_serial_jtag json mbedtls nvs_flash)
