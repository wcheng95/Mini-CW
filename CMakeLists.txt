cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mini_cw)

partition_table_get_partition_info(MINI_CW_FATFS_OFFSET "--partition-name fatfs" "offset")
if(NOT MINI_CW_FATFS_OFFSET)
    message(FATAL_ERROR "Mini-CW merged image requires a partition named 'fatfs'")
endif()

idf_build_get_property(MINI_CW_PYTHON PYTHON)
add_dependencies(app fatfs_fatfs_bin)
add_custom_command(TARGET app POST_BUILD
    COMMAND ${MINI_CW_PYTHON} -m esptool --chip esp32s3 merge_bin
            -o "${CMAKE_BINARY_DIR}/MiniCW_V1_1_Merged_Auto.bin"
            @flash_args
            ${MINI_CW_FATFS_OFFSET} "${CMAKE_BINARY_DIR}/fatfs.bin"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Auto-generating merged Mini-CW V1.1 binary with factory FATFS preload..."
)
