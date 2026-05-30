idf_component_register(
    SRCS "platform_hal.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        board_cardputer_adv
        esp_driver_gpio
        esp_hw_support
        log
)
