cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME dlx)
set(EXTENSION_NAME ${TARGET_NAME}_extension)

project(${TARGET_NAME})

# CMAKE_CURRENT_SOURCE_DIR always points to this file's directory,
# regardless of where DuckDB's build machinery invokes cmake from.
set(EXT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(EXTENSION_SOURCES
    ${EXT_ROOT}/src/dlx_extension.cpp
    ${EXT_ROOT}/src/dlx_function.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${EXT_ROOT}/src
    ${EXT_ROOT}/src/include
)
set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD 17)

build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
target_include_directories(${TARGET_NAME}_loadable_extension PRIVATE
    ${EXT_ROOT}/src
    ${EXT_ROOT}/src/include
)
set_property(TARGET ${TARGET_NAME}_loadable_extension PROPERTY CXX_STANDARD 17)
