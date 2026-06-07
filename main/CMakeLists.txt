idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_core
    PRIV_REQUIRES fatfs
)

fatfs_create_spiflash_image(fatfs ../fatfs_image PRESERVE_TIME)
