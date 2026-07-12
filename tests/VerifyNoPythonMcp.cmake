cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

foreach(forbidden IN ITEMS
        ".mcp.json"
        "mcp")
    if(EXISTS "${SOURCE_ROOT}/${forbidden}")
        message(FATAL_ERROR
            "Removed Python MCP path must not exist: ${forbidden}")
    endif()
endforeach()

foreach(required IN ITEMS
        "tools/protocol_reference/README.md"
        "tools/protocol_reference/amem_client.py")
    if(NOT EXISTS "${SOURCE_ROOT}/${required}")
        message(FATAL_ERROR
            "Protocol reference artifact is missing: ${required}")
    endif()
endforeach()

set(product_docs
    README.md
    AGENTS.md
    CLAUDE.md
)
set(forbidden_launch_markers
    "python -m amem_mcp"
    "mcp/server.py"
    "pip install mcp"
    "amem-mcp"
)

foreach(document IN LISTS product_docs)
    file(READ "${SOURCE_ROOT}/${document}" content)
    foreach(marker IN LISTS forbidden_launch_markers)
        string(FIND "${content}" "${marker}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR
                "${document} contains removed MCP launch marker: ${marker}")
        endif()
    endforeach()
endforeach()

message(STATUS "Verified Python MCP runtime remains removed")
