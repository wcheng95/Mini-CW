idf_component_register(
    SRCS
        "ui_service.c"
        "ui_cardputer_port.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        M5Cardputer
        freertos
        log
)
