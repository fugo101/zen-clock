idf_component_register(
        SRCS "settings.c" "settings_table.c" "timezone_fmt.c"
        INCLUDE_DIRS "."
        REQUIRES nvs_flash
)
