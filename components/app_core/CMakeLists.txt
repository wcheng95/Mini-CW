idf_component_register(
    SRCS "app_core.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_service
        cw_trainer_service
        keyer_service
        platform_hal
        storage_service
        ui_service
    PRIV_REQUIRES
        freertos
        log
)
