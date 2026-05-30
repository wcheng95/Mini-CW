idf_component_register(
    SRCS "keyer_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        audio_service
        esp_driver_gpio
        freertos
        log
)
