cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" cmake_source)

function(require_text source label expected)
    string(FIND "${source}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Missing ${label}: ${expected}")
    endif()
endfunction()

function(forbid_text source label forbidden)
    string(FIND "${source}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "Unexpected ${label}: ${forbidden}")
    endif()
endfunction()

foreach(forbidden IN ITEMS
        "option(ENABLE_AI_CHAT"
        "if(ENABLE_AI_CHAT)"
        "if(HAVE_AI_CHAT)"
        "if(HAVE_CAPSTONE)"
        "if(HAVE_KEYSTONE)"
        "AI_CHAT_SOURCES_CANDIDATES"
        "features will be disabled")
    forbid_text("${cmake_source}" "optional required-product feature path" "${forbidden}")
endforeach()

require_text("${cmake_source}" "required Capstone failure"
    "Capstone is required on NativeAgent")
require_text("${cmake_source}" "required Keystone failure"
    "Keystone is required on NativeAgent")
require_text("${cmake_source}" "required OpenSSL package"
    "find_package(OpenSSL REQUIRED)")
require_text("${cmake_source}" "fixed AI source list"
    "set(AI_CHAT_SOURCES")
require_text("${cmake_source}" "unconditional AI source assembly"
    "list(APPEND GUI_SOURCES \${AI_CHAT_SOURCES})")
require_text("${cmake_source}" "required product implementation macros"
    "target_compile_definitions(ImGuiProject PRIVATE\n    HAVE_CAPSTONE\n    HAVE_KEYSTONE\n    HAVE_AI_CHAT")
require_text("${cmake_source}" "required Capstone link"
    "\${CAPSTONE_LIBRARIES}")
require_text("${cmake_source}" "required Keystone link"
    "\${KEYSTONE_LIBRARIES}")
require_text("${cmake_source}" "required OpenSSL link"
    "OpenSSL::SSL\n    OpenSSL::Crypto")

foreach(document IN ITEMS README.md AGENTS.md CLAUDE.md)
    file(READ "${SOURCE_ROOT}/${document}" content)
    forbid_text("${content}" "obsolete AI build option in ${document}"
        "ENABLE_AI_CHAT")
endforeach()

message(STATUS
    "Verified AI Chat, Capstone, and Keystone remain required product features")
