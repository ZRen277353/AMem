cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

foreach(forbidden_path IN ITEMS
        "ipc/IpcServer.cpp"
        "ipc/IpcServer.h"
        "tests/VerifyLegacyHttpIpcGate.cmake")
    if(EXISTS "${SOURCE_ROOT}/${forbidden_path}")
        message(FATAL_ERROR
            "Removed legacy HTTP IPC path must not exist: ${forbidden_path}")
    endif()
endforeach()

set(product_files
    "CMakeLists.txt"
    "main.cpp")
file(GLOB_RECURSE ipc_sources
    RELATIVE "${SOURCE_ROOT}"
    "${SOURCE_ROOT}/ipc/*.cpp"
    "${SOURCE_ROOT}/ipc/*.h")
list(APPEND product_files ${ipc_sources})

set(forbidden_markers
    "ENABLE_LEGACY_HTTP_IPC"
    "HAVE_LEGACY_HTTP_IPC"
    "IpcServer"
    "127.0.0.1:28100"
    "Access-Control-Allow-Origin")

foreach(product_file IN LISTS product_files)
    file(READ "${SOURCE_ROOT}/${product_file}" content)
    foreach(marker IN LISTS forbidden_markers)
        string(FIND "${content}" "${marker}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR
                "${product_file} contains removed legacy IPC marker: ${marker}")
        endif()
    endforeach()
endforeach()

message(STATUS "Verified legacy HTTP IPC product code remains removed")
