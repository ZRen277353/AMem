if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/gui/ai/ChatSession.cpp" CHAT_SESSION)
file(READ "${SOURCE_ROOT}/gui/ai/SessionManager.cpp" SESSION_MANAGER)
file(READ "${SOURCE_ROOT}/gui/ai/ProtectedPersistence.cpp" PROTECTION)
file(READ "${SOURCE_ROOT}/CMakeLists.txt" CMAKE_SOURCE)

foreach(MARKER IN ITEMS
        "CryptProtectData"
        "CryptUnprotectData"
        "AMem.NativeAgent.Session.v1"
        "AMem.NativeAgent.SessionIndex.v1"
        "installTempFile")
    string(FIND "${PROTECTION}" "${MARKER}" MARKER_POS)
    if(MARKER_POS EQUAL -1)
        message(FATAL_ERROR
            "protected persistence is missing required marker: ${MARKER}")
    endif()
endforeach()

foreach(MARKER IN ITEMS
        "saveProtectedJsonDocument"
        "loadProtectedJsonDocument"
        "ProtectedPersistenceKind::Session")
    string(FIND "${CHAT_SESSION}" "${MARKER}" MARKER_POS)
    if(MARKER_POS EQUAL -1)
        message(FATAL_ERROR
            "ChatSession bypasses protected persistence: ${MARKER}")
    endif()
endforeach()

foreach(MARKER IN ITEMS
        "saveProtectedJsonDocument"
        "loadProtectedJsonDocument"
        "migrateSessionArtifacts"
        "ProtectedPersistenceKind::SessionIndex")
    string(FIND "${SESSION_MANAGER}" "${MARKER}" MARKER_POS)
    if(MARKER_POS EQUAL -1)
        message(FATAL_ERROR
            "SessionManager bypasses protected persistence: ${MARKER}")
    endif()
endforeach()

foreach(SOURCE_NAME IN ITEMS CHAT_SESSION SESSION_MANAGER)
    string(FIND "${${SOURCE_NAME}}" "loadJsonDocument(" RAW_LOAD_POS)
    if(NOT RAW_LOAD_POS EQUAL -1)
        message(FATAL_ERROR
            "${SOURCE_NAME} reintroduced plaintext JSON loading")
    endif()
endforeach()

string(FIND "${CMAKE_SOURCE}" "gui/ai/ProtectedPersistence.cpp" SOURCE_POS)
string(FIND "${CMAKE_SOURCE}" "native_session_protection_gate" GATE_POS)
if(SOURCE_POS EQUAL -1 OR GATE_POS EQUAL -1)
    message(FATAL_ERROR
        "protected persistence source/gate is not wired into CMake")
endif()
