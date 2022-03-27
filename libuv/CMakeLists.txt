cmake_minimum_required(VERSION 3.0)
project(minisock-libuv C)

include(FindPkgConfig)

pkg_check_modules(MINISOCK_UV REQUIRED libuv)

include_directories(${MINISOCK_UV_INCLUDE_DIRS} ../include)
add_definitions(${MINISOCK_UV_CFLAGS_OTHER})

add_library(minisock_uv_worker STATIC
    libuv-worker.c)

if(NOT MSVC)
    target_compile_options(minisock_uv_worker
        PRIVATE
        -Wall -pedantic)
endif()

target_link_libraries(minisock_uv_worker
    ${MINISOCK_UV_LIBRARIES})

