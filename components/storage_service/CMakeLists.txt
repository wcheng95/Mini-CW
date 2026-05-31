idf_component_register(
    SRCS "storage_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        cw_trainer_service
    PRIV_REQUIRES
        log
)
