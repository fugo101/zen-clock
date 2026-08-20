idf_component_register(
        SRCS "src/bsp_display.c"
        "src/bsp_battery.c"
        "src/bsp_backlight.c"
        "src/bsp_buttons.c"
        INCLUDE_DIRS "include" "../../include"
        PRIV_INCLUDE_DIRS "priv_include"
        REQUIRES esp_lvgl_port driver freertos esp_lcd lvgl esp_timer soc esp_adc lcd_backlight esp_driver_ledc esp_driver_gpio adc_battery_estimation
)
