idf_component_register(
    SRCS
        "audio_service.c"
        "audio_output_port.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        board_cardputer_adv
        freertos
        log
)
