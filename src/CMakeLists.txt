# ZenClock main component

idf_component_register(
        SRCS "main.c" "app_handlers.c"
        INCLUDE_DIRS "." "../include"
        REQUIRES backoff battery_view bsp input_policy lvgl esp_lvgl_port ui settings deep_sleep
        wifi_manager sntp_sync ble_provisioning microlink
)
