idf_component_register(
    SRCS
        "cw_trainer_service.c"
        "cw_lesson_mode.c"
        "cw_word_mode.c"
        "cw_callsign_mode.c"
        "cw_plaintext_mode.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES
        audio_service
        keyer_service
    PRIV_REQUIRES
        esp_hw_support
        log
)
