# ZenClock UI component — modular LVGL interface

idf_component_register(
        SRCS "ui.c" "clock_face_text.c" "status_bar.c" "prov_screen.c"
        "nav.c" "menu_screen.c" "settings_screen.c" "device_info_screen.c" "ui_utils.c" "ui_list.c"
        "fonts/lv_font_ds_digital_48.c"
        "fonts/lv_font_ds_digital_16.c"
        INCLUDE_DIRS "."
        REQUIRES lvgl bsp battery_view settings deep_sleep wifi_manager microlink sntp_sync esp_app_format
)
