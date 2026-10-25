idf_component_register(
    SRCS "bridge.c" "link_frame.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_driver_uart esp_driver_usb_serial_jtag
)
