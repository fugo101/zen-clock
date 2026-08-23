idf_component_register(
        SRCS "src/ble_provisioning.c" "src/prov_session.c"
        INCLUDE_DIRS "include"
        REQUIRES nvs_flash esp_event bt protocomm
        PRIV_REQUIRES espressif__network_provisioning
)
