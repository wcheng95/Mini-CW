idf_component_register(
    SRCS "cw_trainer_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_service
        keyer_service
    PRIV_REQUIRES
        esp_hw_support
        log
)
