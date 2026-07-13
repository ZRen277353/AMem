cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

set(tool_definitions "${SOURCE_ROOT}/gui/ai/ToolDefinitions.cpp")
file(READ "${tool_definitions}" source)
set(ipc_catalog "${SOURCE_ROOT}/ipc/IpcMethodCatalog.cpp")
file(READ "${ipc_catalog}" ipc_source)

string(REGEX MATCHALL
    "registerTool\\([ \t\r\n]*\"[^\"]+\""
    registrations
    "${source}")

set(actual_names "")
foreach(registration IN LISTS registrations)
    string(REGEX REPLACE
        ".*\"([^\"]+)\"$"
        "\\1"
        name
        "${registration}")
    list(APPEND actual_names "${name}")
endforeach()
list(SORT actual_names)

set(expected_names
    breakpoint_hits
    breakpoint_remove
    breakpoint_resume
    breakpoint_set
    breakpoint_suspend
    disassemble
    driver_initialize
    lua_execute
    memory_read
    memory_read_value
    memory_write
    memory_write_value
    module_list
    module_resolve
    pointer_resolve
    process_list
    process_open
    scan_clear
    scan_refine
    scan_results
    scan_start
    status
    symbol_list
    symbol_resolve
)
list(SORT expected_names)

if(NOT actual_names STREQUAL expected_names)
    message(FATAL_ERROR
        "Agent catalog mismatch.\nExpected: ${expected_names}\nActual: ${actual_names}")
endif()

string(REGEX MATCHALL
    "constexpr const char\\* kSchema[A-Za-z0-9_]+"
    schema_declarations
    "${source}")
string(REGEX MATCHALL
    "\"additionalProperties\"[ \t\r\n]*:[ \t\r\n]*false"
    closed_schema_markers
    "${source}")
list(LENGTH schema_declarations schema_count)
list(LENGTH closed_schema_markers closed_schema_count)
if(NOT schema_count EQUAL 22 OR NOT closed_schema_count EQUAL schema_count)
    message(FATAL_ERROR
        "Every canonical tool schema must be a closed root object. "
        "Schemas: ${schema_count}; closed markers: ${closed_schema_count}")
endif()

string(REGEX MATCHALL
    "\\{\"[a-z_]+\", IpcCapability::"
    ipc_registrations
    "${ipc_source}")
set(ipc_names "")
foreach(registration IN LISTS ipc_registrations)
    string(REGEX REPLACE
        ".*\\{\"([a-z_]+)\".*"
        "\\1"
        name
        "${registration}")
    list(APPEND ipc_names "${name}")
endforeach()
list(SORT ipc_names)
if(NOT ipc_names STREQUAL expected_names)
    message(FATAL_ERROR
        "Native IPC catalog mismatch.\nExpected: ${expected_names}\nActual: ${ipc_names}")
endif()

foreach(forbidden IN ITEMS "client_singleton.h" "AppContext.h")
    string(FIND "${source}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "ToolDefinitions.cpp must not depend on ${forbidden}")
    endif()
endforeach()

list(LENGTH actual_names tool_count)
message(STATUS "Verified ${tool_count} canonical Agent and native IPC methods")
