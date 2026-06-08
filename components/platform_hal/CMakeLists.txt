idf_component_register(
    SRCS "platform_hal.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        board_cardputer_adv
        esp_driver_gpio
        esp_driver_i2c
        esp_hw_support
        esp_timer
        log
)
