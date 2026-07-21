FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
FILE(GLOB_RECURSE wifi_sources ${CMAKE_SOURCE_DIR}/src/wifi/*.c)

idf_component_register(SRCS ${app_sources} ${wifi_sources}
                       INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src"
                                     "${CMAKE_SOURCE_DIR}/src/config"
                                     "${CMAKE_SOURCE_DIR}/src/drivers"
                                     "${CMAKE_SOURCE_DIR}/src/wifi"
                                     "${CMAKE_SOURCE_DIR}/src/ui")