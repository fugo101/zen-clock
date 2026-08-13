idf_component_register(
        SRCS "settings.c" "timezone_fmt.c"
        INCLUDE_DIRS "."
        REQUIRES nvs_flash
)
