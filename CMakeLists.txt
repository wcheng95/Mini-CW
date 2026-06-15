cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mini_cw)

idf_build_get_property(MINI_CW_PYTHON PYTHON)
add_custom_command(TARGET app POST_BUILD
    COMMAND ${MINI_CW_PYTHON} -m esptool --chip esp32s3 merge_bin
            -o "${CMAKE_BINARY_DIR}/MiniCW_V1_1_Merged_Auto.bin"
            @flash_args
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Auto-generating payload-free merged Mini-CW V1.1 binary..."
)
