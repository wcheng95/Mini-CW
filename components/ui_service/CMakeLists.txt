idf_component_register(
    SRCS
        "ui_service.c"
        "ui_cardputer_port.cpp"
        "ui_screen.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    PRIV_REQUIRES
        M5Cardputer
        audio_service
        cw_trainer_service
        freertos
        keyer_service
        log
        platform_hal
)
