cmake_minimum_required(VERSION 3.1 FATAL_ERROR)

add_definitions("-DACSDK_LOG_MODULE=aip")
add_library(AIP SHARED
    AudioInputProcessor.cpp)
target_include_directories(AIP PUBLIC
    "${AIP_SOURCE_DIR}/include"
    "${AFML_SOURCE_DIR}/include"
    "${AVSCommon_INCLUDE_DIRS}")
target_link_libraries(AIP
    AVSCommon
    ADSL
    AFML)

# install target
asdk_install()
