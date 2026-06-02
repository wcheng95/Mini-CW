idf_component_register(
    SRCS
        "keyer_service.c"
        "keyer_decoder.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    PRIV_REQUIRES
        audio_service
        esp_driver_gpio
        freertos
        log
)
